#include "ReadingStatsSlotFile.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <bitset>
#include <cstring>
#include <vector>

namespace ReadingStatsSlotFile {
namespace {

constexpr uint8_t kMagic[4] = {'R', 'S', 'T', 'B'};
constexpr size_t kDaySize = 6;

// Meta copy: header, directory, global days, CRC.
constexpr size_t kVersionAt = 4;
constexpr size_t kSlotCountAt = 6;
constexpr size_t kSeqAt = 8;
constexpr size_t kTotalSecondsAt = 12;
constexpr size_t kTotalSessionsAt = 16;
constexpr size_t kTotalPagesAt = 20;
constexpr size_t kLongestStreakAt = 24;
constexpr size_t kBookCountAt = 26;
constexpr size_t kGlobalDayCountAt = 28;
constexpr size_t kHeaderSize = 32;
constexpr size_t kEntrySize = 36;
constexpr size_t kEntriesAt = kHeaderSize;
constexpr size_t kGlobalDaysAt = kEntriesAt + kEntryCount * kEntrySize;
constexpr size_t kMetaCrcAt = kGlobalDaysAt + kGlobalDayCapacity * kDaySize;
static_assert(kMetaCrcAt == 6032, "the meta layout moved");
static_assert(kMetaCrcAt + 4 <= kMetaSize, "the meta outgrew its copy");

// A directory entry, from its first byte (the key's).
constexpr size_t kEntrySlotAt = 16;  // slot + 1: 0 marks a free entry, so a zeroed image has none
constexpr size_t kEntryDayCountAt = 17;
constexpr size_t kEntryProgressAt = 18;
constexpr size_t kEntryFinishedAt = 20;
constexpr size_t kEntrySecondsAt = 24;
constexpr size_t kEntryLastReadAt = 28;

// A book slot, from its first byte (the key's).
constexpr size_t kSlotSecondsAt = 16;
constexpr size_t kSlotPagesAt = 20;
constexpr size_t kSlotSessionsAt = 24;
constexpr size_t kSlotFirstReadAt = 28;
constexpr size_t kSlotLastReadAt = 36;
constexpr size_t kSlotLastFinishedAt = 44;
constexpr size_t kSlotFinishedAt = 52;
constexpr size_t kSlotProgressAt = 54;
constexpr size_t kSlotDayCountAt = 55;
constexpr size_t kSlotTitleLenAt = 56;
constexpr size_t kSlotAuthorLenAt = 58;
constexpr size_t kSlotDaysAt = 60;
constexpr size_t kSlotTitleAt = kSlotDaysAt + kBookDayCapacity * kDaySize;
constexpr size_t kSlotAuthorAt = kSlotTitleAt + kTitleMax;
constexpr size_t kSlotCrcAt = kSlotSize - 4;
static_assert(kSlotAuthorAt + kAuthorMax == 900, "the slot layout moved");
static_assert(kSlotAuthorAt + kAuthorMax <= kSlotCrcAt, "the slot outgrew its size");

void put16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void put64(uint8_t* p, const int64_t v) {
  const auto u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(u >> (8 * i));
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

int64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return static_cast<int64_t>(v);
}

void putDays(uint8_t* p, const std::vector<DayBucket>& days, const size_t first) {
  for (size_t i = first; i < days.size(); ++i, p += kDaySize) {
    put16(p, days[i].dayIndex);
    put32(p + 2, days[i].seconds);
  }
}

void getDays(const uint8_t* p, const size_t count, std::vector<DayBucket>& days) {
  days.clear();
  days.reserve(count);
  for (size_t i = 0; i < count; ++i, p += kDaySize) days.push_back({get16(p), get32(p + 2)});
}

int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

uint8_t rawSlot(const uint8_t* image, const size_t index) {
  return image[kEntriesAt + index * kEntrySize + kEntrySlotAt];
}

}  // namespace

bool parseDocId(const std::string& docId, DocKey& key) {
  if (docId.size() != 2 * key.size()) return false;
  for (size_t i = 0; i < key.size(); ++i) {
    const int hi = hexDigit(docId[2 * i]);
    const int lo = hexDigit(docId[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    key[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

std::string formatDocId(const DocKey& key) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text(2 * key.size(), '0');
  for (size_t i = 0; i < key.size(); ++i) {
    text[2 * i] = kHex[key[i] >> 4];
    text[2 * i + 1] = kHex[key[i] & 0x0F];
  }
  return text;
}

size_t cutLength(const std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return text.size();
  size_t n = maxBytes;
  // Back off to the first byte of the character the limit falls inside.
  while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;
  return n;
}

Entry entryFor(const DocKey& key, const BookReadingStats& book, const uint8_t slot) {
  Entry entry;
  entry.key = key;
  entry.slot = slot;
  entry.dayCount = static_cast<uint8_t>(std::min(book.days.size(), kBookDayCapacity));
  entry.progress = book.progress;
  entry.finishedCount = book.finishedCount;
  entry.totalSeconds = book.totalSeconds;
  entry.lastReadEpoch = static_cast<int64_t>(book.lastReadEpoch);
  return entry;
}

// ---- Meta -------------------------------------------------------------------------------------

std::unique_ptr<Meta> Meta::create() { return makeUniqueNoThrow<Meta>(); }

void Meta::clear() { std::memset(image_, 0, sizeof(image_)); }

uint32_t Meta::seq() const { return get32(image_ + kSeqAt); }

void Meta::setSeq(const uint32_t seq) { put32(image_ + kSeqAt, seq); }

uint32_t Meta::totalSeconds() const { return get32(image_ + kTotalSecondsAt); }

void Meta::readTotals(ReadingTotals& totals) const {
  totals.totalSeconds = get32(image_ + kTotalSecondsAt);
  totals.totalSessions = get32(image_ + kTotalSessionsAt);
  totals.totalPagesTurned = get32(image_ + kTotalPagesAt);
  totals.longestStreak = get16(image_ + kLongestStreakAt);
  getDays(image_ + kGlobalDaysAt, std::min<size_t>(get16(image_ + kGlobalDayCountAt), kGlobalDayCapacity),
          totals.globalDays);
}

bool Meta::writeTotals(const ReadingTotals& totals) {
  if (totals.globalDays.size() > kGlobalDayCapacity) return false;
  put32(image_ + kTotalSecondsAt, totals.totalSeconds);
  put32(image_ + kTotalSessionsAt, totals.totalSessions);
  put32(image_ + kTotalPagesAt, totals.totalPagesTurned);
  put16(image_ + kLongestStreakAt, totals.longestStreak);
  put16(image_ + kGlobalDayCountAt, static_cast<uint16_t>(totals.globalDays.size()));
  std::memset(image_ + kGlobalDaysAt, 0, kGlobalDayCapacity * kDaySize);
  putDays(image_ + kGlobalDaysAt, totals.globalDays, 0);
  return true;
}

Entry Meta::entry(const size_t index) const {
  Entry entry;
  if (index >= kEntryCount || rawSlot(image_, index) == 0) return entry;
  const uint8_t* p = image_ + kEntriesAt + index * kEntrySize;
  std::memcpy(entry.key.data(), p, entry.key.size());
  entry.slot = static_cast<uint8_t>(p[kEntrySlotAt] - 1);
  entry.dayCount = p[kEntryDayCountAt];
  entry.progress = p[kEntryProgressAt];
  entry.finishedCount = get16(p + kEntryFinishedAt);
  entry.totalSeconds = get32(p + kEntrySecondsAt);
  entry.lastReadEpoch = get64(p + kEntryLastReadAt);
  return entry;
}

void Meta::setEntry(const size_t index, const Entry& entry) {
  if (index >= kEntryCount) return;
  uint8_t* p = image_ + kEntriesAt + index * kEntrySize;
  std::memset(p, 0, kEntrySize);
  if (!entry.used()) return;
  std::memcpy(p, entry.key.data(), entry.key.size());
  p[kEntrySlotAt] = static_cast<uint8_t>(entry.slot + 1);
  p[kEntryDayCountAt] = entry.dayCount;
  p[kEntryProgressAt] = entry.progress;
  put16(p + kEntryFinishedAt, entry.finishedCount);
  put32(p + kEntrySecondsAt, entry.totalSeconds);
  put64(p + kEntryLastReadAt, entry.lastReadEpoch);
}

size_t Meta::find(const DocKey& key) const {
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (rawSlot(image_, i) != 0 && std::memcmp(image_ + kEntriesAt + i * kEntrySize, key.data(), key.size()) == 0) {
      return i;
    }
  }
  return kEntryCount;
}

size_t Meta::freeEntry() const {
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (rawSlot(image_, i) == 0) return i;
  }
  return kEntryCount;
}

uint8_t Meta::freeSlot() const {
  std::bitset<kSlotCount> taken;
  for (size_t i = 0; i < kEntryCount; ++i) {
    const uint8_t raw = rawSlot(image_, i);
    if (raw != 0 && raw <= kSlotCount) taken.set(raw - 1);
  }
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    if (!taken.test(slot)) return static_cast<uint8_t>(slot);
  }
  return kNoSlot;
}

size_t Meta::bookCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kEntryCount; ++i) count += rawSlot(image_, i) != 0 ? 1 : 0;
  return count;
}

