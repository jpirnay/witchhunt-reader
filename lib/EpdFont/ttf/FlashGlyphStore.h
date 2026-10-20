#pragma once

#include <NorFlash.h>

#include <cstdint>
#include <memory>

#include "EpdFontData.h"
#include "GlyphStoreFormat.h"

/// Glyph records staged in a caller-owned buffer exactly as they will sit in the log, so
/// FlashGlyphStore::commit() writes a whole batch with ONE program call (one per segment when the batch crosses into
/// the next). On the X3 each write call costs about 100 µs on top of its bytes, and one call per batch also programs
/// each flash page once rather than once per record. On boards with PSRAM, keep the buffer in internal RAM (flash
/// writes from PSRAM go in 32-byte pieces).
class GlyphBatch {
 public:
  GlyphBatch(uint8_t* buffer, const uint32_t capacity) : buffer_(buffer), capacity_(capacity) {}

  /// Stages a record for `key` with `metrics` (its dataOffset is set at commit) and returns where the caller
  /// writes the bitmap: all metrics.dataLength bytes of it, since they start as 0xFF, which draws as solid ink.
  /// Returns nullptr when `key` is not a glyph key, the bitmap's length does not match its width and height
  /// or is over glyphstore::kMaxBitmapBytes, or the buffer has no room left (commit, then add again). Keys in
  /// one batch must be distinct and not yet stored.
  uint8_t* add(uint32_t key, const EpdGlyph& metrics);
  /// Takes back the record of the last add() (its bitmap could not be drawn). Only once per add().
  void dropLast();

  uint16_t count() const { return count_; }
  void clear() {
    used_ = 0;
    count_ = 0;
    lastUsed_ = 0;
  }

 private:
  friend class FlashGlyphStore;

  uint8_t* buffer_;
  uint32_t capacity_;
  uint32_t used_ = 0;
  uint32_t lastUsed_ = 0;  // used_ before the last add()
  uint16_t count_ = 0;
};

/// The persistent TTF glyph store in the `spiffs` partition:
/// - a write-once hash index and a log of rendered glyphs, a ring of segments: records are appended at the head
///   segment, the head moves into the next free segment when a record does not fit, and the oldest live segment is
///   the tail;
/// - a kerning pair index, and a face table that gives each family's pixel sizes a small index for the glyph keys;
/// - the fonts' own bytes, in style blocks: a family set's (spec 11.1), one family or several, all in one store.
///
/// Rules it keeps:
/// - Every byte is programmed at most once between erases.
/// - A record is written before the slot that points at it, so a power cut leaves at worst bytes that
///   nothing references, and the next open resumes past them.
/// - A slot names its record's segment by the epoch the segment got when the head moved into it, so a slot into a
///   segment that is no longer live, or was opened again since, is never served. The index is rebuilt before the
///   epochs a slot can name span enough to wrap its 12 bits.
/// - A rebuild of the index gives a slot back only to records a slot named before it: one without (its write cut
///   before its slot) may be torn, and stays unreferenced.
/// - Erases happen only at install, wipe, when the head moves into a segment, when the oldest is reclaimed and when the
///   index is rebuilt, and only on sectors that hold data.
/// - A lookup reads mapped flash in place: a hit is a pointer to the EpdGlyph inside the log.
///
/// Lifetimes: every pointer the store hands out (find()'s EpdGlyph, logBase(), mapStyle()) stays valid until
/// close(), open(), beginInstall(), wipe() or makeRoom(). The NorFlash must outlive the store.
///
/// Not thread-safe: TtfFont calls it only under RenderLock.
class FlashGlyphStore {
 public:
  /// What open() found, and what the caller does about it:
  /// - Ready.
  /// - NotInstalled, OtherFamily: install the family.
  /// - MapFailed: the mapping budget shrank since the install (a firmware update); install again with a
  ///   fresh plan, or render without the store for this session.
  /// - NoMemory: render without the store for now; never a reason to install again.
  enum class OpenResult : uint8_t { Ready, NotInstalled, OtherFamily, MapFailed, NoMemory };
  /// Found: a stored glyph. NoGlyph: the font has no glyph for this key (markNoGlyph()). NotStored: render it.
  enum class Lookup : uint8_t { NotStored, NoGlyph, Found };
  /// What commit() and markNoGlyph() did:
  /// - Stored: everything is findable.
  /// - Refused: nothing was written. A key was not a glyph key or used a face index not claimed since the
  ///   last wipe, a record is larger than a segment's payload, or the store is not open. Wipe pending does not
  ///   change.
  /// - Full: no free segment for the batch, the index past its load limit, or a probe chain full (needsRoom()),
  ///   or writes were already stopped. Writes stop until room is made; nothing is wiped. Part of a batch may have
  ///   been stored (find() tells).
  /// - WriteFailed: a write failed. Part of a batch may have been stored. Wipe is now pending, and later writes are
  ///   refused with Full until wipe().
  enum class Commit : uint8_t { Stored, Refused, Full, WriteFailed };

