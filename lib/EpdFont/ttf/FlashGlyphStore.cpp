#include "FlashGlyphStore.h"

#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstring>

using namespace glyphstore;

static_assert(sizeof(EpdGlyph) + 4 == kRecordHeaderBytes, "a record header is the key plus one EpdGlyph");
static_assert(offsetof(EpdGlyph, dataOffset) % 4 == 0, "EpdGlyph::dataOffset must be readable in place");
static_assert(NorFlash::kSectorBytes == kSectorBytes && NorFlash::kMapPageBytes == kPageBytes,
              "the format and the flash agree on sector and page sizes");

namespace {

// Loads from mapped flash. Each is 4-byte aligned: mappings start on a page, and every offset the store
// reads at is a multiple of 4. Saying so lets the compiler emit plain loads; without it RISC-V calls
// memcpy for every field, which costs more than the lookup itself. A record's EpdGlyph is read in place
// (recordGlyph()) for the same reason: copying 16 bytes out is a memcpy call too.
uint32_t flashU32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, __builtin_assume_aligned(bytes, 4), sizeof value);
  return value;
}

Slot flashSlot(const uint8_t* table, const uint32_t i) {
  Slot slot;
  std::memcpy(&slot, __builtin_assume_aligned(table + static_cast<size_t>(i) * kSlotBytes, 4), sizeof slot);
  return slot;
}

// Loads from a caller's batch buffer, which need not be aligned.
uint32_t ramU32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof value);
  return value;
}

EpdGlyph ramGlyph(const uint8_t* bytes) {
  EpdGlyph glyph;
  std::memcpy(&glyph, bytes, sizeof glyph);
  return glyph;
}

bool slotErased(const Slot& slot) { return slot.key == kErased && slot.value == kErased; }

uint32_t nextSlot(const uint32_t i, const uint32_t slots) { return i + 1 == slots ? 0 : i + 1; }

uint32_t tableBytes(const uint32_t slots) { return alignUp(slots * kSlotBytes, kSectorBytes); }

// A reclaim copies a record through this much stack, a piece at a time: flash cannot be programmed from mapped flash.
// The first piece holds the record's header, whose dataOffset the copy rewrites.
constexpr uint32_t kCopyPieceBytes = 512;
static_assert(kCopyPieceBytes >= kRecordHeaderBytes && kCopyPieceBytes % 4 == 0, "a piece holds a record's header");

// The most entries a table of `capacity` takes before it counts as full.
uint32_t limit(const uint32_t capacity, const uint32_t percent) {
  return static_cast<uint32_t>(uint64_t{capacity} * percent / 100);
}

// Slots in use: every one not wholly erased, a slot cut short included. Of them, the "no glyph" marks go to `marks`
// (a cut leaves extra 1 bits, so a torn slot never reads as one).
uint32_t countUsed(const uint8_t* table, const uint32_t slots, uint32_t* marks = nullptr) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < slots; ++i) {
    const Slot slot = flashSlot(table, i);
    if (slotErased(slot)) continue;
    ++used;
    if (marks && slot.value == kNoGlyph) ++*marks;
  }
  return used;
}

// A segment header that only a complete write produces (a cut leaves extra 1 bits, which fail the check), of a
// segment not retired.
bool liveHeader(const SegmentHeader& h) {
  return h.magic == kSegmentMagic && h.epoch != kErased && h.epochCheck == ~h.epoch && h.retired == kErased;
}

// The regions are sector-aligned, in the order index, pairs, log, after the header and state sectors, and
// inside the flash. Each table is at least kMaxProbe slots, so a probe never wraps onto itself.
bool validGeometry(const Geometry& g, const uint32_t flashSize) {
  if (g.styleCount == 0 || g.styleCount > kMaxStyles) return false;
  if (g.indexSlots < kMaxProbe || g.pairSlots < kMaxProbe || g.logSize == 0 || g.logSize % kSectorBytes != 0) {
    return false;
  }
  // The log is a ring of 4 to 64 equal segments of whole sectors, and under kMaxLogBytes so its offsets fit a slot
  // value.
  if (g.segmentBytes < kSectorBytes || g.segmentBytes % kSectorBytes != 0 || g.logSize % g.segmentBytes != 0 ||
      g.logSize / g.segmentBytes < kMinSegments || g.logSize / g.segmentBytes > kMaxSegments ||
      g.logSize >= kMaxLogBytes) {
    return false;
  }
  if (g.indexOffset % kSectorBytes != 0 || g.pairOffset % kSectorBytes != 0 || g.logOffset % kSectorBytes != 0) {
    return false;
  }
  const uint64_t indexEnd = uint64_t{g.indexOffset} + uint64_t{g.indexSlots} * kSlotBytes;
  const uint64_t pairEnd = uint64_t{g.pairOffset} + uint64_t{g.pairSlots} * kSlotBytes;
  return g.indexOffset >= kFirstRegionOffset && indexEnd <= g.pairOffset && pairEnd <= g.logOffset &&
         uint64_t{g.logOffset} + g.logSize <= flashSize;
}

// The style blocks are sector-aligned, in order, between the state sector and the index, the SD-mode ones after
// every flash-mode one; an SD-mode style's outlines lie in its file and past its block.
bool validStyles(const StyleInfo* styles, const Geometry& g) {
  uint32_t floor = kFirstRegionOffset;
  bool sd = false;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    const StyleInfo& style = styles[s];
    if (style.offset < floor || style.offset % kSectorBytes != 0 || style.size == 0) return false;
    if (uint64_t{style.offset} + style.size > g.indexOffset || style.mode > kModeSd) return false;
    if (style.mode == kModeFlash && sd) return false;
    if (style.mode == kModeSd) {
      const SdOutlines& o = style.outlines;
      if (o.fontOffset < style.size || uint64_t{o.fontOffset} + o.length > UINT32_MAX ||
          uint64_t{o.fileOffset} + o.length > style.fileSize) {
        return false;
      }
      sd = true;
    }
    floor = style.offset + style.size;
  }
  return true;
}

bool validHeader(const Header& h, const uint32_t flashSize) {
  return h.magic == kMagic && h.version == kFormatVersion && h.headerBytes == sizeof(Header) &&
         h.check == fnv1a(&h, offsetof(Header, check)) && validGeometry(h.geometry, flashSize) &&
         validStyles(h.styles, h.geometry);
}

}  // namespace

// --- Install ---

bool FlashGlyphStore::beginInstall(const Layout& layout, const uint8_t identity[kIdentityBytes],
                                   const StyleInfo* sdInfo, const uint8_t keepStyles) {
  close();
  const Geometry& g = layout.geometry;
  bool sd = false;
  for (uint8_t s = 0; s < g.styleCount && s < kMaxStyles; ++s) sd = sd || layout.styleMode[s] == kModeSd;
  if (!validGeometry(g, flash_.size()) || (sd && !sdInfo)) {
    LOG_ERR("GST", "install: the layout does not fit the flash, or SD mode lacks its files");
    return false;
  }
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "install: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return false;
  }
  header->magic = kMagic;
  header->version = kFormatVersion;
  header->headerBytes = sizeof(Header);
  std::memcpy(header->identity, identity, kIdentityBytes);
  header->geometry = g;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    StyleInfo& style = header->styles[s];
    if (layout.styleMode[s] == kModeSd) style = sdInfo[s];
    style.offset = layout.styleOffset[s];
    style.size = layout.styleSize[s];
    style.mode = layout.styleMode[s];
  }
  if (!validStyles(header->styles, g)) {
    LOG_ERR("GST", "install: the style blocks or their outlines do not fit");
    return false;
  }

  // The header sector goes first: from here until finishInstall() the store reads as not installed.
  bool erased = eraseWritten(0, kFirstRegionOffset);
  for (uint8_t s = 0; erased && s < g.styleCount; ++s) {
    if ((keepStyles & (1u << s)) != 0) continue;
    erased = eraseWritten(layout.styleOffset[s], alignUp(layout.styleSize[s], kSectorBytes));
  }
  erased = erased && eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) &&
           eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) && eraseWritten(g.logOffset, g.logSize);
  if (!erased) {
    LOG_ERR("GST", "install: erase failed");
    return false;
  }
  installing_ = std::move(header);
  return true;
}

