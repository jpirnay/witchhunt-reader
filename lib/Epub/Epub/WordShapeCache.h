#pragma once
// Ours (writing modes step 2, PR 3), not upstream's, after crosspoint-reader PR #3787's direct-mapped run cache: the
// section build's shaped words, two to a set, in the lent arena's free tail.

#include <cstddef>
#include <cstdint>

class BuildArena;
class ShapedStore;

/// The build's cache of shaped words. A chapter repeats its words, and shaping one costs about 2.3 ms on the X3 (the
/// OpenType engine's GSUB and GPOS walk): an unglued Indic token's pieces are kept here once shaped, and replayed into
/// the paragraph store wherever the token comes again with the same identity (its text's 64-bit hash and length, and
/// a key of its font, style, size and the book's language, keyFor()).
///
/// Where: the top `bytes` of a lent build arena, [capacity - bytes, capacity), in sets of two ways of kWayBytes. The
/// arena still counts those bytes as free, so no other arena user loses room to the cache, and any allocation may
/// overwrite them (docs/memory-allocation-strategy.md §4, Class C). The arena's watermark says when one has
/// (BuildArena::peakSinceMark): the cache then empties itself, and fills again once the cursor is back down. It is
/// checked before every read and every write of a slot, and is sound because nothing writes an arena's memory except
/// through alloc() (§6).
///
/// A set holds two entries of up to a way's bytes; a third evicts the one used least recently (a bit in way 0's flags
/// byte). An entry of up to kDoubleEntryBytes takes both ways, evicting both, and a later one-way entry evicts it
/// whole. Its second way keeps a marker where an entry's header would begin (length 0, kDoubleTail), so neither way
/// can be read as an entry of its own but the first.
///
/// The parser owns one for the parse: armed in setup(), disarmed at the end of finalize(), before the build gives the
/// arena's blocks back. A disarmed cache never touches the region again.
class WordShapeCache {
 public:
  struct Stats {
    uint32_t hits = 0;      // lookups replayed
    uint32_t misses = 0;    // lookups the tail held no entry for (or that the store refused part-way)
    uint32_t inserts = 0;   // entries stored
    uint32_t bypassed = 0;  // lookups made while the tail could not be used
    uint32_t oversize = 0;  // entries too large for a set (or a token over 255 bytes): not stored
    uint32_t clears = 0;    // times an allocation reached the tail and the slots were emptied
  };

  /// The tail on a lent build arena (S).
  static constexpr size_t kWordCacheBytes = 16384;
  /// The room kept free below the tail, for the parse lane's own blocks (the open page block, a shaping call's block,
  /// a TrueType miss): arm() needs S + kRoomBelow free, and an emptied cache arms again once the cursor is back at
  /// capacity - S - kRoomBelow or below. The X3 and X4 lent arenas have it at the parse's start (41,428 and about
  /// 37,156 B free); the owned 10 KB arena of a released build does not.
  static constexpr size_t kRoomBelow = 16384;
  /// A build arena of at least this capacity is the 256 KB block of PSRAM that every S3 board lends instead of its
  /// framebuffer (HalCapabilities::keepsSecondaryBuffer(), HalDisplay::borrowSecondaryBuffer()). No C3 arena comes
  /// near it: the lent framebuffer is 52,272 B on the X3 and 48,000 B on the X4.
  static constexpr size_t kLargeArenaBytes = 256 * 1024;
  /// The tail on such an arena: 1,024 sets, 2,048 entries, where S holds 256. On the host, chapter 2 of the Arabic and
  /// Hindi bench books (tools/writing-modes-bench) hits 40% and 67% of its lookups in it, against 28% and 51% in S, and
  /// no larger tail hits more than a point above that: 64 KB 37% and 65%, 192 KB 41% and 67%. Half the arena, so the
  /// parse lane (to about 24 KB above what the parse holds on those chapters) stays below it; at 224 KB it reached the
  /// tail and emptied it seven times.
  static constexpr size_t kLargeWordCacheBytes = 128 * 1024;
  /// The tail a parse asks for first on an arena of `capacity` bytes (armForBuild): kLargeWordCacheBytes from
  /// kLargeArenaBytes on, else S.
  static constexpr size_t bytesFor(const size_t capacity) {
    return capacity >= kLargeArenaBytes ? kLargeWordCacheBytes : kWordCacheBytes;
  }
  /// A way: 64 B holds 99.0% of the long Hindi book's shaped tokens (make_test_shaped_indic.py --long: 2,428 of 2,452),
  /// and the rest take both ways of a set (the longest 75 B). 128 sets of two in S.
  static constexpr size_t kWayBytes = 64;
  static constexpr size_t kWays = 2;
  static constexpr size_t kSetBytes = kWays * kWayBytes;
  /// An entry, byte by byte from its way's first: the text's length in bytes (0: no entry starts here; a token of more
  /// than 255 bytes is not stored), flags, its pieces, the key and the text's hash (little-endian); then per piece its
  /// kind, begin, end, records and form bytes, and its records or form.
  static constexpr size_t kLengthAt = 0;
  static constexpr size_t kFlagsAt = 1;
  static constexpr size_t kPiecesAt = 2;
  static constexpr size_t kKeyAt = 3;
  static constexpr size_t kHashAt = 7;
  static constexpr size_t kEntryHeaderBytes = 15;
  static constexpr size_t kPieceHeaderBytes = 5;
  /// Flags: way 0 holds the first half of a double entry; way 1 holds its second half; and, in way 0's byte whatever
  /// it holds, the set's way 1 was used more recently than its way 0.
  static constexpr uint8_t kDoubleHead = 0x01;
  static constexpr uint8_t kDoubleTail = 0x02;
  static constexpr uint8_t kWay1Recent = 0x80;
  /// A double entry's bytes: way 0's, then way 1's after its two marker bytes.
  static constexpr size_t kTailMarkerBytes = kFlagsAt + 1;
  static constexpr size_t kDoubleEntryBytes = kSetBytes - kTailMarkerBytes;

