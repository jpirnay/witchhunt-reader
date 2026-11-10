#include "HyphenationStore.h"

#include <Logging.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr char kMagic[4] = {'W', 'H', 'H', 'S'};

uint32_t crc32(const uint32_t crc, const void* data, const uint32_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t*>(data), len);
}

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeLe32(const uint32_t v, uint8_t* p) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void encodeEntry(const HyphenationStore::Entry& e, uint8_t* out) {
  out[0] = static_cast<uint8_t>(e.code[0]);
  out[1] = static_cast<uint8_t>(e.code[1]);
  out[2] = e.minPrefix;
  out[3] = e.minSuffix;
  writeLe32(e.offset, out + 4);
  writeLe32(e.size, out + 8);
  writeLe32(e.rootOffset, out + 12);
  writeLe32(e.crc32, out + 16);
  writeLe32(e.sequence, out + 20);
}

void decodeEntry(const uint8_t* in, HyphenationStore::Entry& e) {
  e.code[0] = static_cast<char>(in[0]);
  e.code[1] = static_cast<char>(in[1]);
  e.code[2] = '\0';
  e.minPrefix = in[2];
  e.minSuffix = in[3];
  e.offset = readLe32(in + 4);
  e.size = readLe32(in + 8);
  e.rootOffset = readLe32(in + 12);
  e.crc32 = readLe32(in + 16);
  e.sequence = readLe32(in + 20);
}

bool sameCode(const char* a, const char* b) { return a[0] == b[0] && a[1] == b[1]; }

uint32_t sectorsFor(const uint32_t bytes) {
  return (bytes + HyphenationStore::kSectorBytes - 1) / HyphenationStore::kSectorBytes;
}

}  // namespace

bool readPackHeader(HyphenationPackSource& src, hyphpack::Header& out) {
  uint8_t bytes[hyphpack::kHeaderBytes];
  if (src.size() < hyphpack::kHeaderBytes || !src.read(0, bytes, sizeof(bytes))) return false;
  hyphpack::Header h;
  if (!hyphpack::parseHeader(bytes, h)) return false;
  if (src.size() != hyphpack::kHeaderBytes + h.payloadSize) return false;
  out = h;
  return true;
}

uint32_t HyphenationStore::capacity() const {
  const uint32_t reserved = kHeaderSectors * kSectorBytes;
  return flash_.size() > reserved ? flash_.size() - reserved : 0;
}

bool HyphenationStore::readEntry(const int8_t copy, const uint8_t index, Entry& out) const {
  uint8_t bytes[kEntryBytes];
  if (!flash_.read(headerBase(copy) + index * kEntryBytes, bytes, kEntryBytes)) return false;
  decodeEntry(bytes, out);
  return true;
}

bool HyphenationStore::entryValid(const Entry& e) const {
  return e.code[0] >= 'a' && e.code[0] <= 'z' && e.code[1] >= 'a' && e.code[1] <= 'z' && e.size != 0 &&
         e.rootOffset < e.size && e.offset % kSectorBytes == 0 && e.offset <= payloadEnd() &&
         e.size <= payloadEnd() - e.offset;
}

bool HyphenationStore::validCopy(const int8_t copy, uint8_t& count, uint32_t& generation) const {
  uint8_t trailer[kTrailerBytes];
  if (!flash_.read(headerBase(copy) + kTrailerOffset, trailer, kTrailerBytes)) return false;
  if (std::memcmp(trailer, kMagic, sizeof(kMagic)) != 0 || trailer[4] != kFormatVersion) return false;
  const uint8_t n = trailer[5];
  if (n > kMaxEntries || trailer[6] != 0 || trailer[7] != 0) return false;
  uint32_t crc = 0;
  for (uint8_t i = 0; i < n; ++i) {
    uint8_t bytes[kEntryBytes];
    if (!flash_.read(headerBase(copy) + i * kEntryBytes, bytes, kEntryBytes)) return false;
    crc = crc32(crc, bytes, kEntryBytes);
    Entry e;
    decodeEntry(bytes, e);
    if (!entryValid(e)) return false;
  }
  crc = crc32(crc, trailer, 12);
  if (crc != readLe32(trailer + 12)) return false;
  count = n;
  generation = readLe32(trailer + 8);
  return true;
}

bool HyphenationStore::mount() {
  unmapAll();
  active_ = -1;
  count_ = 0;
  generation_ = 0;
  if (flash_.size() < (kHeaderSectors + 1) * kSectorBytes) {
    LOG_ERR("HYS", "region of %u B is too small", static_cast<unsigned>(flash_.size()));
    return false;
  }
  for (int8_t copy = 0; copy < static_cast<int8_t>(kHeaderSectors); ++copy) {
    uint8_t n = 0;
    uint32_t gen = 0;
    if (!validCopy(copy, n, gen)) continue;
    if (active_ < 0 || gen > generation_) {
      active_ = copy;
      count_ = n;
      generation_ = gen;
    }
  }
  LOG_INF("HYS", "mounted: %u languages, generation %u", count_, static_cast<unsigned>(generation_));
  return true;
}