bool FlashGlyphStore::writeStyleData(const uint8_t style, const uint32_t offset, const void* data, const uint32_t len) {
  if (!installing_ || style >= installing_->geometry.styleCount) return false;
  const StyleInfo& info = installing_->styles[style];
  if (offset > info.size || len > info.size - offset) return false;
  return flash_.program(info.offset + offset, data, len);
}

bool FlashGlyphStore::finishInstall() {
  if (!installing_) return false;
  installing_->check = fnv1a(installing_.get(), offsetof(Header, check));
  const bool written = flash_.program(0, installing_.get(), sizeof(Header));
  installing_.reset();
  if (!written) {
    LOG_ERR("GST", "install: header write failed");
  }
  return written;
}

// --- Session ---

FlashGlyphStore::OpenResult FlashGlyphStore::open(const uint8_t identity[kIdentityBytes]) {
  close();
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "open: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return OpenResult::NoMemory;
  }
  if (!flash_.read(0, header.get(), sizeof(Header)) || !validHeader(*header, flash_.size())) {
    return OpenResult::NotInstalled;
  }
  if (std::memcmp(header->identity, identity, kIdentityBytes) != 0) return OpenResult::OtherFamily;
  const Geometry& g = header->geometry;
  region_ = flash_.map(g.indexOffset, g.logOffset + g.logSize - g.indexOffset);
  if (!region_) {
    LOG_ERR("GST", "open: cannot map the index and log (%u B, %u map pages free)",
            static_cast<unsigned>(g.logOffset + g.logSize - g.indexOffset),
            static_cast<unsigned>(flash_.freeMapPages()));
    return OpenResult::MapFailed;
  }
  geometry_ = g;
  firstSd_ = g.styleCount;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    styleOffset_[s] = header->styles[s].offset;
    styleSize_[s] = header->styles[s].size;
    if (header->styles[s].mode == kModeSd && firstSd_ == g.styleCount) firstSd_ = s;
  }
  index_ = region_;
  pairs_ = region_ + (g.pairOffset - g.indexOffset);
  log_ = region_ + (g.logOffset - g.indexOffset);
  if (firstSd_ < g.styleCount) {
    // The SD-mode blocks, which stay mapped, go right beside the regions, before any flash-mode block: a flash-mode
    // block mapped after them leaves, when it goes, a gap that joins the free pages past it. Mapped after one, they
    // would split those pages, and a larger flash-mode block could find no run long enough to map into.
    const uint8_t last = g.styleCount - 1;
    sdMap_ = flash_.map(styleOffset_[firstSd_], styleOffset_[last] + styleSize_[last] - styleOffset_[firstSd_]);
    if (!sdMap_) {
      LOG_ERR("GST", "open: cannot map the SD-mode styles' tables (%u map pages free)",
              static_cast<unsigned>(flash_.freeMapPages()));
      close();
      return OpenResult::MapFailed;
    }
  }
  uint32_t wipeWord = kErased;
  if (!flash_.read(kWipeWordOffset, &wipeWord, sizeof wipeWord)) wipeWord = 0;  // unreadable: assume pending
  wipePending_ = wipeWord != kErased;
  writesStopped_ = wipePending_;  // what filled the store, or failed, last session is not known
  for (uint8_t i = 0; i < kFaceEntries; ++i) {
    FaceEntry entry{};
    if (readFaceEntry(i, entry) && entry.claimed == 0) claimedFaces_ |= 1u << i;
  }
  readJournal();
  findRing();
  usedSlots_ = countUsed(index_, g.indexSlots, &marks_);
  usedPairs_ = countUsed(pairs_, g.pairSlots);
  open_ = true;
  LOG_INF("GST", "open: %u of %u segments live, log %u of %u B, epoch %u, %u of %u slots (%u live), %u pairs%s%s",
          static_cast<unsigned>(live_), static_cast<unsigned>(segmentCount()), static_cast<unsigned>(logUsed()),
          static_cast<unsigned>(g.logSize), static_cast<unsigned>(nextEpoch_), static_cast<unsigned>(usedSlots_),
          static_cast<unsigned>(g.indexSlots), static_cast<unsigned>(liveSlots_), static_cast<unsigned>(usedPairs_),
          journal_.done ? "" : ", a rebuild to redo", wipePending_ ? ", wipe pending" : "");
  // A ring already near full keeps its counts from the start, so the first draws count; open() runs at a set's load,
  // not inside a draw. A pending wipe would drop them.
  if (!wipePending_ && countsWanted()) keepCounts();
  return OpenResult::Ready;
}

void FlashGlyphStore::close() {
  if (styleMap_) flash_.unmap(styleMap_);
  if (sdMap_) flash_.unmap(sdMap_);
  if (region_) flash_.unmap(region_);
  styleMap_ = sdMap_ = nullptr;
  mappedStyle_ = 0xFF;
  firstSd_ = 0;
  region_ = index_ = pairs_ = log_ = nullptr;
  geometry_ = Geometry{};
  resetRing();
  journal_ = Journal{};
  rebuilds_ = 0;
  rebuildStarved_ = false;
  usedSlots_ = marks_ = usedPairs_ = claimedFaces_ = 0;
  dropCounts();
  open_ = false;
  wipePending_ = false;
  writesStopped_ = false;
  installing_.reset();  // an install that was never finished
}

const uint8_t* FlashGlyphStore::mapStyle(const uint8_t style) {
  if (!open_ || style >= geometry_.styleCount) return nullptr;
  // The SD-mode blocks, the last ones, are mapped together from open() on: the budget counted them all.
  if (style >= firstSd_) return sdMap_ + (styleOffset_[style] - styleOffset_[firstSd_]);
  // Flash mode: one style at a time, so the budget holds only the largest.
  if (styleMap_ && mappedStyle_ == style) return styleMap_;
  if (styleMap_) flash_.unmap(styleMap_);
  styleMap_ = flash_.map(styleOffset_[style], styleSize_[style]);
  mappedStyle_ = styleMap_ ? style : 0xFF;
  if (!styleMap_) {
    LOG_ERR("GST", "cannot map style %u (%u B)", style, static_cast<unsigned>(styleSize_[style]));
  }
  return styleMap_;
}

bool FlashGlyphStore::styleInfo(const uint8_t style, StyleInfo& out) const {
  if (!open_ || style >= geometry_.styleCount) return false;
  return flash_.read(static_cast<uint32_t>(offsetof(Header, styles) + style * sizeof(StyleInfo)), &out, sizeof out);
}

// --- The log ---

// A record the scan can step over: a glyph key, a bitmap length that matches its size and the cap, a dataOffset
// that points just past its own header, and an end at or before `end` (its segment's). A header torn by a power cut
// fails one of these.
bool FlashGlyphStore::plausibleRecord(const uint32_t offset, const uint32_t end) const {
  if (offset % 4 != 0 || offset > end || end - offset < kRecordHeaderBytes) return false;
  if (!isGlyphKey(flashU32(log_ + offset))) return false;
  const EpdGlyph& glyph = recordGlyph(offset);
  return glyph.dataLength <= kMaxBitmapBytes && glyph.dataLength == glyphDataBytes(glyph.width, glyph.height, true) &&
         glyph.dataOffset == offset + kRecordHeaderBytes && recordBytes(glyph.dataLength) <= end - offset;
}

