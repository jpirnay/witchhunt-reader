#pragma once
// BlockStreamReader — the STREAMING Stage-2 source (plan v2). Reads a v5 content.bin one LOGICAL
// block at a time, seeking per spine, without ever holding a whole spine (let alone the book) in
// RAM. This is the counterpart to ContentBinWriter and the piece that lets Stage-2 lay out and
// stream pages to disk while resident RAM stays ~one block. See
// docs/stage1-revised-plan-v2-2026-07-26.md.
//
// v6 (Increment E): only the small, fixed spine-offset index is loaded up front (it sits right after
// the header). Each spine is SELF-CONTAINED — its aux region carries the spine's own style table and
// chapter entries alongside its anchors/labels — so openSpine() loads them per spine and nothing is
// book-global. A spine whose index slot is 0 is NOT YET AVAILABLE (still being written / never
// compiled): spineAvailable(i) reports this so a consumer can chase a producer's write frontier.
//
// Usage:
//   FsFile f; Storage.openFileForRead(...);   // host tests: HalFile::fromString(bytes)
//   BlockStreamReader r;
//   if (!r.open(f)) { stale/corrupt -> recompile }
//   if (r.fingerprint() != bookFingerprint) { stale -> recompile }
//   for spine i in [0, r.spineCount()):
//     if (!r.spineAvailable(i)) continue;   // frontier: skip spines not yet committed
//     r.openSpine(i);
//     Block b;
//     while (r.nextLogicalBlock(b)) { ...lay out b, stream page, drop b... }
//
// nextLogicalBlock reassembles 8 KB kContinuation splits into one logical block incrementally
// (bounded to one logical block), so the caller sees the same block stream the walk produced.

#include <HalStorage.h>  // FsFile

#include <cstdint>
#include <string>
#include <vector>

#include "CompiledContent.h"

namespace compiled {

class LayoutSink;  // fwd — replaySpine drives one; defined in LayoutSink.h

class BlockStreamReader {
 public:
  BlockStreamReader() = default;

  // Read + validate the header, then load the fixed spine-offset index (right after the header).
  // Returns false on a bad magic/version, a truncated file, or an out-of-range offset (caller treats
  // as stale → recompile). `file` is caller-owned and must outlive the reader. A partially-written
  // file (some index slots still 0) opens fine; use spineAvailable() per spine.
  bool open(FsFile& file);

  uint64_t fingerprint() const { return fingerprint_; }
  uint32_t spineCount() const { return static_cast<uint32_t>(spineOffsets_.size()); }
  // The CURRENT spine's local style table (v6), loaded by openSpine. Empty until openSpine succeeds.
  const std::vector<CssStyle>& spineStylePool() const { return spineStylePool_; }
  // True if spine i's index slot is committed (offset != 0) — i.e. the spine is fully written and
  // replayable. A producer commits the slot only after the whole spine (incl. its aux) is on disk.
  bool spineAvailable(uint32_t i) const { return i < spineOffsets_.size() && spineOffsets_[i] != 0; }

  // Re-read ONLY the fixed spine-offset index (cheap: no magic/version/fingerprint re-check) so a
  // consumer chasing a live producer's write frontier picks up spines committed since open(). Returns
  // false on I/O error. Header count is unchanged (fixed at begin), so the index size is unchanged.
  bool refreshIndex();

  // The CURRENT spine's chapter entries (v6: per-spine), loaded by openSpine. Empty until openSpine.
  const std::vector<Chapter>& spineChapters() const { return spineChapters_; }

  // Position at spine i's block stream. Reads the spine header (firstCharOffset + blockCount +
  // auxOffset) AND pre-loads the spine's SELF-CONTAINED aux region from auxOffset (anchors + labels +
  // style table + chapters), so spineAnchors()/spineLabels()/spineStylePool()/spineChapters() are
  // available BEFORE the block stream. Returns false on I/O error, i out of range, or i not yet
  // available (slot 0).
  bool openSpine(uint32_t i);
  uint32_t spineFirstCharOffset() const { return spineFirstCharOffset_; }
  // Total on-disk records in the current spine (valid after openSpine). Diagnostic / bounds check.
  uint32_t spineBlockCount() const { return spineBlockCount_; }

  // NOTE (v6): the book-level readChapters() is gone — chapters are per-spine (spineChapters()),
  // loaded by openSpine, so a self-contained spine replays without any book-global table.

  // The current spine's anchors/labels (loaded up front by openSpine). Their blockIndex is a RECORD
  // index; nextLogicalBlock() reports the first-record index of each logical block via
  // currentFirstRecordIndex() so callers can cross-reference.
  const std::vector<Anchor>& spineAnchors() const { return spineAnchors_; }
  const std::vector<PageBreakLabel>& spineLabels() const { return spineLabels_; }

  // v7: the current spine's baked per-LOGICAL-block offset table. Index i is the i-th block
  // nextLogicalBlock() returns. The table is NOT loaded resident (a whole-novel single spine has
  // thousands of entries — a std::vector<BlockOffset> resize aborted on the fragmented heap under
  // -fno-exceptions, the same class as the writer's chunked-index fix). openSpine records only its
  // file offset + count; blockOffsetAt() reads one 12-byte entry on demand. 0 for a v6 file (none
  // ship) or a spine with no blocks.
  uint32_t spineLogicalBlockCount() const { return spineBlockOffsetCount_; }
  // Read logical block i's baked offset entry on demand (one seek + 12-byte read). False if i is out
  // of range or on I/O error. Leaves the file cursor at the entry's end — callers that need the block
  // stream position must reseek (seekToBlock does).
  bool blockOffsetAt(uint32_t i, BlockOffset* out) const;

