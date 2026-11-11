#include "FlashLangPartition.h"

#include <Logging.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "FlashArea.h"
#include "FlashFontPartition.h"

namespace FlashLangPartition {

static constexpr char MAGIC[4] = {'C', 'P', 'L', 'P'};
static constexpr size_t WRITE_CHUNK = 512;
// The area erases in whole sectors and maps in whole MMU pages.
static constexpr size_t SECTOR_BYTES = NorFlash::kSectorBytes;
static constexpr size_t MAP_PAGE_BYTES = NorFlash::kMapPageBytes;

static const uint8_t* s_mmapPtr = nullptr;
static size_t s_mapBytes = 0;  // how much of the slot the live mapping covers
static Failure s_lastFailure = Failure::None;

static constexpr size_t roundUp(const size_t v, const size_t to) { return (v + to - 1) / to * to; }

// CRC-32 as zlib computes it, chained from 0. ROM code: costs no flash.
static uint32_t crc32(const uint32_t crc, const void* data, const size_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t*>(data), static_cast<uint32_t>(len));
}

// On-flash header, read and written as a unit. Packed layout is fixed by the
// comment in the header file; the struct mirrors it so the offsets stay in one
// place rather than being recomputed at each access.
struct Header {
  char magic[4];
  uint8_t version;
  uint8_t langIndex;
  uint16_t keyCount;
  uint32_t stamp;
  uint32_t tableOff;
  uint32_t dataOff;
  uint32_t dataLen;
  uint32_t extOff;  // 0 with extLen 0: no extension
  uint32_t extLen;
  uint32_t stringsCrc;  // the data, then the table
  uint32_t extCrc;
};
static_assert(sizeof(Header) == HEADER_BYTES, "language slot header layout changed");

static NorFlash* findArea() {
  NorFlash* const area = FlashArea::get();
  if (!area) LOG_ERR("FLP", "no flash area");
  return area;
}

// Byte offset of the language slot within the area. Equal to
// FlashFontPartition's ceiling, and 64 KB aligned because the area both
// starts on and is a multiple of 64 KB — esp_partition_mmap requires that.
// Format 1 (2.40) kept a 64 KB slot at the partition's end, behind a 32 B header: this slot's upper half.
static constexpr size_t FORMAT1_SLOT_BYTES = 64 * 1024;
static constexpr size_t FORMAT1_HEADER_BYTES = 32;

static size_t slotOffset(const NorFlash* area) {
  if (!area || area->size() <= FlashFontPartition::LANG_RESERVED_BYTES) return 0;
  return static_cast<size_t>(area->size()) - FlashFontPartition::LANG_RESERVED_BYTES;
}

static bool readHeader(const NorFlash* area, Header& out) {
  const size_t off = slotOffset(area);
  if (off == 0) return false;
  if (!area->read(static_cast<uint32_t>(off), &out, sizeof(Header))) return false;
  if (memcmp(out.magic, MAGIC, 4) != 0) return false;
  if (out.version != FORMAT_VERSION) return false;
  return true;
}

// The table and the strings where build() puts them, inside the slot. A header that says otherwise is
// not one this firmware wrote.
static bool layoutValid(const Header& h) {
  return h.tableOff == HEADER_BYTES && h.dataOff >= h.tableOff + static_cast<uint32_t>(h.keyCount) * 2u &&
         static_cast<size_t>(h.dataOff) + h.dataLen <= FlashFontPartition::LANG_RESERVED_BYTES;
}

// The extension a header describes, if it is one this firmware can trust: after the strings and
// inside the slot. Anything else reads as none.
static bool extensionValid(const Header& h) {
  return h.extLen != 0 && h.extOff >= h.dataOff + h.dataLen && h.extOff % 4 == 0 &&
         static_cast<size_t>(h.extOff) + h.extLen <= FlashFontPartition::LANG_RESERVED_BYTES;
}

bool holds(const uint8_t langIndex, const uint32_t stamp, const uint32_t extBytes) {
  const NorFlash* const area = findArea();
  if (!area) return false;
  Header h;
  if (!readHeader(area, h)) return false;
  const uint32_t ext = extensionValid(h) ? h.extLen : 0;
  return h.langIndex == langIndex && h.stamp == stamp && ext == extBytes;
}

// Buffers a producer's bytes and programs them in WRITE_CHUNK pieces from `base` on. For the strings it
// also keeps the table: beginKey() points the key's entry at the next byte written.
class ChunkedSlotWriter final : public StringsWriter {
 public:
  ChunkedSlotWriter(NorFlash* area, const size_t base, const size_t limit, uint8_t* chunk, uint8_t* scratch,
                    const size_t scratchBytes, uint16_t* table = nullptr, const uint16_t keyCount = 0)
      : area_(area),
        base_(base),
        limit_(limit),
        chunk_(chunk),
        scratch_(scratch),
        scratchBytes_(scratchBytes),
        table_(table),
        keyCount_(keyCount) {}