// Bytes in use in `segment`, its header included: where writing resumes when it is the head. Calls visit(offset)
// for each record the walk steps over, in log order.
template <typename Visit>
uint32_t FlashGlyphStore::scanLog(const uint32_t segment, Visit&& visit) const {
  const uint32_t start = segment * geometry_.segmentBytes;
  const uint32_t size = start + geometry_.segmentBytes;
  uint32_t offset = start + kSegmentHeaderBytes;
  for (;;) {
    while (plausibleRecord(offset, size)) {
      const uint32_t record = offset;
      offset += recordBytes(recordGlyph(offset).dataLength);
      visit(record);
    }
    // The walk stopped at an erased key, a torn record or the segment's end. A cut write spans at most one record,
    // and can have left programmed bytes after the stop, even behind a key that still reads erased.
    const uint32_t window = size - offset < kMaxRecordBytes ? size : offset + kMaxRecordBytes;
    uint32_t end = offset;  // one past the last programmed byte in the window
    for (uint32_t i = window; i > offset; --i) {
      if (log_[i - 1] != 0xFF) {
        end = i;
        break;
      }
    }
    if (end == offset) return offset - start;  // nothing after the stop: writing resumes here
    // Every session after a torn write resumed on a sector boundary past it, inside the segment or, at its end, in
    // the next one. If one of the boundaries up to the programmed bytes starts a record, the walk carries on there;
    // otherwise it goes on past them, at the next boundary. Without this, a later boot would stop at the same torn
    // record and write over what those sessions stored.
    uint32_t next = alignUp(end, kSectorBytes);
    for (uint32_t boundary = alignUp(offset + 1, kSectorBytes); boundary < next; boundary += kSectorBytes) {
      if (plausibleRecord(boundary, size)) {
        next = boundary;
        break;
      }
    }
    if (next >= size) return geometry_.segmentBytes;  // the segment is closed: nothing more goes into it
    LOG_INF("GST", "log: torn write at %u; carrying on at %u", static_cast<unsigned>(offset),
            static_cast<unsigned>(next));
    offset = next;
  }
}

// Finds the ring from the segment headers. The head is the live segment with the largest epoch. Walking back from
// it, each segment with a live header and an epoch below the one after it is live too; the first that is not ends
// the run, and every segment outside it is free. With no live header the ring is empty: the first record opens
// segment 0.
void FlashGlyphStore::findRing() {
  resetRing();
  const uint32_t count = segmentCount();
  uint32_t found[kMaxSegments];
  uint32_t head = count;
  for (uint32_t s = 0; s < count; ++s) {
    SegmentHeader header;
    std::memcpy(&header, __builtin_assume_aligned(log_ + s * geometry_.segmentBytes, 4), sizeof header);
    found[s] = liveHeader(header) ? header.epoch : kErased;
    if (found[s] != kErased && (head == count || found[s] > found[head])) head = s;
  }
  if (head == count) return;
  nextEpoch_ = found[head] + 1;
  segEpoch_[head] = found[head];
  head_ = tail_ = static_cast<uint8_t>(head);
  live_ = 1;
  for (uint32_t s = head; live_ < count;) {
    const uint32_t prev = s == 0 ? count - 1 : s - 1;
    if (found[prev] == kErased || found[prev] >= found[s]) break;
    segEpoch_[prev] = found[prev];
    tail_ = static_cast<uint8_t>(prev);
    ++live_;
    s = prev;
  }
  // The head resumes after its last record; the segments behind it are closed. A record counts as a live slot when a
  // slot names it, as a reclaim counts what it takes away: one whose write a cut stopped before its slot does not.
  for (uint32_t s = tail_, n = 0; n < live_; s = s + 1 == count ? 0 : s + 1, ++n) {
    const uint32_t epoch = segEpoch_[s];
    const uint32_t used = scanLog(s, [this, epoch](const uint32_t offset) {
      if (findSlot(flashU32(log_ + offset), glyphSlotValue(epoch, recordGlyph(offset).dataOffset)) != UINT32_MAX) {
        ++liveSlots_;
      }
    });
    if (s == head_) headUsed_ = used;
  }
}

// Calls visit(segment, offset) for each record of the live segments, from the tail, in log order.
template <typename Visit>
void FlashGlyphStore::walkLive(Visit&& visit) const {
  const uint32_t count = segmentCount();
  for (uint32_t s = tail_, n = 0; n < live_; s = s + 1 == count ? 0 : s + 1, ++n) {
    scanLog(s, [&visit, s](const uint32_t offset) { visit(s, offset); });
  }
}

// Opens the segment after the head (with none live, head_ itself): erases it if it holds data (a header a cut tore,
// or a retired segment whose erase a cut stopped), writes its header with the next epoch, and makes it the head. The
// header's `retired` word stays erased until the segment is retired. False with no free segment or no epoch left before
// a rebuild (callers check segmentsToOpen() first), or when a write failed.
bool FlashGlyphStore::moveHead() {
  const uint32_t count = segmentCount();
  if (live_ >= count || epochsLeft() == 0) return false;
  const uint32_t next = live_ == 0 ? head_ : (head_ + 1u == count ? 0 : head_ + 1u);
  const uint32_t at = geometry_.logOffset + next * geometry_.segmentBytes;
  const uint32_t epoch = nextEpoch_++;
  const SegmentHeader header{kSegmentMagic, epoch, ~epoch, kErased};
  if (!eraseWritten(at, geometry_.segmentBytes) || !flash_.program(at, &header, offsetof(SegmentHeader, retired))) {
    return false;
  }
  segEpoch_[next] = epoch;
  if (live_ == 0) tail_ = static_cast<uint8_t>(next);
  head_ = static_cast<uint8_t>(next);
  ++live_;
  headUsed_ = kSegmentHeaderBytes;
  return true;
}

void FlashGlyphStore::placeRecord(const uint32_t bytes, uint32_t& used, uint32_t& opens) const {
  if (!fitsSegment(used, bytes)) {
    ++opens;
    used = kSegmentHeaderBytes;
  }
  used += bytes;
}

// Every segment free, as after a wipe; the epochs start again at 1, since no slot names one any more.
void FlashGlyphStore::resetRing() {
  for (uint32_t& epoch : segEpoch_) epoch = kErased;
  head_ = tail_ = live_ = 0;
  headUsed_ = 0;
  nextEpoch_ = 1;
  liveSlots_ = 0;
  chainFull_ = indexFull_ = roomAsked_ = false;
}

// --- Erasing ---

// Erases are what wear the chip, and one costs 4-6 ms even on a blank sector (measured on the X3), so only
// sectors that hold data are erased; neighbouring ones go in one call.
bool FlashGlyphStore::eraseWritten(const uint32_t offset, const uint32_t len) {
  uint32_t runStart = 0;
  uint32_t runLen = 0;
  for (uint32_t sector = offset; sector < offset + len; sector += kSectorBytes) {
    if (!isBlank(sector)) {
      if (runLen == 0) runStart = sector;
      runLen += kSectorBytes;
      continue;
    }
    if (runLen != 0 && !flash_.erase(runStart, runLen)) return false;
    runLen = 0;
  }
  return runLen == 0 || flash_.erase(runStart, runLen);
}

// Through the mapping when the sector is in it (a wipe), else by reading small chunks (an install).
bool FlashGlyphStore::isBlank(const uint32_t sector) const {
  if (region_ && sector >= geometry_.indexOffset && sector + kSectorBytes <= geometry_.logOffset + geometry_.logSize) {
    const uint8_t* bytes = region_ + (sector - geometry_.indexOffset);
    for (uint32_t i = 0; i < kSectorBytes; i += 4) {
      if (flashU32(bytes + i) != kErased) return false;
    }
    return true;
  }
  uint8_t chunk[256];
  for (uint32_t at = 0; at < kSectorBytes; at += sizeof chunk) {
    if (!flash_.read(sector + at, chunk, sizeof chunk)) return false;  // unreadable: erase it
    for (const uint8_t b : chunk) {
      if (b != 0xFF) return false;
    }
  }
  return true;
}

