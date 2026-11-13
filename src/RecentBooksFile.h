#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

struct RecentBook;

// /.crosspoint/recent-books.bin: the books lately opened, a fixed slot each, so that opening, removing or moving a book
// writes only what changed (docs/file-formats.md). This is the layout; RecentBooksStore is the store on top.
//
//   0     header, 512 B: magic, version, slot count, slot size
//   512   slot table, kSlotCount x 8 B: entry i describes slot i, the hash of its book's path (0: free) and the order
//         it was last opened in (its seq, the highest the newest)
//   1536  slots, 1 KB each, written as books come: path, title, author, series, cover, metadata stamp, CRC
//
// Every change writes in place: a listed book opened again its entry (one sector), a removal its entry (hash 0), a new
// book its slot and then its entry. A power cut loses at most the change under way: a row counts only when its slot's
// CRC holds and its path hashes to its entry (RecentBooksStore), so a slot written for a new book whose entry was not
// is not listed.
//
// Nothing of the list is held in RAM: the table is read 8 entries at a time (forEachEntry), a slot when its row is
// wanted.
namespace RecentBooksFile {

constexpr char kPath[] = "/.crosspoint/recent-books.bin";
constexpr uint8_t kSlotCount = 100;
constexpr uint32_t kTableOffset = 512;
constexpr uint32_t kEntrySize = 8;
constexpr uint32_t kSlotsOffset = 1536;
constexpr uint32_t kSlotSize = 1024;
static_assert(kTableOffset + kSlotCount * kEntrySize <= kSlotsOffset, "the table runs into the slots");

// The most bytes a slot keeps of each field: the path whole or not at all (a cut path names another file), the others
// cut at a character.
constexpr size_t kPathMax = 360;
constexpr size_t kTitleMax = 200;
constexpr size_t kAuthorMax = 100;
constexpr size_t kSeriesMax = 80;
constexpr size_t kCoverMax = 260;

struct Entry {
  uint32_t hash = 0;  // pathHash() of the slot's book; 0: the slot is free
  uint32_t seq = 0;   // when the book was last opened, against the others: the highest is the newest
  bool used() const { return hash != 0; }
};

// A path's key in the table: FNV-1a, never 0, which marks a free entry.
uint32_t pathHash(std::string_view path);

class File {
 public:
  // How an open went: the file of this layout, open for reading and writing; not there; there but of another layout
  // or cut short (started afresh); or there and not readable now (left alone: a card that fails a read today may not
  // tomorrow, and starting afresh would lose the list).
  enum class Open : uint8_t { Ok, Missing, Foreign, Unreadable };
  // `path`: kPath, or a test's.
  Open open(const char* path = kPath);
  // A fresh, empty recent-books.bin over whatever was there: the header and a table of free entries, no slots yet.
  bool create(const char* path = kPath);
  void close() { file_.close(); }

  // The entries kSlotCount, in slot order, read from the card a chunk at a time: visit(slot, entry). False when a
  // read failed; the entries already visited stand.
  template <typename Visit>
  bool forEachEntry(Visit&& visit) {
    Entry chunk[kChunk];
    for (uint8_t first = 0; first < kSlotCount; first = static_cast<uint8_t>(first + kChunk)) {
      const uint8_t count = static_cast<uint8_t>(kSlotCount - first < kChunk ? kSlotCount - first : kChunk);
      if (!readEntries(first, chunk, count)) return false;
      for (uint8_t i = 0; i < count; ++i) visit(static_cast<uint8_t>(first + i), chunk[i]);
    }
    return true;
  }
  bool readEntry(uint8_t slot, Entry& out) { return readEntries(slot, &out, 1); }
  bool writeEntry(uint8_t slot, const Entry& entry);

  // How many slots the file holds: it grows a slot at a time, and only by the one after its last.
  uint8_t slotsOnCard();
  // The slot's book, its CRC held. Says nothing of the entry: the caller checks the path against it.
  bool readSlot(uint8_t slot, RecentBook& out);
  // Writes the book into the slot: one already on the card, or the next one. False when the path is longer than a slot
  // keeps, or the write failed.
  bool writeSlot(uint8_t slot, const RecentBook& book);

 private:
  static constexpr uint8_t kChunk = 8;  // 64 B a read
  bool readEntries(uint8_t first, Entry* out, uint8_t count);
  HalFile file_;
};

}  // namespace RecentBooksFile
