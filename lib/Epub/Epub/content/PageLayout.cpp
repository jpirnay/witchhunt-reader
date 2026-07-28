#include "PageLayout.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "FootnotePreviewLayout.h"
#include "ImageLayout.h"
#include "LaidOutBlock.h"

// P1 — the microreader-style forward "collect" pull core, TEXT-only
// (docs/pull-core-plan-microreader-guided-2026-07-27.md §3 P1). Replaces the G2a scaffold body of
// layoutPage for text spines: a per-block LaidOutBlock cache holds each block's page-independent line
// set, and collectPageForward places lines with LayoutSink's exact makePages/addLineToPage Y math,
// owning the page-break boundary and emitting a line-granularity end cursor. A spine containing any
// Image/Hr/Table block FALLS BACK to the scaffold (drive one LayoutSink page) — P2-P4 add those.
//
// Byte-identity to the whole-spine LayoutSink golden is the gate (PageLayoutTest). To make the placed
// PageLine/TextBlock objects identical, a block's lines come from the SAME ParsedText call LayoutSink
// makes; the collect loop reproduces only the vertical accounting. The three deviations:
//   D1 hyphen retry — reproduced through the SAME addLineToPage callback contract (below): the retry
//     fires inside ParsedText exactly as it does for LayoutSink, so the cached line becomes no-hyphen
//     at the last-before-break position. Correctness (byte-identical) over purity, per the plan.
//   D2 empty-block margin-collapse merge — mirror onBlock's pending-merge accumulation; when a page
//     starts at block N>0, walk back over the immediately-preceding empty/<br> run to rebuild it.
//   D3 aux-font latch — seed resolveBlockFont's auxFontId_ from the cursor at page start; carry the
//     (possibly advanced) value out in the end cursor.

namespace compiled {
namespace {

// Reverse of stage1MapStyleSpan: on-disk styleSpan bitmask -> EpdFontFamily::Style (mirrors
// LayoutSink::spanToFontStyle, kept local so the pull core does not reach into the sink).
EpdFontFamily::Style spanToFontStyle(uint8_t span) {
  int s = EpdFontFamily::REGULAR;
  if (span & kSpanBold) s |= EpdFontFamily::BOLD;
  if (span & kSpanItalic) s |= EpdFontFamily::ITALIC;
  if (span & kSpanUnderline) s |= EpdFontFamily::UNDERLINE;
  if (span & kSpanStrikethrough) s |= EpdFontFamily::STRIKETHROUGH;
  if (span & kSpanSuper) s |= EpdFontFamily::SUP;
  if (span & kSpanSub) s |= EpdFontFamily::SUB;
  if (span & kSpanSmallCaps) s |= EpdFontFamily::SMALL_CAPS;
  return static_cast<EpdFontFamily::Style>(s);
}

// True when this logical block forces the pull core onto the scaffold fallback. P2 handles atomic
// block Images + HRs; Tables (P4) and float images (P3, an inline image on a Text block) still fall
// back. Text blocks with footnote previews / footnotes / xpath are handled by the pull core.
bool needsScaffold(const Block& b) {
  if (b.type == BlockType::Table) return true;                    // P4
  if (b.type == BlockType::Text && !b.inlineImageEntryPath.empty()) return true;  // float image — P3
  return false;
}

// The subset of LayoutSink's settings + text-path state the pull core reproduces. One instance lays
// out exactly ONE page from a cursor. The vertical math (marginTop collapse, paddingTop, per-line
// break, marginBottom, paddingBottom, extra-paragraph spacing) and the footnote assignment mirror
// LayoutSink::makePages/addLineToPage precisely; the empty-block merge mirrors LayoutSink::onBlock.
class PullDriver {
 public:
  PullDriver(GfxRenderer& renderer, const LayoutParams& params, int32_t auxFontIdSeed, int imageCounterSeed)
      : renderer_(renderer),
        fontId_(params.fontId),
        lineCompression_(params.lineCompression),
        extraParagraphSpacing_(params.extraParagraphSpacing),
        paragraphAlignment_(params.paragraphAlignment),
        viewportWidth_(params.viewportWidth),
        viewportHeight_(params.viewportHeight),
        hyphenationEnabled_(params.hyphenationEnabled),
        bionicReadingEnabled_(params.bionicReadingEnabled),
        embeddedStyle_(params.embeddedStyle),
        fontSizeLadder_(params.fontSizeLadder),
        imageBasePath_(params.imageBasePath),
        epubFilePath_(params.epubFilePath),
        auxFontId_(auxFontIdSeed),
        imageCounter_(imageCounterSeed) {}