// --- Glyphs ---

uint8_t* GlyphBatch::add(const uint32_t key, const EpdGlyph& metrics) {
  // A record whose key or length is malformed would read as torn to the log scan.
  if (!isGlyphKey(key) || metrics.dataLength > kMaxBitmapBytes ||
      metrics.dataLength != glyphDataBytes(metrics.width, metrics.height, true)) {
    return nullptr;
  }
  const uint32_t bytes = recordBytes(metrics.dataLength);
  if (bytes > capacity_ - used_) return nullptr;
  uint8_t* record = buffer_ + used_;
  std::memset(record, 0xFF, bytes);  // the padding stays erased
  std::memcpy(record, &key, sizeof key);
  EpdGlyph glyph = metrics;
  glyph.dataOffset = 0;  // known at commit, when the record's place in the log is
  std::memcpy(record + 4, &glyph, sizeof glyph);
  lastUsed_ = used_;
  used_ += bytes;
  ++count_;
  return record + kRecordHeaderBytes;
}

void GlyphBatch::dropLast() {
  if (count_ == 0 || used_ == lastUsed_) return;
  used_ = lastUsed_;
  --count_;
}

// The record a glyph slot points at, if it is complete and really this key's: the slot value packs the record's
// dataOffset with the epoch its segment had when the slot was written. That segment must still be live with that
// epoch (a segment freed, or opened again since, holds other records now or will), and the record must lie inside
// what the segment holds.
const EpdGlyph* FlashGlyphStore::validRecord(const uint32_t key, const uint32_t value) const {
  const uint32_t dataOffset = slotDataOffset(value);
  const uint32_t segment = dataOffset / geometry_.segmentBytes;
  if (segment >= segmentCount() || segEpoch_[segment] == kErased ||
      (segEpoch_[segment] & kSlotEpochMask) != slotEpoch(value)) {
    return nullptr;
  }
  const uint32_t start = segment * geometry_.segmentBytes;
  const uint32_t end = start + (segment == head_ ? headUsed_ : geometry_.segmentBytes);
  if (dataOffset < start + kSegmentHeaderBytes + kRecordHeaderBytes || dataOffset > end) return nullptr;
  const uint32_t offset = dataOffset - kRecordHeaderBytes;
  if (flashU32(log_ + offset) != key || !plausibleRecord(offset, end)) return nullptr;
  return &recordGlyph(offset);
}

FlashGlyphStore::Lookup FlashGlyphStore::find(const uint32_t key, const EpdGlyph** glyph) const {
  if (!open_ || !isGlyphKey(key)) return Lookup::NotStored;
  const uint32_t slots = geometry_.indexSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    const Slot slot = flashSlot(index_, i);
    if (slotErased(slot)) return Lookup::NotStored;
    if (slot.key != key) continue;
    if (slot.value == kNoGlyph) return Lookup::NoGlyph;
    if (const EpdGlyph* found = validRecord(key, slot.value)) {
      if (glyph) *glyph = found;
      return Lookup::Found;
    }
    // A slot cut short, or one whose record never completed: keep probing.
  }
  return Lookup::NotStored;
}

// Puts {key, value} in the first erased slot of its probe chain, whose index goes to `at`.
FlashGlyphStore::Insert FlashGlyphStore::insert(const uint32_t tableOffset, const uint8_t* table, const uint32_t slots,
                                                const uint32_t key, const uint32_t value, uint32_t* at) {
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    // A slot in use, or one cut short (some bits programmed), is never programmed again.
    if (!slotErased(flashSlot(table, i))) continue;
    const Slot slot{key, value};
    if (!flash_.program(tableOffset + i * kSlotBytes, &slot, sizeof slot)) return Insert::WriteFailed;
    if (at) *at = i;
    return Insert::Done;
  }
  return Insert::ChainFull;
}

// One glyph slot (a record's glyphSlotValue(), or kNoGlyph), and what a failure means for the store: a full probe
// chain stops writes until the index is rebuilt, a failed write until a wipe.
FlashGlyphStore::Commit FlashGlyphStore::insertGlyphSlot(const uint32_t key, const uint32_t value, uint32_t* at) {
  switch (insert(geometry_.indexOffset, index_, geometry_.indexSlots, key, value, at)) {
    case Insert::Done:
      ++usedSlots_;
      if (value == kNoGlyph) {
        ++marks_;
      } else {
        ++liveSlots_;
      }
      return Commit::Stored;
    case Insert::ChainFull:
      LOG_INF("GST", "full: a probe chain passed %u slots; the index needs a rebuild",
              static_cast<unsigned>(kMaxProbe));
      chainFull_ = true;
      writesStopped_ = true;
      return Commit::Full;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "slot write failed; wipe pending");
  stopWrites();
  return Commit::WriteFailed;
}

FlashGlyphStore::Commit FlashGlyphStore::commit(GlyphBatch& batch) {
  const Commit result = batch.count_ == 0 ? Commit::Stored : writeBatch(batch);
  // A commit runs inside a page draw or a section build: the counts' block is only marked due here, and taken at the
  // next safe point (keepCountsIfDue()).
  if (open_ && !wipePending_ && !useCounts_ && !countsTried_ && countsWanted()) countsDue_ = true;
  batch.clear();  // the only place a batch is cleared, so no record is ever written twice
  return result;
}

FlashGlyphStore::Commit FlashGlyphStore::writeBatch(GlyphBatch& batch) {
  if (!open_) return Commit::Refused;
  const uint32_t segmentBytes = geometry_.segmentBytes;
  // Every key must be claimed and every record must fit a segment's payload. The batch is laid out as it will be
  // written, to count the segments it opens: with none live, the first record opens one.
  uint32_t used = live_ == 0 ? segmentBytes : headUsed_;
  uint32_t opens = 0;
  for (uint32_t at = 0; at < batch.used_;) {
    const uint32_t bytes = recordBytes(ramGlyph(batch.buffer_ + at + 4).dataLength);
    if (!faceClaimed(ramU32(batch.buffer_ + at))) {
      LOG_ERR("GST", "commit refused: a key uses a face index not claimed since the last wipe");
      return Commit::Refused;
    }
    if (!fitsSegment(kSegmentHeaderBytes, bytes)) {
      LOG_ERR("GST", "commit refused: a %u B record is larger than a segment", static_cast<unsigned>(bytes));
      return Commit::Refused;
    }
    placeRecord(bytes, used, opens);
    at += bytes;
  }
  if (writesStopped_) return Commit::Full;
  if (usedSlots_ + batch.count_ > limit(geometry_.indexSlots, kIndexLoadPercent)) {
    LOG_INF("GST", "full: %u of %u slots; the index needs a rebuild", static_cast<unsigned>(usedSlots_),
            static_cast<unsigned>(geometry_.indexSlots));
    indexFull_ = true;
    writesStopped_ = true;
    return Commit::Full;
  }
  if (opens > freeSegments()) {
    LOG_INF("GST", "full: %u of %u segments free, %u B batch; room must be made", static_cast<unsigned>(freeSegments()),
            static_cast<unsigned>(segmentCount()), static_cast<unsigned>(batch.used_));
    stopForRoom();
    return Commit::Full;
  }
  if (opens > epochsLeft()) {
    LOG_INF("GST", "full: epoch %u is %u past the last rebuild's; the index needs a rebuild",
            static_cast<unsigned>(nextEpoch_), static_cast<unsigned>(nextEpoch_ - journal_.floor));
    writesStopped_ = true;
    return Commit::Full;
  }
  // The records go one program call per segment: each run is what fits the head, which moves on in between. Each
  // bitmap's offset, relative to the log base, is known once its run's place is.
  for (uint32_t at = 0; at < batch.used_;) {
    if (live_ == 0 || !fitsSegment(headUsed_, recordBytes(ramGlyph(batch.buffer_ + at + 4).dataLength))) {
      if (!moveHead()) {
        LOG_ERR("GST", "opening a log segment failed; wipe pending");
        stopWrites();
        return Commit::WriteFailed;
      }
    }
    const uint32_t first = at;
    const uint32_t base = head_ * segmentBytes + headUsed_;
    while (at < batch.used_) {
      EpdGlyph glyph = ramGlyph(batch.buffer_ + at + 4);
      const uint32_t bytes = recordBytes(glyph.dataLength);
      if (!fitsSegment(headUsed_ + (at - first), bytes)) break;
      glyph.dataOffset = base + (at - first) + kRecordHeaderBytes;
      std::memcpy(batch.buffer_ + at + 4, &glyph, sizeof glyph);
      at += bytes;
    }
    if (!flash_.program(geometry_.logOffset + base, batch.buffer_ + first, at - first)) {
      LOG_ERR("GST", "log write of %u B at %u failed; wipe pending", static_cast<unsigned>(at - first),
              static_cast<unsigned>(base));
      stopWrites();
      return Commit::WriteFailed;
    }
    headUsed_ += at - first;
  }
  // Then the slots, so a probe never finds a slot whose record is incomplete. A slot's value is never 0 (its offset
  // is past the segment's header): a cut can only leave extra 1 bits, so a torn slot never reads as kNoGlyph.
  for (uint32_t at = 0; at < batch.used_;) {
    const EpdGlyph glyph = ramGlyph(batch.buffer_ + at + 4);
    const uint32_t epoch = segEpoch_[glyph.dataOffset / segmentBytes];
    const Commit slot = insertGlyphSlot(ramU32(batch.buffer_ + at), glyphSlotValue(epoch, glyph.dataOffset));
    if (slot != Commit::Stored) return slot;
    at += recordBytes(glyph.dataLength);
  }
  return Commit::Stored;
}

