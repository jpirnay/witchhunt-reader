#include "SpinePageIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace SpinePageIndex {
namespace {

// Header: magic "PI", format version, section layout version, property hash (4), spine count (2).
// Entries follow, one per spine item: pages (2), inflated bytes (4). Pages 0 = not recorded.
constexpr uint8_t kFormatVersion = 1;
constexpr size_t kHeaderSize = 10;
constexpr size_t kEntrySize = 6;
// Entries moved per read or write: 192 bytes of stack.
constexpr size_t kChunkEntries = 32;

std::string indexPath(const std::string& bookCachePath) { return bookCachePath + "/pages.bin"; }

void encodeHeader(uint8_t (&out)[kHeaderSize], const Variant variant, const uint16_t spineCount) {
  out[0] = 'P';
  out[1] = 'I';
  out[2] = kFormatVersion;
  out[3] = variant.layoutVersion;
  memcpy(out + 4, &variant.propertyHash, sizeof(variant.propertyHash));
  memcpy(out + 8, &spineCount, sizeof(spineCount));
}

// True when `f` holds a complete table for exactly this variant of this book.
bool holdsVariant(FsFile& f, const Variant variant, const uint16_t spineCount) {
  uint8_t want[kHeaderSize];
  uint8_t have[kHeaderSize];
  encodeHeader(want, variant, spineCount);
  return f.size() >= kHeaderSize + static_cast<size_t>(spineCount) * kEntrySize && f.seek(0) &&
         f.read(have, kHeaderSize) == static_cast<int>(kHeaderSize) && memcmp(want, have, kHeaderSize) == 0;
}

// Starts the table over for `variant`: the header, then an unrecorded entry per spine item.
bool create(const std::string& path, const Variant variant, const uint16_t spineCount) {
  FsFile f;
  if (!Storage.openFileForWrite("SPI", path, f)) {
    f.close();
    return false;
  }
  uint8_t head[kHeaderSize];
  encodeHeader(head, variant, spineCount);
  bool ok = f.write(head, kHeaderSize) == kHeaderSize;
  const uint8_t zeros[kChunkEntries * kEntrySize] = {};
  for (size_t left = static_cast<size_t>(spineCount) * kEntrySize; ok && left > 0;) {
    const size_t n = std::min(left, sizeof(zeros));
    ok = f.write(zeros, n) == n;
    left -= n;
  }
  f.close();
  if (!ok) {
    LOG_ERR("SPI", "Could not write %s", path.c_str());
    Storage.remove(path.c_str());
  }
  return ok;
}

}  // namespace

void record(const std::string& bookCachePath, const Variant variant, const int spineCount, const int spineIndex,
            const uint16_t pages, const uint32_t bytes) {
  if (spineCount <= 0 || spineCount > UINT16_MAX || spineIndex < 0 || spineIndex >= spineCount) return;
  const auto count = static_cast<uint16_t>(spineCount);
  const std::string path = indexPath(bookCachePath);

  FsFile f;
  bool ready = false;
  if (Storage.exists(path.c_str())) {
    ready = Storage.openFileForUpdate("SPI", path, f) && holdsVariant(f, variant, count);
    if (!ready) f.close();
  }
  if (!ready) {
    if (!create(path, variant, count)) return;
    if (!Storage.openFileForUpdate("SPI", path, f)) {
      f.close();
      return;
    }
  }

  uint8_t entry[kEntrySize];
  memcpy(entry, &pages, sizeof(pages));
  memcpy(entry + sizeof(pages), &bytes, sizeof(bytes));
  const size_t offset = kHeaderSize + static_cast<size_t>(spineIndex) * kEntrySize;
  uint8_t stored[kEntrySize];
  const bool unchanged = f.seek(offset) && f.read(stored, kEntrySize) == static_cast<int>(kEntrySize) &&
                         memcmp(stored, entry, kEntrySize) == 0;
  if (!unchanged && (!f.seek(offset) || f.write(entry, kEntrySize) != kEntrySize)) {
    LOG_ERR("SPI", "Could not record spine %d in %s", spineIndex, path.c_str());
  }
  f.close();
}

Totals sumRange(const std::string& bookCachePath, const Variant variant, const int spineCount, const int first,
                const int last, const int current) {
  Totals totals;
  if (spineCount <= 0 || spineCount > UINT16_MAX || first < 0 || last < first || last >= spineCount) return totals;
  const std::string path = indexPath(bookCachePath);
  // A plain open, not openFileForRead: no table yet is normal (a fresh book, new settings) and
  // openFileForRead logs every miss.
  FsFile f = Storage.open(path.c_str());
  if (!f) return totals;
  if (!holdsVariant(f, variant, static_cast<uint16_t>(spineCount)) ||
      !f.seek(kHeaderSize + static_cast<size_t>(first) * kEntrySize)) {
    f.close();
    return totals;
  }

  uint8_t chunk[kChunkEntries * kEntrySize];
  for (int spine = first; spine <= last;) {
    const size_t n = std::min(static_cast<size_t>(last - spine + 1), kChunkEntries);
    if (f.read(chunk, n * kEntrySize) != static_cast<int>(n * kEntrySize)) break;
    for (size_t i = 0; i < n; ++i, ++spine) {
      uint16_t pages;
      uint32_t bytes;
      memcpy(&pages, chunk + i * kEntrySize, sizeof(pages));
      memcpy(&bytes, chunk + i * kEntrySize + sizeof(pages), sizeof(bytes));
      if (pages == 0 || spine == current) continue;
      if (spine < current) {
        totals.pagesBefore += pages;
        totals.bytesBefore += bytes;
        totals.filesBefore++;
      } else {
        totals.pagesAfter += pages;
        totals.bytesAfter += bytes;
        totals.filesAfter++;
      }
    }
  }
  f.close();
  return totals;
}

int lastUnrecorded(const std::string& bookCachePath, const Variant variant, const int spineCount, const int first,
                   const int last) {
  if (spineCount <= 0 || spineCount > UINT16_MAX || first < 0 || last < first || last >= spineCount) return -1;
  FsFile f = Storage.open(indexPath(bookCachePath).c_str());
  if (!f) return last;
  if (!holdsVariant(f, variant, static_cast<uint16_t>(spineCount))) {
    f.close();
    return last;
  }
  int found = -1;
  if (f.seek(kHeaderSize + static_cast<size_t>(first) * kEntrySize)) {
    uint8_t chunk[kChunkEntries * kEntrySize];
    for (int spine = first; spine <= last;) {
      const size_t n = std::min(static_cast<size_t>(last - spine + 1), kChunkEntries);
      if (f.read(chunk, n * kEntrySize) != static_cast<int>(n * kEntrySize)) break;
      for (size_t i = 0; i < n; ++i, ++spine) {
        static constexpr uint8_t kNone[kEntrySize] = {};
        if (memcmp(chunk + i * kEntrySize, kNone, kEntrySize) == 0) found = spine;
      }
    }
  }
  f.close();
  return found;
}

void discard(const std::string& bookCachePath) {
  const std::string path = indexPath(bookCachePath);
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
}

}  // namespace SpinePageIndex