  int32_t auxFontId() const { return auxFontId_; }
  int imageCounter() const { return imageCounter_; }

  // --- onBlock's empty-block merge (D2) ---

  // Fold `block`'s style into pending-merge state exactly as LayoutSink::onBlock's text path does.
  // Fills `out` with the block's page-independent lines and the final (merged, pre-font-resolve)
  // style. Returns false for an empty (<br>/wrapper) block: it contributed only to the running merge
  // and produces no page content. Mirrors the onBlock/layoutTextBlock split.
  bool prepareBlock(Block&& block, const CssStyle& style, LaidOutBlock& out) {
    if (block.type == BlockType::Image) return prepareImage(block, style, out);
    if (block.type == BlockType::Hr) return prepareHr(out);

    const bool isHeading = (block.flags & kStartsChapter) != 0;
    const bool fromBr = (block.flags & kFromBrElement) != 0;
    out.isContinuation = (block.flags & kContinuation) != 0;
    out.preformatted = (block.flags & kPreformatted) != 0;
    out.pageBreakBefore = (block.flags & kPageBreakBefore) != 0;

    const CssTextAlign contextAlign = hasPendingMerge_ ? pendingMergeStyle_.alignment : lastBlockAlignment_;
    const bool contextAlignDefined =
        hasPendingMerge_ ? pendingMergeStyle_.textAlignDefined : lastBlockAlignmentDefined_;

    BlockStyle blockStyle;
    if (fromBr) {
      blockStyle.alignment = contextAlign;
      blockStyle.textAlignDefined = contextAlignDefined;
      blockStyle.fromBrElement = true;
      if (style.hasTextIndent()) {
        const float emSize = static_cast<float>(renderer_.getFontAscenderSize(fontId_));
        blockStyle.textIndent = style.textIndent.toPixelsInt16(emSize, static_cast<float>(viewportWidth_));
        blockStyle.textIndentDefined = true;
      }
    } else {
      blockStyle = buildBlockStyle(style, isHeading);
    }

    if (block.words.empty()) {
      // Empty wrapper / <br>: accumulate into the pending merge, no page content. Mirrors onBlock's
      // empty-block ALIGNMENT RESET + merge accumulation.
      if (!blockStyle.fromBrElement) {
        blockStyle.textAlignDefined = false;
        blockStyle.alignment = (paragraphAlignment_ == static_cast<uint8_t>(CssTextAlign::None))
                                   ? CssTextAlign::Justify
                                   : static_cast<CssTextAlign>(paragraphAlignment_);
      }
      if (hasPendingMerge_) {
        BlockStyle incoming = blockStyle;
        if (pendingMergeFromBr_) incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineGapPx());
        pendingMergeStyle_ = pendingMergeStyle_.getCombinedBlockStyle(incoming);
      } else {
        pendingMergeStyle_ = blockStyle;
      }
      pendingMergeFromBr_ = blockStyle.fromBrElement;
      hasPendingMerge_ = true;
      out.isEmptyBlock = true;
      return false;
    }

    // Non-empty: fold any pending empty-block merge into its style first.
    if (hasPendingMerge_) {
      BlockStyle incoming = blockStyle;
      if (pendingMergeFromBr_) incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineGapPx());
      blockStyle = pendingMergeStyle_.getCombinedBlockStyle(incoming);
      hasPendingMerge_ = false;
      pendingMergeFromBr_ = false;
    }

    lastBlockAlignment_ = blockStyle.alignment;
    lastBlockAlignmentDefined_ = blockStyle.textAlignDefined;

    out.style = blockStyle;
    out.isEmptyBlock = false;
    layoutBlockLines(block, out);
    return true;
  }

  // --- collect one page (forward) ---

