#pragma once
// Ours (writing modes step 2, PR 3), not upstream's: crosspoint-reader PR #3787's direct-mapped run cache, as a cache
// of the section build's shaped words in the lent arena's free tail.

#include <cstddef>
#include <cstdint>

class BuildArena;
class ShapedStore;

/// The build's cache of shaped words. A chapter repeats its words, and shaping one costs about 2.3 ms on the X3 (the
/// OpenType engine's GSUB and GPOS walk): an unglued Indic token's pieces are kept here once shaped, and replayed into
/// the paragraph store wherever the token comes again with the same identity (its text's 64-bit hash and length, and
/// a key of its font, style, size and the book's language, keyFor()).
///
/// Where: the top `bytes` of a lent build arena, [capacity - bytes, capacity), in direct-mapped slots of kSlotBytes.
/// The arena still counts those bytes as free, so no other arena user loses room to the cache, and any allocation may
/// overwrite them (docs/memory-allocation-strategy.md §4, Class C). The arena's watermark says when one has
/// (BuildArena::peakSinceMark): the cache then empties itself, and fills again once the cursor is back down. It is
/// checked before every read and every write of a slot, and is sound because nothing writes an arena's memory except
/// through alloc() (§6).
///
/// The parser owns one for the parse: armed in setup(), disarmed at the end of finalize(), before the build gives the
/// arena's blocks back. A disarmed cache never touches the region again.
class WordShapeCache {
 public:
  struct Stats {
    uint32_t hits = 0;      // lookups replayed
    uint32_t misses = 0;    // lookups the tail held no entry for (or that the store refused part-way)
    uint32_t inserts = 0;   // entries stored
    uint32_t bypassed = 0;  // lookups made while the tail could not be used, and entries too large for a slot
    uint32_t clears = 0;    // times an allocation reached the tail and the slots were emptied
  };

  /// The tail on a lent build arena (S).
  static constexpr size_t kWordCacheBytes = 16384;
  /// The room kept free below the tail, for the parse lane's own blocks (the open page block, a shaping call's block,
  /// a TrueType miss): arm() needs S + kRoomBelow free, and an emptied cache arms again once the cursor is back at
  /// capacity - S - kRoomBelow or below. The X3 and X4 lent arenas have it at the parse's start (41,428 and about
  /// 37,156 B free); the owned 10 KB arena of a released build does not.
  static constexpr size_t kRoomBelow = 16384;
  /// A slot: 64 B holds 99.0% of the long Hindi book's shaped tokens (make_test_shaped_indic.py --long: 2,428 of 2,452;
  /// the longest takes 74 B), 256 slots in S. An entry is kEntryHeaderBytes, then per piece kPieceHeaderBytes and its
  /// records or form; a token of more than 255 bytes is not stored.
  static constexpr size_t kSlotBytes = 64;
  static constexpr size_t kEntryHeaderBytes = 14;  // length (1), pieces (1), key (4), hash (8)
  static constexpr size_t kPieceHeaderBytes = 5;   // kind, begin, end, records, bytes (1 each)

  /// Takes the top `bytes` of `arena` (whole slots of it), empty. False, and every call bypassed, when the arena has
  /// less than bytes + kRoomBelow free, or when arming is turned off (setEnabled).
  bool arm(BuildArena& arena, size_t bytes);
  /// Lets go of the arena: no call reads or writes it again. The stats and bytes() stay, for the build's log line.
  void disarm();

  /// The entry for (textHash, length, key), its pieces written through the store's sink into its open entry: true on
  /// a hit. False on a miss, with nothing written; also when the sink refused a piece part-way, with what it took left
  /// in the open entry (the caller abandons it).
  bool replay(uint64_t textHash, uint16_t length, uint32_t key, ShapedStore& store);
  /// Stores token `token`'s committed pieces as the entry for (textHash, length, key), in place of whatever held its
  /// slot. Nothing is stored while the tail cannot be used (the lookup before it counted that), or for an entry larger
  /// than a slot (counted as bypassed).
  void insert(uint64_t textHash, uint16_t length, uint32_t key, const ShapedStore& store, size_t token);

  /// The cache's key: a token's shape key (ParsedText::shapeKey) with the language tags it is shaped in folded in
  /// (TypographyProfile::shapingLanguage, ot::languageTagsFor's zero-terminated array; null: the script's default).
  static uint32_t keyFor(uint32_t shapeKey, const uint32_t* languageTags);

  const Stats& stats() const { return stats_; }
  /// The tail's size the cache was armed with; 0 when it never armed.
  size_t bytes() const { return bytes_; }

  /// For tests: off, arm() refuses, so a build runs as without the cache. On by default.
  static void setEnabled(bool enabled) { enabled_ = enabled; }

 private:
  // Whether the slots may be read and written now: the tail intact since the last mark; else emptied (counted once),
  // and armed again with a new mark when the cursor is back down.
  bool tailUsable();
  void emptySlots();
  uint8_t* slotFor(uint64_t textHash, uint16_t length, uint32_t key) const;

  BuildArena* arena_ = nullptr;
  uint8_t* tail_ = nullptr;  // the slots: arena_->tail(bytes_)
  size_t bytes_ = 0;
  size_t slots_ = 0;
  bool intact_ = false;  // no allocation has reached the tail since the mark, and the slots hold what was put there
  Stats stats_;
  static bool enabled_;
};