bool HyphenationStore::entryAt(const uint8_t index, Entry& out) const {
  return active_ >= 0 && index < count_ && readEntry(active_, index, out);
}

bool HyphenationStore::find(const char* code, Entry& out) const {
  if (!code || active_ < 0) return false;
  for (uint8_t i = 0; i < count_; ++i) {
    Entry e;
    if (readEntry(active_, i, e) && sameCode(e.code, code)) {
      out = e;
      return true;
    }
  }
  return false;
}

bool HyphenationStore::commit(const uint16_t keep, const Entry* added) {
  const int8_t target = active_ == 0 ? 1 : 0;
  const uint32_t base = headerBase(target);
  if (!flash_.erase(base, kSectorBytes)) return false;
  uint32_t crc = 0;
  uint8_t n = 0;
  uint8_t bytes[kEntryBytes];
  for (uint8_t i = 0; i < count_; ++i) {
    if ((keep & (1u << i)) == 0) continue;
    Entry e;
    if (!readEntry(active_, i, e)) return false;
    encodeEntry(e, bytes);
    if (!flash_.program(base + n * kEntryBytes, bytes, kEntryBytes)) return false;
    crc = crc32(crc, bytes, kEntryBytes);
    ++n;
  }
  if (added) {
    if (n >= kMaxEntries) return false;
    encodeEntry(*added, bytes);
    if (!flash_.program(base + n * kEntryBytes, bytes, kEntryBytes)) return false;
    crc = crc32(crc, bytes, kEntryBytes);
    ++n;
  }
  uint8_t trailer[kTrailerBytes] = {};
  std::memcpy(trailer, kMagic, sizeof(kMagic));
  trailer[4] = kFormatVersion;
  trailer[5] = n;
  writeLe32(generation_ + 1, trailer + 8);
  writeLe32(crc32(crc, trailer, 12), trailer + 12);
  // The trailer last, in one program: until it lands the other copy stays live.
  if (!flash_.program(base + kTrailerOffset, trailer, kTrailerBytes)) return false;
  active_ = target;
  count_ = n;
  generation_ += 1;
  return true;
}