void Meta::seal() {
  std::memcpy(image_, kMagic, sizeof(kMagic));
  put16(image_ + kVersionAt, kVersion);
  put16(image_ + kSlotCountAt, static_cast<uint16_t>(kSlotCount));
  put16(image_ + kBookCountAt, static_cast<uint16_t>(bookCount()));
  put32(image_ + kMetaCrcAt, crc32(image_, kMetaCrcAt));
}

bool Meta::valid() const {
  if (std::memcmp(image_, kMagic, sizeof(kMagic)) != 0) return false;
  if (get16(image_ + kVersionAt) != kVersion || get16(image_ + kSlotCountAt) != kSlotCount) return false;
  if (get32(image_ + kMetaCrcAt) != crc32(image_, kMetaCrcAt)) return false;
  if (get16(image_ + kGlobalDayCountAt) > kGlobalDayCapacity) return false;
  // Every book in a slot of its own: two entries on one slot would lose a book at the next update.
  std::bitset<kSlotCount> taken;
  size_t books = 0;
  for (size_t i = 0; i < kEntryCount; ++i) {
    const uint8_t raw = rawSlot(image_, i);
    if (raw == 0) continue;
    if (raw > kSlotCount || taken.test(raw - 1)) return false;
    taken.set(raw - 1);
    ++books;
  }
  return books == get16(image_ + kBookCountAt);
}