  bool beginKey(const uint16_t id) override {
    const size_t at = written_ + fill_;
    // Bit 15 is FALLBACK, so a string must start below 32 KB. gen_i18n.py refuses a pack with that much.
    if (failed_ || !table_ || id >= keyCount_ || at >= FALLBACK) {
      LOG_ERR("FLP", "key %u refused at %u B", id, static_cast<unsigned>(at));
      failed_ = true;
      return false;
    }
    table_[id] = static_cast<uint16_t>(at);
    return true;
  }

  bool write(const void* data, size_t len) override {
    if (failed_ || written_ + fill_ + len > limit_) {
      failed_ = true;
      return false;
    }
    crc_ = crc32(crc_, data, len);
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (len > 0) {
      const size_t take = std::min(len, WRITE_CHUNK - fill_);
      memcpy(chunk_ + fill_, bytes, take);
      fill_ += take;
      bytes += take;
      len -= take;
      if (fill_ == WRITE_CHUNK && !flush()) return false;
    }
    return true;
  }
  uint8_t* scratch() override { return scratch_; }
  size_t scratchBytes() const override { return scratchBytes_; }

  // Programs what is still buffered. The total written, once this succeeds.
  bool finish() { return flush(); }
  size_t written() const { return written_; }
  // CRC32 of every byte handed to write(): what map() expects to read back.
  uint32_t crc() const { return crc_; }

 private:
  bool flush() {
    if (failed_) return false;
    if (fill_ == 0) return true;
    if (!area_->program(static_cast<uint32_t>(base_ + written_), chunk_, static_cast<uint32_t>(fill_))) {
      LOG_ERR("FLP", "extension write failed at %u B", static_cast<unsigned>(written_));
      failed_ = true;
      return false;
    }
    written_ += fill_;
    fill_ = 0;
    return true;
  }

  NorFlash* area_;
  size_t base_;
  size_t limit_;
  uint8_t* chunk_;
  uint8_t* scratch_;
  size_t scratchBytes_;
  uint16_t* table_;
  uint16_t keyCount_;
  size_t fill_ = 0;
  size_t written_ = 0;
  uint32_t crc_ = 0;
  bool failed_ = false;
};