  explicit FlashGlyphStore(NorFlash& flash) : flash_(flash) { resetRing(); }
  ~FlashGlyphStore() { close(); }
  FlashGlyphStore(const FlashGlyphStore&) = delete;
  FlashGlyphStore& operator=(const FlashGlyphStore&) = delete;

  // --- Install ---

  /// Erases the header first, so a power cut from here on reads as "not installed". Then erases every
  /// sector of the new layout that holds data, but for the style blocks in `keepStyles` (a bit per block), which
  /// the caller found holding their data already and does not write. A layout with SD-mode blocks needs `sdInfo`,
  /// a StyleInfo per block: each SD-mode style's file stamp, size and outlines (in the file, and past the block);
  /// offsets, sizes and modes come from `layout`.
  bool beginInstall(const glyphstore::Layout& layout, const uint8_t identity[glyphstore::kIdentityBytes],
                    const glyphstore::StyleInfo* sdInfo, uint8_t keepStyles = 0);
  /// Copies style data into a style block, each byte once. Only between beginInstall() and finishInstall().
  bool writeStyleData(uint8_t style, uint32_t offset, const void* data, uint32_t len);
  /// Writes the header. open() succeeds from now on.
  bool finishInstall();

  // --- Session ---

  /// Maps the store (the index, the pairs and the log, and the SD-mode styles' blocks) and finds where writing
  /// resumes.
  OpenResult open(const uint8_t identity[glyphstore::kIdentityBytes]);
  void close();
  bool isOpen() const { return open_; }
  uint8_t styleCount() const { return geometry_.styleCount; }
  /// EpdFontData::bitmap for every TTF face: EpdGlyph::dataOffset is relative to it.
  const uint8_t* logBase() const { return log_; }
  /// A style's data, mapped. Flash-mode blocks map one at a time: the pointer stays valid until mapStyle() is
  /// called for another flash-mode style, so re-fetch it after a miss in another style. SD-mode blocks are mapped
  /// together by open(), and stay mapped.
  const uint8_t* mapStyle(uint8_t style);
  /// A style's header record (its mode; in SD mode its file and outlines). False when not open.
  bool styleInfo(uint8_t style, glyphstore::StyleInfo& out) const;
  /// Bumped by every wipe(). A face that holds a face index re-claims it with faceIndex() when this changed.
  uint32_t generation() const { return generation_; }

  // --- Glyphs ---

  Lookup find(uint32_t key, const EpdGlyph** glyph) const;
  /// Writes the staged records, one program call per segment they go to, then one slot per record, and clears the
  /// batch. A record never crosses a segment's end: one that does not fit the head moves the head on.
  Commit commit(GlyphBatch& batch);
  /// Records that the font has no glyph for this key: a slot with no record.
  Commit markNoGlyph(uint32_t key);

  // --- Use counts ---

  /// Counts one draw of `glyph`, a pointer find() handed out: adds one to its slot's count (at most 3) and to
  /// drawsCounted(). Does nothing while counts are not kept, or for anything that is not a live record of the log.
  /// Const because the font set draws through const paths; the counts are RAM, not store state.
  void noteDrawn(const EpdGlyph* glyph) const;
  /// Counts are kept from the commit that leaves the live segments at kCountsAtPercent of the log, or few free
  /// segments, or from markFull(), until close() or wipe(). Refused memory is logged once and not asked for again until
  /// open().
  bool countsKept() const { return useCounts_ != nullptr; }
  /// The count (0 to 3) of the slot find() answers with for `key`; 0 when counts are not kept or `key` is not stored.
  uint8_t useCount(uint32_t key) const;
  /// Draws counted since open(), saturated ones included.
  uint32_t drawsCounted() const { return drawsCounted_; }

