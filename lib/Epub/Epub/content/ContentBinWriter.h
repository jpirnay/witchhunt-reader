#pragma once
// ContentBinWriter — the STREAMING Stage-1 persistence sink (plan v2). Unlike the whole-book
// ContentSink (which accumulates the entire CompiledContent in RAM before writing — a multi-MB
// regression on a big-single-spine book), this appends each block to content.bin AS THE WALK
// EMITS IT and drops it. Peak added RAM ≈ one block + the (small) style pool + the spine-offset
// index. See docs/stage1-revised-plan-v2-2026-07-26.md.
//
// It is a BlockSink: attach it to the walk (Section::setStage1Sink) exactly like ContentSink, and
// the walk drives it block-by-block. The 8 KB split (appendTextSplit) still applies per block.
//
// v6 (Increment E): each spine is SELF-CONTAINED — its aux region holds the spine's OWN style table
// and chapter entries alongside its anchors/labels — and the spine-offset index is PRE-ALLOCATED at a
// fixed location right after the header (spineCount slots, all 0). onSpineEnd() commits the spine's
// start offset into its index slot, so a consumer can read + replay any committed spine (slot != 0)
// the instant it finishes, WHILE later spines are still being written. There is no book-global style
// pool / chapters section and no finish()-time header back-patch of section offsets.
//
// Usage (the caller owns the FsFile — opened read/write + seekable, so the index slots + spine
// headers can be back-patched):
//   FsFile f; Storage.openFileForWrite("SCT", path, f);   // device; host tests: HalFile::forReadWrite()
//   ContentBinWriter w;
//   w.begin(f, spineCount, fingerprint);     // header + zeroed spineCount-slot index
//   for each spine i in [0, spineCount):
//     w.beginSpine();                        // (also happens lazily on the first onBlock)
//     ... walk drives onBlock/onAnchor/onChapter/onFootnote/onPageBreakLabel/onXPathAdvance ...
//     w.onSpineEnd();                        // flush aux (anchors+labels+styles+chapters); commit slot i
//   w.finish();                              // flushes the file; no index/pool append. Caller closes.
//
// The spine cursor MUST advance monotonically (spine i then i+1 …) within one begin/finish so the
// index slots commit in order; a producer that reorders which spine to compile reopens/reseeks per
// spine (Increment E producer). No device shipped v5 → clean break, no migration.

#include <BufferedFileIO.h>  // serialization::BufferedFileWriter
#include <HalStorage.h>      // FsFile

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "BlockSink.h"
#include "CompiledContent.h"

struct FootnoteEntry;

namespace compiled {

class ContentBinWriter : public BlockSink {
 public:
  ContentBinWriter() = default;
  ~ContentBinWriter() override = default;

  // Begin writing to `file` (caller-owned, opened read/write + seekable) for a book of `spineCount`
  // spines. Writes a fresh v6 header + zeroed index (TRUNCATES any existing content — the file must
  // be opened for write/truncate). Returns false on I/O error. fingerprint is the source book's ZIP
  // content fingerprint.
  bool begin(FsFile& file, uint32_t spineCount, uint64_t fingerprint);

  // Open an EXISTING content.bin to APPEND (Increment F cross-session): validate its header
  // (magic/version/fingerprint/spineCount — a mismatch returns false so the caller truncate+begin()s
  // instead), load its committed slots (spineCommitted()), and position at EOF so new spine sections
  // append without disturbing already-committed spines + their data. `file` must be opened
  // read/write + seekable. Returns false on I/O error or a stale/foreign/corrupt file.
  bool openExisting(FsFile& file, uint32_t spineCount, uint64_t fingerprint);

  // True if spine `i`'s index slot is already committed (from a prior session via openExisting, or
  // committed this session). Lets the caller skip re-emitting a spine content.bin already covers.
  bool spineCommitted(uint32_t i) const { return i < committed_.size() && committed_[i]; }

  // BlockSink — driven by the walk. onBlock serializes the block immediately and drops it.
  void onBlock(Block&& block, const CssStyle& style) override;
  void onAnchor(const std::string& id) override;
  void onChapter(uint8_t level, const std::string& title) override;
  void onPageBreakLabel(const std::string& label) override;
  void onFootnote(int wordIndex, const FootnoteEntry& entry) override;
  void onXPathAdvance(uint16_t paragraphIndex, uint16_t listItemIndex, uint32_t bodyChildByteOffset) override;
  void onSpineEnd() override;