  // Place lines from `blocks` starting at line `startLine` of block 0, filling one page. Returns the
  // completed page; sets `endBlockOffset` (into `blocks`) and `endLine` to the first line that did NOT
  // fit (the next page's start), or endBlockOffset==blocks.size() when everything fit (spine end for
  // this window). `pendingFootnotesByBlock` supplies each block's footnote refs.
  std::unique_ptr<Page> collectPageForward(const std::vector<LaidOutBlock>& blocks,
                                           const std::vector<std::vector<FootnoteRef>>& footnotes, size_t startLine,
                                           size_t& endBlockOffset, size_t& endLine, bool& hitEnd) {
    currentPage_.reset(new Page());
    currentPageNextY_ = 0;
    lastBlockMarginBottom_ = 0;
    pageDone_ = false;

    for (size_t bi = 0; bi < blocks.size() && !pageDone_; ++bi) {
      const LaidOutBlock& lb = blocks[bi];
      // kPageBreakBefore: a TOC-boundary / CSS page-break-before block starts a fresh page when the
      // current page already has content (mirrors LayoutSink::onBlock). It becomes the next page's
      // start. Never breaks on the page's own first block (a leading break emits no blank page).
      if (lb.pageBreakBefore && !currentPage_->elements.empty()) {
        endBlockOffset = bi;
        endLine = 0;
        hitEnd = false;
        finalizeAssignedFootnotes();
        return std::move(currentPage_);
      }
      size_t brokeAt = 0;
      bool placedAll;
      if (lb.kind == LaidOutBlock::Kind::Text) {
        const size_t firstLine = (bi == 0) ? startLine : 0;
        brokeAt = firstLine;
        placedAll = placeBlock(lb, footnotes[bi], firstLine, brokeAt);
      } else {
        placedAll = placeAtomic(lb, brokeAt);  // atomic Image/Hr: brokeAt 0 = block starts next page
      }
      if (!placedAll) {
        endBlockOffset = bi;
        endLine = brokeAt;
        hitEnd = false;
        finalizeAssignedFootnotes();
        return std::move(currentPage_);
      }
    }

    // Everything fit: this is the spine's last page (for the fed window). The end cursor is spine end.
    endBlockOffset = blocks.size();
    endLine = 0;
    hitEnd = true;
    finalizeAssignedFootnotes();
    return std::move(currentPage_);
  }

  // Upper-bound placed height of a prepared block (top spacing + every line + bottom spacing), for the
  // window-gathering stop condition only. Not used for placement — collectPageForward does the exact
  // Y math — so a slight over-estimate just reads one extra block, which is harmless.
  // `fromLine` skips the lines already consumed by earlier pages (a mid-block resume), so the window
  // estimate counts only what this page can place — otherwise the resumed block's off-page lines
  // inflate the estimate and the window stops early, dropping a block that would still fit.
  int blockPlacedHeight(const LaidOutBlock& lb, size_t fromLine = 0) const {
    if (lb.kind == LaidOutBlock::Kind::Image)
      return lb.imageSpacingTop + lb.imageHeight + lb.imageSpacingBottom;
    if (lb.kind == LaidOutBlock::Kind::Hr) return 2 * lb.hrMarginV + 1;
    const BlockStyle& bs = lb.style;
    const int lineHeight = effectiveLineHeight(bs);
    int h = 0;
    if (fromLine == 0 && !lb.isContinuation) h += std::max<int>(0, bs.marginTop) + std::max<int>(0, bs.paddingTop);
    for (size_t li = fromLine; li < lb.lines.size(); ++li) {
      const uint8_t maxPct = lb.lines[li]->maxSizePct();
      h += (maxPct != 100) ? lineHeight * maxPct / 100 : lineHeight;
    }
    h += std::max<int>(0, bs.marginBottom) + std::max<int>(0, bs.paddingBottom);
    if (extraParagraphSpacing_ && !lb.preformatted) h += lineHeight / 2;
    return h;
  }

 private:
  // Port of LayoutSink::buildBlockStyle.
  BlockStyle buildBlockStyle(const CssStyle& style, bool isHeading) const {
    const float emSize = static_cast<float>(renderer_.getFontAscenderSize(fontId_));
    const CssTextAlign defaultAlign =
        isHeading ? CssTextAlign::Center : static_cast<CssTextAlign>(paragraphAlignment_);
    BlockStyle bs = BlockStyle::fromCssStyle(style, emSize, defaultAlign, viewportWidth_);
    if (isHeading) bs.textAlignDefined = true;
    if (embeddedStyle_ && style.hasTextAlign() && paragraphAlignment_ == static_cast<uint8_t>(CssTextAlign::None)) {
      bs.alignment = style.textAlign;
      bs.textAlignDefined = true;
    }
    return bs;
  }

