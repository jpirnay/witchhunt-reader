#pragma once

#include <NorFlash.h>

#include <cstdint>

#include "HyphenationPack.h"
#include "SerializedHyphenationTrie.h"

// Where a pack's bytes come from when the store copies it in: the .cphyph file on the SD card on the device, a
// buffer in tests. Offsets are from the start of the file, header included.
class HyphenationPackSource {
 public:
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;

 protected:
  ~HyphenationPackSource() = default;
};

// Reads and checks a pack's header: a CPHY v1 header whose payload is exactly the rest of the source.
bool readPackHeader(HyphenationPackSource& src, hyphpack::Header& out);

// The hyphenation tries the reader has used, copied from their SD packs into a region of NOR flash and read in place
// through a mapping, as the tries compiled into the image were: the Liang walk runs over the same bytes through the
// same flash cache and MMU, and holds no RAM.
//
// A cache, not an install: the SD file stays the original, and anything the store loses (a power cut, an eviction,
// another firmware writing the region) is copied again from it the next time a book needs it.
//
// Region layout (offsets from the region's start; the region starts on a 64 KB boundary):
//   payloads             from sector 0, each starting on a sector, packed in any order.
//   last two sectors     two header copies, A and B. The live one is the valid copy with the higher generation.
//
// A header copy:
//   [16 x 24 B]  entries, `count` of them from the start, each:
//                [2 B] code  [1 B] minPrefix  [1 B] minSuffix  [4 B] payload offset in the region
//                [4 B] payload size  [4 B] root offset  [4 B] payload CRC32  [4 B] sequence (the generation it
//                was copied in: the oldest goes first when room is needed)
//   [16 B]       trailer at byte 384: [4 B] "WHHS"  [1 B] version  [1 B] count  [2 B] 0  [4 B] generation
//                [4 B] CRC32 of the entries and the trailer's first 12 bytes
//
// Rules it keeps:
// - A payload is written to sectors no live entry names, read back, and only then named by a header, which is
//   written to the copy that is not live, trailer last. A power cut leaves the old header standing and at worst
//   sectors nothing names.
// - Room is made by dropping the oldest entries in a header of their own before their sectors are erased: a cut
//   between the two loses only what was dropped.
// - Every byte is programmed once between erases. Erases happen only when a language is copied in.
//
// Lifetimes: the patterns map() hands out stay valid until fill() or unmapAll(); fill() unmaps everything before it
// erases. Not thread-safe; the reader calls it at book open, before any build runs.
class HyphenationStore {
 public:
  static constexpr uint32_t kSectorBytes = NorFlash::kSectorBytes;
  static constexpr uint32_t kHeaderSectors = 2;
  static constexpr uint8_t kMaxEntries = 16;
  // Languages mapped at once: the open book's, and one more for a page from another book (a dictionary, a
  // library preview). Each costs the MMU pages its trie spans (German: 4).
  static constexpr uint8_t kMaxMapped = 2;
  static constexpr uint8_t kFormatVersion = 1;

  struct Entry {
    char code[3] = {};
    uint8_t minPrefix = 0;
    uint8_t minSuffix = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t rootOffset = 0;
    uint32_t crc32 = 0;
    uint32_t sequence = 0;
  };

  enum class Fill : uint8_t {
    Stored,       // copied in and named by the live header
    Unchanged,    // the store already holds these bytes for this language
    NoSpace,      // the payload is larger than the whole region
    BadSource,    // the source could not be read, or its bytes do not match the header's CRC
    FlashFailed,  // an erase, program or read-back failed; the header still names what it named before
    WouldDrop,    // asked not to drop another language, and there is no room without dropping one
  };

  explicit HyphenationStore(NorFlash& flash) : flash_(flash) {}
  ~HyphenationStore() { unmapAll(); }
  HyphenationStore(const HyphenationStore&) = delete;
  HyphenationStore& operator=(const HyphenationStore&) = delete;

  // Finds the live header. An erased or unreadable region mounts as an empty store. False only when the region is
  // too small to hold the headers and one sector of payload.
  bool mount();

  uint8_t count() const { return count_; }
  // Payload bytes the region holds: everything before the two header sectors.
  uint32_t capacity() const;
  bool entryAt(uint8_t index, Entry& out) const;
  bool find(const char* code, Entry& out) const;

  // Copies the pack `header` describes (from readPackHeader) out of `src` into the store, dropping the oldest
  // entries while there is no room, and replacing this language's entry if it holds other bytes. `scratch` is the
  // copy buffer (1 KB is plenty: a program call costs ~0.1 ms on top of its bytes on the X3). Unmaps everything first.
  // With `mayDrop` false no other language is dropped: WouldDrop, and nothing written, when the pack does not fit
  // beside them (a pack copied at its download, which a book open copies later if it must).
  Fill fill(HyphenationPackSource& src, const hyphpack::Header& header, uint8_t* scratch, uint32_t scratchBytes,
            bool mayDrop = true);

  // The patterns of `code`, mapped, and on their first mapping checked against their CRC (one read of the trie).
  // False when the store has no entry for it, the mapping budget is spent, kMaxMapped languages are mapped
  // already, or the bytes fail their CRC (then fill() it again from the card).
  // `crc32`, when given, receives the trie's CRC.
  bool map(const char* code, SerializedHyphenationPatterns& out, uint32_t* crc32 = nullptr);
  void unmapAll();

 private:
  static constexpr uint32_t kEntryBytes = 24;
  static constexpr uint32_t kTrailerOffset = kMaxEntries * kEntryBytes;
  static constexpr uint32_t kTrailerBytes = 16;

  struct Mapped {
    char code[3] = {};
    const uint8_t* ptr = nullptr;
    uint32_t size = 0;
    uint32_t rootOffset = 0;
    uint32_t crc32 = 0;
  };

  // The headers sit in the region's last two sectors, so payloads start on its first 64 KB block: German's 51 sectors
  // erase as three blocks and three sectors rather than two blocks and 19 sectors.
  uint32_t payloadEnd() const { return (flash_.size() / kSectorBytes - kHeaderSectors) * kSectorBytes; }
  uint32_t headerBase(int8_t copy) const { return payloadEnd() + static_cast<uint32_t>(copy) * kSectorBytes; }
  bool readEntry(int8_t copy, uint8_t index, Entry& out) const;
  bool entryValid(const Entry& e) const;
  // The header copy's entry count and generation when it is valid.
  bool validCopy(int8_t copy, uint8_t& count, uint32_t& generation) const;
  // Writes a header to the copy that is not live: the live entries whose bit is set in `keep`, then `added` when
  // given, under the next generation. On success it is the live copy.
  bool commit(uint16_t keep, const Entry* added);

  NorFlash& flash_;
  int8_t active_ = -1;  // the live header copy; -1: none (an empty store)
  uint8_t count_ = 0;
  uint32_t generation_ = 0;
  Mapped mapped_[kMaxMapped];
};
