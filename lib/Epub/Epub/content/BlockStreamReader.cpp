#include "BlockStreamReader.h"

#include <utility>

#include "BlockSerialization.h"
#include "Serialization.h"

namespace compiled {
namespace {

using serialization::readPod;

// Merge a continuation record's words/text/footnotes/previews into the logical block being built,
// rebasing textOff and word-indexed side data by the current word base. Mirrors the coalescing the
// two-pass harness did per spine, but here it is incremental (one logical block at a time).
void mergeContinuation(Block& into, const Block& cont) {
  const uint32_t wordBase = static_cast<uint32_t>(into.words.size());
  const uint32_t textBase = static_cast<uint32_t>(into.text.size());
  for (Word w : cont.words) {
    w.textOff += textBase;
    into.words.push_back(w);
  }
  into.text += cont.text;
  for (FootnoteRef fn : cont.footnotes) {
    fn.wordIndex += wordBase;
    into.footnotes.push_back(fn);
  }
  for (PreviewRun pr : cont.footnotePreviews) {
    pr.startWord += wordBase;
    into.footnotePreviews.push_back(pr);
  }
}

}  // namespace

bool BlockStreamReader::open(FsFile& file) {
  file_ = &file;
  ok_ = false;
  spineOffsets_.clear();
  spineStylePool_.clear();
  spineChapters_.clear();
  if (!file) return false;

  if (!file.seekSet(0)) return false;
  char magic[4] = {};
  if (file.read(magic, 4) != 4) return false;
  for (int i = 0; i < 4; ++i) {
    if (magic[i] != kMagic[i]) return false;
  }
  uint8_t version = 0;
  readPod(file, version);
  if (version != kVersion) return false;
  readPod(file, fingerprint_);
  uint32_t spineCount = 0;
  readPod(file, spineCount);

  const uint32_t fileSize = static_cast<uint32_t>(file.fileSize());
  // The spine-offset index is fixed right after the header (spineCount × u32). A 0 slot = spine not
  // yet committed (partial file / never compiled), which is legal — spineAvailable() reports it. Only
  // a NON-ZERO offset past EOF is corruption.
  if (fileSize < kHeaderSize + spineCount * sizeof(uint32_t)) return false;  // index truncated
  if (!file.seekSet(kHeaderSize)) return false;
  spineOffsets_.resize(spineCount);
  for (uint32_t i = 0; i < spineCount; ++i) {
    readPod(file, spineOffsets_[i]);
    if (spineOffsets_[i] > fileSize) return false;  // committed offset past EOF → corrupt
  }

  ok_ = static_cast<bool>(file);
  return ok_;
}

bool BlockStreamReader::refreshIndex() {
  if (!ok_ || !file_) return false;
  const uint32_t fileSize = static_cast<uint32_t>(file_->fileSize());
  if (!file_->seekSet(kHeaderSize)) return false;
  for (uint32_t i = 0; i < spineOffsets_.size(); ++i) {
    uint32_t off = 0;
    readPod(*file_, off);
    if (off > fileSize) return false;  // committed offset past EOF → corrupt
    // Slots only advance 0 -> committed; never un-commit. Keep the max so a concurrent partial read
    // of a slot mid-write can never regress an already-seen availability.
    if (off != 0) spineOffsets_[i] = off;
  }
  return static_cast<bool>(*file_);
}

bool BlockStreamReader::openSpine(uint32_t i) {
  if (!ok_ || !file_ || i >= spineOffsets_.size()) return false;
  if (spineOffsets_[i] == 0) return false;  // spine not yet committed (frontier)
  if (!file_->seekSet(spineOffsets_[i])) return false;
  uint32_t auxOffset = 0;
  readPod(*file_, spineFirstCharOffset_);
  readPod(*file_, spineBlockCount_);
  readPod(*file_, auxOffset);
  if (!*file_) return false;
  const uint32_t firstBlockOffset = static_cast<uint32_t>(file_->position());

  // Pre-load the spine's SELF-CONTAINED aux region from auxOffset (anchors + labels + style table +
  // chapters, in the exact order ContentBinWriter::onSpineEnd wrote them) so all are available
  // before the blocks stream.
  spineAnchors_.clear();
  spineLabels_.clear();
  spineStylePool_.clear();
  spineChapters_.clear();
  spineBlockOffsetsFileStart_ = 0;
  spineBlockOffsetCount_ = 0;
  if (auxOffset != 0) {
    if (auxOffset > static_cast<uint32_t>(file_->fileSize())) return false;
    if (!file_->seekSet(auxOffset)) return false;
    if (!readAnchors(*file_, spineAnchors_)) return false;
    if (!readLabels(*file_, spineLabels_)) return false;
    if (!readStylePool(*file_, spineStylePool_)) return false;
    if (!compiled::readChapters(*file_, spineChapters_)) return false;
    // v7 baked seek table: read only its u32 count and record where the entries start; the entries
    // themselves stay on disk (blockOffsetAt reads one on demand). A giant single spine has thousands
    // of entries — loading them into a std::vector aborted on the fragmented heap.
    uint32_t offCount = 0;
    readPod(*file_, offCount);
    if (!*file_) return false;
    spineBlockOffsetCount_ = offCount;
    spineBlockOffsetsFileStart_ = static_cast<uint32_t>(file_->position());
  }
  // Rewind to the first block to begin streaming.
  if (!file_->seekSet(firstBlockOffset)) return false;
  spineBlocksRemaining_ = spineBlockCount_;
  currentFirstRecordIndex_ = 0;
  haveLookahead_ = false;
  return static_cast<bool>(*file_);
}

bool BlockStreamReader::blockOffsetAt(uint32_t i, BlockOffset* out) const {
  if (!ok_ || !file_ || !out || i >= spineBlockOffsetCount_) return false;
  // Entries are fixed 12 bytes (fileOffset, charOffset, recordIndex), laid out contiguously from
  // spineBlockOffsetsFileStart_ — index by arithmetic, read the one entry.
  const uint32_t entrySize = 3 * static_cast<uint32_t>(sizeof(uint32_t));
  if (!file_->seekSet(spineBlockOffsetsFileStart_ + i * entrySize)) return false;
  readPod(*file_, out->fileOffset);
  readPod(*file_, out->charOffset);
  readPod(*file_, out->recordIndex);
  return static_cast<bool>(*file_);
}

bool BlockStreamReader::seekToBlock(uint32_t blockIndex) {
  if (!ok_ || !file_) return false;
  BlockOffset bo;
  if (!blockOffsetAt(blockIndex, &bo)) return false;
  if (!file_->seekSet(bo.fileOffset)) return false;
  // Restore the record-stream state as if we had read forward to this logical block's first record.
  // recordIndex is that first record's index; currentFirstRecordIndex_ is derived as
  // spineBlockCount_ - spineBlocksRemaining_ - (lookahead?1:0), so set remaining accordingly and drop
  // any pending continuation lookahead (the merge restarts cleanly at the new position).
  spineBlocksRemaining_ = spineBlockCount_ - bo.recordIndex;
  currentFirstRecordIndex_ = bo.recordIndex;
  haveLookahead_ = false;
  return static_cast<bool>(*file_);
}

bool BlockStreamReader::readOneRecord(Block& rec) {
  if (spineBlocksRemaining_ == 0) return false;
  rec = Block{};
  if (!readBlock(*file_, rec)) {
    ok_ = false;
    return false;
  }
  --spineBlocksRemaining_;
  return true;
}

bool BlockStreamReader::nextLogicalBlock(Block& out) {
  if (!ok_ || !file_) return false;

  // The record index of the block we are about to return = total records consumed so far. (The
  // lookahead, if present, was already counted; subtract it so the index points at THIS block.)
  currentFirstRecordIndex_ = spineBlockCount_ - spineBlocksRemaining_ - (haveLookahead_ ? 1 : 0);

  // Take the base: either the lookahead from a prior call, or the next on-disk record.
  if (haveLookahead_) {
    out = std::move(lookahead_);
    haveLookahead_ = false;
  } else if (!readOneRecord(out)) {
    return false;  // spine end
  }

  // Absorb following kContinuation records into `out` until the next record is a fresh block (which
  // becomes the lookahead) or the spine ends. Bounded to ONE logical block.
  while (spineBlocksRemaining_ > 0) {
    Block next;
    if (!readOneRecord(next)) return false;  // read error mid-spine
    if ((next.flags & kContinuation) != 0 && next.type == BlockType::Text) {
      mergeContinuation(out, next);
    } else {
      lookahead_ = std::move(next);
      haveLookahead_ = true;
      break;
    }
  }
  return true;
}

bool BlockStreamReader::readSpineAux(std::vector<Anchor>& anchors, std::vector<PageBreakLabel>& labels) {
  if (!ok_ || !file_) return false;
  // Must be called after the block stream is fully consumed (cursor sits at the aux tables). A
  // pending lookahead would mean blocks remain — refuse.
  if (haveLookahead_ || spineBlocksRemaining_ != 0) return false;
  if (!readAnchors(*file_, anchors)) return false;
  return readLabels(*file_, labels);
}

}  // namespace compiled
