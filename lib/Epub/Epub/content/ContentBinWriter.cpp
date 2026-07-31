#include "ContentBinWriter.h"

#include <Logging.h>

#include <algorithm>
#include <utility>

#include "BlockSerialization.h"
#include "Epub/FootnoteEntry.h"
#include "Serialization.h"

namespace compiled {
namespace {

using serialization::readPod;
using serialization::writePod;

// Write the v6 fixed header: magic + version + fingerprint + spineCount. The spine-offset index
// (spineCount × u32) follows immediately and is written separately by begin().
void writeHeader(FsFile& f, uint64_t fingerprint, uint32_t spineCount) {
  f.write(reinterpret_cast<const uint8_t*>(kMagic), 4);
  writePod(f, kVersion);
  writePod(f, fingerprint);
  writePod(f, spineCount);
}

}  // namespace

bool ContentBinWriter::begin(FsFile& file, uint32_t spineCount, uint64_t fingerprint) {
  file_ = &file;
  ok_ = static_cast<bool>(file);
  spineCount_ = spineCount;
  fingerprint_ = fingerprint;
  nextSpineIndex_ = 0;
  spineStyles_.clear();
  spineChapters_.clear();
  spineOpen_ = false;
  spineHasBlock_ = false;
  blockCount_ = 0;
  anchors_.clear();
  pageBreakLabels_.clear();
  pendingFootnotes_.clear();
  pendingXPath_ = false;
  if (!ok_) return false;
  // Header, then a pre-allocated spine-offset index of spineCount zeroed slots. onSpineEnd() commits
  // each slot as its spine finishes; a 0 slot = "spine not yet available". File cursor lands right
  // after the index, where the first spine's records begin.
  writeHeader(*file_, fingerprint_, spineCount_);
  for (uint32_t i = 0; i < spineCount_; ++i) writePod(*file_, static_cast<uint32_t>(0));
  ok_ = static_cast<bool>(*file_);
  // Buffer the append stream from here (EOF, past the header + index). Back-patches into the header
  // region flush + rebase around their raw seeks.
  bw_.emplace(*file_);
  return ok_;
}

bool ContentBinWriter::openExisting(FsFile& file, uint32_t spineCount, uint64_t fingerprint) {
  file_ = &file;
  ok_ = false;
  spineCount_ = spineCount;
  fingerprint_ = fingerprint;
  nextSpineIndex_ = 0;
  committed_.assign(spineCount, false);
  spineStyles_.clear();
  spineChapters_.clear();
  spineOpen_ = false;
  spineHasBlock_ = false;
  blockCount_ = 0;
  anchors_.clear();
  pageBreakLabels_.clear();
  pendingFootnotes_.clear();
  pendingXPath_ = false;
  lastSpineDataWritten_ = false;
  if (!file) return false;

  // Validate the existing file's header (magic/version/fingerprint/spineCount) — a mismatch means it
  // is stale/foreign, so the caller should truncate-and-begin() instead.
  if (!file.seekSet(0)) return false;
  char magic[4] = {};
  if (file.read(magic, 4) != 4) return false;
  for (int i = 0; i < 4; ++i)
    if (magic[i] != kMagic[i]) return false;
  uint8_t version = 0;
  readPod(file, version);
  if (version != kVersion) return false;
  uint64_t fp = 0;
  readPod(file, fp);
  uint32_t sc = 0;
  readPod(file, sc);
  if (fp != fingerprint || sc != spineCount) return false;

  // Load the committed slots so beginSpineAt on an already-done spine is a no-op and spineCommitted()
  // answers the caller's "is this spine already covered?" query.
  const uint32_t fileSize = static_cast<uint32_t>(file.fileSize());
  if (fileSize < kHeaderSize + spineCount * sizeof(uint32_t)) return false;  // index truncated
  if (!file.seekSet(kHeaderSize)) return false;
  for (uint32_t i = 0; i < spineCount; ++i) {
    uint32_t off = 0;
    readPod(file, off);
    if (off > fileSize) return false;  // committed offset past EOF → corrupt; caller truncates
    if (off != 0) committed_[i] = true;
  }
  // Append new spine sections at EOF; committed slots + their data are left intact.
  if (!file.seekSet(fileSize)) return false;
  ok_ = static_cast<bool>(file);
  bw_.emplace(file);  // buffer appends from EOF (see begin())
  return ok_;
}

void ContentBinWriter::commitSpineOffset(uint32_t spineIndex, uint32_t offset) {
  if (!ok_ || !file_ || spineIndex >= spineCount_) return;
  // Drain the buffered append stream so the file cursor is the true append point, patch the index
  // slot directly, then restore the cursor + rebase the buffer to keep appending.
  if (bw_ && !bw_->flush()) {
    ok_ = false;
    return;
  }
  const uint32_t here = static_cast<uint32_t>(file_->position());
  if (!file_->seekSet(kHeaderSize + spineIndex * sizeof(uint32_t))) {
    ok_ = false;
    return;
  }
  writePod(*file_, offset);
  file_->seekSet(here);  // resume appending
  if (bw_) bw_->rebase();
  ok_ = static_cast<bool>(*file_);
  if (ok_ && spineIndex < committed_.size()) committed_[spineIndex] = true;
}

bool ContentBinWriter::withReadableFile(const std::function<void(FsFile&)>& reader) {
  // Cooperative in-place read: the consumer reads committed spines THROUGH the writer's own open handle
  // (SdFat allows only ONE handle per file, and the producer holds it open the whole compile). Reader
  // and producer never run concurrently (one stepped loop), so the only hazard is the shared file
  // CURSOR + the buffered writer's logical append point. Mirror the back-patch discipline: flush bw_ so
  // the file cursor is the true append end, save it, let the reader seek/read freely, then restore the
  // cursor + rebase bw_ so the next append lands correctly. `reader` must NOT write. Returns ok().
  if (!ok_ || !file_) return false;
  if (bw_ && !bw_->flush()) {
    ok_ = false;
    return false;
  }
  const uint32_t here = static_cast<uint32_t>(file_->position());
  reader(*file_);
  file_->seekSet(here);  // restore the append cursor
  if (bw_) bw_->rebase();
  ok_ = static_cast<bool>(*file_);
  return ok_;
}

void ContentBinWriter::beginSpine() { beginSpineAt(nextSpineIndex_++); }

void ContentBinWriter::beginSpineAt(uint32_t spineIndex) {
  if (!ok_ || !file_ || !bw_) return;
  spineIndexBeingWritten_ = spineIndex;
  // Append offset = the buffered writer's flush-aware position (bytes not yet flushed count).
  spineStartOffset_ = bw_->position();
  spineFirstCharOffset_ = 0;
  blockCount_ = 0;
  spineStyles_.clear();
  anchors_.clear();
  pageBreakLabels_.clear();
  spineChapters_.clear();
  clearBlockOffsets();
  spineOpen_ = true;
  spineHasBlock_ = false;
  // Placeholder spine header: firstCharOffset + blockCount + auxOffset, all patched at onSpineEnd
  // once known. auxOffset lets the reader seek straight to the (small) per-spine aux region
  // (anchors + labels + style table + chapters) and load it BEFORE streaming the blocks, so
  // onAnchor/onPageBreakLabel can fire ahead of the block they introduce and styleId resolves
  // against this spine's own pool. Reserve the slots now so blocks follow.
  bw_->writePod(static_cast<uint32_t>(0));  // firstCharOffset
  bw_->writePod(static_cast<uint32_t>(0));  // blockCount
  bw_->writePod(static_cast<uint32_t>(0));  // auxOffset (of the per-spine aux region)
  ok_ = static_cast<bool>(*bw_);
}

bool ContentBinWriter::appendBlockOffset(const BlockOffset& bo) {
  // Temp-file path (device compile): stream the 12 B entry to disk so the table is O(1) resident
  // regardless of block count. Open the temp file (truncating) on the first append of the spine.
  // If the temp open FAILS (e.g. an SD handle is momentarily unavailable in the reader context),
  // DON'T fail the whole spine — fall back to the in-RAM vector for this spine (blockOffsetStreaming_
  // stays false so spliceBlockOffsets writes from RAM). Every spine's table is small except a giant
  // single spine; the RAM fallback only risks the O(blocks) footprint in that rare case, and only when
  // the sidecar couldn't open — far better than aborting the spine outright (the King's Avatar
  // 1732-tiny-spine "OOM growing block-offset index (block 0)" that was really a temp-open failure).
  if (!blockOffsetTmpPath_.empty() && !blockOffsetStreamFailed_) {
    if (!blockOffsetStreaming_) {
      if (Storage.openFileForWrite("CBW", blockOffsetTmpPath_, blockOffsetTmp_)) {
        blockOffsetTmpWriter_.emplace(blockOffsetTmp_);
        blockOffsetStreaming_ = true;
      } else {
        // Couldn't open the sidecar this spine — degrade to RAM for it (logged once).
        LOG_INF("CBW", "block-offset sidecar open failed; using in-RAM table this spine");
        blockOffsetStreamFailed_ = true;
      }
    }
    if (blockOffsetStreaming_) {
      blockOffsetTmpWriter_->writePod(bo.fileOffset);
      blockOffsetTmpWriter_->writePod(bo.charOffset);
      blockOffsetTmpWriter_->writePod(bo.recordIndex);
      if (!static_cast<bool>(*blockOffsetTmpWriter_)) return false;  // a mid-stream WRITE error is real
      ++blockOffsetCount_;
      return true;
    }
  }
  // In-RAM path (no temp path — host tests / small spines — or a sidecar-open fallback this spine).
  blockOffsetMem_.push_back(bo);
  ++blockOffsetCount_;
  return true;
}

bool ContentBinWriter::spliceBlockOffsets(serialization::BufferedFileWriter& out) {
  // Same on-disk format as writeBlockOffsets(): u32 count, then count × {fileOffset,charOffset,
  // recordIndex}. From the temp file (bounded copy) when streaming, else the in-RAM vector.
  out.writePod(blockOffsetCount_);
  if (blockOffsetStreaming_) {
    if (blockOffsetCount_ == 0) return static_cast<bool>(out);  // nothing streamed
    if (!blockOffsetTmpWriter_) return false;                   // count>0 but no writer → inconsistent
    if (!blockOffsetTmpWriter_->flush()) return false;          // land all buffered entries on SD
    // Close the write handle before reopening for read: SdFat does not like the same path open for
    // write and read at once. (clearBlockOffsets at the next beginSpine also drops these; safe.)
    blockOffsetTmpWriter_.reset();
    if (blockOffsetTmp_) blockOffsetTmp_.close();
    // Copy the temp file's bytes into the aux region through a bounded stack buffer (fixed working
    // set — never the whole table in RAM). Open the temp file for read at offset 0.
    FsFile in;
    if (!Storage.openFileForRead("CBW", blockOffsetTmpPath_, in)) return false;
    const uint32_t total = blockOffsetCount_ * 3u * static_cast<uint32_t>(sizeof(uint32_t));
    uint8_t buf[512];
    uint32_t copied = 0;
    while (copied < total) {
      const uint32_t want = std::min<uint32_t>(sizeof(buf), total - copied);
      const int got = in.read(buf, want);
      if (got <= 0) {
        in.close();
        return false;
      }
      if (!out.write(buf, static_cast<size_t>(got))) {
        in.close();
        return false;
      }
      copied += static_cast<uint32_t>(got);
    }
    in.close();
    return static_cast<bool>(out);
  }
  for (const BlockOffset& bo : blockOffsetMem_) {
    out.writePod(bo.fileOffset);
    out.writePod(bo.charOffset);
    out.writePod(bo.recordIndex);
  }
  return static_cast<bool>(out);
}

void ContentBinWriter::clearBlockOffsets() {
  // Streaming path: drop the writer + close the handle (appendBlockOffset re-opens truncating for the
  // next spine). In-RAM path: clear + release. Reset the per-spine streaming flags so the NEXT spine
  // re-attempts the sidecar (a transient open failure on one spine must not disable streaming for the
  // rest of the book).
  blockOffsetTmpWriter_.reset();
  if (blockOffsetTmp_) blockOffsetTmp_.close();
  blockOffsetMem_.clear();
  blockOffsetMem_.shrink_to_fit();
  blockOffsetCount_ = 0;
  blockOffsetStreaming_ = false;
  blockOffsetStreamFailed_ = false;
}

bool ContentBinWriter::writeOneRecord(const Block& rec) {
  if (!ok_ || !bw_) return false;
  if (!writeBlock(*bw_, rec)) {
    ok_ = false;
    return false;
  }
  ++blockCount_;
  return true;
}

bool ContentBinWriter::flushBlock(Block&& block) {
  // Apply the 8 KB split, writing each resulting record immediately and dropping it.
  splitTextBlock(std::move(block), [this](Block&& rec) { writeOneRecord(rec); });
  return ok_;
}

void ContentBinWriter::onBlock(Block&& block, const CssStyle& style) {
  if (!ok_) return;
  if (!spineOpen_) beginSpine();

  block.styleId = internStyle(spineStyles_, style);  // per-spine local pool (v6)

  // Attach footnotes/xpath accumulated during this block's build (before onBlock flushed it).
  block.footnotes = std::move(pendingFootnotes_);
  pendingFootnotes_.clear();
  if (pendingXPath_) {
    block.hasXPath = true;
    block.xpath = pendingXPathCounters_;
    pendingXPath_ = false;
  }

  if (!spineHasBlock_) {
    spineFirstCharOffset_ = block.charOffset;
    spineHasBlock_ = true;
  }

  // v7: record this LOGICAL block's start (file position before its first record, its char offset,
  // and the record index of its first record = the current running record count) for the baked
  // block-offset table. One entry per onBlock — the kContinuation split records that flushBlock may
  // append all belong to this same logical block, so they share this entry. recordIndex lets the
  // reader's seekToBlock restore currentFirstRecordIndex_ (anchors/labels/chapters key on it).
  if (!appendBlockOffset(BlockOffset{bw_ ? bw_->position() : 0, block.charOffset, blockCount_})) {
    LOG_ERR("CBW", "OOM growing block-offset index (block %lu) — aborting spine", static_cast<unsigned long>(blockCount_));
    ok_ = false;
    return;
  }

  if (block.type == BlockType::Text) {
    flushBlock(std::move(block));  // may emit multiple continuation records
  } else {
    writeOneRecord(block);
  }
}

void ContentBinWriter::onAnchor(const std::string& id) {
  if (!ok_) return;
  if (!spineOpen_) beginSpine();
  // Block granularity: the anchor introduces the block at the current block count (charOffsetInBlock
  // 0), matching the producer's stage1EmitPendingAnchor model.
  anchors_.push_back(Anchor{id, blockCount_, 0});
}

void ContentBinWriter::onChapter(uint8_t level, const std::string& title) {
  if (!ok_) return;
  if (!spineOpen_) beginSpine();
  // The heading block was emitted immediately before this call: point at the last block. v6 stores
  // chapters PER-SPINE (in the spine's aux region); spineIndex is this spine's own index.
  const uint32_t blockIndex = blockCount_ == 0 ? 0 : blockCount_ - 1;
  spineChapters_.push_back(Chapter{static_cast<uint16_t>(spineIndexBeingWritten_), blockIndex, level, title});
}

void ContentBinWriter::onPageBreakLabel(const std::string& label) {
  if (!ok_) return;
  if (!spineOpen_) beginSpine();
  pageBreakLabels_.push_back(PageBreakLabel{label, blockCount_});
}

void ContentBinWriter::onFootnote(int wordIndex, const FootnoteEntry& entry) {
  if (!ok_) return;
  pendingFootnotes_.push_back({static_cast<uint32_t>(wordIndex), entry});
}

void ContentBinWriter::onXPathAdvance(uint16_t paragraphIndex, uint16_t listItemIndex, uint32_t bodyChildByteOffset) {
  if (!ok_) return;
  pendingXPath_ = true;
  pendingXPathCounters_ = {paragraphIndex, listItemIndex, bodyChildByteOffset};
}

void ContentBinWriter::onSpineEnd() {
  if (!ok_ || !file_) return;
  if (!spineOpen_) {
    // A spine with no blocks at all: still emit an (empty) spine so its index slot commits.
    beginSpine();
  }
  // Write this spine's SELF-CONTAINED aux region after its block stream: anchors + labels + the
  // spine's own style table + the spine's own chapters. Remember where it starts (auxOffset). The
  // reader loads all of it up front in openSpine so styleId/anchors/labels/chapters resolve before
  // the blocks stream. Order here MUST match BlockStreamReader::openSpine's read order.
  if (!bw_) {
    ok_ = false;
    return;
  }
  const uint32_t auxOffset = bw_->position();  // flush-aware append offset
  writeAnchors(*bw_, anchors_);
  writeLabels(*bw_, pageBreakLabels_);
  writeStylePool(*bw_, spineStyles_);
  writeChapters(*bw_, spineChapters_);
  spliceBlockOffsets(*bw_);  // v7: baked per-block offset table (after chapters); streamed from temp file
  // Back-patch the spine header (firstCharOffset + blockCount + auxOffset). Drain the buffer first so
  // the file cursor is the true append end, patch directly, restore + rebase to keep appending.
  if (!bw_->flush()) {
    ok_ = false;
    return;
  }
  const uint32_t here = static_cast<uint32_t>(file_->position());
  if (!file_->seekSet(spineStartOffset_)) {
    ok_ = false;
    return;
  }
  writePod(*file_, spineFirstCharOffset_);
  writePod(*file_, blockCount_);
  writePod(*file_, auxOffset);
  file_->seekSet(here);  // resume appending at end
  bw_->rebase();
  ok_ = static_cast<bool>(*file_);
  if (!ok_) return;

  // Flush the spine's blocks + aux + patched header FIRST so the DATA is durable before any slot
  // that advertises it. The index-slot COMMIT is separate (commitSpine): in autoCommit mode
  // (whole-book compile / producer — the spine is known clean) we commit here; otherwise the caller
  // (the reader's tee build) commits explicitly ONLY after confirming the build was not
  // degraded/truncated, so a bad parse never publishes a bad content.bin spine.
  file_->flush();
  lastSpineDataOffset_ = spineStartOffset_;
  lastSpineIndex_ = spineIndexBeingWritten_;
  lastSpineDataWritten_ = true;
  if (autoCommitSpines_) commitSpine(spineIndexBeingWritten_);
  spineOpen_ = false;
  pendingFootnotes_.clear();
  pendingXPath_ = false;
}

bool ContentBinWriter::commitSpine(uint32_t spineIndex) {
  // Publish the spine written by the most recent onSpineEnd: commit its start offset into the index
  // slot + flush, so a reader that sees the non-zero slot is guaranteed the durable data behind it.
  // Called automatically by onSpineEnd in autoCommit mode; called EXPLICITLY by the reader's tee
  // build after confirming a clean parse (so a degraded/truncated build never commits). A spineIndex
  // that does not match the last-written spine, or no data written, is a no-op (caller bug guard).
  if (!ok_ || !file_ || !lastSpineDataWritten_ || spineIndex != lastSpineIndex_) return false;
  commitSpineOffset(lastSpineIndex_, lastSpineDataOffset_);
  file_->flush();
  lastSpineDataWritten_ = false;  // consumed; a re-commit is a no-op
  return ok_;
}

bool ContentBinWriter::finish() {
  if (!ok_ || !file_) return false;
  // v6: nothing book-level to append (index committed slot-by-slot; styles + chapters are per-spine).
  // Just make sure any open spine was closed out; a well-formed caller already called onSpineEnd. In
  // non-autoCommit mode a spine whose data was written but never explicitly committed stays
  // uncommitted (slot 0) — correct: the caller chose not to publish it.
  if (spineOpen_) onSpineEnd();
  // Backstop: every onSpineEnd already flushes bw_, but drain it once more so no buffered tail is
  // left unwritten if a caller finishes without a trailing spine.
  if (bw_ && !bw_->flush()) ok_ = false;
  ok_ = ok_ && static_cast<bool>(*file_);
  return ok_;
}

}  // namespace compiled