  // Port of LayoutSink::resolveBlockFont (D3 aux-font latch, seeded from the cursor at ctor).
  void resolveBlockFont(BlockStyle& bs) {
    if (bs.fontResolved) return;
    bs.fontResolved = true;
    if (bs.headingFontId != 0 || bs.fontSizeMultiplier == 1.0f) return;
    const FontSizeLadder::Resolved r = fontSizeLadder_.resolve(bs.fontSizeMultiplier * 100.0f);
    if (r.fontId == 0) {
      bs.fontSizeMultiplier = r.residual;
      return;
    }
    if (auxFontId_ == 0) auxFontId_ = r.fontId;
    if (r.fontId != auxFontId_) return;
    bs.headingFontId = r.fontId;
    bs.fontSizeMultiplier = r.residual;
  }

  int effectiveFontId(const BlockStyle& bs) const { return bs.headingFontId != 0 ? bs.headingFontId : fontId_; }
  int effectiveLineHeight(const BlockStyle& bs) const {
    return static_cast<int>(renderer_.getLineHeight(effectiveFontId(bs)) * lineCompression_ * bs.fontSizeMultiplier +
                            0.5f);
  }
  int16_t lineGapPx() const {
    return static_cast<int16_t>(renderer_.getLineHeight(fontId_) * lineCompression_ + 0.5f);
  }

  // Produce the block's page-independent lines through the SAME ParsedText path LayoutSink uses:
  // abbreviate inline footnote previews to the viewport (the one settings-dependent block mutation,
  // shared with LayoutSink), then foldUniformWordSizes + the >96-word mid-block flush replayed so
  // line breaks land identically. blockStartY/lineHeight are 0 (no floats in the text-only pull
  // path), so the lines do not depend on page Y.
  void layoutBlockLines(Block& block, LaidOutBlock& out) {
    abbreviateFootnotePreviews(block, renderer_, fontId_, viewportWidth_);
    auto text = std::unique_ptr<ParsedText>(
        new ParsedText(extraParagraphSpacing_, hyphenationEnabled_, out.style, bionicReadingEnabled_));

    auto collect = [&out](const std::shared_ptr<TextBlock>& tb, bool, bool) {
      out.lines.push_back(tb);
      out.lineWordCounts.push_back(tb->wordCount());
      return ParsedText::LineProcessResult::Accepted;
    };

    for (const Word& w : block.words) {
      const char* wtext = &block.text[w.textOff];
      const EpdFontFamily::Style fontStyle = spanToFontStyle(w.styleSpan);
      const bool attach = (w.styleSpan & kSpanAttachPrev) != 0;
      text->addWord(wtext, fontStyle, /*underline=*/false, attach, w.sizePct);

      if (text->size() > 96) {
        auto& splitStyle = text->getBlockStyle();
        resolveBlockFont(splitStyle);
        const int horizontalInset = splitStyle.totalHorizontalInset();
        const uint16_t effectiveWidth = (horizontalInset < viewportWidth_)
                                            ? static_cast<uint16_t>(viewportWidth_ - horizontalInset)
                                            : viewportWidth_;
        text->layoutAndExtractLines(renderer_, fontId_, effectiveWidth, collect, /*includeLastLine=*/false,
                                    /*blockStartY=*/0, /*lineHeight=*/0);
        // LayoutSink's >96-word flush places these lines via addLineToPage directly (bypassing
        // makePages' top-margin), and the final makePages runs as a continuation (skips top-margin).
        // Net: a block that flushed mid-layout applies NO top margin/padding. Flag it so placeBlock
        // matches (otherwise we'd add the block's marginTop that LayoutSink dropped).
        out.flushedMidBlock = true;
      }
    }

    if (!out.isContinuation) text->foldUniformWordSizes();
    resolveBlockFont(text->getBlockStyle());
    // Adopt the possibly font-resolved style (headingFontId/residual) so placement metrics match.
    out.style = text->getBlockStyle();
    const int horizontalInset = out.style.totalHorizontalInset();
    const uint16_t effectiveWidth =
        (horizontalInset < viewportWidth_) ? static_cast<uint16_t>(viewportWidth_ - horizontalInset) : viewportWidth_;
    text->layoutAndExtractLines(renderer_, fontId_, effectiveWidth, collect, /*includeLastLine=*/true,
                                /*blockStartY=*/0, /*lineHeight=*/0);
  }

  // Allocate the next per-spine image cache path (mirrors LayoutSink::nextImageCachePath).
  std::string nextImageCachePath(const std::string& entryPath) {
    std::string ext;
    const size_t extPos = entryPath.find_last_of('.');
    if (extPos != std::string::npos) ext = entryPath.substr(extPos);
    return imageBasePath_ + std::to_string(imageCounter_++) + ext;
  }