  // Reposition the block stream to LOGICAL block `blockIndex` of the current spine (0-based, as
  // reported by currentFirstRecordIndex()/the block-offset table). The next nextLogicalBlock() returns
  // that block. Requires an open spine and a valid index; returns false otherwise. O(1) — one seek to
  // the baked offset, no scan. Clears the continuation lookahead so the merge restarts cleanly.
  bool seekToBlock(uint32_t blockIndex);

  // Record index (0-based within the spine) of the FIRST record of the logical block most recently
  // returned by nextLogicalBlock(). Anchors/labels/chapters are keyed on this.
  uint32_t currentFirstRecordIndex() const { return currentFirstRecordIndex_; }

  // Read the next LOGICAL block of the current spine into `out` (merging any kContinuation records).
  // Returns true and fills `out` when a block was read; false at spine end or on error (check
  // ok()). The caller owns `out` and may move from it.
  bool nextLogicalBlock(Block& out);

  // Read the next RAW on-disk record (no kContinuation merge). Used by the whole-book reader, which
  // stores the split records as-is. Returns false at spine end. Do not mix with nextLogicalBlock.
  bool nextRawRecord(Block& out) { return readOneRecord(out); }

  // Read the current spine's anchor + label tables (positioned after the last block). Call ONLY
  // after nextLogicalBlock has returned false (spine fully consumed), which leaves the cursor at the
  // aux tables. Bounded (per-spine small).
  bool readSpineAux(std::vector<Anchor>& anchors, std::vector<PageBreakLabel>& labels);

  bool ok() const { return ok_; }

 private:
  bool readOneRecord(Block& rec);  // one on-disk record (base or continuation)

  FsFile* file_ = nullptr;
  bool ok_ = false;
  uint64_t fingerprint_ = 0;
  std::vector<uint32_t> spineOffsets_;  // fixed index right after the header; 0 slot = not available

  // Current spine cursor (v6: style pool + chapters are per-spine, loaded by openSpine).
  uint32_t spineBlockCount_ = 0;       // total on-disk records in the current spine
  uint32_t spineBlocksRemaining_ = 0;  // on-disk records left to read
  uint32_t spineFirstCharOffset_ = 0;
  uint32_t currentFirstRecordIndex_ = 0;  // first-record index of the last logical block returned
  std::vector<Anchor> spineAnchors_;
  std::vector<PageBreakLabel> spineLabels_;
  std::vector<CssStyle> spineStylePool_;  // this spine's local style table
  std::vector<Chapter> spineChapters_;    // this spine's chapter entries
  // v7 baked per-logical-block offset table: kept on disk, not resident (see blockOffsetAt). The
  // file offset is of the FIRST entry (just past the u32 count writeBlockOffsets wrote).
  uint32_t spineBlockOffsetsFileStart_ = 0;
  uint32_t spineBlockOffsetCount_ = 0;

  // One-record lookahead so nextLogicalBlock can peek whether the following record is a
  // continuation of the base it just read.
  bool haveLookahead_ = false;
  Block lookahead_;
};

// Replay spine `spineIndex` of an OPEN BlockStreamReader through `sink`, reproducing the walk's
// BlockSink call order (anchors/labels/footnotes/xpath before onBlock, chapters after) so the
// LayoutSink produces the same pages a live parse would. Streams one logical block at a time — no
// spine materialized. v6: chapters + styles are the spine's OWN (loaded by openSpine), so the caller
// passes nothing book-global. Returns false on any read error (check reader.ok()). Shared by the host
// harness (replayFromContentBin) and the device Section read-back build so they never drift.
//
// Run-to-completion; equivalent to replaySpineBegin(...) then replaySpineStep(..., 0) to Done.
bool replaySpine(BlockStreamReader& reader, uint32_t spineIndex, LayoutSink& sink);

// SLICED replay (Increment E consumer): the same per-spine replay, resumable across ticks so a huge
// spine never blocks the loop. Hold a SpineReplayCursor across calls; it caches the cross-block state
// (anchor/label lists + this spine's index) that openSpine loaded, so replaySpineStep only walks the
// block stream. The reader + sink must be the SAME instances (and stay alive) across calls.
struct SpineReplayCursor {
  uint32_t spineIndex = 0;
  bool started = false;   // openSpine done
  bool finished = false;  // onSpineEnd fired (terminal)
  bool ok = true;         // false on any read error
};

// Open `spineIndex` and prime `cur`. Returns false if the spine is not available / open failed.
bool replaySpineBegin(BlockStreamReader& reader, uint32_t spineIndex, SpineReplayCursor& cur);

// Replay at most `maxBlocks` logical blocks of the current spine through `sink` (maxBlocks == 0 =
// run the spine to completion in one call). Returns true while blocks remain (call again); false
// once the spine is fully replayed (onSpineEnd fired) OR on error — check cur.finished / cur.ok to
// distinguish. Fires the same onAnchor/onPageBreakLabel/onFootnote/onXPathAdvance/onBlock/onChapter
// order as replaySpine, so the produced pages are identical regardless of slice boundaries.
// Timing-agnostic (block-count budget); the caller wraps this in its own millis() budget loop so the
// layout layer stays free of a timing dependency.
bool replaySpineStep(BlockStreamReader& reader, LayoutSink& sink, uint32_t maxBlocks, SpineReplayCursor& cur);

}  // namespace compiled
