#pragma once

#include <cstddef>
#include <cstdint>

#include "LangPack.h"

// The conversion after an update from a firmware that kept its languages in the image (2.40, 2.41RC).
// Such a firmware unpacked the selected language into the flash slot as one NUL-terminated string per
// StrId of ITS OWN, in its order, so the slot names no key, only an index. The slot survives the update;
// with the key order of the release that wrote it (lib/I18n/legacy/, LEGACY_LAYOUTS) its strings are
// written out as a pack on the card, and the slot is rebuilt from that like any other pack. The reader
// keeps their language without a download, and the next update finds the pack on the card.
namespace langpack {

// An old slot as FlashLangPartition::mapLegacy() maps it.
struct OldSlot {
  const uint16_t* table;
  const char* data;
  uint32_t dataLen;
  uint16_t keyCount;
};

// Whether `s` is a slot as an old firmware built it. Its table was derived by walking the data entry by
// entry, so the walk is repeated: every key's entry starts where the table says when the table names a
// string, is empty when the table names English (bit 15), the English offsets rise with the key as the
// English strings did, and the data holds exactly one entry per key. A slot of another release's key
// count, or a damaged one, fails.
bool oldSlotSound(const OldSlot& s);

// Writes a pack without compressing it: the payload as stored deflate blocks, which every inflate reads.
// The firmware has no deflate encoder, and the pack a conversion writes is a few KB larger for it, on
// the card. Out::rewind() returns to the file's start, where finish() writes the header once the
// payload's CRC is known.
class PackWriter {
 public:
  struct Out {
    bool (*write)(void* ctx, const void* data, size_t len);
    bool (*rewind)(void* ctx);
    void* ctx;
  };
  // `block` holds one stored block while it fills (at most 65,535 B of it are used).
  PackWriter(const Out& out, uint8_t* block, size_t blockBytes);
  // `code` as the file name spells it ("de"), `name` the language's own name.
  bool begin(const char* code, const char* name);
  bool add(uint32_t hash, const char* value);
  bool finish();
  const Header& header() const { return header_; }

 private:
  bool put(const void* data, size_t len);
  bool emit(bool final);
  bool out(const void* data, size_t len);

  Out out_;
  uint8_t* block_;
  size_t blockBytes_;
  size_t fill_ = 0;
  Header header_;
  bool failed_ = false;
};

// The strings of `s` into `writer`, each under the hash of the key `ids` maps its index to (this
// firmware's StrId, an index into `hashes`); an index mapped to NO_KEY, a key this firmware has not, and
// an entry left to English are skipped. False on a write failure.
bool convertOldSlot(const OldSlot& s, const uint16_t* ids, const uint32_t* hashes, uint16_t keyCount,
                    PackWriter& writer);

}  // namespace langpack
