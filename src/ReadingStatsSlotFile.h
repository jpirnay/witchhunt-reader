#pragma once

#include <HalStorage.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ReadingStatsTypes.h"

// The reading history on the card: one file of fixed size, never resized.
//
//   0       meta copy A    header, directory of kEntryCount books, global days, CRC
//   6144    meta copy B    the same, one generation apart (seq)
//   12288   kSlotCount slots of kSlotSize bytes, one book each; one more than the cap
//
// Updates are copy-on-write: a book goes into a slot no entry refers to, then the meta goes into the
// copy that is not the valid newest one. Power lost at any point leaves the previous history
// readable (docs/design/reading-stats-slot-file.md). This file is the
// layout only; the store (ReadingStats.h) owns the arithmetic and the order of the writes, and does
// the flushing. Every field is little-endian and written field by field.
namespace ReadingStatsSlotFile {

constexpr char kPath[] = "/.crosspoint/reading-stats.bin";

constexpr uint16_t kVersion = 1;
constexpr size_t kEntryCount = 100;             // the book cap
constexpr size_t kSlotCount = kEntryCount + 1;  // the spare an update writes into
constexpr size_t kGlobalDayCapacity = 400;
constexpr size_t kBookDayCapacity = 60;
constexpr size_t kTitleMax = 320;  // bytes of UTF-8
constexpr size_t kAuthorMax = 160;

constexpr size_t kMetaSize = 6144;
constexpr size_t kSlotSize = 1024;
constexpr size_t kSlotsOffset = 2 * kMetaSize;
constexpr size_t kFileSize = kSlotsOffset + kSlotCount * kSlotSize;

constexpr uint8_t kNoSlot = 0xFF;
constexpr uint8_t kNoCopy = 0xFF;

// A document id as its 16 MD5 bytes.
using DocKey = std::array<uint8_t, 16>;

// The 32 hex characters KOReaderDocumentId makes, as 16 bytes; false for anything else.
bool parseDocId(const std::string& docId, DocKey& key);
// Lowercase hex, as KOReaderDocumentId writes it.
std::string formatDocId(const DocKey& key);

// CRC-32 as zlib computes it (IEEE, reflected, 0xEDB88320).
uint32_t crc32(const uint8_t* data, size_t size);

// How many bytes of `text` fit in `maxBytes` without splitting a UTF-8 character.
size_t cutLength(const std::string& text, size_t maxBytes);

// One directory entry, decoded: what everything but the book screens needs.
struct Entry {
  DocKey key{};
  uint8_t slot = kNoSlot;  // kNoSlot: the entry is free
  uint8_t dayCount = 0;
  uint8_t progress = 0;
  uint16_t finishedCount = 0;
  uint32_t totalSeconds = 0;
  int64_t lastReadEpoch = 0;
  bool used() const { return slot != kNoSlot; }
};

// The entry for `book` kept in `slot`.
Entry entryFor(const DocKey& key, const BookReadingStats& book, uint8_t slot);

// A meta copy as its on-card image, edited in place. 6 KB: always on the heap (create()).
class Meta {
 public:
  // An empty history, seq 0, not sealed. nullptr when the heap has no 6 KB block.
  static std::unique_ptr<Meta> create();
  void clear();

  uint32_t seq() const;
  void setSeq(uint32_t seq);
  uint32_t totalSeconds() const;

  void readTotals(ReadingTotals& totals) const;
  // False when there are more global days than the image holds.
  bool writeTotals(const ReadingTotals& totals);

  Entry entry(size_t index) const;                  // a free Entry for a free index or one out of range
  void setEntry(size_t index, const Entry& entry);  // a free Entry frees the index

  size_t find(const DocKey& key) const;  // kEntryCount when absent
  size_t freeEntry() const;              // kEntryCount when every entry is used
  uint8_t freeSlot() const;              // the lowest slot no entry refers to; kNoSlot if none
  size_t bookCount() const;

  // Magic, version, slot count, book count and CRC: call before writing the image out.
  void seal();
  // All of those hold, and no two entries share a slot.
  bool valid() const;

  const uint8_t* data() const { return image_; }
  uint8_t* data() { return image_; }

 private:
  uint8_t image_[kMetaSize] = {};
};

// A book into a slot image of kSlotSize bytes: title and author cut to their limits at a character
// boundary, at most kBookDayCapacity days (the newest).
void encodeSlot(const DocKey& key, const BookReadingStats& book, uint8_t* out);
// The book in a slot image, docId formatted from the slot's key; false when the slot fails its
// check (a free, torn or damaged slot).
bool decodeSlot(const uint8_t* in, DocKey& key, BookReadingStats& book);

enum class Load : uint8_t { Ok, Corrupt, IoError };

// The valid newest meta copy into `meta`; `live` is its copy (0 or 1). Corrupt when neither copy
// holds or the file is not kFileSize bytes.
Load loadMeta(HalFile& file, Meta& meta, uint8_t& live);
// A sealed meta into copy 0 or 1. No flush.
bool writeMeta(HalFile& file, const Meta& meta, uint8_t copy);
bool readSlot(HalFile& file, uint8_t slot, uint8_t* out);
// No flush.
bool writeSlot(HalFile& file, uint8_t slot, const uint8_t* in);
// A new file of kFileSize zero bytes at `path` (no valid meta yet), flushed.
bool createZeroed(const char* path);

}  // namespace ReadingStatsSlotFile