FlashGlyphStore::Commit FlashGlyphStore::markNoGlyph(const uint32_t key) {
  if (!open_ || !isGlyphKey(key) || !faceClaimed(key)) return Commit::Refused;
  if (writesStopped_) return Commit::Full;
  if (usedSlots_ + 1 > limit(geometry_.indexSlots, kIndexLoadPercent)) {
    LOG_INF("GST", "full: %u of %u slots; the index needs a rebuild", static_cast<unsigned>(usedSlots_),
            static_cast<unsigned>(geometry_.indexSlots));
    indexFull_ = true;
    writesStopped_ = true;
    return Commit::Full;
  }
  return insertGlyphSlot(key, kNoGlyph);
}

// --- Use counts ---

// The slot holding exactly this key and value, probed as find() does; UINT32_MAX when none. open() uses it before the
// store counts as open, once the index is mapped.
uint32_t FlashGlyphStore::findSlot(const uint32_t key, const uint32_t value) const {
  if (!index_) return UINT32_MAX;
  const uint32_t slots = geometry_.indexSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    const Slot slot = flashSlot(index_, i);
    if (slotErased(slot)) break;
    if (slot.key == key && slot.value == value) return i;
  }
  return UINT32_MAX;
}

// A slot's count, two bits of useCounts_; 0 while counts are not kept.
uint8_t FlashGlyphStore::countAt(const uint32_t slot) const {
  if (!useCounts_) return 0;
  return (useCounts_[slot / 4] >> ((slot % 4) * 2)) & 3u;
}

void FlashGlyphStore::setCount(const uint32_t slot, const uint8_t count) {
  if (!useCounts_ || slot >= geometry_.indexSlots) return;
  uint8_t& byte = useCounts_[slot / 4];
  const uint32_t shift = (slot % 4) * 2;
  byte = static_cast<uint8_t>((byte & ~(3u << shift)) | ((count & 3u) << shift));
}

void FlashGlyphStore::dropCounts() {
  useCounts_.reset();
  countsTried_ = false;
  countsDue_ = false;
  drawsCounted_ = 0;
}

// Counts are only wanted when room will have to be made: once the live segments reach kCountsAtPercent of the log, or
// the free ones come within one of the target.
bool FlashGlyphStore::countsWanted() const {
  return uint64_t{live_} * 100 >= uint64_t{segmentCount()} * kCountsAtPercent ||
         freeSegments() <= kFreeSegmentsTarget + 1;
}

bool FlashGlyphStore::keepCountsIfDue() {
  if (countsDue_) keepCounts();
  return countsKept();
}

// One try a session: a refusal is not asked again until open().
void FlashGlyphStore::keepCounts() {
  countsDue_ = false;
  if (useCounts_ || countsTried_) return;
  countsTried_ = true;
  const uint32_t bytes = geometry_.indexSlots / 4;
  useCounts_ = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!useCounts_) {
    LOG_ERR("GST", "no memory for the use counts (%u B); room is made without them", static_cast<unsigned>(bytes));
    return;
  }
  LOG_INF("GST", "use counts kept (%u B): %u of %u segments live", static_cast<unsigned>(bytes),
          static_cast<unsigned>(live_), static_cast<unsigned>(segmentCount()));
}

void FlashGlyphStore::noteDrawn(const EpdGlyph* glyph) const {
  if (!useCounts_ || !glyph) return;
  // A record of the log: its EpdGlyph lies past a segment header and a key, and its key sits right before it.
  const uintptr_t at = reinterpret_cast<uintptr_t>(glyph);
  const uintptr_t base = reinterpret_cast<uintptr_t>(log_);
  if (at < base + kSegmentHeaderBytes + 4 || at - base >= geometry_.logSize || (at - base) % 4 != 0) return;
  const uint32_t offset = static_cast<uint32_t>(at - base) - 4;
  const uint32_t key = flashU32(log_ + offset);
  if (!isGlyphKey(key)) return;
  const uint32_t dataOffset = glyph->dataOffset;
  const uint32_t segment = dataOffset / geometry_.segmentBytes;
  if (segment >= segmentCount() || dataOffset != offset + kRecordHeaderBytes) return;
  const uint32_t value = glyphSlotValue(segEpoch_[segment], dataOffset);
  if (validRecord(key, value) != glyph) return;
  const uint32_t slot = findSlot(key, value);
  if (slot == UINT32_MAX) return;
  ++drawsCounted_;
  uint8_t& byte = useCounts_[slot / 4];
  const uint32_t shift = (slot % 4) * 2;
  const uint32_t count = (byte >> shift) & 3u;
  if (count < 3) byte = static_cast<uint8_t>(byte + (1u << shift));
}

uint8_t FlashGlyphStore::useCount(const uint32_t key) const {
  if (!useCounts_ || !open_ || !isGlyphKey(key)) return 0;
  const uint32_t slots = geometry_.indexSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    const Slot slot = flashSlot(index_, i);
    if (slotErased(slot)) return 0;
    if (slot.key != key) continue;
    if (slot.value == kNoGlyph) return 0;
    if (validRecord(key, slot.value)) return countAt(i);
  }
  return 0;
}

// --- Faces ---

bool FlashGlyphStore::readFaceEntry(const uint8_t i, FaceEntry& entry) const {
  return flash_.read(kFaceTableOffset + i * static_cast<uint32_t>(sizeof(FaceEntry)), &entry, sizeof entry);
}

