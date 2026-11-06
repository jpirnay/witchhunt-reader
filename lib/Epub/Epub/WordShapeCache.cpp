// Ours (writing modes step 2, PR 3). The slots live in a lent arena's free tail; see WordShapeCache.h for why every
// read and write of them is behind tailUsable().
#include "WordShapeCache.h"

#include <BuildArena.h>
#include <GlyphRun.h>

#include <cstdint>
#include <cstring>

#include "ShapedStore.h"

bool WordShapeCache::enabled_ = true;

namespace {

// A slot, byte by byte (the tail is a framebuffer's bytes, read unaligned):
//   [0] the text's length in bytes, 0 for an empty slot   [1] the pieces
//   [2..5] the key, [6..13] the text's hash, little-endian
//   then each piece: kind, begin, end, records, form bytes; then the form (the Glyphs piece's encoded records, the
//   Reordered piece's UTF-8; a Text piece has none).
constexpr size_t kLengthAt = 0;
constexpr size_t kPiecesAt = 1;
constexpr size_t kKeyAt = 2;
constexpr size_t kHashAt = 6;
// The most language tags keyFor() folds: ot::languageTagsFor's arrays hold a handful.
constexpr size_t kMaxLanguageTags = 16;

void put32(uint8_t* at, const uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t* at, const uint64_t v) {
  for (int i = 0; i < 8; ++i) at[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get32(const uint8_t* at) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(at[i]) << (8 * i);
  return v;
}
uint64_t get64(const uint8_t* at) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(at[i]) << (8 * i);
  return v;
}

uint32_t fold(uint32_t key, const uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    key ^= static_cast<uint8_t>(value >> (8 * i));
    key *= 16777619u;  // FNV-1a, as ParsedText's shape key
  }
  return key;
}

// Whether the entry in `slot` reads whole within the slot: every piece's header and form inside it, a known kind, its
// bytes of the text in order and within the text, and a Glyphs piece's records exactly its form.
bool entryReads(const uint8_t* slot) {
  const size_t length = slot[kLengthAt];
  size_t at = WordShapeCache::kEntryHeaderBytes;
  for (unsigned p = 0; p < slot[kPiecesAt]; ++p) {
    if (at + WordShapeCache::kPieceHeaderBytes > WordShapeCache::kSlotBytes) return false;
    const uint8_t* piece = slot + at;
    const auto kind = static_cast<IndicShaper::PieceKind>(piece[0]);
    const size_t begin = piece[1], end = piece[2], records = piece[3], bytes = piece[4];
    at += WordShapeCache::kPieceHeaderBytes;
    if (at + bytes > WordShapeCache::kSlotBytes || begin > end || end > length) return false;
    switch (kind) {
      case IndicShaper::PieceKind::Text:
        if (records != 0 || bytes != 0) return false;
        break;
      case IndicShaper::PieceKind::Reordered:
        if (records != 0) return false;
        break;
      case IndicShaper::PieceKind::Glyphs: {
        size_t pos = 0;
        for (size_t r = 0; r < records; ++r) {
          GlyphRecord record;
          size_t read = 0;
          if (!glyph_run::decode(slot + at + pos, bytes - pos, &record, &read)) return false;
          pos += read;
        }
        if (pos != bytes) return false;
        break;
      }
      default:
        return false;
    }
    at += bytes;
  }
  return true;
}

}  // namespace

bool WordShapeCache::arm(BuildArena& arena, const size_t bytes) {
  disarm();
  bytes_ = 0;
  const size_t slots = bytes / kSlotBytes;
  if (!enabled_ || !arena.valid() || slots == 0 || bytes > SIZE_MAX - kRoomBelow) return false;
  // Room for the tail and the parse lane's own blocks below it, so that those never reach it.
  if (arena.capacity() - arena.used() < bytes + kRoomBelow) return false;
  arena_ = &arena;
  slots_ = slots;
  bytes_ = slots * kSlotBytes;
  tail_ = arena.tail(bytes_);
  arena.markPeak();
  emptySlots();
  intact_ = true;
  return true;
}

void WordShapeCache::disarm() {
  arena_ = nullptr;
  tail_ = nullptr;
  slots_ = 0;
  intact_ = false;
}

bool WordShapeCache::tailUsable() {
  if (arena_ == nullptr) return false;
  const size_t tailStart = arena_->capacity() - bytes_;
  if (intact_) {
    // No byte handed out since the mark ends past the tail's first: the slots are as the cache left them.
    if (arena_->peakSinceMark() <= tailStart) return true;
    intact_ = false;  // an allocation reached the tail: whatever the slots hold is not ours any more
    ++stats_.clears;
  }
  // Armed again only where arm() would: with the room below the tail free, the cursor far under it. The slots are
  // written only now that no live allocation can lie in them.
  if (arena_->used() > tailStart - kRoomBelow) return false;
  arena_->markPeak();
  emptySlots();
  intact_ = true;
  return true;
}

void WordShapeCache::emptySlots() {
  for (size_t s = 0; s < slots_; ++s) tail_[s * kSlotBytes + kLengthAt] = 0;
}

