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
///   segment that is no longer live, or was opened again since, is never served.
/// - Erases happen only at install, wipe and when the head moves into a segment, and only on sectors that hold data.
/// - A lookup reads mapped flash in place: a hit is a pointer to the EpdGlyph inside the log.
///
/// Lifetimes: every pointer the store hands out (find()'s EpdGlyph, logBase(), mapStyle()) stays valid until
/// close(), open(), beginInstall() or wipe(). The NorFlash must outlive the store.
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

  // --- A full store, and the wipe ---

  bool wipePending() const { return wipePending_; }
  /// Stops writes and records wipe pending, whatever the store holds. The serial test API's CMD:TTF:FULL. False when
  /// the store is not open or the wipe word could not be written.
  bool markFull();
  /// Nothing is written until room is made or the next wipe: the ring or the index filled (needsRoom()), a write
  /// failed, or the store opened with a wipe pending. A full face table alone does not stop writes.
  bool writesStopped() const { return writesStopped_; }
  /// Room is to be made: fewer than kFreeSegmentsTarget segments are free, or the index needs a rebuild (past its
  /// load limit, or a probe chain full).
  bool needsRoom() const {
    return open_ && (freeSegments() < glyphstore::kFreeSegmentsTarget || indexFull_ || chainFull_);
  }
  /// Records wipe pending in flash, then erases the index, the pair index and the log (only sectors that hold
  /// data) and last the state sector (face table and wipe word). A cut anywhere leaves wipe pending, and the
  /// next wipe() finishes the job. Call it only where no pointer into the log and no face index is in use:
  /// at book open, or before a section build. False: not done; it stays pending.
  bool wipe();

  // --- Stats ---

  /// Bytes the live segments hold, their headers included: whole segments behind the head, and the head up to where
  /// writing resumes.
  uint32_t logUsed() const { return live_ == 0 ? 0 : (live_ - 1u) * geometry_.segmentBytes + headUsed_; }
  /// Index slots in use, stale ones included.
  uint32_t usedSlots() const { return usedSlots_; }
  /// Index slots that name a live record, or mark a glyph the font lacks.
  uint32_t liveSlots() const { return liveSlots_; }
  uint32_t usedPairSlots() const { return usedPairs_; }
  uint32_t segmentCount() const { return geometry_.segmentBytes == 0 ? 0 : geometry_.logSize / geometry_.segmentBytes; }
  uint32_t freeSegments() const { return segmentCount() - live_; }
  /// The epoch the next segment the head moves into gets.
  uint32_t epoch() const { return nextEpoch_; }
  const glyphstore::Geometry& geometry() const { return geometry_; }

 private:
  enum class Insert : uint8_t { Done, ChainFull, WriteFailed };

  // The EpdGlyph of the record at `offset` in the log, read in place (4-byte aligned in mapped flash).
  const EpdGlyph& recordGlyph(const uint32_t offset) const {
    return *reinterpret_cast<const EpdGlyph*>(__builtin_assume_aligned(log_ + offset + 4, 4));
  }
  bool plausibleRecord(uint32_t offset, uint32_t end) const;
  const EpdGlyph* validRecord(uint32_t key, uint32_t value) const;
  uint32_t scanLog(uint32_t segment, uint32_t* records) const;
  void findRing();
  bool moveHead();
  void resetRing();
  Insert insert(uint32_t tableOffset, const uint8_t* table, uint32_t slots, uint32_t key, uint32_t value);
  Commit insertGlyphSlot(uint32_t key, uint32_t value);
  Commit writeBatch(GlyphBatch& batch);
  bool faceClaimed(uint32_t key) const { return (claimedFaces_ >> glyphstore::keyFaceIndex(key)) & 1u; }
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
  bool chainFull_ = false;  // a probe chain filled: the index needs a rebuild
  bool indexFull_ = false;  // the index passed kIndexLoadPercent: the same
  uint32_t usedSlots_ = 0;
  uint32_t usedPairs_ = 0;
  uint32_t claimedFaces_ = 0;  // bit i: face entry i is claimed
  uint32_t generation_ = 0;
  bool open_ = false;
  bool wipePending_ = false;
  bool writesStopped_ = false;
  std::unique_ptr<glyphstore::Header> installing_;  // written by finishInstall()
};