bool FlashGlyphStore::faceIndex(const uint8_t family, const uint32_t ppem26_6, uint8_t* index) {
  if (!open_ || family >= kMaxStyles || ppem26_6 > 0xFFFFFFu || index == nullptr) return false;
  const uint32_t face = faceOf(family, ppem26_6);
  int free = -1;
  uint8_t freeCount = 0;
  for (uint8_t i = 0; i < kFaceEntries; ++i) {
    FaceEntry entry{};
    if (!readFaceEntry(i, entry)) return false;
    if (entry.face == face && entry.claimed == 0) {
      *index = i;
      return true;
    }
    // Free only if wholly erased: an entry cut short is never programmed again.
    if (entry.face == kErased && entry.claimed == kErased) {
      if (free < 0) free = i;
      ++freeCount;
    }
  }
  if (wipePending_) return false;
  if (free < 0) {
    // Only a new face is refused: the claimed ones go on storing, and the wipe at the next book open frees the
    // table.
    LOG_INF("GST", "full: all %u faces in use; wipe pending", static_cast<unsigned>(kFaceEntries));
    wipePending_ = true;
    recordWipePending();
    return false;
  }
  const FaceEntry claim{face, 0};  // `claimed` lands last and marks the entry in use
  if (!flash_.program(kFaceTableOffset + static_cast<uint32_t>(free) * sizeof(FaceEntry), &claim, sizeof claim)) {
    LOG_ERR("GST", "face table write failed; wipe pending");
    stopWrites();
    return false;
  }
  claimedFaces_ |= 1u << free;
  *index = static_cast<uint8_t>(free);
  if (freeCount == 1) {
    // That was the last entry: the next book open empties the table, before a new body size needs one.
    LOG_INF("GST", "the last of %u faces taken; wipe pending", static_cast<unsigned>(kFaceEntries));
    wipePending_ = true;
    recordWipePending();
  }
  return true;
}

// --- Kerning pairs ---

bool FlashGlyphStore::findPair(const uint16_t left, const uint16_t right, const uint8_t family, const uint8_t style,
                               int16_t* fontUnits) const {
  const uint32_t key = pairKey(left, right);
  if (!open_ || key == kErased) return false;
  const uint32_t owner = pairStyle(family, style);
  const uint32_t slots = geometry_.pairSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    const Slot slot = flashSlot(pairs_, i);
    if (slotErased(slot)) return false;
    // The value's top bits are written as zero, so a value cut short reads as some other family or style.
    if (slot.key != key || (slot.value >> 16) != owner) continue;
    if (fontUnits) *fontUnits = static_cast<int16_t>(slot.value & 0xFFFF);
    return true;
  }
  return false;
}

bool FlashGlyphStore::storePair(const uint16_t left, const uint16_t right, const uint8_t family, const uint8_t style,
                                const int16_t fontUnits) {
  const uint32_t key = pairKey(left, right);
  if (!open_ || writesStopped_ || key == kErased || family >= kMaxStyles || style > 3) return false;
  // A full pair index only stops storing pairs; they come from the GPOS reader instead (never a wipe).
  if (usedPairs_ + 1 > limit(geometry_.pairSlots, kIndexLoadPercent)) return false;
  const uint32_t value = (pairStyle(family, style) << 16) | static_cast<uint16_t>(fontUnits);
  switch (insert(geometry_.pairOffset, pairs_, geometry_.pairSlots, key, value)) {
    case Insert::Done:
      ++usedPairs_;
      return true;
    case Insert::ChainFull:
      return false;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "pair write failed; wipe pending");
  stopWrites();
  return false;
}

// --- Making room ---

FlashGlyphStore::Room FlashGlyphStore::makeRoom(const Budget budget, RoomWork* work) {
  RoomWork local;
  RoomWork& done = work ? *work : local;
  done = RoomWork{};
  // With a wipe pending, a write failed (what the flash holds is not known for sure) or the face table is full: only
  // the wipe goes on from there.
  if (!open_ || wipePending_) return Room::Failed;
  const uint32_t steps = budget == Budget::Idle ? 1 : segmentCount() + 1;
  bool rebuildTried = false;
  for (uint32_t step = 0; step < steps; ++step) {
    const Step next = nextStep(budget, rebuildTried);
    if (next == Step::None) break;
    if (next == Step::Reclaim) {
      if (!reclaimTail(done)) return Room::Failed;
      roomAsked_ = false;
      continue;
    }
    rebuildTried = true;
    const Rebuilt rebuilt = rebuildIndex(done);
    if (rebuilt == Rebuilt::Failed) return Room::Failed;
    if (rebuilt == Rebuilt::Wiped) break;
  }
  // Writes stopped for room resume once a segment is free (one was reclaimed, or at most one is live); a batch that
  // does not fit stops them again. A full index, or the epochs spent, keep them stopped until the rebuild.
  if (live_ < 2) roomAsked_ = false;
  writesStopped_ = roomAsked_ || indexFull_ || chainFull_ || epochsLeft() == 0;
  return needsRoom(budget) ? Room::More : Room::Enough;
}

// What the next step of makeRoom() is. A rebuild comes at most once a call, done or not: done, it clears every reason
// for one. One that found no memory is tried again only by an Open.
FlashGlyphStore::Step FlashGlyphStore::nextStep(const Budget budget, const bool rebuildTried) const {
  const bool mayRebuild = !rebuildTried && (budget == Budget::Open || !rebuildStarved_);
  // The index stops writes: it is rebuilt now, in idle time too. A full index with few slots to free would fill again
  // soon after, so while two or more segments are live a reclaim comes first and leaves its slots stale. The "no
  // glyph" marks count as freeable here: when they are what fills the index, the rebuild that forgets them is the
  // only way on. A full probe chain, or the epochs spent, are a rebuild's alone to clear.
  if (mayRebuild && (indexFull_ || chainFull_ || epochsLeft() == 0)) {
    return (chainFull_ || epochsLeft() == 0 || crowds(usedSlots_ - liveSlots_) || live_ < 2) ? Step::Rebuild
                                                                                             : Step::Reclaim;
  }
  if (live_ >= 2 && (roomAsked_ || freeSegments() < kFreeSegmentsTarget)) return Step::Reclaim;
  // The other reasons wait for a chapter or book open, and come after its reclaims: the slots those leave stale go
  // with the rebuild.
  if (mayRebuild && budget == Budget::Open && rebuildWaitsForOpen()) return Step::Rebuild;
  return Step::None;
}

// `slots` index slots are kRebuildStalePercent of the index or more: enough for a rebuild that frees them to pay.
bool FlashGlyphStore::crowds(const uint32_t slots) const {
  return uint64_t{slots} * 100 >= uint64_t{geometry_.indexSlots} * kRebuildStalePercent;
}

// A rebuild that waits for a chapter or book open: a power cut stopped the last one, kRebuildEpochSpan epochs have
// passed since the last that finished, or stale slots crowd the index. The "no glyph" marks are not stale: a family
// that lacks a whole script would have each open rebuild the index only to learn them again.
bool FlashGlyphStore::rebuildWaitsForOpen() const {
  return !journal_.done || nextEpoch_ - journal_.floor >= kRebuildEpochSpan || crowds(usedSlots_ - liveSlots_ - marks_);
}

// Epochs the head may still give out before the index must be rebuilt. Every slot names an epoch from the last
// finished rebuild's base on, so keeping the newest under kRebuildEpochLimit past it keeps them all distinct in a
// slot's 12 bits, whether or not an Open comes to run the rebuild kRebuildEpochSpan asks for.
uint32_t FlashGlyphStore::epochsLeft() const {
  const uint32_t span = nextEpoch_ - journal_.floor;
  return span >= kRebuildEpochLimit ? 0 : kRebuildEpochLimit - span;
}