  // Open the NEXT spine in sequence (index = the internal cursor, post-incremented). Optional — the
  // first onBlock of a spine opens one lazily. Use this for the whole-book/host path that writes
  // spines 0..N in order.
  void beginSpine();

  // Open spine `spineIndex` explicitly (Increment E producer): the spine's records are APPENDED at
  // the current file position (physical order = compile order) but its start offset commits into
  // index slot `spineIndex` at onSpineEnd, so the producer may compile spines in ANY order (e.g.
  // read-position order) and the reader still seeks each by its logical index. `spineIndex` must be
  // < spineCount and not already committed. The internal sequential cursor is left untouched.
  void beginSpineAt(uint32_t spineIndex);

  // autoCommit (default true): onSpineEnd commits the spine's index slot immediately (whole-book
  // compile / producer — the spine is known clean). Set false for the reader's TEE build, where the
  // spine's data is written by onSpineEnd but the slot is published only by an explicit commitSpine()
  // AFTER the caller confirms the build was not CSS-degraded / truncated, so a bad parse never
  // publishes a bad content.bin spine.
  void setAutoCommit(bool on) { autoCommitSpines_ = on; }

  // Publish the spine written by the most recent onSpineEnd (commit its index slot + flush). No-op in
  // autoCommit mode (already committed) or if spineIndex != the last-written spine. Returns ok().
  bool commitSpine(uint32_t spineIndex);

  // Flush the file. v6 writes no book-level index/pool/chapters (they are per-spine), so this only
  // closes out any open spine and syncs. Returns false on any I/O error.
  bool finish();

  // Cooperative in-place READ of committed spines through the writer's own open handle. Flushes the
  // buffered append stream so the file cursor is the true append end, saves it, invokes `reader` with
  // the file (which may seek/read committed regions freely — e.g. open a BlockStreamReader on it), then
  // restores the append cursor + rebases the buffer so the next write lands correctly. The producer and
  // consumer run cooperatively (never concurrently), so no locking is needed; `reader` MUST NOT write.
  // This is how the reader renders a page from content.bin WHILE the producer still holds it open —
  // SdFat permits only one handle per file. Returns ok().
  bool withReadableFile(const std::function<void(FsFile&)>& reader);

  bool ok() const { return ok_; }

 private:
  bool flushBlock(Block&& block);  // apply 8 KB split, write each record, count blocks
  bool writeOneRecord(const Block& rec);
  void commitSpineOffset(uint32_t spineIndex, uint32_t offset);  // back-patch index slot spineIndex

  FsFile* file_ = nullptr;  // caller-owned
  // Buffered append sink over *file_. The per-block record stream and per-spine aux tables are
  // thousands of tiny writes (~4 writePod per word); unbuffered that dominated the compile
  // (~6 s / 4097 blocks on a 570 KB spine, device-measured). All appends go through bw_; the rare
  // seek back-patches (spine header at onSpineEnd, index slot at commitSpineOffset) flush bw_, seek
  // *file_ directly, then bw_.rebase(). bw_.position() is the flush-aware append offset onBlock
  // records into the block-offset table. Reset (rebound to *file_) in begin()/openExisting().
  std::optional<serialization::BufferedFileWriter> bw_;
  bool ok_ = false;
  uint32_t spineCount_ = 0;
  uint64_t fingerprint_ = 0;
  uint32_t nextSpineIndex_ = 0;   // which index slot the NEXT beginSpine() commits (monotonic)
  bool autoCommitSpines_ = true;  // see setAutoCommit
  // Last spine whose DATA onSpineEnd wrote (for a deferred commitSpine in non-autoCommit mode).
  bool lastSpineDataWritten_ = false;
  uint32_t lastSpineIndex_ = 0;
  uint32_t lastSpineDataOffset_ = 0;
  // Committed slots (populated by openExisting + each commitSpineOffset). Empty after a fresh begin()
  // — nothing committed yet — so spineCommitted() is false for all, which is correct. See openExisting.
  std::vector<bool> committed_;