uint8_t* WordShapeCache::slotFor(const uint64_t textHash, const uint16_t length, const uint32_t key) const {
  uint32_t h = static_cast<uint32_t>(textHash) ^ static_cast<uint32_t>(textHash >> 32);
  h ^= key * 0x9E3779B1u;  // Fibonacci hashing: the key's high bits reach the low ones
  h ^= length;
  h ^= h >> 16;
  return tail_ + (h % slots_) * kSlotBytes;
}

uint32_t WordShapeCache::keyFor(const uint32_t shapeKey, const uint32_t* const languageTags) {
  // Null (the script's default) and an empty list are told apart, as ShapingFace::plan tells their plans apart.
  if (languageTags == nullptr) return fold(shapeKey, 0xFFFFFFFFu);
  uint32_t key = shapeKey;
  for (size_t n = 0; n < kMaxLanguageTags && languageTags[n] != 0; ++n) key = fold(key, languageTags[n]);
  return fold(key, 0);
}

bool WordShapeCache::replay(const uint64_t textHash, const uint16_t length, const uint32_t key, ShapedStore& store) {
  if (!tailUsable()) {
    ++stats_.bypassed;
    return false;
  }
  uint8_t* const slot = slotFor(textHash, length, key);
  if (slot[kLengthAt] == 0 || slot[kLengthAt] != length || get32(slot + kKeyAt) != key ||
      get64(slot + kHashAt) != textHash) {
    ++stats_.misses;
    return false;
  }
  if (!entryReads(slot)) {  // never written so: emptied rather than trusted
    slot[kLengthAt] = 0;
    ++stats_.misses;
    return false;
  }
  size_t at = kEntryHeaderBytes;
  for (unsigned p = 0; p < slot[kPiecesAt]; ++p) {
    const uint8_t* const piece = slot + at;
    const auto kind = static_cast<IndicShaper::PieceKind>(piece[0]);
    const size_t records = piece[3], bytes = piece[4];
    const uint8_t* const form = piece + kPieceHeaderBytes;
    bool written = store.beginPiece(kind, piece[1], piece[2]);
    if (written && kind == IndicShaper::PieceKind::Glyphs) {
      size_t pos = 0;
      for (size_t r = 0; written && r < records; ++r) {
        GlyphRecord record;
        size_t read = 0;
        glyph_run::decode(form + pos, bytes - pos, &record, &read);  // entryReads() decoded them all
        pos += read;
        written = store.addGlyph(record);
      }
    } else if (written && kind == IndicShaper::PieceKind::Reordered && bytes > 0) {
      written = store.addBytes(reinterpret_cast<const char*>(form), bytes);
    }
    if (!written) {  // the store had no room: the caller abandons the open entry and shapes the token
      ++stats_.misses;
      return false;
    }
    at += kPieceHeaderBytes + bytes;
  }
  ++stats_.hits;
  return true;
}

void WordShapeCache::insert(const uint64_t textHash, const uint16_t length, const uint32_t key,
                            const ShapedStore& store, const size_t token) {
  if (!tailUsable()) return;
  const auto pieces = store.committedPieces(token);
  if (pieces.empty()) return;
  // The entry's size, and whether each field fits its byte.
  size_t need = kEntryHeaderBytes;
  bool fits = length != 0 && length <= UINT8_MAX && pieces.size() <= UINT8_MAX;
  for (const ShapedStore::Piece& piece : pieces) {
    if (!fits) break;
    need += kPieceHeaderBytes + piece.bytes;
    fits = need <= kSlotBytes && piece.end <= length && piece.count <= UINT8_MAX;
  }
  if (!fits) {
    ++stats_.bypassed;
    return;
  }
  uint8_t* const slot = slotFor(textHash, length, key);
  slot[kLengthAt] = 0;  // empty until the entry is whole
  slot[kPiecesAt] = static_cast<uint8_t>(pieces.size());
  put32(slot + kKeyAt, key);
  put64(slot + kHashAt, textHash);
  size_t at = kEntryHeaderBytes;
  for (const ShapedStore::Piece& piece : pieces) {
    uint8_t* const header = slot + at;
    header[0] = static_cast<uint8_t>(piece.kind);
    header[1] = static_cast<uint8_t>(piece.begin);
    header[2] = static_cast<uint8_t>(piece.end);
    header[3] = static_cast<uint8_t>(piece.count);
    header[4] = static_cast<uint8_t>(piece.bytes);
    at += kPieceHeaderBytes;
    if (piece.kind == IndicShaper::PieceKind::Glyphs) {
      const GlyphRun run = store.glyphs(piece);
      if (run.bytes > 0) std::memcpy(slot + at, run.records, run.bytes);
    } else if (piece.kind == IndicShaper::PieceKind::Reordered) {
      const auto form = store.form(piece);
      if (!form.empty()) std::memcpy(slot + at, form.data(), form.size());
    }
    at += piece.bytes;
  }
  slot[kLengthAt] = static_cast<uint8_t>(length);
  ++stats_.inserts;
}