  // --- Faces ---

  /// The face-table index for `family` (its index in the set) at a pixels-per-em (26.6), claiming a free entry (a
  /// flash write) on first use. Every family's faces share the table. False when all entries are taken, on a write
  /// error, or for a new face while wipe is pending. Taking the last entry makes the wipe pending but stops nothing
  /// else: the claimed faces go on storing their glyphs until the wipe. A wipe empties the table: indices claimed
  /// before it are void, and commits under them are refused until they are claimed again.
  bool faceIndex(uint8_t family, uint32_t ppem26_6, uint8_t* index);

  // --- Kerning pairs ---

  /// A pair's kerning in font units, for one style of one family of the set.
  bool findPair(uint16_t left, uint16_t right, uint8_t family, uint8_t style, int16_t* fontUnits) const;
  /// Stores a pair's kerning in font units. False when it was not stored: the pair index is full (pairs then
  /// come from the GPOS reader every time; never a reason to wipe), writes are stopped, or a write failed (which
  /// stops them).
  bool storePair(uint16_t left, uint16_t right, uint8_t family, uint8_t style, int16_t fontUnits);

  // --- Making room, and the wipe ---

  /// How long makeRoom() may take: Idle, one step (a reader's idle tick); Open, until there is enough room (a chapter
  /// or book open, behind the busy indicator).
  enum class Budget : uint8_t { Idle, Open };
  /// What makeRoom() left:
  /// - Enough: needsRoom(budget) is false, and writes go on.
  /// - More: room is still to be made; writes go on into what is free, unless the index stops them.
  /// - Failed: a write failed (wipe now pending), or the store is not open or already has a wipe pending: only wipe()
  ///   or open() goes on from here.
  enum class Room : uint8_t { Enough, More, Failed };
  /// What one makeRoom() call did: segments reclaimed, the glyphs they copied forward and the ones they dropped, and
  /// whether the index was rebuilt.
  struct RoomWork {
    uint16_t reclaimed = 0;
    uint16_t kept = 0;
    uint16_t dropped = 0;
    bool rebuilt = false;
  };
  /// Makes room a step at a time: Idle takes one step, Open steps until enough (at most segmentCount() reclaims and one
  /// rebuild). A step is one of:
  /// - A reclaim of the oldest segment, while two or more are live: its glyphs whose use count is above 0 are copied
  ///   to the head, at most half a segment's payload of them (the most used first, then the newest), and only when
  ///   they fit the head and the free segments; the rest are dropped, and the segment is erased. A copy's count is the
  ///   old one halved, so a glyph not drawn again is dropped within three passes.
  /// - A rebuild of the index from the log: the index is erased, and each live record a slot named gets a slot again,
  ///   with its count; the "no glyph" marks are forgotten. When the index stops writes the rebuild comes first, in
  ///   idle time too; but with under kRebuildStalePercent of the slots stale and two or more segments live, a rebuild
  ///   would soon fill again, so a reclaim comes first and leaves its slots stale. The other reasons for a rebuild
  ///   wait for Open, and come after its reclaims. With the rebuild journal full, the store is wiped instead
  ///   (generation() moves on).
  /// Writes stopped for room resume, unless the index stops them. Fills `work` with what this call did. Call it only
  /// where no record of the log is in use: it invalidates every pointer into the log.
  Room makeRoom(Budget budget, RoomWork* work = nullptr);
  /// Room is to be made by makeRoom(budget): writes stopped for room (a batch that did not fit, or markFull()), fewer
  /// than kFreeSegmentsTarget segments free, or the index stops writes (past its load limit, or a probe chain full).
  /// With Open, also a rebuild that waits for one: kRebuildEpochSpan epochs since the last rebuild began, the last one
  /// cut short by a power cut, or kRebuildStalePercent of the index's slots stale.
  bool needsRoom(Budget budget = Budget::Idle) const {
    return open_ && (roomAsked_ || freeSegments() < glyphstore::kFreeSegmentsTarget || indexFull_ || chainFull_ ||
                     (budget == Budget::Open && rebuildWaitsForOpen()));
  }
  /// The epoch the last rebuild began from, the tail's then: no slot written since names an older one. 0 with no
  /// rebuild since the last wipe.
  uint32_t rebuildBase() const { return journal_.base; }
  /// Rebuilds since open().
  uint32_t rebuilds() const { return rebuilds_; }
  /// Acts as if the store had just filled up: writes stop, the use counts are kept from now on if they are not yet,
  /// and the next makeRoom() reclaims the oldest segment however many are free, when two or more are live; either way
  /// writes resume after it. The serial test API's CMD:TTF:FULL. False when the store is not open.
  bool markFull();
  /// Nothing is written until room is made or the next wipe: the ring or the index filled, or markFull() (needsRoom()),
  /// a write failed, or the store opened with a wipe pending. A full face table alone does not stop writes.
  bool writesStopped() const { return writesStopped_; }
  bool wipePending() const { return wipePending_; }
  /// Records wipe pending in flash, then erases the index, the pair index and the log (only sectors that hold
  /// data) and last the state sector (face table, wipe word, rebuild journal). A cut anywhere leaves wipe pending,
  /// and the next wipe() finishes the job. Call it only where no pointer into the log and no face index is in use:
  /// at book open, or before a section build. False: not done; it stays pending.
  bool wipe();