  // Prepare a standalone (centered) block image, mirroring LayoutSink::placeBlockImage's non-placement
  // half: resolve the display size against the pending-merge container, consume the wrapper spacing,
  // build the ImageBlock + cache path. The page-break/placement is deferred to placeImage so a block
  // that ends on a page and the one that starts on it can reuse this slot.
  bool prepareImage(const Block& block, const CssStyle& imgStyle, LaidOutBlock& out) {
    out.kind = LaidOutBlock::Kind::Image;
    int containerWidth = viewportWidth_;
    if (hasPendingMerge_) {
      const int inset = pendingMergeStyle_.totalHorizontalInset();
      if (inset > 0 && inset < viewportWidth_) containerWidth = viewportWidth_ - inset;
    }
    const float emSize = static_cast<float>(renderer_.getFontAscenderSize(fontId_));
    const ImageDisplaySize ds = computeImageDisplaySize(block.width, block.height, imgStyle, viewportWidth_,
                                                        viewportHeight_, containerWidth, emSize);

    int spacingTop = 0, spacingBottom = 0;
    if (hasPendingMerge_) {
      spacingTop = std::max(0, static_cast<int>(pendingMergeStyle_.marginTop)) +
                   std::max(0, static_cast<int>(pendingMergeStyle_.paddingTop));
      spacingBottom = std::max(0, static_cast<int>(pendingMergeStyle_.marginBottom)) +
                      std::max(0, static_cast<int>(pendingMergeStyle_.paddingBottom));
    }
    // The wrapper spacing is consumed around the image; clear the merge so it doesn't leak forward.
    hasPendingMerge_ = false;
    pendingMergeFromBr_ = false;

    const std::string cachePath = nextImageCachePath(block.entryPath);
    out.image = std::make_shared<ImageBlock>(cachePath, static_cast<int16_t>(ds.width),
                                             static_cast<int16_t>(ds.height), block.alt, epubFilePath_,
                                             block.entryPath);
    out.imageX = static_cast<int16_t>((viewportWidth_ - ds.width) / 2);
    out.imageHeight = static_cast<int16_t>(ds.height);
    out.imageSpacingTop = static_cast<int16_t>(spacingTop);
    out.imageSpacingBottom = static_cast<int16_t>(spacingBottom);
    return true;
  }

  // Prepare a horizontal rule, mirroring LayoutSink::placeHr's geometry. NOTE: placeHr's pending-merge
  // handling (materializing an empty block before the rule + carrying a br-gap forward) is not modeled
  // here yet — a spine that hits that path falls back to the scaffold (needsScaffold gates a pending
  // <br>/empty run before an HR). The common case (a bare <hr/>) is handled.
  bool prepareHr(LaidOutBlock& out) {
    out.kind = LaidOutBlock::Kind::Hr;
    const int lineHeight = static_cast<int>(renderer_.getLineHeight(fontId_) * lineCompression_ + 0.5f);
    out.hrMarginV = static_cast<int16_t>(lineHeight / 2);
    out.hrWidth = static_cast<int16_t>(viewportWidth_ / 2);
    out.hrX = static_cast<int16_t>(viewportWidth_ / 4);
    return true;
  }