HyphenationStore::Fill HyphenationStore::fill(HyphenationPackSource& src, const hyphpack::Header& header,
                                              uint8_t* scratch, const uint32_t scratchBytes, const bool mayDrop) {
  if (!scratch || scratchBytes == 0) return Fill::BadSource;
  const uint32_t totalSectors = payloadEnd() / kSectorBytes;
  const uint32_t need = sectorsFor(header.payloadSize);
  if (need > totalSectors) return Fill::NoSpace;

  // The live entries as sector runs, and which of them stay. On the stack: at most 16, 12 B each.
  struct Run {
    uint16_t start;
    uint16_t sectors;
    uint32_t sequence;
    bool kept;
  };
  Run runs[kMaxEntries];
  uint16_t keep = 0;
  bool replacing = false;
  for (uint8_t i = 0; i < count_; ++i) {
    Entry e;
    if (!readEntry(active_, i, e)) return Fill::FlashFailed;
    runs[i] = {static_cast<uint16_t>(e.offset / kSectorBytes), static_cast<uint16_t>(sectorsFor(e.size)), e.sequence,
               true};
    if (sameCode(e.code, header.code)) {
      if (e.size == header.payloadSize && e.crc32 == header.crc32 && e.rootOffset == header.rootOffset &&
          e.minPrefix == header.minPrefix && e.minSuffix == header.minSuffix) {
        return Fill::Unchanged;
      }
      runs[i].kept = false;  // replaced
      replacing = true;
    } else {
      keep |= static_cast<uint16_t>(1u << i);
    }
  }

  // The first gap that fits among the runs kept; while there is none, drop the oldest kept run. The entry count is
  // a limit too: a full table drops its oldest entry as well.
  uint32_t at = 0;
  for (;;) {
    uint8_t keptCount = 0;
    for (uint8_t i = 0; i < count_; ++i) keptCount += runs[i].kept ? 1 : 0;
    bool found = false;
    if (keptCount < kMaxEntries) {
      uint32_t cursor = 0;
      for (;;) {
        // The kept run that starts first at or after the cursor.
        int8_t next = -1;
        for (uint8_t i = 0; i < count_; ++i) {
          if (!runs[i].kept || runs[i].start + runs[i].sectors <= cursor) continue;
          if (next < 0 || runs[i].start < runs[next].start) next = static_cast<int8_t>(i);
        }
        const uint32_t gapEnd = next < 0 ? totalSectors : runs[next].start;
        if (gapEnd >= cursor && gapEnd - cursor >= need) {
          at = cursor;
          found = true;
          break;
        }
        if (next < 0) break;
        cursor = std::max<uint32_t>(cursor, runs[next].start + runs[next].sectors);
      }
    }
    if (found) break;
    if (!mayDrop) return Fill::WouldDrop;
    int8_t oldest = -1;
    for (uint8_t i = 0; i < count_; ++i) {
      if (runs[i].kept && (oldest < 0 || runs[i].sequence < runs[oldest].sequence)) oldest = static_cast<int8_t>(i);
    }
    if (oldest < 0) return Fill::NoSpace;  // cannot happen: an empty region holds `need` sectors
    runs[oldest].kept = false;
    keep &= static_cast<uint16_t>(~(1u << oldest));
    LOG_INF("HYS", "dropping the oldest language to make room for %s", header.code);
  }

  unmapAll();
  // What is dropped or replaced leaves the header before its sectors may be erased.
  if (keep != static_cast<uint16_t>((1u << count_) - 1u) || replacing) {
    if (!commit(keep, nullptr)) {
      LOG_ERR("HYS", "header write failed");
      return Fill::FlashFailed;
    }
  }

  const uint32_t base = at * kSectorBytes;
  if (!flash_.erase(base, need * kSectorBytes)) {
    LOG_ERR("HYS", "erase of %u sectors at %u failed", static_cast<unsigned>(need), static_cast<unsigned>(base));
    return Fill::FlashFailed;
  }
  uint32_t crc = 0;
  for (uint32_t done = 0; done < header.payloadSize;) {
    const uint32_t len = std::min(scratchBytes, header.payloadSize - done);
    if (!src.read(hyphpack::kHeaderBytes + done, scratch, len)) return Fill::BadSource;
    crc = crc32(crc, scratch, len);
    if (!flash_.program(base + done, scratch, len)) {
      LOG_ERR("HYS", "program at %u failed", static_cast<unsigned>(base + done));
      return Fill::FlashFailed;
    }
    done += len;
  }
  if (crc != header.crc32) {
    LOG_ERR("HYS", "%s: the pack's payload fails its CRC", header.code);
    return Fill::BadSource;
  }
  // Read back: a byte that did not program must not be named by a header.
  crc = 0;
  for (uint32_t done = 0; done < header.payloadSize;) {
    const uint32_t len = std::min(scratchBytes, header.payloadSize - done);
    if (!flash_.read(base + done, scratch, len)) return Fill::FlashFailed;
    crc = crc32(crc, scratch, len);
    done += len;
  }
  if (crc != header.crc32) {
    LOG_ERR("HYS", "%s: read-back fails the CRC", header.code);
    return Fill::FlashFailed;
  }

  Entry added;
  added.code[0] = header.code[0];
  added.code[1] = header.code[1];
  added.minPrefix = header.minPrefix;
  added.minSuffix = header.minSuffix;
  added.offset = base;
  added.size = header.payloadSize;
  added.rootOffset = header.rootOffset;
  added.crc32 = header.crc32;
  added.sequence = generation_ + 1;
  const uint16_t all = static_cast<uint16_t>((1u << count_) - 1u);
  if (!commit(all, &added)) {
    LOG_ERR("HYS", "header write failed");
    return Fill::FlashFailed;
  }
  LOG_INF("HYS", "%s stored: %u B at %u", header.code, static_cast<unsigned>(header.payloadSize),
          static_cast<unsigned>(base));
  return Fill::Stored;
}

bool HyphenationStore::map(const char* code, SerializedHyphenationPatterns& out, uint32_t* crc32Out) {
  if (!code) return false;
  Mapped* freeSlot = nullptr;
  for (Mapped& m : mapped_) {
    if (m.ptr && sameCode(m.code, code)) {
      out = {m.rootOffset, m.ptr, m.size};
      if (crc32Out) *crc32Out = m.crc32;
      return true;
    }
    if (!m.ptr && !freeSlot) freeSlot = &m;
  }
  Entry e;
  if (!find(code, e)) return false;
  if (!freeSlot) {
    LOG_ERR("HYS", "%s: %u languages are mapped already", code, kMaxMapped);
    return false;
  }
  const uint8_t* ptr = flash_.map(e.offset, e.size);
  if (!ptr) {
    LOG_ERR("HYS", "%s: map of %u B failed", code, static_cast<unsigned>(e.size));
    return false;
  }
  // Other firmware writes this flash too (an older build's font cache reached this far): the header standing is no
  // proof the bytes behind it are the ones it names.
  if (crc32(0, ptr, e.size) != e.crc32) {
    LOG_ERR("HYS", "%s: the stored trie fails its CRC", code);
    flash_.unmap(ptr);
    return false;
  }
  freeSlot->code[0] = e.code[0];
  freeSlot->code[1] = e.code[1];
  freeSlot->ptr = ptr;
  freeSlot->size = e.size;
  freeSlot->rootOffset = e.rootOffset;
  freeSlot->crc32 = e.crc32;
  out = {e.rootOffset, ptr, e.size};
  if (crc32Out) *crc32Out = e.crc32;
  return true;
}

void HyphenationStore::unmapAll() {
  for (Mapped& m : mapped_) {
    if (m.ptr) flash_.unmap(m.ptr);
    m = Mapped{};
  }
}