  // Current spine state (all small; RESET at each spine — never a whole spine of blocks). v6: the
  // style pool + chapters are PER-SPINE (written into the spine's aux region at onSpineEnd), so a
  // committed spine is self-describing.
  bool spineOpen_ = false;
  bool spineHasBlock_ = false;
  uint32_t spineIndexBeingWritten_ = 0;  // index slot this open spine will commit at onSpineEnd
  uint32_t spineStartOffset_ = 0;        // where this spine's [firstCharOffset|blockCount|auxOffset] sits
  uint32_t spineFirstCharOffset_ = 0;
  uint32_t blockCount_ = 0;                      // running count; back-patched into the spine header at onSpineEnd
  std::vector<CssStyle> spineStyles_;            // this spine's local, deduped style pool
  std::vector<Anchor> anchors_;                  // this spine only
  std::vector<PageBreakLabel> pageBreakLabels_;  // this spine only
  std::vector<Chapter> spineChapters_;           // this spine's chapter entries only

  // v7: one entry per LOGICAL block (captured at the start of each onBlock, before its records are
  // written), baked into the aux region at onSpineEnd for O(1) seekToBlock in the reader.
  //
  // MEMORY (R1): the table grows one 12 B entry PER BLOCK, so a whole-novel single spine (Small Gods,
  // ~4097 blocks) accumulated ~48 KB held resident until onSpineEnd — device OOM at the reader's tight
  // heap (a chunked list only removed the contiguous-realloc abort, NOT the O(blocks) total). So when
  // a temp path is set (setBlockOffsetTempPath, the device compile) each entry STREAMS to a sidecar
  // file (O(1) resident, FreeInk's fixed-working-set principle) and is spliced into the aux region at
  // onSpineEnd through a bounded copy buffer. When no temp path is set (host tests, tiny spines) a
  // small in-RAM vector is the fallback. On-disk format is unchanged, so the reader is untouched.
  std::string blockOffsetTmpPath_;  // set via setBlockOffsetTempPath; empty → in-RAM fallback
  FsFile blockOffsetTmp_;           // open on first append when a temp path is set
  std::optional<serialization::BufferedFileWriter> blockOffsetTmpWriter_;  // batches the 12 B appends
  std::vector<BlockOffset> blockOffsetMem_;  // in-RAM fallback (temp path unset OR sidecar-open failed)
  uint32_t blockOffsetCount_ = 0;
  // Per-spine streaming state (reset in clearBlockOffsets). streaming_ = the sidecar is open and in use
  // for THIS spine; streamFailed_ = the sidecar open failed this spine so we degraded to the RAM vector
  // (don't keep retrying the open mid-spine). Both drive spliceBlockOffsets' path choice — NOT the mere
  // presence of blockOffsetTmpPath_ (which stays set across spines).
  bool blockOffsetStreaming_ = false;
  bool blockOffsetStreamFailed_ = false;
  // Append one entry (temp file when a path is set, else in-RAM vector). False on I/O/OOM → ok_=false.
  bool appendBlockOffset(const BlockOffset& bo);
  // Write count + every entry to `out` (the aux region): from the temp file (bounded copy) or the
  // in-RAM vector. Mirrors writeBlockOffsets' on-disk format.
  bool spliceBlockOffsets(serialization::BufferedFileWriter& out);
  // Reset the table (truncate the temp file / clear the vector + zero the count). Called at beginSpine.
  void clearBlockOffsets();

 public:
  // Set the sidecar path the per-block offset table streams to during compile (e.g.
  // <cachePath>/blockoff.tmp). When set, the table never accumulates in RAM (bounded working set for
  // arbitrarily large spines). Optional — unset keeps the small in-RAM fallback (host tests).
  void setBlockOffsetTempPath(const std::string& path) { blockOffsetTmpPath_ = path; }

 private:
  // Footnotes/xpath arrive DURING a block's build (before its onBlock). Buffered, attached at onBlock.
  std::vector<FootnoteRef> pendingFootnotes_;
  bool pendingXPath_ = false;
  XPathCounters pendingXPathCounters_;
};

}  // namespace compiled
