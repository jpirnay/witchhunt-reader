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

using C = WordShapeCache;

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

uint8_t* wayOf(uint8_t* set, const size_t way) { return set + way * C::kWayBytes; }
bool doubleHead(const uint8_t* set) { return (set[C::kFlagsAt] & C::kDoubleHead) != 0; }

// Whether the entry starting at `way` carries this identity (its header lies in that way, a double entry's too).
bool holds(const uint8_t* way, const uint64_t textHash, const uint16_t length, const uint32_t key) {
  return length != 0 && way[C::kLengthAt] == length && get32(way + C::kKeyAt) == key &&
         get64(way + C::kHashAt) == textHash;
}

// The way's entry gone. A double entry's goes from both ways; way 0 keeps the set's recency bit.
void emptyEntry(uint8_t* set, const size_t way) {
  if (way == 0 && doubleHead(set)) {
    uint8_t* second = wayOf(set, 1);
    second[C::kLengthAt] = 0;
    second[C::kFlagsAt] = 0;
  }
  uint8_t* at = wayOf(set, way);
  at[C::kLengthAt] = 0;
  at[C::kFlagsAt] = way == 0 ? static_cast<uint8_t>(at[C::kFlagsAt] & C::kWay1Recent) : 0;
}

// `way` the set's most recently used.
void touch(uint8_t* set, const size_t way) {
  if (way == 1)
    set[C::kFlagsAt] = static_cast<uint8_t>(set[C::kFlagsAt] | C::kWay1Recent);
  else
    set[C::kFlagsAt] = static_cast<uint8_t>(set[C::kFlagsAt] & ~C::kWay1Recent);
}

// The entry starting at `way` as one run of bytes in `out` (kDoubleEntryBytes of room): a way's bytes, or a double
// entry's two halves joined. Its length; 0 when no whole entry starts there: an empty way, a second half, or a double
// entry whose second way does not carry the marker (the halves disagree).
size_t gather(uint8_t* set, const size_t way, uint8_t* out) {
  const uint8_t* at = wayOf(set, way);
  if (at[C::kLengthAt] == 0) return 0;
  if (way == 0 && doubleHead(set)) {
    const uint8_t* second = wayOf(set, 1);
    if (second[C::kLengthAt] != 0 || (second[C::kFlagsAt] & C::kDoubleTail) == 0) return 0;
    std::memcpy(out, at, C::kWayBytes);
    std::memcpy(out + C::kWayBytes, second + C::kTailMarkerBytes, C::kWayBytes - C::kTailMarkerBytes);
    return C::kDoubleEntryBytes;
  }
  if ((at[C::kFlagsAt] & (C::kDoubleHead | C::kDoubleTail)) != 0) return 0;  // a double's half where none can be
  std::memcpy(out, at, C::kWayBytes);
  return C::kWayBytes;
}