bool build(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount, const Strings& strings,
           const Extension* ext) {
  if (keyCount == 0 || !strings.produce || strings.maxBytes == 0) return false;

  NorFlash* const area = findArea();
  if (!area) return false;
  const size_t slot = slotOffset(area);
  if (slot == 0) {
    LOG_ERR("FLP", "partition too small for a language slot");
    return false;
  }

  // A live mapping would go stale under the erase below.
  unmap();

  // 4-byte align the table length so the data that follows starts aligned too,
  // matching FlashFontPartition's own convention for flash writes. An odd key
  // count would otherwise leave every data write 2-byte aligned.
  const size_t tableOff = HEADER_BYTES;
  const size_t tableBytes = (static_cast<size_t>(keyCount) * sizeof(uint16_t) + 3u) & ~static_cast<size_t>(3u);
  const size_t tableWords = tableBytes / sizeof(uint16_t);
  const size_t dataOff = tableOff + tableBytes;
  const size_t maxBytes = strings.maxBytes;
  if (dataOff + maxBytes > FlashFontPartition::LANG_RESERVED_BYTES) {
    LOG_ERR("FLP", "language %u needs %u B, slot is %u B", langIndex, static_cast<unsigned>(dataOff + maxBytes),
            static_cast<unsigned>(FlashFontPartition::LANG_RESERVED_BYTES));
    return false;
  }

  // The extension goes after the strings, 4-byte aligned for the tables in it, so its room is judged
  // by the strings' bound before they are written. One that does not fit is dropped here rather than
  // failing the strings.
  const size_t extBound = roundUp(dataOff + maxBytes, 4);
  if (ext && ext->bytes > 0 && extBound + ext->bytes > FlashFontPartition::LANG_RESERVED_BYTES) {
    LOG_ERR("FLP", "language %u extension needs %u B past %u B, slot is %u B - building the strings alone", langIndex,
            static_cast<unsigned>(ext->bytes), static_cast<unsigned>(extBound),
            static_cast<unsigned>(FlashFontPartition::LANG_RESERVED_BYTES));
    ext = nullptr;
  }
  if (ext && ext->bytes == 0) ext = nullptr;

  // Only the sectors this build may write: the slot is twice what a pack without an extension needs.
  // Bytes past them may hold an earlier, longer build; the header bounds every read, so they are
  // never looked at.
  const size_t used = ext ? extBound + ext->bytes : dataOff + maxBytes;
  if (!area->erase(static_cast<uint32_t>(slot), static_cast<uint32_t>(roundUp(used, SECTOR_BYTES)))) {
    LOG_ERR("FLP", "erase failed");
    return false;
  }

  // All three buffers are transient — freed before this function returns.
  auto scratch = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[SCRATCH_BYTES]);
  auto chunk = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[WRITE_CHUNK]);
  auto table = std::unique_ptr<uint16_t[]>(new (std::nothrow) uint16_t[tableWords]);
  if (!scratch || !chunk || !table) {
    LOG_ERR("FLP", "out of memory building language %u", langIndex);
    return false;
  }
  // Every key English until the producer writes its string; the alignment pad past keyCount too, so it
  // is written as a known value rather than whatever the heap held.
  for (size_t i = 0; i < tableWords; ++i) table[i] = FALLBACK;

  // The strings. The writer's CRC is over the bytes as they are written, from RAM: a byte that did not
  // program, or that something else overwrites later, then fails map()'s check of what flash holds.
  ChunkedSlotWriter strWriter(area, slot + dataOff, std::min<size_t>(maxBytes, FALLBACK), chunk.get(), scratch.get(),
                              SCRATCH_BYTES, table.get(), keyCount);
  if (!strings.produce(strings.ctx, strWriter) || !strWriter.finish()) {
    LOG_ERR("FLP", "language %u strings failed after %u B", langIndex, static_cast<unsigned>(strWriter.written()));
    return false;
  }
  const size_t written = strWriter.written();

  if (!area->program(static_cast<uint32_t>(slot + tableOff), table.get(), static_cast<uint32_t>(tableBytes))) {
    LOG_ERR("FLP", "table write failed");
    return false;
  }
  const uint32_t stringsCrc = crc32(strWriter.crc(), table.get(), tableBytes);

  // The extension, through the staging chunk; the strings' scratch is free now and is its own.
  const size_t extOff = roundUp(dataOff + written, 4);
  uint32_t extLen = 0;
  uint32_t extCrc = 0;
  if (ext) {
    ChunkedSlotWriter writer(area, slot + extOff, ext->bytes, chunk.get(), scratch.get(), SCRATCH_BYTES);
    if (ext->produce(ext->ctx, writer) && writer.finish() && writer.written() == ext->bytes) {
      extLen = ext->bytes;
      extCrc = writer.crc();
    } else {
      LOG_ERR("FLP", "language %u extension failed after %u of %u B - the strings stand alone", langIndex,
              static_cast<unsigned>(writer.written()), static_cast<unsigned>(ext->bytes));
    }
  }

  // Header last: until this lands the slot reads as absent.
  Header h = {};
  memcpy(h.magic, MAGIC, 4);
  h.version = FORMAT_VERSION;
  h.langIndex = langIndex;
  h.keyCount = keyCount;
  h.stamp = stamp;
  h.tableOff = static_cast<uint32_t>(tableOff);
  h.dataOff = static_cast<uint32_t>(dataOff);
  h.dataLen = static_cast<uint32_t>(written);
  h.extOff = extLen ? static_cast<uint32_t>(extOff) : 0;
  h.extLen = extLen;
  h.stringsCrc = stringsCrc;
  h.extCrc = extCrc;
  if (!area->program(static_cast<uint32_t>(slot), &h, sizeof(h))) {
    LOG_ERR("FLP", "header write failed");
    return false;
  }

  LOG_INF("FLP", "language %u built: %u B of strings for %u keys, %u B extension", langIndex,
          static_cast<unsigned>(written), keyCount, static_cast<unsigned>(extLen));

  // A format-1 slot (2.40) in the upper half is read once, to convert it (mapLegacy); with this slot
  // built it has served. Retired, so a pack removed from the card later reads as missing rather than
  // as that firmware's strings again. Format 2 lay where this slot does and is gone already, as is a
  // format-1 slot this build's erase reached.
  const size_t format1 = static_cast<size_t>(area->size()) - FORMAT1_SLOT_BYTES;
  uint8_t old[5];
  if (format1 >= slot + roundUp(used, SECTOR_BYTES) && area->read(static_cast<uint32_t>(format1), old, sizeof(old)) &&
      memcmp(old, MAGIC, 4) == 0 && old[4] == 1 && area->erase(static_cast<uint32_t>(format1), SECTOR_BYTES)) {
    LOG_INF("FLP", "an older firmware's format 1 slot retired");
  }
  // An area held in RAM keeps the slot across a power cut from here on (NorFlash::sync); flash has it already.
  area->sync();
  return true;
}