// Reclaims the tail segment T. A record of T is a candidate when its slot's use count is above 0. At most half T's
// payload of candidates is kept, the most used first and the newest among equals, and only when all of it fits the
// head and the segments it may open (free, with an epoch left). Those are copied to the head, oldest first; then T is
// retired and erased, and its slots go stale. False on a failed write (wipe pending).
bool FlashGlyphStore::reclaimTail(RoomWork& work) {
  const uint32_t tail = tail_;
  const uint32_t epoch = segEpoch_[tail];
  struct Seen {
    uint32_t key;
    uint32_t bytes;
    uint32_t slot;  // UINT32_MAX: no slot names the record, so it was never findable
    uint8_t count;
  };
  const auto seen = [this, epoch](const uint32_t offset) {
    const EpdGlyph& glyph = recordGlyph(offset);
    Seen s{flashU32(log_ + offset), recordBytes(glyph.dataLength), 0, 0};
    s.slot = findSlot(s.key, glyphSlotValue(epoch, glyph.dataOffset));
    s.count = s.slot == UINT32_MAX ? 0 : countAt(s.slot);
    return s;
  };

  // The candidates' bytes at each count. Half the payload goes to the highest counts first: `level` is the lowest
  // count kept, and of the records at it the oldest are dropped until `over` bytes are.
  uint32_t levelBytes[4] = {};
  if (useCounts_) {
    scanLog(tail, [&](const uint32_t offset) {
      const Seen s = seen(offset);
      levelBytes[s.count] += s.bytes;
    });
  }
  uint32_t room = (geometry_.segmentBytes - kSegmentHeaderBytes) / 2;
  uint8_t level = 1;
  uint32_t over = 0;
  for (uint8_t count = 3; count >= 1; --count) {
    level = count;
    if (levelBytes[count] > room) {
      over = levelBytes[count] - room;
      break;
    }
    room -= levelBytes[count];
  }

  // Where the dropped ones at `level` end, and whether what is kept fits the head and the segments it may open. A ring
  // that filled completely has no room for copies, so its first reclaim drops the segment whole.
  uint32_t cutoff = 0;  // the records at `level` before this offset are dropped
  uint32_t used = headUsed_;
  uint32_t opens = 0;
  if (levelBytes[1] + levelBytes[2] + levelBytes[3] != 0) {
    scanLog(tail, [&](const uint32_t offset) {
      const Seen s = seen(offset);
      if (s.count < level) return;
      if (s.count == level && over > 0) {
        over = over > s.bytes ? over - s.bytes : 0;
        cutoff = offset + s.bytes;
        return;
      }
      placeRecord(s.bytes, used, opens);
    });
  }
  const bool keep = opens <= segmentsToOpen();

  // The copies, each record before its slot; and the records a slot names, whose slots go stale with T.
  const uint16_t keptBefore = work.kept;
  uint32_t named = 0;
  bool failed = false;
  scanLog(tail, [&](const uint32_t offset) {
    if (failed) return;
    const Seen s = seen(offset);
    if (s.slot == UINT32_MAX) return;
    ++named;
    if (keep && s.count >= level && (s.count > level || offset >= cutoff)) {
      failed = !copyRecord(offset, s.key, s.bytes, s.count, work);
    }
  });
  if (failed) return false;

  // T leaves the ring before its erase: its `retired` word goes first, so a cut in the erase never leaves it reading
  // as live, and a failed erase leaves nothing of it served.
  const uint32_t at = geometry_.logOffset + tail * geometry_.segmentBytes;
  const uint32_t retired = 0;
  if (!flash_.program(at + offsetof(SegmentHeader, retired), &retired, sizeof retired)) {
    LOG_ERR("GST", "retiring segment %u failed; wipe pending", static_cast<unsigned>(tail));
    stopWrites();
    return false;
  }
  segEpoch_[tail] = kErased;
  tail_ = static_cast<uint8_t>(tail + 1u == segmentCount() ? 0 : tail + 1u);
  --live_;
  const uint32_t kept = static_cast<uint16_t>(work.kept - keptBefore);
  liveSlots_ -= named;
  if (!eraseWritten(at, geometry_.segmentBytes)) {
    LOG_ERR("GST", "erasing segment %u failed; wipe pending", static_cast<unsigned>(tail));
    stopWrites();
    return false;
  }
  // Counted once the segment is gone, as it is reclaimed.
  work.dropped = static_cast<uint16_t>(work.dropped + named - kept);
  ++work.reclaimed;
  LOG_INF("GST", "segment %u reclaimed: %u glyphs kept, %u dropped; %u of %u segments free",
          static_cast<unsigned>(tail), static_cast<unsigned>(kept), static_cast<unsigned>(named - kept),
          static_cast<unsigned>(freeSegments()), static_cast<unsigned>(segmentCount()));
  return true;
}

// Copies the record at `from` to the head (moving the head on when it does not fit) with its dataOffset rewritten for
// its new place, then gives it a slot whose count is `count` halved. False on a failed write (wipe pending). A full
// probe chain leaves the copy unfindable, and the index waiting for its rebuild.
bool FlashGlyphStore::copyRecord(const uint32_t from, const uint32_t key, const uint32_t bytes, const uint8_t count,
                                 RoomWork& work) {
  if (!fitsSegment(headUsed_, bytes) && !moveHead()) {
    LOG_ERR("GST", "opening a log segment failed; wipe pending");
    stopWrites();
    return false;
  }
  const uint32_t to = head_ * geometry_.segmentBytes + headUsed_;
  alignas(4) uint8_t piece[kCopyPieceBytes];
  for (uint32_t done = 0; done < bytes;) {
    const uint32_t n = bytes - done < kCopyPieceBytes ? bytes - done : kCopyPieceBytes;
    std::memcpy(piece, log_ + from + done, n);
    if (done == 0) {
      EpdGlyph glyph = ramGlyph(piece + 4);
      glyph.dataOffset = to + kRecordHeaderBytes;
      std::memcpy(piece + 4, &glyph, sizeof glyph);
    }
    if (!flash_.program(geometry_.logOffset + to + done, piece, n)) {
      LOG_ERR("GST", "copying a record to %u failed; wipe pending", static_cast<unsigned>(to));
      stopWrites();
      return false;
    }
    done += n;
  }
  headUsed_ += bytes;
  uint32_t slot = 0;
  const Commit inserted = insertGlyphSlot(key, glyphSlotValue(segEpoch_[head_], to + kRecordHeaderBytes), &slot);
  if (inserted == Commit::Stored) {
    ++work.kept;
    setCount(slot, count / 2);
  }
  return inserted != Commit::WriteFailed;
}

// --- The index rebuild ---