  // --- Stats ---

  /// Bytes the live segments hold, their headers included: whole segments behind the head, and the head up to where
  /// writing resumes.
  uint32_t logUsed() const { return live_ == 0 ? 0 : (live_ - 1u) * geometry_.segmentBytes + headUsed_; }
  /// Index slots in use, stale ones included.
  uint32_t usedSlots() const { return usedSlots_; }
  /// Index slots that name a live record. The rest of usedSlots() is what a rebuild frees: slots gone stale, and the
  /// "no glyph" marks it forgets.
  uint32_t liveSlots() const { return liveSlots_; }
  uint32_t usedPairSlots() const { return usedPairs_; }
  uint32_t segmentCount() const { return geometry_.segmentBytes == 0 ? 0 : geometry_.logSize / geometry_.segmentBytes; }
  uint32_t freeSegments() const { return segmentCount() - live_; }
  /// The epoch the next segment the head moves into gets.
  uint32_t epoch() const { return nextEpoch_; }
  const glyphstore::Geometry& geometry() const { return geometry_; }

 private:
  enum class Insert : uint8_t { Done, ChainFull, WriteFailed };
  enum class Step : uint8_t { None, Reclaim, Rebuild };
  // What a rebuild came to: done, the store wiped instead (a full journal), no memory for its plan (nothing written;
  // it waits), or a failed write (wipe pending).
  enum class Rebuilt : uint8_t { Done, Wiped, NoMemory, Failed };
  // The journal of index rebuilds in the state sector, as open() read it and the rebuilds since wrote it.
  struct Journal {
    uint32_t used = 0;  // entries in use
    uint32_t base = 0;  // the last entry's
    bool done = true;   // the last entry's `done` is programmed, or there is none
  };