// ---- Slots ------------------------------------------------------------------------------------

void encodeSlot(const DocKey& key, const BookReadingStats& book, uint8_t* out) {
  std::memset(out, 0, kSlotSize);
  std::memcpy(out, key.data(), key.size());
  put32(out + kSlotSecondsAt, book.totalSeconds);
  put32(out + kSlotPagesAt, book.pagesTurned);
  put32(out + kSlotSessionsAt, book.sessions);
  put64(out + kSlotFirstReadAt, static_cast<int64_t>(book.firstReadEpoch));
  put64(out + kSlotLastReadAt, static_cast<int64_t>(book.lastReadEpoch));
  put64(out + kSlotLastFinishedAt, static_cast<int64_t>(book.lastFinishedEpoch));
  put16(out + kSlotFinishedAt, book.finishedCount);
  out[kSlotProgressAt] = book.progress;
  // The newest days: they are sorted ascending.
  const size_t first = book.days.size() > kBookDayCapacity ? book.days.size() - kBookDayCapacity : 0;
  out[kSlotDayCountAt] = static_cast<uint8_t>(book.days.size() - first);
  putDays(out + kSlotDaysAt, book.days, first);
  const size_t titleLen = cutLength(book.title, kTitleMax);
  const size_t authorLen = cutLength(book.author, kAuthorMax);
  put16(out + kSlotTitleLenAt, static_cast<uint16_t>(titleLen));
  put16(out + kSlotAuthorLenAt, static_cast<uint16_t>(authorLen));
  std::memcpy(out + kSlotTitleAt, book.title.data(), titleLen);
  std::memcpy(out + kSlotAuthorAt, book.author.data(), authorLen);
  put32(out + kSlotCrcAt, crc32(out, kSlotCrcAt));
}