// Rebuilds the index from the log, journaled in the state sector: the entry's `base` before the index is touched, its
// `done` after the last slot, so a cut in between leaves the rebuild to be done again at the next open.
// 1. The plan, in walk order (from the tail, each segment's records in log order): whether a slot names each record,
//    and that slot's count. Only those records get a slot again: one with none had its write cut before its slot, and
//    may be torn. Without memory for the counts they start at 0; without memory for the rest the rebuild waits for an
//    Open.
// 2. The index is erased, and each planned record is inserted again with its segment's epoch, unless find() finds its
//    key already (a duplicate a cut left, the copy of a reclaim cut short: the first stays). Its count follows.
// A cut after the erase loses the slots not yet inserted: their glyphs are drawn and stored again. With the journal
// full the store is wiped instead.
FlashGlyphStore::Rebuilt FlashGlyphStore::rebuildIndex(RoomWork& work) {
  if (journal_.used >= kJournalEntries) {
    LOG_INF("GST", "the rebuild journal is full (%u entries): the store is wiped instead",
            static_cast<unsigned>(kJournalEntries));
    return wipe() ? Rebuilt::Wiped : Rebuilt::Failed;
  }
  uint32_t records = 0;
  walkLive([&records](uint32_t, uint32_t) { ++records; });
  const uint32_t namedBytes = records / 8 + 1;
  const uint32_t countBytes = useCounts_ ? records / 4 + 1 : 0;
  auto plan = makeUniqueNoThrow<uint8_t[]>(namedBytes + countBytes);
  bool counted = countBytes != 0;
  if (!plan && counted) {
    plan = makeUniqueNoThrow<uint8_t[]>(namedBytes);
    counted = false;
    if (plan) {
      LOG_ERR("GST", "rebuild: no memory for the counts (%u B); they start at 0", static_cast<unsigned>(countBytes));
    }
  }
  if (!plan) {
    // Logged once: idle time does not try again (each try walks the log), an Open does.
    if (!rebuildStarved_) {
      LOG_ERR("GST", "rebuild: no memory for its plan (%u B); it waits for a chapter or book open",
              static_cast<unsigned>(namedBytes + countBytes));
    }
    rebuildStarved_ = true;
    return Rebuilt::NoMemory;
  }
  rebuildStarved_ = false;
  uint8_t* named = plan.get();
  uint8_t* counts = counted ? plan.get() + namedBytes : nullptr;
  uint32_t i = 0;
  walkLive([&](const uint32_t segment, const uint32_t offset) {
    const uint32_t slot =
        findSlot(flashU32(log_ + offset), glyphSlotValue(segEpoch_[segment], recordGlyph(offset).dataOffset));
    if (slot != UINT32_MAX) {
      named[i / 8] = static_cast<uint8_t>(named[i / 8] | 1u << (i % 8));
      if (counts) counts[i / 4] = static_cast<uint8_t>(counts[i / 4] | countAt(slot) << (i % 4 * 2));
    }
    ++i;
  });

  // The oldest epoch any slot from here on can name: the tail's, or with none live the next one.
  const uint32_t base = live_ == 0 ? nextEpoch_ : segEpoch_[tail_];
  const uint32_t entry = kJournalOffset + journal_.used * static_cast<uint32_t>(sizeof(JournalEntry));
  if (!flash_.program(entry + offsetof(JournalEntry, base), &base, sizeof base)) {
    LOG_ERR("GST", "rebuild: journal write failed; wipe pending");
    stopWrites();
    return Rebuilt::Failed;
  }
  journal_ = Journal{journal_.used + 1, base, false, journal_.floor};
  if (!eraseWritten(geometry_.indexOffset, tableBytes(geometry_.indexSlots))) {
    LOG_ERR("GST", "rebuild: erasing the index failed; wipe pending");
    stopWrites();
    return Rebuilt::Failed;
  }
  if (useCounts_) std::memset(useCounts_.get(), 0, geometry_.indexSlots / 4);
  uint32_t inserted = 0;
  Insert result = Insert::Done;
  i = 0;
  walkLive([&](const uint32_t segment, const uint32_t offset) {
    const uint32_t at = i++;
    if (result != Insert::Done || ((named[at / 8] >> (at % 8)) & 1u) == 0) return;
    const uint32_t key = flashU32(log_ + offset);
    if (find(key, nullptr) == Lookup::Found) return;
    uint32_t slot = 0;
    result = insert(geometry_.indexOffset, index_, geometry_.indexSlots, key,
                    glyphSlotValue(segEpoch_[segment], recordGlyph(offset).dataOffset), &slot);
    if (result != Insert::Done) return;
    ++inserted;
    if (counts) setCount(slot, (counts[at / 4] >> (at % 4 * 2)) & 3u);
  });
  if (result != Insert::Done) {
    LOG_ERR("GST", "rebuild: %s; wipe pending",
            result == Insert::ChainFull ? "a probe chain filled" : "a slot write failed");
    stopWrites();
    return Rebuilt::Failed;
  }
  const uint32_t finished = 0;
  if (!flash_.program(entry + offsetof(JournalEntry, done), &finished, sizeof finished)) {
    LOG_ERR("GST", "rebuild: journal write failed; wipe pending");
    stopWrites();
    return Rebuilt::Failed;
  }
  journal_.done = true;
  journal_.floor = base;
  usedSlots_ = liveSlots_ = inserted;
  marks_ = 0;
  indexFull_ = chainFull_ = false;
  ++rebuilds_;
  work.rebuilt = true;
  LOG_INF("GST", "index rebuilt from epoch %u: %u of %u records have slots, of %u slots; journal %u of %u",
          static_cast<unsigned>(base), static_cast<unsigned>(inserted), static_cast<unsigned>(records),
          static_cast<unsigned>(geometry_.indexSlots), static_cast<unsigned>(journal_.used),
          static_cast<unsigned>(kJournalEntries));
  return Rebuilt::Done;
}

// Entries are used in order, so the first wholly erased one ends the journal. An unreadable journal counts as full: a
// rebuild then wipes rather than program entries it could not read.
void FlashGlyphStore::readJournal() {
  journal_ = Journal{};
  constexpr uint32_t kChunk = 32;
  JournalEntry chunk[kChunk];
  for (uint32_t first = 0; first < kJournalEntries; first += kChunk) {
    const uint32_t n = kJournalEntries - first < kChunk ? kJournalEntries - first : kChunk;
    if (!flash_.read(kJournalOffset + first * static_cast<uint32_t>(sizeof(JournalEntry)), chunk,
                     n * static_cast<uint32_t>(sizeof(JournalEntry)))) {
      journal_.used = kJournalEntries;
      return;
    }
    for (uint32_t i = 0; i < n; ++i) {
      if (chunk[i].base == kErased && chunk[i].done == kErased) return;
      const bool done = chunk[i].done != kErased;
      journal_ = Journal{first + i + 1, chunk[i].base, done, done ? chunk[i].base : journal_.floor};
    }
  }
}

bool FlashGlyphStore::markFull() {
  if (!open_) return false;
  stopForRoom();
  keepCounts();
  return true;
}

// --- Wipe pending, and the wipe ---

// The ring is full, or markFull(): nothing is written until room is made, and the next makeRoom() reclaims. Nothing is
// lost either, so no wipe.
void FlashGlyphStore::stopForRoom() {
  writesStopped_ = true;
  roomAsked_ = true;
}

// A write failed: what the flash holds is not known for sure, so only a wipe makes the store writable again.
void FlashGlyphStore::stopWrites() {
  writesStopped_ = true;
  if (wipePending_) return;
  wipePending_ = true;  // holds for the session even if the flash write below fails
  recordWipePending();
}

// Programs the wipe word, unless it already reads programmed, and reads it back.
bool FlashGlyphStore::recordWipePending() {
  uint32_t word = kErased;
  if (flash_.read(kWipeWordOffset, &word, sizeof word) && word != kErased) return true;
  const uint32_t zero = 0;
  if (!flash_.program(kWipeWordOffset, &zero, sizeof zero) || !flash_.read(kWipeWordOffset, &word, sizeof word) ||
      word == kErased) {
    LOG_ERR("GST", "could not record wipe pending in flash");
    return false;
  }
  return true;
}

bool FlashGlyphStore::wipe() {
  if (!open_) return false;
  // Wipe pending reaches flash before anything is erased. Then a cut anywhere below leaves it set and the next
  // wipe() finishes the job; without it a half-erased log would read as an empty store and be written over.
  wipePending_ = true;
  writesStopped_ = true;
  if (!recordWipePending()) return false;
  const Geometry& g = geometry_;
  // Everything a lookup can reach goes first; the state sector, which holds the wipe word, goes last.
  if (!eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) || !eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) ||
      !eraseWritten(g.logOffset, g.logSize) || !flash_.erase(kStateSectorOffset, kSectorBytes)) {
    LOG_ERR("GST", "wipe: erase failed; wipe stays pending");
    return false;
  }
  resetRing();
  journal_ = Journal{};
  rebuildStarved_ = false;
  usedSlots_ = marks_ = usedPairs_ = claimedFaces_ = 0;
  dropCounts();
  ++generation_;
  wipePending_ = false;
  writesStopped_ = false;
  return true;
}