  // The EpdGlyph of the record at `offset` in the log, read in place (4-byte aligned in mapped flash).
  const EpdGlyph& recordGlyph(const uint32_t offset) const {
    return *reinterpret_cast<const EpdGlyph*>(__builtin_assume_aligned(log_ + offset + 4, 4));
  }
  // The one rule for where records go: a record of `bytes` fits a segment with `used` bytes in use, its header
  // included, when it ends at or before the segment's end. One that does not goes to the next segment.
  bool fitsSegment(const uint32_t used, const uint32_t bytes) const { return bytes <= geometry_.segmentBytes - used; }
  // Lays a record of `bytes` after `used` bytes of a segment: in it when it fits, else past the header of the next
  // one, counted in `opens`.
  void placeRecord(uint32_t bytes, uint32_t& used, uint32_t& opens) const;
  bool plausibleRecord(uint32_t offset, uint32_t end) const;
  const EpdGlyph* validRecord(uint32_t key, uint32_t value) const;
  template <typename Visit>
  uint32_t scanLog(uint32_t segment, Visit&& visit) const;
  template <typename Visit>
  void walkLive(Visit&& visit) const;
  void findRing();
  bool moveHead();
  void resetRing();
  Step nextStep(Budget budget, bool rebuilt) const;
  bool staleCrowds() const;
  bool rebuildWaitsForOpen() const;
  bool reclaimTail(RoomWork& work);
  bool copyRecord(uint32_t from, uint32_t key, uint32_t bytes, uint8_t count, RoomWork& work);
  Rebuilt rebuildIndex(RoomWork& work);
  void readJournal();
  Insert insert(uint32_t tableOffset, const uint8_t* table, uint32_t slots, uint32_t key, uint32_t value,
                uint32_t* at = nullptr);
  Commit insertGlyphSlot(uint32_t key, uint32_t value, uint32_t* at = nullptr);
  Commit writeBatch(GlyphBatch& batch);
  bool faceClaimed(uint32_t key) const { return (claimedFaces_ >> glyphstore::keyFaceIndex(key)) & 1u; }
  uint32_t findSlot(uint32_t key, uint32_t value) const;
  uint8_t countAt(uint32_t slot) const;
  void setCount(uint32_t slot, uint8_t count);
  void keepCountsIfNeeded();
  void keepCounts();
  void dropCounts();
  bool readFaceEntry(uint8_t i, glyphstore::FaceEntry& entry) const;
  bool eraseWritten(uint32_t offset, uint32_t len);
  bool isBlank(uint32_t sector) const;
  void stopWrites();
  void stopForRoom();
  bool recordWipePending();

  NorFlash& flash_;
  glyphstore::Geometry geometry_{};
  uint32_t styleOffset_[glyphstore::kMaxStyles] = {};
  uint32_t styleSize_[glyphstore::kMaxStyles] = {};
  const uint8_t* region_ = nullptr;  // one mapping: index, pair index, log
  const uint8_t* index_ = nullptr;
  const uint8_t* pairs_ = nullptr;
  const uint8_t* log_ = nullptr;
  const uint8_t* styleMap_ = nullptr;  // the flash-mode style mapped now
  const uint8_t* sdMap_ = nullptr;     // every SD-mode style's block
  uint8_t mappedStyle_ = 0xFF;
  uint8_t firstSd_ = 0;  // the first SD-mode block; styleCount() when none
  // The ring. A live segment's epoch is the one in its header; a free one's is kErased. The live segments run from
  // the tail to the head in ring order, their epochs growing; with none live, head_ is the segment the first record
  // opens.
  uint32_t segEpoch_[glyphstore::kMaxSegments];
  uint8_t head_ = 0;
  uint8_t tail_ = 0;
  uint8_t live_ = 0;
  uint32_t headUsed_ = 0;  // bytes used in the head, its header included
  uint32_t nextEpoch_ = 1;
  uint32_t liveSlots_ = 0;
  bool chainFull_ = false;  // a probe chain filled: writes stop until the index is rebuilt
  bool indexFull_ = false;  // the index passed kIndexLoadPercent: the same
  bool roomAsked_ = false;  // writes stopped for room: the next makeRoom() reclaims, however many segments are free
  Journal journal_;
  uint32_t rebuilds_ = 0;  // since open()
  uint32_t usedSlots_ = 0;
  uint32_t usedPairs_ = 0;
  uint32_t claimedFaces_ = 0;  // bit i: face entry i is claimed
  uint32_t generation_ = 0;
  bool open_ = false;
  bool wipePending_ = false;
  bool writesStopped_ = false;
  // Two bits an index slot, from the commit that nears a full ring or from markFull(). The pointee is changed from
  // const noteDrawn().
  std::unique_ptr<uint8_t[]> useCounts_;
  bool countsTried_ = false;  // memory was asked for this session
  mutable uint32_t drawsCounted_ = 0;
  std::unique_ptr<glyphstore::Header> installing_;  // written by finishInstall()
};
