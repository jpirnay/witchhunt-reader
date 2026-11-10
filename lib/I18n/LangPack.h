#pragma once

#include <cstddef>
#include <cstdint>

// A UI language pack on the SD card: /.crosspoint/lang/<code>.cplang, one per language but English, which
// is in the image. scripts/gen_i18n.py writes them (--packs), the Font Manager installs them, I18n reads
// the selected language's into the flash slot (FlashLangPartition).
//
// A pack names each string by a hash of its key's NAME, never by a StrId: the pack master holds serves
// every firmware from 2.41 on. A key the firmware does not know is skipped, a key the pack lacks reads
// as English.
//
//   header, HEADER_BYTES, little-endian:
//     [4]  magic "CPLG"
//     [1]  format version (FORMAT_VERSION)
//     [1]  0
//     [2]  entries in the payload
//     [4]  language code, ASCII lower case, NUL-padded ("de")
//     [4]  CRC32 of the payload as stored
//     [4]  payload length as stored
//     [4]  payload length inflated
//     [4]  extension length as stored (0: none), right after the payload
//     [4]  extension length inflated
//     [4]  CRC32 of the extension as stored
//     [28] native name, UTF-8, NUL-padded
//   payload:   raw deflate, 4 KB window, of entries [u32 key hash][UTF-8 value, NUL]
//   extension: raw deflate, 4 KB window: the bytes the slot carries after the strings
//
// The window is what keeps the inflate ring at 4 KB (FlashLangPartition::INFLATE_RING_BYTES).
namespace langpack {

constexpr uint8_t FORMAT_VERSION = 1;
constexpr size_t HEADER_BYTES = 64;
constexpr size_t CODE_BYTES = 4;
constexpr size_t NAME_BYTES = 28;
constexpr size_t WINDOW_BYTES = 4096;
// What inflateSection() needs beside the ring: a block of the card read, and one of output.
constexpr size_t READ_BYTES = 512;
constexpr size_t OUT_BYTES = 256;
constexpr size_t IO_BYTES = READ_BYTES + OUT_BYTES;
constexpr uint16_t NO_KEY = 0xFFFF;

struct Header {
  uint16_t entries = 0;
  char code[CODE_BYTES + 1] = {};
  uint32_t payloadCrc = 0;
  uint32_t payloadLen = 0;
  uint32_t rawLen = 0;
  uint32_t extLen = 0;
  uint32_t extRawLen = 0;
  uint32_t extCrc = 0;
  char name[NAME_BYTES + 1] = {};

  uint32_t payloadOffset() const { return HEADER_BYTES; }
  uint32_t extOffset() const { return HEADER_BYTES + payloadLen; }
};

// FNV-1a of a key's name ("STR_SETTINGS_TITLE"). gen_i18n.py fails the build on two keys with one hash.
constexpr uint32_t keyHash(const char* name) {
  uint32_t h = 0x811C9DC5u;
  for (; *name; ++name) {
    h ^= static_cast<uint8_t>(*name);
    h *= 0x01000193u;
  }
  return h;
}

// Reads a header. False for another magic or format, an empty or malformed code, or lengths that
// cannot be a pack's (an extension inflating to nothing, a payload past 1 MB).
bool parseHeader(const uint8_t* bytes, size_t len, Header& out);
void encodeHeader(const Header& h, uint8_t (&out)[HEADER_BYTES]);

// What the slot holds for a pack, identified: its payload and extension, and the firmware's StrId order
// (KEY_LAYOUT_STAMP). A firmware that adds keys rebuilds from the card; one that does not keeps the slot.
uint32_t slotStamp(const Header& h, uint32_t keyLayoutStamp);

// The StrId of `hash`, NO_KEY when the firmware has none: a binary search over `byHash`, the StrIds in
// ascending order of `hashes[StrId]`.
uint16_t findKey(uint32_t hash, const uint32_t* hashes, const uint16_t* byHash, uint16_t count);

// The bytes of a section as stored, from where it starts: read() returns how many it gave (0 at the end
// of the source), or < 0 on an error.
struct ByteSource {
  int (*read)(void* ctx, uint8_t* buf, size_t len);
  void* ctx;
};

// The inflated bytes of a section, in order. False stops the inflate.
struct ByteSink {
  bool (*write)(void* ctx, const uint8_t* data, size_t len);
  void* ctx;
};

// Inflates a section of `len` bytes as stored into `sink`, checking it against its CRC and its inflated
// length. `ring` is WINDOW_BYTES, `io` IO_BYTES: nothing else is allocated but the inflate's state
// (~1.3 KB, freed on return). False on a read error, a stream that does not inflate, a CRC or a length
// that differs, or the sink refusing.
bool inflateSection(const ByteSource& src, uint32_t len, uint32_t rawLen, uint32_t crc, uint8_t* ring, uint8_t* io,
                    const ByteSink& sink);

// Receives the entries of a payload: begin(), then the value's bytes in pieces (none for an empty one),
// then end(). Any returning false stops the parse.
class EntrySink {
 public:
  virtual bool begin(uint32_t hash) = 0;
  virtual bool bytes(const char* data, size_t len) = 0;
  virtual bool end() = 0;

 protected:
  ~EntrySink() = default;
};

// Splits inflated payload bytes into entries, across any boundary the inflate cuts them at.
class EntryParser {
 public:
  explicit EntryParser(EntrySink& sink) : sink_(sink) {}
  bool feed(const uint8_t* data, size_t len);
  // Between entries: a payload that ends anywhere else is cut short.
  bool atBoundary() const { return hashBytes_ == 0; }
  uint32_t entries() const { return entries_; }

 private:
  EntrySink& sink_;
  uint32_t hash_ = 0;
  uint8_t hashBytes_ = 0;
  uint32_t entries_ = 0;
};

}  // namespace langpack
