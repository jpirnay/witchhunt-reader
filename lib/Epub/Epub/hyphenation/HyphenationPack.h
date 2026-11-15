#pragma once

#include <cstddef>
#include <cstdint>

// A hyphenation pack on the SD card: `hyph-<code>.cphyph`, one language's Liang trie.
//
// The file layout is CPHY v1, crosspoint-reader PR #3706's ("feat: hyphenation manager", Uri Tauber), kept byte for
// byte so a pack built for CrossPoint or CrossInk installs here and the other way round. Little-endian:
//
//   [ 4 B] magic "CPHY"
//   [ 1 B] version 1
//   [ 2 B] language code, two lowercase ASCII letters ("de")
//   [ 1 B] minimum characters before a break (lefthyphenmin)
//   [ 1 B] minimum characters after a break (righthyphenmin)
//   [ 1 B] flags, 0
//   [ 2 B] reserved, 0
//   [ 4 B] root offset into the payload
//   [ 4 B] payload size
//   [ 4 B] CRC32 of the payload (zlib's)
//   [payload] the Typst hypher trie with its 4-byte root prefix removed: exactly the bytes the generated
//             hyph-<code>.trie.h headers hold, so SerializedHyphenationPatterns reads it unchanged.
//
// The pack carries data only. How a language's letters are told and lowered, and its minima, stay in the
// firmware's language table (LanguageRegistry.cpp); a pack whose minima disagree with that table is refused
// rather than obeyed, so a pack cannot change how the firmware hyphenates beyond its patterns.
namespace hyphpack {

static constexpr size_t kHeaderBytes = 24;
static constexpr uint8_t kVersion = 1;
// The largest payload a pack may declare. The largest trie in the hypher set is Hungarian's, 353,879 B; German's,
// the largest that ships today, is 206,259 B.
static constexpr uint32_t kMaxPayloadBytes = 512u * 1024u;

struct Header {
  char code[3] = {};  // NUL-terminated
  uint8_t minPrefix = 0;
  uint8_t minSuffix = 0;
  uint32_t rootOffset = 0;
  uint32_t payloadSize = 0;
  uint32_t crc32 = 0;
};

// Reads the 24 header bytes. False when they are not a CPHY v1 header this firmware reads: another magic or
// version, flags or reserved bits set, a code that is not two lowercase letters, an empty or oversized payload, or
// a root offset outside it.
bool parseHeader(const uint8_t* bytes, Header& out);

// The 24 header bytes for `h` (the pack builder's half, for tests and tools).
void writeHeader(const Header& h, uint8_t* out);

}  // namespace hyphpack

// Where a pack's bytes come from: the .cphyph file on the SD card on the device, a buffer in tests. The store copies
// a pack in from one (HyphenationStore::fill), and a C3 without the store reads the trie through one in place
// (PagedTrieReader). Offsets are from the start of the file, header included.
class HyphenationPackSource {
 public:
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;

 protected:
  ~HyphenationPackSource() = default;
};
