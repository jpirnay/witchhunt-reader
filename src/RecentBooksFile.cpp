#include "RecentBooksFile.h"

#include <Crc32.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "RecentBooksStore.h"

namespace RecentBooksFile {
namespace {

constexpr uint8_t kMagic[4] = {'R', 'C', 'N', 'T'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderUsed = 20;  // magic, version, slot count, slot size, table and slots offsets

// A slot: record version, metadata stamp, the five lengths, the five strings, CRC over all before it; zeros after.
constexpr uint8_t kRecordVersion = 1;
constexpr size_t kRecordHead = 1 + 4 + 5 * 2;
static_assert(kRecordHead + kPathMax + kTitleMax + kAuthorMax + kSeriesMax + kCoverMax + 4 <= kSlotSize,
              "a full slot outgrows its size");

void put16(uint8_t* at, const uint16_t v) {
  at[0] = static_cast<uint8_t>(v);
  at[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* at, const uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }
uint32_t get32(const uint8_t* at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
         (static_cast<uint32_t>(at[3]) << 24);
}

// How many bytes of `text` a field of `maxBytes` keeps, ending on a whole character.
size_t keptLength(const std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return text.size();
  return static_cast<size_t>(utf8SafeTruncateBuffer(text.data(), static_cast<int>(maxBytes)));
}

uint32_t slotOffset(const uint8_t slot) { return kSlotsOffset + static_cast<uint32_t>(slot) * kSlotSize; }

}  // namespace

uint32_t pathHash(const std::string_view path) {
  uint32_t hash = 2166136261u;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash != 0 ? hash : 1;
}

File::Open File::open(const char* const path) {
  if (!Storage.exists(path)) return Open::Missing;
  if (!Storage.openFileForUpdate("RCB", path, file_)) return Open::Unreadable;
  if (file_.fileSize() < kSlotsOffset) {
    file_.close();
    return Open::Foreign;
  }
  uint8_t head[kHeaderUsed];
  if (!file_.seek(0) || file_.read(head, sizeof(head)) != static_cast<int>(sizeof(head))) {
    file_.close();
    return Open::Unreadable;
  }
  if (std::memcmp(head, kMagic, sizeof(kMagic)) != 0 || get16(head + 4) != kVersion || get16(head + 6) != kSlotCount ||
      get32(head + 8) != kSlotSize || get32(head + 12) != kTableOffset || get32(head + 16) != kSlotsOffset) {
    LOG_ERR("RCB", "%s is not a recent-books list this firmware reads", path);
    file_.close();
    return Open::Foreign;
  }
  return Open::Ok;
}

bool File::create(const char* const path) {
  // The header and the free table, the slots' offset in zeros; slots are added as books come.
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotsOffset);
  if (!image) {
    LOG_ERR("RCB", "No memory to start %s", path);
    return false;
  }
  std::memset(image.get(), 0, kSlotsOffset);
  std::memcpy(image.get(), kMagic, sizeof(kMagic));
  put16(image.get() + 4, kVersion);
  put16(image.get() + 6, kSlotCount);
  put32(image.get() + 8, kSlotSize);
  put32(image.get() + 12, kTableOffset);
  put32(image.get() + 16, kSlotsOffset);
  if (!Storage.openFileForWrite("RCB", path, file_) || file_.write(image.get(), kSlotsOffset) != kSlotsOffset) {
    LOG_ERR("RCB", "Could not start %s", path);
    file_.close();
    return false;
  }
  file_.flush();
  // Reopened in the mode every later change writes in.
  file_.close();
  return open(path) == Open::Ok;
}

bool File::readEntries(const uint8_t first, Entry* out, const uint8_t count) {
  if (first >= kSlotCount || count == 0 || count > kChunk || first + count > kSlotCount) return false;
  uint8_t bytes[kChunk * kEntrySize];
  const size_t size = static_cast<size_t>(count) * kEntrySize;
  if (!file_.seek(kTableOffset + static_cast<uint32_t>(first) * kEntrySize) ||
      file_.read(bytes, size) != static_cast<int>(size)) {
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    out[i].hash = get32(bytes + i * kEntrySize);
    out[i].seq = get32(bytes + i * kEntrySize + 4);
  }
  return true;
}

bool File::writeEntry(const uint8_t slot, const Entry& entry) {
  if (slot >= kSlotCount) return false;
  uint8_t bytes[kEntrySize];
  put32(bytes, entry.hash);
  put32(bytes + 4, entry.seq);
  if (!file_.seek(kTableOffset + static_cast<uint32_t>(slot) * kEntrySize) ||
      file_.write(bytes, sizeof(bytes)) != sizeof(bytes)) {
    LOG_ERR("RCB", "Could not write entry %u", slot);
    return false;
  }
  file_.flush();
  return true;
}

uint8_t File::slotsOnCard() {
  const size_t size = file_.fileSize();
  if (size <= kSlotsOffset) return 0;
  return static_cast<uint8_t>(std::min<size_t>((size - kSlotsOffset) / kSlotSize, kSlotCount));
}

bool File::readSlot(const uint8_t slot, RecentBook& out) {
  if (slot >= slotsOnCard()) return false;
  uint8_t head[kRecordHead];
  if (!file_.seek(slotOffset(slot)) || file_.read(head, sizeof(head)) != static_cast<int>(sizeof(head)) ||
      head[0] != kRecordVersion) {
    return false;
  }
  constexpr size_t kMax[5] = {kPathMax, kTitleMax, kAuthorMax, kSeriesMax, kCoverMax};
  std::string* const fields[5] = {&out.path, &out.title, &out.author, &out.series, &out.coverBmpPath};
  uint16_t lengths[5];
  for (int f = 0; f < 5; ++f) {
    lengths[f] = get16(head + 5 + 2 * f);
    if (lengths[f] > kMax[f]) return false;
  }
  uint32_t crc = FsHelpers::crc32(head, sizeof(head));
  for (int f = 0; f < 5; ++f) {
    fields[f]->resize(lengths[f]);
    if (lengths[f] > 0 && file_.read(&(*fields[f])[0], lengths[f]) != static_cast<int>(lengths[f])) return false;
    crc = FsHelpers::crc32(fields[f]->data(), lengths[f], crc);
  }
  uint8_t stored[4];
  if (file_.read(stored, sizeof(stored)) != static_cast<int>(sizeof(stored)) || get32(stored) != crc) return false;
  out.metadataStamp = get32(head + 1);
  return true;
}

bool File::writeSlot(const uint8_t slot, const RecentBook& book) {
  if (slot >= kSlotCount || slot > slotsOnCard()) return false;
  if (book.path.empty() || book.path.size() > kPathMax) {
    LOG_ERR("RCB", "Path too long for a recent-books slot (%u B): %s", static_cast<unsigned>(book.path.size()),
            book.path.c_str());
    return false;
  }
  auto image = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!image) {
    LOG_ERR("RCB", "No memory to write a recent-books slot");
    return false;
  }
  uint8_t* const out = image.get();
  std::memset(out, 0, kSlotSize);
  out[0] = kRecordVersion;
  put32(out + 1, book.metadataStamp);
  const std::string* const fields[5] = {&book.path, &book.title, &book.author, &book.series, &book.coverBmpPath};
  constexpr size_t kMax[5] = {kPathMax, kTitleMax, kAuthorMax, kSeriesMax, kCoverMax};
  size_t at = kRecordHead;
  for (int f = 0; f < 5; ++f) {
    const size_t length = keptLength(*fields[f], kMax[f]);
    put16(out + 5 + 2 * f, static_cast<uint16_t>(length));
    std::memcpy(out + at, fields[f]->data(), length);
    at += length;
  }
  put32(out + at, FsHelpers::crc32(out, at));
  if (!file_.seek(slotOffset(slot)) || file_.write(out, kSlotSize) != kSlotSize) {
    LOG_ERR("RCB", "Could not write slot %u", slot);
    return false;
  }
  file_.flush();
  return true;
}

}  // namespace RecentBooksFile