  // Place an atomic Image or Hr block. Returns true when it fit; false when it forced a page break
  // (the block itself starts the next page — brokeAt = 0, meaning "before this block's content").
  bool placeAtomic(const LaidOutBlock& lb, size_t& brokeAt) {
    if (lb.kind == LaidOutBlock::Kind::Image) {
      const int totalHeight = lb.imageSpacingTop + lb.imageHeight + lb.imageSpacingBottom;
      if (!currentPage_->elements.empty() && currentPageNextY_ + totalHeight > viewportHeight_) {
        brokeAt = 0;
        pageDone_ = true;
        return false;
      }
      currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + lb.imageSpacingTop);
      currentPage_->elements.push_back(std::make_shared<PageImage>(lb.image, lb.imageX, currentPageNextY_));
      currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + lb.imageHeight + lb.imageSpacingBottom);
      lastBlockMarginBottom_ = 0;
      brokeAt = 1;
      return true;
    }
    // Hr: half-line margin above, 1px rule, half-line below (mirrors placeHr).
    currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + lb.hrMarginV);
    if (currentPageNextY_ + 1 + lb.hrMarginV > viewportHeight_ && !currentPage_->elements.empty()) {
      // Undo the margin we just added; the rule starts the next page.
      currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ - lb.hrMarginV);
      brokeAt = 0;
      pageDone_ = true;
      return false;
    }
    currentPage_->elements.push_back(std::make_shared<PageHR>(lb.hrX, currentPageNextY_, lb.hrWidth));
    currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + 1 + lb.hrMarginV);
    lastBlockMarginBottom_ = 0;
    brokeAt = 1;
    return true;
  }

  // Place one prepared block's lines [firstLine..] onto the current page, reproducing
  // makePages + addLineToPage. Returns true when the whole block fit; false when the page broke inside
  // it, setting `brokeAt` to the first unplaced line index.
  bool placeBlock(const LaidOutBlock& lb, const std::vector<FootnoteRef>& footnotes, size_t firstLine,
                  size_t& brokeAt) {
    const BlockStyle& bs = lb.style;
    const int lineHeight = effectiveLineHeight(bs);

    // Top spacing only when starting the block fresh (line 0), not a continuation, and it did not
    // trigger a >96-word mid-block flush — mirrors makePages (which applies marginTop/paddingTop once
    // at the block's first makePages call) AND its interaction with layoutTextBlock's flush, which
    // consumes the block's early lines marginless and leaves makePages a continuation.
    if (firstLine == 0 && !lb.isContinuation && !lb.flushedMidBlock) {
      if (bs.marginTop > 0) {
        const int16_t collapse = std::min(lastBlockMarginBottom_, bs.marginTop);
        currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + (bs.marginTop - collapse));
      }
      if (bs.paddingTop > 0) currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + bs.paddingTop);
    }
    if (firstLine == 0) lastBlockMarginBottom_ = 0;

    // Reset the per-block footnote cursor when starting the block fresh; when resuming mid-block, the
    // words already consumed by earlier pages are counted so footnote anchors resolve to the right
    // page (matches wordsExtractedInBlock_ semantics).
    if (firstLine == 0) {
      wordsExtractedInBlock_ = 0;
      pendingFootnotes_.assign(footnotes.begin(), footnotes.end());
      std::sort(pendingFootnotes_.begin(), pendingFootnotes_.end(),
                [](const FootnoteRef& a, const FootnoteRef& b) { return a.wordIndex < b.wordIndex; });
    }

    const int16_t xOffset = bs.leftInset();

    for (size_t li = firstLine; li < lb.lines.size(); ++li) {
      const std::shared_ptr<TextBlock>& line = lb.lines[li];
      int lh = lineHeight;
      const uint8_t maxPct = line->maxSizePct();
      if (maxPct != 100) lh = lh * maxPct / 100;

      if (currentPageNextY_ + lh > viewportHeight_) {
        // Page break BEFORE placing this line: the block continues onto the next page.
        brokeAt = li;
        pageDone_ = true;
        return false;
      }

      wordsExtractedInBlock_ += line->wordCount();
      assignFootnotesUpTo(wordsExtractedInBlock_);
      currentPage_->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY_));
      currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + lh);
    }

    // Whole block placed: any remaining footnotes for this block belong to the current page (the
    // makePages tail fallback for anchors at the exact block end).
    for (const auto& fn : pendingFootnotes_) currentPage_->addFootnote(fn.entry.number, fn.entry.href);
    pendingFootnotes_.clear();

    // Bottom spacing (makePages tail).
    if (bs.marginBottom > 0) {
      currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + bs.marginBottom);
      lastBlockMarginBottom_ = bs.marginBottom;
    } else {
      lastBlockMarginBottom_ = 0;
    }
    if (bs.paddingBottom > 0) currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + bs.paddingBottom);
    if (extraParagraphSpacing_ && !lb.preformatted) currentPageNextY_ = static_cast<int16_t>(currentPageNextY_ + lineHeight / 2);

    brokeAt = lb.lines.size();
    return true;
  }

  void assignFootnotesUpTo(int wordsExtracted) {
    auto it = pendingFootnotes_.begin();
    while (it != pendingFootnotes_.end() && static_cast<int>(it->wordIndex) <= wordsExtracted) {
      currentPage_->addFootnote(it->entry.number, it->entry.href);
      ++it;
    }
    pendingFootnotes_.erase(pendingFootnotes_.begin(), it);
  }

  void finalizeAssignedFootnotes() { pendingFootnotes_.clear(); }

  GfxRenderer& renderer_;
  const int fontId_;
  const float lineCompression_;
  const bool extraParagraphSpacing_;
  const uint8_t paragraphAlignment_;
  const uint16_t viewportWidth_;
  const uint16_t viewportHeight_;
  const bool hyphenationEnabled_;
  const bool bionicReadingEnabled_;
  const bool embeddedStyle_;
  FontSizeLadder fontSizeLadder_;
  const std::string imageBasePath_;
  const std::string epubFilePath_;
  int32_t auxFontId_ = 0;
  int imageCounter_ = 0;  // per-spine image-cache-path counter (carried in the cursor, like auxFontId_)

  bool hasPendingMerge_ = false;
  BlockStyle pendingMergeStyle_;
  bool pendingMergeFromBr_ = false;
  CssTextAlign lastBlockAlignment_ = CssTextAlign::Justify;
  bool lastBlockAlignmentDefined_ = false;

  std::unique_ptr<Page> currentPage_;
  int16_t currentPageNextY_ = 0;
  int16_t lastBlockMarginBottom_ = 0;
  int wordsExtractedInBlock_ = 0;
  bool pageDone_ = false;
  std::vector<FootnoteRef> pendingFootnotes_;
};