bool decodeSlot(const uint8_t* in, DocKey& key, BookReadingStats& book) {
  if (get32(in + kSlotCrcAt) != crc32(in, kSlotCrcAt)) return false;
  const uint8_t dayCount = in[kSlotDayCountAt];
  const uint16_t titleLen = get16(in + kSlotTitleLenAt);
  const uint16_t authorLen = get16(in + kSlotAuthorLenAt);
  if (dayCount > kBookDayCapacity || titleLen > kTitleMax || authorLen > kAuthorMax) return false;
  std::memcpy(key.data(), in, key.size());
  book = BookReadingStats{};
  book.docId = formatDocId(key);
  book.title.assign(reinterpret_cast<const char*>(in + kSlotTitleAt), titleLen);
  book.author.assign(reinterpret_cast<const char*>(in + kSlotAuthorAt), authorLen);
  book.totalSeconds = get32(in + kSlotSecondsAt);
  book.pagesTurned = get32(in + kSlotPagesAt);
  book.sessions = get32(in + kSlotSessionsAt);
  book.firstReadEpoch = static_cast<time_t>(get64(in + kSlotFirstReadAt));
  book.lastReadEpoch = static_cast<time_t>(get64(in + kSlotLastReadAt));
  book.lastFinishedEpoch = static_cast<time_t>(get64(in + kSlotLastFinishedAt));
  book.finishedCount = get16(in + kSlotFinishedAt);
  book.progress = in[kSlotProgressAt];
  getDays(in + kSlotDaysAt, dayCount, book.days);
  return true;
}

// ---- The file ---------------------------------------------------------------------------------

Load loadMeta(HalFile& file, Meta& meta, uint8_t& live) {
  live = kNoCopy;
  if (file.size() != kFileSize) return Load::Corrupt;
  bool marked[2] = {false, false};
  uint32_t seqs[2] = {0, 0};
  for (uint8_t copy = 0; copy < 2; ++copy) {
    uint8_t head[kSeqAt + 4];
    if (!file.seek(copy * kMetaSize) || file.read(head, sizeof(head)) != static_cast<int>(sizeof(head))) {
      return Load::IoError;
    }
    marked[copy] = std::memcmp(head, kMagic, sizeof(kMagic)) == 0;
    seqs[copy] = get32(head + kSeqAt);
  }
  // The newer copy first; one that fails its checks (a write torn by a power cut) gives way to
  // the other, which the torn write never touched.
  const uint8_t newer = seqs[1] > seqs[0] ? 1 : 0;
  const uint8_t order[2] = {newer, static_cast<uint8_t>(1 - newer)};
  for (const uint8_t copy : order) {
    if (!marked[copy]) continue;
    if (!file.seek(copy * kMetaSize) || file.read(meta.data(), kMetaSize) != static_cast<int>(kMetaSize)) {
      return Load::IoError;
    }
    if (meta.valid()) {
      live = copy;
      return Load::Ok;
    }
    LOG_ERR("RST", "Meta copy %c fails its checks", copy == 0 ? 'A' : 'B');
  }
  return Load::Corrupt;
}

bool writeMeta(HalFile& file, const Meta& meta, const uint8_t copy) {
  return copy < 2 && file.seek(copy * kMetaSize) && file.write(meta.data(), kMetaSize) == kMetaSize;
}

bool readSlot(HalFile& file, const uint8_t slot, uint8_t* out) {
  return slot < kSlotCount && file.seek(kSlotsOffset + slot * kSlotSize) &&
         file.read(out, kSlotSize) == static_cast<int>(kSlotSize);
}

bool writeSlot(HalFile& file, const uint8_t slot, const uint8_t* in) {
  return slot < kSlotCount && file.seek(kSlotsOffset + slot * kSlotSize) && file.write(in, kSlotSize) == kSlotSize;
}

bool createZeroed(const char* path) {
  auto zeros = makeUniqueNoThrow<uint8_t[]>(kSlotSize);
  if (!zeros) return false;
  FsFile file;
  if (!Storage.openFileForWrite("RST", path, file)) return false;
  for (size_t written = 0; written < kFileSize; written += kSlotSize) {
    if (file.write(zeros.get(), kSlotSize) != kSlotSize) return false;
  }
  file.flush();
  return true;
}

}  // namespace ReadingStatsSlotFile
