#include "ContentBinWriter.h"

#include <Logging.h>
#include <Memory.h>

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
  return ok_;
}

void ContentBinWriter::commitSpineOffset(uint32_t spineIndex, uint32_t offset) {
  if (!ok_ || !file_ || spineIndex >= spineCount_) return;
  const uint32_t here = static_cast<uint32_t>(file_->position());
  if (!file_->seekSet(kHeaderSize + spineIndex * sizeof(uint32_t))) {
    ok_ = false;
    return;
  }
  writePod(*file_, offset);
  file_->seekSet(here);  // resume appending
  ok_ = static_cast<bool>(*file_);
  if (ok_ && spineIndex < committed_.size()) committed_[spineIndex] = true;
}

void ContentBinWriter::beginSpine() { beginSpineAt(nextSpineIndex_++); }

void ContentBinWriter::beginSpineAt(uint32_t spineIndex) {
  if (!ok_ || !file_) return;
  spineIndexBeingWritten_ = spineIndex;
  spineStartOffset_ = static_cast<uint32_t>(file_->position());
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
  writePod(*file_, static_cast<uint32_t>(0));  // firstCharOffset
  writePod(*file_, static_cast<uint32_t>(0));  // blockCount
  writePod(*file_, static_cast<uint32_t>(0));  // auxOffset (of the per-spine aux region)
  ok_ = static_cast<bool>(*file_);
}

bool ContentBinWriter::appendBlockOffset(const BlockOffset& bo) {
  const uint32_t slot = blockOffsetCount_ % BlockOffsetChunk::kEntries;
  if (slot == 0) {
    // Current chunk full (or none yet): allocate the next. makeUniqueNoThrow — a failed alloc
    // returns null (never throws) under -fno-exceptions, so the caller aborts the build cleanly.
    auto chunk = makeUniqueNoThrow<BlockOffsetChunk>();
    if (!chunk) return false;
    BlockOffsetChunk* raw = chunk.get();
    if (blockOffsetTail_) {
      blockOffsetTail_->next = std::move(chunk);
    } else {
      blockOffsetHead_ = std::move(chunk);
    }
    blockOffsetTail_ = raw;
  }
  blockOffsetTail_->entries[slot] = bo;
  ++blockOffsetCount_;
  return true;
}

bool ContentBinWriter::writeBlockOffsetChunks(FsFile& out) const {
  // Same on-disk format as writeBlockOffsets(): u32 count, then count x {fileOffset,charOffset,
  // recordIndex}. Walk the chunk list in order, emitting the live entries of each.
  writePod(out, blockOffsetCount_);
  uint32_t remaining = blockOffsetCount_;
  for (const BlockOffsetChunk* c = blockOffsetHead_.get(); c && remaining > 0; c = c->next.get()) {
    const uint32_t n = remaining < BlockOffsetChunk::kEntries ? remaining : BlockOffsetChunk::kEntries;
    for (uint32_t i = 0; i < n; ++i) {
      writePod(out, c->entries[i].fileOffset);
      writePod(out, c->entries[i].charOffset);
      writePod(out, c->entries[i].recordIndex);
    }
    remaining -= n;
  }
  return static_cast<bool>(out);
}

void ContentBinWriter::clearBlockOffsets() {
  // Release LIFO: unique_ptr chain would recurse-destroy on a long list (stack depth = chunk count),
  // so unlink head-first iteratively. Each reset frees one chunk; its `next` is already detached.
  while (blockOffsetHead_) {
    std::unique_ptr<BlockOffsetChunk> head = std::move(blockOffsetHead_);
    blockOffsetHead_ = std::move(head->next);
  }
  blockOffsetTail_ = nullptr;
  blockOffsetCount_ = 0;
}

bool ContentBinWriter::writeOneRecord(const Block& rec) {
  if (!ok_ || !file_) return false;
  if (!writeBlock(*file_, rec)) {
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
  if (!appendBlockOffset(BlockOffset{static_cast<uint32_t>(file_->position()), block.charOffset, blockCount_})) {
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
  const uint32_t auxOffset = static_cast<uint32_t>(file_->position());
  writeAnchors(*file_, anchors_);
  writeLabels(*file_, pageBreakLabels_);
  writeStylePool(*file_, spineStyles_);
  writeChapters(*file_, spineChapters_);
  writeBlockOffsetChunks(*file_);  // v7: baked per-block offset table (after chapters), from chunk list
  // Back-patch the spine header (firstCharOffset + blockCount + auxOffset).
  const uint32_t here = static_cast<uint32_t>(file_->position());
  if (!file_->seekSet(spineStartOffset_)) {
    ok_ = false;
    return;
  }
  writePod(*file_, spineFirstCharOffset_);
  writePod(*file_, blockCount_);
  writePod(*file_, auxOffset);
  file_->seekSet(here);  // resume appending at end
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
  ok_ = ok_ && static_cast<bool>(*file_);
  return ok_;
}

}  // namespace compiled
