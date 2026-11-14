#pragma once

#include <FlashLangPartition.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// The interface language read from the card a string at a time, for a board whose flash area has no room for the
// language slot (FlashLangPartition::Failure::NoArea or SlotTooSmall: a partition table without a usable `spiffs`
// partition, on a board with no PSRAM to hold the area's image instead; docs/design/flash-area.md, backend (c)).
//
// The slot hands I18n::get() pointers into mapped flash, which cost no RAM. Without it a resident copy of a pack's
// strings would take 17-31 KB of heap (German ~17 KB, Ukrainian ~31 KB), which the X3 does not have to spare while a
// section builds. Here the pack is unpacked once into a card file, and a key's string is read from it the first time
// anything asks for it, into an arena that only grows: a session holds the strings of the screens it drew, not the
// pack's. A pointer get() hands out stays valid until the store is destroyed (a language change), as the slot's do
// until it is rebuilt.
//
// The file, little-endian:
//   [ 4 B] magic "CPLS"
//   [ 1 B] format version
//   [ 1 B] language index
//   [ 2 B] key count
//   [ 4 B] stamp: langpack::slotStamp(), as the slot's (the pack and the firmware's StrId order)
//   [ 4 B] data length
//   [ 4 B] 0
//   [table] key count x {u16 offset in data, or FlashLangPartition::FALLBACK: the key reads as English;
//                        u16 length, its NUL included}
//   [data]  the strings, NUL-terminated, in the order the pack holds them
// It is written beside the old one and put in place whole (StringsFile::commit()), so a cut leaves the old file or
// the new one. A header that does not match, or a file whose length disagrees with it, is built again.
namespace langcard {

constexpr char MAGIC[4] = {'C', 'P', 'L', 'S'};
constexpr uint8_t FORMAT_VERSION = 1;
constexpr uint32_t HEADER_BYTES = 20;
constexpr uint32_t ENTRY_BYTES = 4;
// The arena grows in chunks of this. A string never crosses a chunk, so it bounds a string's length too: the longest
// any translation has is 353 B (Ukrainian's STR_READING_FONT_HYPHENATION_OFFER). Small, so a chunk fits the holes of
// a fragmented heap rather than splitting one of its large blocks.
constexpr uint32_t CHUNK_BYTES = 1024;
// A handle is a chunk index (6 bits) and an offset in it (10 bits); index 63 is never used, so 0xFFFE and 0xFFFF are
// free to mean "English" and "not read yet". 63 KB: more than the 32 KB the strings can span (15-bit offsets).
constexpr uint32_t MAX_CHUNKS = 63;
// How long a card that refused a read, or a heap that refused a chunk, is left alone: every row of every frame asks
// for its string, and each would otherwise try the card (or walk the heap) again.
constexpr uint32_t RETRY_AFTER_MS = 1000;

/// The card file, through HalStorage on the device (CardStringsFile.h) and a buffer in the tests. CardStrings calls it
/// under its own lock, from one task at a time.
class StringsFile {
 public:
  /// The file's length; 0 when there is none or it cannot be read now.
  virtual uint32_t size() = 0;
  /// Copies `len` bytes at `offset`. False when the file is not there or not readable now (no card, USB Drive).
  virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;
  /// Lets go of whatever read() holds open: before the file is replaced, and after a read that failed.
  virtual void release() = 0;
  /// A new file beside the one in place: create(), append() its bytes in order, patch() bytes already appended (the
  /// table and the header, last), then commit() puts it in place of the old one, or discard() drops it.
  virtual bool create() = 0;
  virtual bool append(const void* src, uint32_t len) = 0;
  virtual bool patch(uint32_t offset, const void* src, uint32_t len) = 0;
  virtual bool commit() = 0;
  virtual void discard() = 0;

 protected:
  ~StringsFile() = default;
};

/// Whether the arena may take `bytes` more from the heap. The device asks HeapFit for a floor beside them (I18n.cpp).
using GrowthGate = bool (*)(size_t bytes);

class CardStrings {
 public:
  CardStrings(StringsFile& file, GrowthGate mayGrow) : file_(file), mayGrow_(mayGrow) {}
  ~CardStrings();
  CardStrings(const CardStrings&) = delete;
  CardStrings& operator=(const CardStrings&) = delete;

  /// Makes the card file hold the pack of `langIndex` (built from `strings` when it holds another pack, another
  /// firmware's keys, or does not read) and serves it from now on. Transient while it builds: the producer's scratch,
  /// a 512 B write buffer and keyCount x 4 B of table. Kept: keyCount x 2 B of handles. False, serving nothing, when
  /// the file cannot be written or there is no memory: the caller stays on English.
  bool open(uint8_t langIndex, uint32_t stamp, uint16_t keyCount, const FlashLangPartition::Strings& strings);

  /// The string of key `id`; nullptr when it reads as English, for good (the pack has none) or for now (the card or
  /// the heap refused: tried again RETRY_AFTER_MS later). Lock-free once a key has been read; the first ask reads the
  /// card under the store's lock. Any task.
  const char* get(uint16_t id);

  /// Whether the last open() built the file (rather than finding it as it should be).
  bool built() const { return built_; }
  /// Bytes the arena holds in strings, and in chunks taken from the heap.
  uint32_t internedBytes() const { return interned_; }
  uint32_t arenaBytes() const { return chunkCount_ * CHUNK_BYTES; }
  /// Reads of the card file since open(), the table entry and the string of a key counting once.
  uint32_t cardReads() const { return cardReads_; }

 private:
  static constexpr uint16_t UNREAD = 0xFFFF;
  static constexpr uint16_t ENGLISH = 0xFFFE;

  bool holdsPack(uint8_t langIndex, uint32_t stamp, uint16_t keyCount);
  bool build(uint8_t langIndex, uint32_t stamp, uint16_t keyCount, const FlashLangPartition::Strings& strings);
  const char* load(uint16_t id);
  // Room for `bytes` in the arena, under lock_: where they go, and the key's handle for them. False: no chunk to be
  // had.
  bool reserve(uint32_t bytes, char** at, uint16_t* handle);
  void deferRetry();

  StringsFile& file_;
  GrowthGate mayGrow_;
  std::mutex lock_;                      // the file, the arena's tail, and a key's handle while it is read
  std::unique_ptr<uint16_t[]> handles_;  // a key's string: chunk << 10 | offset, ENGLISH or UNREAD
  uint16_t keyCount_ = 0;
  uint32_t dataLen_ = 0;
  char* chunks_[MAX_CHUNKS] = {};
  uint32_t chunkCount_ = 0;
  uint32_t tail_ = CHUNK_BYTES;  // bytes used in the last chunk: none to begin with
  uint32_t interned_ = 0;
  uint32_t cardReads_ = 0;
  uint32_t retryAt_ = 0;
  bool retryPending_ = false;
  bool built_ = false;
};

}  // namespace langcard