  /// Takes the top `bytes` of `arena` (whole sets of it), empty. False, and every call bypassed, when the arena has
  /// less than bytes + kRoomBelow free, or when arming is turned off (setEnabled).
  bool arm(BuildArena& arena, size_t bytes);
  /// What the parse arms with: arm() with bytesFor(arena.capacity()), and with S where that larger tail does not fit
  /// (a parse holding a large stylesheet). On the C3 arenas that is arm() with S alone.
  bool armForBuild(BuildArena& arena);
  /// Lets go of the arena: no call reads or writes it again. The stats and bytes() stay, for the build's log line.
  void disarm();

  /// The entry for (textHash, length, key), its pieces written through the store's sink into its open entry: true on
  /// a hit. False on a miss, with nothing written; also when the sink refused a piece part-way, with what it took left
  /// in the open entry (the caller abandons it).
  bool replay(uint64_t textHash, uint16_t length, uint32_t key, ShapedStore& store);
  /// Stores token `token`'s committed pieces as the entry for (textHash, length, key) in its set, in place of an entry
  /// of that identity, else in a free way, else in place of the way used least recently (both, for an entry of two
  /// ways). Nothing is stored while the tail cannot be used (the lookup before it counted that), or for an entry too
  /// large for a set (counted as oversize).
  void insert(uint64_t textHash, uint16_t length, uint32_t key, const ShapedStore& store, size_t token);

  /// The cache's key: a token's shape key (ParsedText::shapeKey) with the language tags it is shaped in folded in
  /// (TypographyProfile::shapingLanguage, ot::languageTagsFor's zero-terminated array; null: the script's default).
  static uint32_t keyFor(uint32_t shapeKey, const uint32_t* languageTags);

  const Stats& stats() const { return stats_; }
  /// The tail's size the cache was armed with; 0 when it never armed.
  size_t bytes() const { return bytes_; }
  /// The set an identity's entry goes to, counted from the tail's first byte; SIZE_MAX unarmed. For tests.
  size_t setOf(uint64_t textHash, uint16_t length, uint32_t key) const;

  /// For tests: off, arm() refuses, so a build runs as without the cache. On by default.
  static void setEnabled(bool enabled) { enabled_ = enabled; }

 private:
  // Whether the slots may be read and written now: the tail intact since the last mark; else emptied (counted once),
  // and armed again with a new mark when the cursor is back down.
  bool tailUsable();
  void emptySlots();

  BuildArena* arena_ = nullptr;
  uint8_t* tail_ = nullptr;  // the sets: arena_->tail(bytes_)
  size_t bytes_ = 0;
  size_t sets_ = 0;
  bool intact_ = false;  // no allocation has reached the tail since the mark, and the slots hold what was put there
  Stats stats_;
  static bool enabled_;
};
