#include "ContentSink.h"

#include "BlockSerialization.h"  // splitTextBlock — the shared 8 KB record split
#include "Epub/FootnoteEntry.h"

namespace compiled {

void ContentSink::beginSpine() {
  content_.spines.emplace_back();
  spineOpen_ = true;
  spineHasBlock_ = false;
}

void ContentSink::appendTextSplit(Block&& block) {
  SpineContent& spine = content_.spines.back();
  splitTextBlock(std::move(block), [&spine](Block&& rec) { spine.blocks.push_back(std::move(rec)); });
}

void ContentSink::onBlock(Block&& block, const CssStyle& style) {
  if (!spineOpen_) beginSpine();
  SpineContent& spine = content_.spines.back();

  // Intern the block's resolved style (image/table blocks pass CssStyle{} — an empty style
  // still interns to a valid pool id, shared across all such blocks).
  block.styleId = internStyle(content_, style);

  // Attach footnotes/xpath accumulated during this block's build (before onBlock flushed it).
  block.footnotes = std::move(pendingFootnotes_);
  pendingFootnotes_.clear();
  if (pendingXPath_) {
    block.hasXPath = true;
    block.xpath = pendingXPathCounters_;
    pendingXPath_ = false;
  }

  if (!spineHasBlock_) {
    spine.firstCharOffset = block.charOffset;
    spineHasBlock_ = true;
  }

  if (block.type == BlockType::Text) {
    appendTextSplit(std::move(block));
  } else {
    spine.blocks.push_back(std::move(block));
  }
}

void ContentSink::onAnchor(const std::string& id) {
  if (!spineOpen_) beginSpine();
  SpineContent& spine = content_.spines.back();
  // Block granularity: the anchor introduces the block at the current block count
  // (charOffsetInBlock 0), matching the producer's stage1EmitPendingAnchor model.
  spine.anchors.push_back(Anchor{id, static_cast<uint32_t>(spine.blocks.size()), 0});
}

void ContentSink::onChapter(uint8_t level, const std::string& title) {
  if (!spineOpen_) beginSpine();
  const SpineContent& spine = content_.spines.back();
  // The heading block was emitted immediately before this call (stage1FlushBlock order):
  // point at the last block. An empty run would be a producer bug, but guard anyway.
  const uint32_t blockIndex = spine.blocks.empty() ? 0 : static_cast<uint32_t>(spine.blocks.size() - 1);
  content_.chapters.push_back(Chapter{static_cast<uint16_t>(content_.spines.size() - 1), blockIndex, level, title});
}

void ContentSink::onPageBreakLabel(const std::string& label) {
  if (!spineOpen_) beginSpine();
  SpineContent& spine = content_.spines.back();
  const uint32_t blockIndex = static_cast<uint32_t>(spine.blocks.size());
  // Serialized per-spine label table (WBC1 v3) so Stage-2 can replay onPageBreakLabel.
  spine.pageBreakLabels.push_back(PageBreakLabel{label, blockIndex});
  // Legacy in-memory view kept for the dump tool / callers that read labels() directly.
  labels_.push_back(PageLabel{label, static_cast<uint16_t>(content_.spines.size() - 1), blockIndex});
}

void ContentSink::onFootnote(int wordIndex, const FootnoteEntry& entry) {
  // Buffered until the block being built is flushed (onBlock), then attached to it. Stage-2
  // replays these as onFootnote(wordIndex, entry) so each lands on the page of its anchor word.
  pendingFootnotes_.push_back({static_cast<uint32_t>(wordIndex), entry});
}

void ContentSink::onXPathAdvance(uint16_t paragraphIndex, uint16_t listItemIndex, uint32_t bodyChildByteOffset) {
  pendingXPath_ = true;
  pendingXPathCounters_ = {paragraphIndex, listItemIndex, bodyChildByteOffset};
}

void ContentSink::onSpineEnd() {
  spineOpen_ = false;
  // Any footnote/xpath not consumed by an onBlock is dropped intentionally: they only matter
  // attached to a block, and the producer always flushes the owning block before spine end.
  pendingFootnotes_.clear();
  pendingXPath_ = false;
}

}  // namespace compiled