// The G2a scaffold: drive a LayoutSink for exactly one page from a block-boundary cursor. Used as the
// P1 fallback for any spine containing a non-text block (Image/Hr/Table/float), so the whole-corpus
// oracle stays green until P2-P4 land those block types in the pull core.
LaidOutPage scaffoldOnePage(BlockStreamReader& reader, GfxRenderer& renderer, const LayoutParams& params,
                            const PagePosition& start) {
  LaidOutPage out;
  out.start = start;
  out.end = start;

  if (start.blockIndex != 0 && !reader.seekToBlock(start.blockIndex)) return out;

  std::unique_ptr<Page> firstPage;
  bool pageDone = false;
  LayoutSink sink(renderer, params, [&](std::unique_ptr<Page> p) {
    if (!pageDone) {
      firstPage = std::move(p);
      pageDone = true;
    }
  });

  const auto& anchors = reader.spineAnchors();
  const auto& labels = reader.spineLabels();
  const auto& stylePool = reader.spineStylePool();
  const auto& chapters = reader.spineChapters();
  static const CssStyle kEmptyStyle{};

  uint32_t lastBlockIndex = start.blockIndex;
  Block lb;
  while (!pageDone && reader.nextLogicalBlock(lb)) {
    const uint32_t bi = reader.currentFirstRecordIndex();
    lastBlockIndex = bi;
    for (const auto& a : anchors)
      if (a.blockIndex == bi) sink.onAnchor(a.id);
    for (const auto& pl : labels)
      if (pl.blockIndex == bi) sink.onPageBreakLabel(pl.label);
    for (const auto& fn : lb.footnotes) sink.onFootnote(static_cast<int>(fn.wordIndex), fn.entry);
    if (lb.hasXPath)
      sink.onXPathAdvance(lb.xpath.paragraphIndex, lb.xpath.listItemIndex, lb.xpath.bodyChildByteOffset);
    const CssStyle& style = (lb.styleId < stylePool.size()) ? stylePool[lb.styleId] : kEmptyStyle;
    sink.onBlock(std::move(lb), style);
    for (const auto& ch : chapters)
      if (ch.blockIndex == bi) sink.onChapter(ch.level, ch.title);
  }
  if (!reader.ok()) return out;
  if (!pageDone) sink.onSpineEnd();
  if (!firstPage) return out;

  out.page = std::move(firstPage);
  out.atSpineEnd = !pageDone;
  out.end.blockIndex = out.atSpineEnd ? 0 : static_cast<uint16_t>(lastBlockIndex);
  out.end.offset = 0;
  out.ok = true;
  return out;
}

}  // namespace

// Find the merge-chain start: the first block of the contiguous empty/<br> run immediately preceding
// `cursorBlock` (D2). An empty run's collapsed margins fold into the first content block on the page,
// so the driver must replay that run before the cursor block. Bounded — a short separator run. Leaves
// the reader positioned at `scanFrom` on success. Returns cursorBlock (no walk-back) when block 0 or
// when the preceding block is content.
uint16_t findMergeChainStart(BlockStreamReader& reader, uint16_t cursorBlock) {
  uint16_t scanFrom = cursorBlock;
  while (scanFrom > 0) {
    if (!reader.seekToBlock(static_cast<uint32_t>(scanFrom - 1))) break;
    Block prev;
    if (!reader.nextLogicalBlock(prev)) break;
    const bool empty = (prev.type == BlockType::Text) && prev.words.empty();
    if (!empty) break;
    --scanFrom;
  }
  return scanFrom;
}