MapResult map(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount, Mapped* out) {
  if (!out) return MapResult::Absent;

  NorFlash* const area = findArea();
  if (!area) return MapResult::Absent;
  const size_t slot = slotOffset(area);
  if (slot == 0) return MapResult::Absent;

  Header h;
  if (!readHeader(area, h)) return MapResult::Absent;
  if (h.langIndex != langIndex || h.stamp != stamp || h.keyCount != keyCount) return MapResult::Absent;
  if (!layoutValid(h)) return MapResult::Absent;

  // Whole pages, and only as many as the contents span: every page mapped here is one the flash
  // font cache cannot map (the C3 shares 128 of them between the app and every mapping), so a pack
  // without an extension still costs exactly the one page it always did.
  const bool hasExt = extensionValid(h);
  bool withExt = hasExt;
  const size_t stringsEnd = h.dataOff + h.dataLen;
  for (;;) {
    const size_t need = roundUp(withExt ? static_cast<size_t>(h.extOff) + h.extLen : stringsEnd, MAP_PAGE_BYTES);
    if (s_mmapPtr && s_mapBytes >= need) break;
    unmap();
    // The slot starts on a 64 KB boundary, which is what esp_partition_mmap requires.
    s_mmapPtr = area->map(static_cast<uint32_t>(slot), static_cast<uint32_t>(need));
    if (s_mmapPtr) {
      s_mapBytes = need;
      break;
    }
    if (!withExt) {
      LOG_ERR("FLP", "mmap failed");
      return MapResult::MapFailed;
    }
    LOG_ERR("FLP", "mmap of %u B with the extension failed - mapping the strings alone", static_cast<unsigned>(need));
    withExt = false;
  }

  // Nothing is handed out before the bytes behind the header are proved to be the ones it was
  // written with. A corrupt table would otherwise send a string read anywhere.
  const uint8_t* base = s_mmapPtr;
  const uint32_t stringsCrc =
      crc32(crc32(0, base + h.dataOff, h.dataLen), base + h.tableOff, static_cast<size_t>(h.dataOff - h.tableOff));
  if (stringsCrc != h.stringsCrc) {
    LOG_ERR("FLP", "language %u strings fail their CRC (%08lx, header %08lx)", langIndex,
            static_cast<unsigned long>(stringsCrc), static_cast<unsigned long>(h.stringsCrc));
    return MapResult::Corrupt;
  }
  out->offsets = reinterpret_cast<const uint16_t*>(base + h.tableOff);
  out->data = reinterpret_cast<const char*>(base + h.dataOff);
  out->ext = nullptr;
  out->extLen = 0;

  MapResult result = MapResult::Ok;
  if (withExt) {
    const uint32_t extCrc = crc32(0, base + h.extOff, h.extLen);
    if (extCrc == h.extCrc) {
      out->ext = base + h.extOff;
      out->extLen = h.extLen;
    } else {
      LOG_ERR("FLP", "language %u extension fails its CRC (%08lx, header %08lx)", langIndex,
              static_cast<unsigned long>(extCrc), static_cast<unsigned long>(h.extCrc));
      result = MapResult::ExtensionCorrupt;
    }
  } else if (hasExt) {
    result = MapResult::WithoutExtension;
  }
  LOG_DBG("FLP", "language %u mapped: table=%p data=%p ext=%p (%u B)", langIndex, out->offsets, out->data, out->ext,
          static_cast<unsigned>(out->extLen));
  return result;
}

// acquire()'s way out with no strings: why, for lastFailure().
static bool fail(const Failure why) {
  s_lastFailure = why;
  return false;
}