// Whether the entry in `entry` (`room` bytes) reads whole within them: at least one piece, every piece's header and
// form inside, a known kind, its bytes of the text in order and within the text, and a Glyphs piece's records exactly
// its form.
bool entryReads(const uint8_t* entry, const size_t room) {
  const size_t length = entry[C::kLengthAt];
  if (entry[C::kPiecesAt] == 0) return false;
  size_t at = C::kEntryHeaderBytes;
  for (unsigned p = 0; p < entry[C::kPiecesAt]; ++p) {
    if (at + C::kPieceHeaderBytes > room) return false;
    const uint8_t* piece = entry + at;
    const auto kind = static_cast<IndicShaper::PieceKind>(piece[0]);
    const size_t begin = piece[1], end = piece[2], records = piece[3], bytes = piece[4];
    at += C::kPieceHeaderBytes;
    if (at + bytes > room || begin > end || end > length) return false;
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
          if (!glyph_run::decode(entry + at + pos, bytes - pos, &record, &read)) return false;
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

// The entry's pieces through the store's sink. False when the sink refused one.
bool writePieces(const uint8_t* entry, ShapedStore& store) {
  size_t at = C::kEntryHeaderBytes;
  for (unsigned p = 0; p < entry[C::kPiecesAt]; ++p) {
    const uint8_t* const piece = entry + at;
    const auto kind = static_cast<IndicShaper::PieceKind>(piece[0]);
    const size_t records = piece[3], bytes = piece[4];
    const uint8_t* const form = piece + C::kPieceHeaderBytes;
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
    if (!written) return false;
    at += C::kPieceHeaderBytes + bytes;
  }
  return true;
}

}  // namespace

bool WordShapeCache::arm(BuildArena& arena, const size_t bytes) {
  disarm();
  bytes_ = 0;
  const size_t sets = bytes / kSetBytes;
  if (!enabled_ || !arena.valid() || sets == 0 || bytes > SIZE_MAX - kRoomBelow) return false;
  // Room for the tail and the parse lane's own blocks below it, so that those rarely reach it.
  if (arena.capacity() - arena.used() < bytes + kRoomBelow) return false;
  arena_ = &arena;
  sets_ = sets;
  bytes_ = sets * kSetBytes;
  tail_ = arena.tail(bytes_);
  arena.markPeak();
  emptySlots();
  intact_ = true;
  return true;
}

bool WordShapeCache::armForBuild(BuildArena& arena) {
  const size_t bytes = bytesFor(arena.capacity());
  return arm(arena, bytes) || (bytes > kWordCacheBytes && arm(arena, kWordCacheBytes));
}

void WordShapeCache::disarm() {
  arena_ = nullptr;
  tail_ = nullptr;
  sets_ = 0;
  intact_ = false;
}

bool WordShapeCache::tailUsable() {
  if (arena_ == nullptr) return false;
  const size_t tailStart = arena_->capacity() - bytes_;
  if (intact_) {
    // No byte handed out since the mark ends past the tail's first: the slots are as the cache left them. (Unmarked,
    // after an arena reset, it reads past any tail.)
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
  for (size_t way = 0; way < sets_ * kWays; ++way) {
    tail_[way * kWayBytes + kLengthAt] = 0;
    tail_[way * kWayBytes + kFlagsAt] = 0;
  }
}

size_t WordShapeCache::setOf(const uint64_t textHash, const uint16_t length, const uint32_t key) const {
  if (sets_ == 0) return SIZE_MAX;
  // Not Fibonacci hashing: the key times an odd constant spreads its low bits upward, and the last fold brings bits
  // 16-31 down onto 0-15. So with the tail's 128 sets the index reads the key's bits 0-22 alone (and the text hash's
  // 0-6, 16-22, 32-38 and 48-54). Keys are FNV-1a values: two that differ almost always differ there. A mix that every
  // bit reaches moved the shaped test books' hit rates by up to eleven points either way, collision noise, so this one
  // stays.
  uint32_t h = static_cast<uint32_t>(textHash) ^ static_cast<uint32_t>(textHash >> 32);
  h ^= key * 0x9E3779B1u;
  h ^= length;
  h ^= h >> 16;
  return h % sets_;
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
  uint8_t* const set = tail_ + setOf(textHash, length, key) * kSetBytes;
  // A double entry fills the set: only way 0 starts an entry then.
  const size_t ways = doubleHead(set) ? 1 : kWays;
  for (size_t way = 0; way < ways; ++way) {
    if (!holds(wayOf(set, way), textHash, length, key)) continue;
    uint8_t entry[kDoubleEntryBytes];  // stack: at most two ways, one copy
    const size_t room = gather(set, way, entry);
    if (room == 0 || !entryReads(entry, room)) {  // never written so: emptied rather than trusted
      emptyEntry(set, way);
      break;
    }
    touch(set, way);
    if (!writePieces(entry, store)) break;  // the store had no room: the caller abandons the open entry, shapes afresh
    ++stats_.hits;
    return true;
  }
  ++stats_.misses;
  return false;
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
    fits = need <= kDoubleEntryBytes && piece.end <= length && piece.count <= UINT8_MAX;
  }
  if (!fits) {
    ++stats_.oversize;
    return;
  }

  // The entry as one run of bytes, then laid into the set.
  uint8_t entry[kDoubleEntryBytes];  // stack: at most two ways
  entry[kLengthAt] = static_cast<uint8_t>(length);
  entry[kFlagsAt] = 0;
  entry[kPiecesAt] = static_cast<uint8_t>(pieces.size());
  put32(entry + kKeyAt, key);
  put64(entry + kHashAt, textHash);
  size_t at = kEntryHeaderBytes;
  for (const ShapedStore::Piece& piece : pieces) {
    uint8_t* const header = entry + at;
    header[0] = static_cast<uint8_t>(piece.kind);
    header[1] = static_cast<uint8_t>(piece.begin);
    header[2] = static_cast<uint8_t>(piece.end);
    header[3] = static_cast<uint8_t>(piece.count);
    header[4] = static_cast<uint8_t>(piece.bytes);
    at += kPieceHeaderBytes;
    if (piece.kind == IndicShaper::PieceKind::Glyphs) {
      const GlyphRun run = store.glyphs(piece);
      if (run.bytes > 0) std::memcpy(entry + at, run.records, run.bytes);
    } else if (piece.kind == IndicShaper::PieceKind::Reordered) {
      const auto form = store.form(piece);
      if (!form.empty()) std::memcpy(entry + at, form.data(), form.size());
    }
    at += piece.bytes;
  }

  uint8_t* const set = tail_ + setOf(textHash, length, key) * kSetBytes;
  // An entry of this identity already held (a replay the store refused, shaped again) gives up its place first.
  for (size_t way = 0; way < (doubleHead(set) ? 1 : kWays); ++way) {
    if (holds(wayOf(set, way), textHash, length, key)) emptyEntry(set, way);
  }

  if (need > kWayBytes) {
    // Both ways: whatever they held goes. Way 1 keeps the marker; the entry's bytes run on after it.
    std::memcpy(set, entry, kWayBytes);
    set[kFlagsAt] = kDoubleHead;
    uint8_t* const second = wayOf(set, 1);
    second[kLengthAt] = 0;
    second[kFlagsAt] = kDoubleTail;
    std::memcpy(second + kTailMarkerBytes, entry + kWayBytes, need - kWayBytes);
    ++stats_.inserts;
    return;
  }
  // One way: a double entry is evicted whole; else a free way, else the one used least recently.
  if (doubleHead(set)) emptyEntry(set, 0);
  size_t way = 0;
  if (wayOf(set, 0)[kLengthAt] == 0) {
    way = 0;
  } else if (wayOf(set, 1)[kLengthAt] == 0) {
    way = 1;
  } else {
    way = (set[kFlagsAt] & kWay1Recent) != 0 ? 0 : 1;
  }
  std::memcpy(wayOf(set, way), entry, need);
  touch(set, way);  // the set's recency, in way 0's flags byte (which a copy into way 0 has just reset)
  ++stats_.inserts;
}