LaidOutPage layoutPage(BlockStreamReader& reader, GfxRenderer& renderer, const LayoutParams& params,
                       const PagePosition& start) {
  LaidOutPage out;
  out.start = start;
  out.end = start;

  if (!reader.spineAvailable(start.spineIndex)) return out;
  if (!reader.openSpine(start.spineIndex)) return out;

  // D2 walk-back over the empty/<br> run preceding the cursor, then stream forward from there.
  const uint16_t cursorBlock = start.blockIndex;
  const uint16_t scanFrom = findMergeChainStart(reader, cursorBlock);
  if (!reader.seekToBlock(scanFrom)) return out;

  const auto& stylePool = reader.spineStylePool();
  static const CssStyle kEmptyStyle{};

  PullDriver driver(renderer, params, start.auxFontId, start.imageCounter);

  std::vector<LaidOutBlock> window;        // content blocks only (empty blocks fold into the merge)
  std::vector<std::vector<FootnoteRef>> windowFootnotes;
  std::vector<uint32_t> windowBlockIndex;  // logical block index per window entry
  size_t startLine = 0;                    // mid-block line offset within the first content block
  bool startLineCaptured = false;
  bool scaffoldFallback = false;

  // Accumulate content height as blocks are prepared; stop once the window is tall enough to
  // guarantee this page breaks inside it (or the spine ends). We read ONE block PAST the budget: a
  // block that exactly completes the page must be followed by the next block so collectPageForward
  // sees the overflow and reports the break (rather than fitting everything and claiming spine end).
  // Over-reading is harmless — collectPageForward stops at the first line that doesn't fit and never
  // touches later blocks.
  int accumulatedHeight = 0;
  const int pageBudget = params.viewportHeight;
  bool budgetExceeded = false;

  Block lb;
  while (reader.nextLogicalBlock(lb)) {
    const uint32_t bi = reader.currentFirstRecordIndex();
    if (needsScaffold(lb)) {
      scaffoldFallback = true;
      break;
    }
    const bool atOrPastCursor = bi >= cursorBlock;
    LaidOutBlock prepared;
    std::vector<FootnoteRef> fns = lb.footnotes;
    const CssStyle& style = (lb.styleId < stylePool.size()) ? stylePool[lb.styleId] : kEmptyStyle;
    const bool hasContent = driver.prepareBlock(std::move(lb), style, prepared);
    if (!hasContent) continue;  // empty block folded into the pending merge

    // The cursor's mid-block line offset applies to the first CONTENT block at/after the cursor.
    if (!startLineCaptured && atOrPastCursor) {
      startLine = start.offset;
      startLineCaptured = true;
    }

    // The resumed block (window entry 0 when the cursor is mid-block) contributes only its
    // remaining lines to this page's height.
    const size_t fromLine = window.empty() ? startLine : 0;
    accumulatedHeight += driver.blockPlacedHeight(prepared, fromLine);
    window.push_back(std::move(prepared));
    windowFootnotes.push_back(std::move(fns));
    windowBlockIndex.push_back(bi);

    // Stop one block AFTER first exceeding the budget: the extra block lets collectPageForward
    // distinguish "page full, more follows" from "spine ends here".
    if (budgetExceeded) break;
    if (accumulatedHeight > pageBudget) budgetExceeded = true;
  }
  if (!reader.ok()) return out;

  if (scaffoldFallback) {
    // A non-text block fell in the window — lay this page out via the trusted scaffold (P2-P4 replace).
    if (!reader.openSpine(start.spineIndex)) return out;
    return scaffoldOnePage(reader, renderer, params, start);
  }
  if (window.empty()) return out;  // no content (empty spine tail)

  size_t endOffset = 0, endLine = 0;
  bool hitEnd = false;
  std::unique_ptr<Page> page =
      driver.collectPageForward(window, windowFootnotes, startLine, endOffset, endLine, hitEnd);
  if (!page || page->elements.empty()) return out;

  out.page = std::move(page);
  out.atSpineEnd = hitEnd;
  out.end.spineIndex = start.spineIndex;
  if (hitEnd) {
    out.end.blockIndex = 0;
    out.end.offset = 0;
  } else {
    out.end.blockIndex = static_cast<uint16_t>(windowBlockIndex[endOffset]);
    out.end.offset = static_cast<uint16_t>(endLine);
  }
  out.end.auxFontId = driver.auxFontId();
  out.end.imageCounter = driver.imageCounter();
  out.ok = true;
  return out;
}

}  // namespace compiled