bool acquire(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount, const Strings& strings,
             const Extension* ext, Mapped* out) {
  s_lastFailure = Failure::None;
  const NorFlash* const area = findArea();
  if (!area) return fail(Failure::NoArea);
  if (slotOffset(area) == 0) {
    LOG_ERR("FLP", "the flash area (%u KB) has no room for the %u KB language slot",
            static_cast<unsigned>(area->size() / 1024),
            static_cast<unsigned>(FlashFontPartition::LANG_RESERVED_BYTES / 1024));
    return fail(Failure::SlotTooSmall);
  }
  const uint32_t extBytes = ext ? ext->bytes : 0;
  if (holds(langIndex, stamp, extBytes)) {
    const MapResult held = map(langIndex, stamp, keyCount, out);
    if (held == MapResult::Ok || held == MapResult::WithoutExtension) {
      LOG_INF("FLP", "language %u: the slot holds this pack, nothing built", langIndex);
      return true;
    }
    // A rebuild cannot give the strings a mapping the MMU has no page for.
    if (held == MapResult::MapFailed) return fail(Failure::Map);
    LOG_ERR("FLP", "language %u slot holds bytes it was not written with - rebuilding", langIndex);
  }
  // Either a different language, or the same one from a different firmware build — the stamp covers
  // both — or the bytes behind the header changed since. One build, whatever it leads to.
  if (!build(langIndex, stamp, keyCount, strings, ext)) return fail(Failure::Build);
  // An extension that fails even when just written is left out: the strings are what matter, and the
  // extension's reader works without it (CjkUiFonts decodes per glyph instead).
  switch (map(langIndex, stamp, keyCount, out)) {
    case MapResult::Ok:
    case MapResult::WithoutExtension:
    case MapResult::ExtensionCorrupt:
      return true;
    case MapResult::MapFailed:
      return fail(Failure::Map);
    case MapResult::Corrupt:
      return fail(Failure::Corrupt);
    case MapResult::Absent:
      // Just built, yet no header for it: what a program that did not land leaves.
      return fail(Failure::Build);
  }
  return fail(Failure::Build);
}

Failure lastFailure() { return s_lastFailure; }

bool mapLegacy(const uint8_t format, LegacySlot* out) {
  if (!out || (format != 1 && format != 2)) return false;
  NorFlash* const area = findArea();
  if (!area) return false;
  // Format 1 kept a 64 KB slot at the partition's end, format 2 this one.
  const size_t slotBytes = format == 1 ? FORMAT1_SLOT_BYTES : FlashFontPartition::LANG_RESERVED_BYTES;
  if (area->size() <= slotBytes) return false;
  const size_t slot = static_cast<size_t>(area->size()) - slotBytes;

  // Both formats start alike: magic, version, language, key count, stamp, table, data, data length;
  // format 2 goes on with the extension and the CRCs.
  Header h = {};
  const size_t headerBytes = format == 1 ? FORMAT1_HEADER_BYTES : HEADER_BYTES;
  if (!area->read(static_cast<uint32_t>(slot), &h, static_cast<uint32_t>(headerBytes))) return false;
  if (memcmp(h.magic, MAGIC, 4) != 0 || h.version != format || h.keyCount == 0) return false;
  if (h.tableOff != headerBytes || h.dataOff < h.tableOff + static_cast<uint32_t>(h.keyCount) * 2u ||
      static_cast<size_t>(h.dataOff) + h.dataLen > slotBytes || h.dataLen == 0) {
    LOG_ERR("FLP", "format %u slot: a header with its strings outside it", format);
    return false;
  }

  unmap();
  const size_t need = roundUp(static_cast<size_t>(h.dataOff) + h.dataLen, MAP_PAGE_BYTES);
  s_mmapPtr = area->map(static_cast<uint32_t>(slot), static_cast<uint32_t>(need));
  if (!s_mmapPtr) {
    LOG_ERR("FLP", "format %u slot: mmap failed", format);
    return false;
  }
  s_mapBytes = need;

  const bool hasExt = format == 2 && h.extLen != 0;
  if (format == 2) {
    const uint32_t crc =
        crc32(crc32(0, s_mmapPtr + h.dataOff, h.dataLen), s_mmapPtr + h.tableOff, h.dataOff - h.tableOff);
    if (crc != h.stringsCrc) {
      LOG_ERR("FLP", "format 2 slot: its strings fail their CRC");
      unmap();
      return false;
    }
  }
  out->format = format;
  out->langIndex = h.langIndex;
  out->keyCount = h.keyCount;
  out->table = reinterpret_cast<const uint16_t*>(s_mmapPtr + h.tableOff);
  out->data = reinterpret_cast<const char*>(s_mmapPtr + h.dataOff);
  out->dataLen = h.dataLen;
  out->hasExtension = hasExt;
  LOG_INF("FLP", "format %u slot found: language %u, %u keys, %u B of strings%s", format, h.langIndex, h.keyCount,
          static_cast<unsigned>(h.dataLen), hasExt ? ", an extension" : "");
  return true;
}

void unmap() {
  if (!s_mmapPtr) return;
  // A mapping exists, so the area does.
  FlashArea::get()->unmap(s_mmapPtr);
  s_mmapPtr = nullptr;
  s_mapBytes = 0;
}

bool isMapped() { return s_mmapPtr != nullptr; }

}  // namespace FlashLangPartition
