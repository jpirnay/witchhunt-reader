#include "LayoutSink.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "BlockStreamReader.h"
#include "Epub/Page.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "FootnotePreviewLayout.h"
#include "ImageLayout.h"
#include "TableLayout.h"

namespace compiled {
namespace {

// Reverse of stage1MapStyleSpan (ChapterHtmlSlimParser.cpp): the on-disk styleSpan bitmask
// back to the EpdFontFamily::Style the layout's ParsedText::addWord expects.
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

}  // namespace

LayoutSink::LayoutSink(GfxRenderer& renderer, LayoutParams params,
                       std::function<void(std::unique_ptr<Page>)> completePageFn)
    : renderer_(renderer),
      completePageFn_(std::move(completePageFn)),
      fontId_(params.fontId),
      lineCompression_(params.lineCompression),
      extraParagraphSpacing_(params.extraParagraphSpacing),
      paragraphAlignment_(params.paragraphAlignment),
      viewportWidth_(params.viewportWidth),
      viewportHeight_(params.viewportHeight),
      hyphenationEnabled_(params.hyphenationEnabled),
      bionicReadingEnabled_(params.bionicReadingEnabled),
      embeddedStyle_(params.embeddedStyle),
      fontSizeLadder_(std::move(params.fontSizeLadder)),
      imageBasePath_(std::move(params.imageBasePath)),
      epubFilePath_(std::move(params.epubFilePath)) {}

LayoutSink::~LayoutSink() = default;

// --- Layout helpers: font resolution, line-height, page emission. ---

void LayoutSink::resolveBlockFont(BlockStyle& bs) {
  if (bs.fontResolved) return;
  bs.fontResolved = true;
  if (bs.headingFontId != 0 || bs.fontSizeMultiplier == 1.0f) return;
  const FontSizeLadder::Resolved r = fontSizeLadder_.resolve(bs.fontSizeMultiplier * 100.0f);
  if (r.fontId == 0) {
    // Nearest rung is the body font (or the ladder is empty): keep the pure-scale path.
    bs.fontSizeMultiplier = r.residual;
    return;
  }
  if (auxFontId_ == 0) auxFontId_ = r.fontId;
  if (r.fontId != auxFontId_) return;  // aux budget already claimed by another size — scale fallback
  bs.headingFontId = r.fontId;
  bs.fontSizeMultiplier = r.residual;
}

int LayoutSink::effectiveLineHeight(const BlockStyle& bs) const {
  return static_cast<int>(renderer_.getLineHeight(effectiveFontId(bs)) * lineCompression_ * bs.fontSizeMultiplier +
                          0.5f);
}

void LayoutSink::emitPage(uint32_t xhtmlByteOffset) {
  paragraphLutPerPage_.push_back({xhtmlByteOffset, xpathParagraphIndex_, xpathListItemIndex_});
  completePageFn_(std::move(currentPage_));
  completedPageCount_++;
  currentPage_.reset(new (std::nothrow) Page());
  currentPageNextY_ = 0;
  lastBlockMarginBottom_ = 0;
  deferredPageImage_.reset();  // the deferred yPos update is moot on a fresh page

  // A floated image never crosses a page boundary, so any active float ended on the
  // page we just emitted. Clear it and drop stale float zones from the block that
  // continues onto the new page.
  activeFloatTop_ = 0;
  activeFloatBottom_ = 0;
  if (currentTextBlock_) {
    currentTextBlock_->getBlockStyle().floatZoneCount = 0;
  }
}

BlockStyle LayoutSink::buildBlockStyle(const CssStyle& style, const bool isHeading) const {
  const float emSize = static_cast<float>(renderer_.getFontAscenderSize(fontId_));
  // Headings default to Center; other blocks default to the user's paragraphAlignment.
  const CssTextAlign defaultAlign = isHeading ? CssTextAlign::Center : static_cast<CssTextAlign>(paragraphAlignment_);
  BlockStyle bs = BlockStyle::fromCssStyle(style, emSize, defaultAlign, viewportWidth_);
  if (isHeading) bs.textAlignDefined = true;
  // Publisher text-align overrides the default when embedded CSS is honored and the user left
  // alignment at None (same condition for headings and blocks).
  if (embeddedStyle_ && style.hasTextAlign() && paragraphAlignment_ == static_cast<uint8_t>(CssTextAlign::None)) {
    bs.alignment = style.textAlign;
    bs.textAlignDefined = true;
  }
  return bs;
}

// Place one laid-out line on the current page: advance the y cursor, break to a new page when the
// line would overflow, shift the line right past any active left-float zone, and assign to this
// page any footnotes whose anchor word has now been laid out.
int LayoutSink::addLineToPage(const std::shared_ptr<TextBlock>& line, const bool lineEndsWithHyphenatedWord,
                              const bool suppressHyphenationRetry) {
  int lineHeight = effectiveLineHeight(line->getBlockStyle());
  const uint8_t maxPct = line->maxSizePct();
  if (maxPct != 100) {
    lineHeight = lineHeight * maxPct / 100;
  }

  if (!currentPage_) {
    currentPage_.reset(new Page());
    currentPageNextY_ = 0;
  }

  if (currentPageNextY_ + lineHeight > viewportHeight_) {
    emitPage(lastBodyChildByteOffset_);
  }

  const bool noRoomForAnotherLine =
      currentPageNextY_ + lineHeight <= viewportHeight_ && currentPageNextY_ + (lineHeight * 2) > viewportHeight_;
  if (lineEndsWithHyphenatedWord && !suppressHyphenationRetry && noRoomForAnotherLine) {
    return static_cast<int>(ParsedText::LineProcessResult::RetryWithoutHyphenation);
  }

  const bool isFirstLineOfBlock = (wordsExtractedInBlock_ == 0);
  wordsExtractedInBlock_ += line->wordCount();

  // Assign any footnotes whose anchor word has now been laid out to the current page.
  auto footnoteIt = pendingFootnotes_.begin();
  while (footnoteIt != pendingFootnotes_.end() && footnoteIt->first <= wordsExtractedInBlock_) {
    currentPage_->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes_.erase(pendingFootnotes_.begin(), footnoteIt);

  // Apply horizontal left inset. For lines overlapping an active LEFT float zone, also shift
  // right by the zone width so text starts after the image. Right floats narrow the line width
  // (handled in widthForLine) but don't shift text.
  int16_t xOffset = line->getBlockStyle().leftInset();
  {
    const auto& bs = line->getBlockStyle();
    for (int zi = 0; zi < bs.floatZoneCount; ++zi) {
      const auto& z = bs.floatZones[zi];
      if (!z.isRight && currentPageNextY_ < z.bottom && currentPageNextY_ + lineHeight > z.top) {
        xOffset = static_cast<int16_t>(xOffset + z.width);
      }
    }
  }
  currentPage_->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY_));

  // On the first line of a block with a deferred inline float image, fix the image's yPos to the
  // line top.
  if (isFirstLineOfBlock && deferredPageImage_) {
    deferredPageImage_->yPos = static_cast<int16_t>(currentPageNextY_);
    deferredPageImage_.reset();
  }

  currentPageNextY_ += lineHeight;
  return static_cast<int>(ParsedText::LineProcessResult::Accepted);
}

// Lay out the current text block into lines and place them: font resolution, CSS margin collapse
// with the previous block, effective width from insets, active-float zone propagation across
// blocks the float spans, line breaking (via addLineToPage), and bottom spacing.
void LayoutSink::makePages() {
  if (layoutFailed_) {
    currentTextBlock_.reset();
    return;
  }
  if (!currentTextBlock_) return;

  if (!currentPage_) {
    currentPage_.reset(new Page());
    currentPageNextY_ = 0;
  }

  if (!currentTextBlock_->isContinuation()) {
    currentTextBlock_->foldUniformWordSizes();
  }
  resolveBlockFont(currentTextBlock_->getBlockStyle());

  const BlockStyle& blockStyle = currentTextBlock_->getBlockStyle();
  const int lineHeight = effectiveLineHeight(blockStyle);

  // CSS fragmentation: the FIRST box on a page contributes no top spacing (a top margin/padding is
  // truncated at a page break). Only a non-first, non-continuation block applies its top margin.
  const bool firstOnPage = currentPage_ && currentPage_->elements.empty();
  if (!currentTextBlock_->isContinuation() && !firstOnPage) {
    if (blockStyle.marginTop > 0) {
      const int16_t collapse = std::min(lastBlockMarginBottom_, blockStyle.marginTop);
      currentPageNextY_ += static_cast<int16_t>(blockStyle.marginTop - collapse);
    }
    if (blockStyle.paddingTop > 0) {
      currentPageNextY_ += blockStyle.paddingTop;
    }
  }
  lastBlockMarginBottom_ = 0;

  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth_) ? static_cast<uint16_t>(viewportWidth_ - horizontalInset) : viewportWidth_;

  // Active-float propagation. A tall float spans several blocks: it is attached to the first
  // (originating) block; here we re-inject the same zone into every later block that still overlaps
  // it vertically, then drop it once layout passes the image bottom.
  const bool isOriginatingBlock = static_cast<bool>(deferredPageImage_);
  if (activeFloatBottom_ > 0 && currentPageNextY_ >= activeFloatBottom_) {
    activeFloatBottom_ = 0;  // layout has moved past the image
  }
  if (!isOriginatingBlock && activeFloatBottom_ > 0 && currentPageNextY_ < activeFloatBottom_ &&
      currentTextBlock_->getBlockStyle().floatZoneCount == 0) {
    BlockStyle& mbs = currentTextBlock_->getBlockStyle();
    auto& z = mbs.floatZones[mbs.floatZoneCount++];
    z.top = activeFloatTop_;  // absolute (already-anchored) image coordinates
    z.bottom = activeFloatBottom_;
    z.width = activeFloatWidth_;
    z.isRight = activeFloatIsRight_;
  }

  // Pre-correct float zone coordinates before line-breaking so widthForLine and the xOffset
  // check in addLineToPage use the same y values. Only the originating block re-anchors its zone
  // (and the image) to the first line top; injected zones already carry absolute image coordinates.
  const int lineHeightForFloat = (blockStyle.floatZoneCount > 0) ? effectiveLineHeight(blockStyle) : 0;
  if (isOriginatingBlock && blockStyle.floatZoneCount > 0) {
    BlockStyle& mbs = currentTextBlock_->getBlockStyle();
    for (int zi = 0; zi < mbs.floatZoneCount; ++zi) {
      const int imgH = mbs.floatZones[zi].bottom - mbs.floatZones[zi].top;
      mbs.floatZones[zi].top = static_cast<int16_t>(currentPageNextY_);
      mbs.floatZones[zi].bottom = static_cast<int16_t>(currentPageNextY_ + imgH);
    }
    activeFloatTop_ = static_cast<int16_t>(currentPageNextY_);
    activeFloatBottom_ = static_cast<int16_t>(currentPageNextY_ + (mbs.floatZones[0].bottom - mbs.floatZones[0].top));
  }

  currentTextBlock_->layoutAndExtractLines(
      renderer_, fontId_, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock, const bool lineEndsWithHyphenatedWord,
             const bool suppressHyphenationRetry) {
        return static_cast<ParsedText::LineProcessResult>(
            addLineToPage(textBlock, lineEndsWithHyphenatedWord, suppressHyphenationRetry));
      },
      /*includeLastLine=*/true, static_cast<int16_t>(currentPageNextY_), lineHeightForFloat);

  // Fallback: transfer any remaining pending footnotes to the current page. Catches edge cases
  // where a footnote's word index equals the exact block size.
  if (!pendingFootnotes_.empty() && currentPage_) {
    for (const auto& [idx, fn] : pendingFootnotes_) {
      currentPage_->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes_.clear();
  }

  if (blockStyle.marginBottom > 0) {
    currentPageNextY_ += blockStyle.marginBottom;
    lastBlockMarginBottom_ = blockStyle.marginBottom;
  } else {
    lastBlockMarginBottom_ = 0;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY_ += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing, suppressed inside <pre> (kPreformatted) so code/preformatted lines
  // are single-spaced.
  if (extraParagraphSpacing_ && !currentBlockPreformatted_) {
    currentPageNextY_ += lineHeight / 2;
  }
}

// Abbreviate each inline footnote preview run to the viewport width, at layout time (Stage-1
// stores the FULL preview text; the width budget is font/viewport-dependent so it belongs here).
// For each run, keep whole note words within a viewport*2 px budget and append an ellipsis when
// some were dropped. Measures BARE note words (parens stripped) so the budget is computed on the
// note text itself, then re-fuses the parens onto the kept boundary words.
void LayoutSink::abbreviatePreviewRuns(Block& block) const {
  abbreviateFootnotePreviews(block, renderer_, fontId_, viewportWidth_);
}

void LayoutSink::layoutTextBlock(Block&& block, const BlockStyle& blockStyle) {
  abbreviatePreviewRuns(block);
  currentBlockPreformatted_ = (block.flags & kPreformatted) != 0;
  currentTextBlock_.reset(
      new (std::nothrow) ParsedText(extraParagraphSpacing_, hyphenationEnabled_, blockStyle, bionicReadingEnabled_));
  if (!currentTextBlock_) return;
  wordsExtractedInBlock_ = 0;

  // A float image rides on this Text block: attach it before layout so its zone is present when
  // the first line breaks. The image's own CSS is not transmitted for floats — the intrinsic dims
  // + default sizing suffice (the float gate already capped size), so pass an empty style.
  if (!block.inlineImageEntryPath.empty()) {
    attachFloatImage(block, CssStyle{}, currentTextBlock_->getBlockStyle());
  }

  // Add words through ParsedText::addWord, flushing an intermediate line-break pass every >96 words
  // (a mid-block memory bound) so page breaks land at the same word positions regardless of how the
  // block was serialized (a block split into 8 KB records is coalesced before it reaches here).
  for (const Word& w : block.words) {
    const char* text = &block.text[w.textOff];
    const EpdFontFamily::Style fontStyle = spanToFontStyle(w.styleSpan);
    const bool attach = (w.styleSpan & kSpanAttachPrev) != 0;
    currentTextBlock_->addWord(text, fontStyle, /*underline=*/false, attach, w.sizePct);

    if (currentTextBlock_->size() > 96) {
      auto& splitBlockStyle = currentTextBlock_->getBlockStyle();
      resolveBlockFont(splitBlockStyle);
      const int horizontalInset = splitBlockStyle.totalHorizontalInset();
      const uint16_t effectiveWidth =
          (horizontalInset < viewportWidth_) ? static_cast<uint16_t>(viewportWidth_ - horizontalInset) : viewportWidth_;
      currentTextBlock_->layoutAndExtractLines(
          renderer_, fontId_, effectiveWidth,
          [this](const std::shared_ptr<TextBlock>& textBlock, const bool lineEndsWithHyphenatedWord,
                 const bool suppressHyphenationRetry) {
            return static_cast<ParsedText::LineProcessResult>(
                addLineToPage(textBlock, lineEndsWithHyphenatedWord, suppressHyphenationRetry));
          },
          /*includeLastLine=*/false, static_cast<int16_t>(currentPageNextY_), /*lineHeight=*/0);
    }
  }
  makePages();
}

std::string LayoutSink::nextImageCachePath(const std::string& entryPath) {
  std::string ext;
  const size_t extPos = entryPath.rfind('.');
  if (extPos != std::string::npos) ext = entryPath.substr(extPos);
  return imageBasePath_ + std::to_string(imageCounter_++) + ext;
}

// A standalone (centered, full-width) block image. The pending empty-block style (figure/div/h1
// wrapper margins accumulated by onBlock's empty-block merge) becomes the image's surrounding
// spacing.
void LayoutSink::placeBlockImage(const Block& block, const CssStyle& imgStyle) {
  // Container width for CSS sizing = viewport minus the pending block's horizontal inset.
  int containerWidth = viewportWidth_;
  if (hasPendingMerge_) {
    const int inset = pendingMergeStyle_.totalHorizontalInset();
    if (inset > 0 && inset < viewportWidth_) containerWidth = viewportWidth_ - inset;
  }
  const float emSize = static_cast<float>(renderer_.getFontAscenderSize(fontId_));
  const ImageDisplaySize ds = computeImageDisplaySize(block.width, block.height, imgStyle, viewportWidth_,
                                                      viewportHeight_, containerWidth, emSize);

  // Spacing from the pending empty-block wrapper style.
  int spacingTop = 0;
  int spacingBottom = 0;
  if (hasPendingMerge_) {
    spacingTop = std::max(0, static_cast<int>(pendingMergeStyle_.marginTop)) +
                 std::max(0, static_cast<int>(pendingMergeStyle_.paddingTop));
    spacingBottom = std::max(0, static_cast<int>(pendingMergeStyle_.marginBottom)) +
                    std::max(0, static_cast<int>(pendingMergeStyle_.paddingBottom));
  }
  // The pending wrapper spacing is consumed around the image; clear it so it does not leak into
  // the next paragraph.
  hasPendingMerge_ = false;
  pendingMergeFromBr_ = false;

  const int totalHeight = spacingTop + ds.height + spacingBottom;
  if (currentPage_ && !currentPage_->elements.empty() && (currentPageNextY_ + totalHeight > viewportHeight_)) {
    emitPage(lastBodyChildByteOffset_);
  } else if (!currentPage_) {
    currentPage_.reset(new Page());
    currentPageNextY_ = 0;
  }

  currentPageNextY_ += static_cast<int16_t>(spacingTop);

  const std::string cachePath = nextImageCachePath(block.entryPath);
  auto imageBlock =
      std::make_shared<ImageBlock>(cachePath, static_cast<int16_t>(ds.width), static_cast<int16_t>(ds.height),
                                   block.alt, epubFilePath_, block.entryPath);
  const int16_t xPos = static_cast<int16_t>((viewportWidth_ - ds.width) / 2);
  currentPage_->elements.push_back(std::make_shared<PageImage>(imageBlock, xPos, currentPageNextY_));
  currentPageNextY_ += static_cast<int16_t>(ds.height);
  currentPageNextY_ += static_cast<int16_t>(spacingBottom);
}

std::unique_ptr<ParsedText> LayoutSink::buildCellText(const TableCell& cell) const {
  // Table cells are built with NO paragraph spacing and NO hyphenation. Using the sink's profile
  // flags here would give cells a first-line indent / hyphenation the grid and the paragraph
  // fallback never apply.
  auto pt = std::unique_ptr<ParsedText>(new ParsedText(/*extraParagraphSpacing=*/false,
                                                       /*hyphenationEnabled=*/false));
  for (const Word& w : cell.words) {
    const char* text = &cell.text[w.textOff];
    const EpdFontFamily::Style fontStyle = spanToFontStyle(w.styleSpan);
    const bool attach = (w.styleSpan & kSpanAttachPrev) != 0;
    pt->addWord(text, fontStyle, /*underline=*/false, attach, w.sizePct);
  }
  return pt;
}

// Paragraph fallback: each non-empty cell's text becomes a sequential paragraph laid out at full
// viewport width.
void LayoutSink::placeTableAsParagraphs(const Block& block) {
  for (const TableRow& row : block.rows) {
    for (const TableCell& cell : row.cells) {
      if (cell.words.empty()) continue;
      // Each cell paragraph keeps the ParsedText default BlockStyle (Justify, no indent), NOT
      // paragraphAlignment — the fallback opens a block per cell but lays out the cell's own text.
      // wordsExtractedInBlock resets per cell.
      wordsExtractedInBlock_ = 0;
      auto cellText = buildCellText(cell);
      cellText->layoutAndExtractLines(renderer_, fontId_, viewportWidth_,
                                      [this](const std::shared_ptr<TextBlock>& tb, const bool h, const bool s) {
                                        return static_cast<ParsedText::LineProcessResult>(addLineToPage(tb, h, s));
                                      });
    }
  }
}

// Grid table layout. Wraps each cell, computes row heights, then packs fragments via the shared
// helper. Cell images are NOT reproduced: the producer does not transmit their intrinsic dims, and
// no corpus book uses grid cell images. Falls back to paragraphs when the grid can't be built
// (too-narrow columns, merged non-full-width cells, over-tall rows).
void LayoutSink::placeTable(const Block& block) {
  if (block.rows.empty()) {
    placeTableAsParagraphs(block);
    return;
  }
  uint8_t columnCount = 0;
  for (const TableRow& r : block.rows) columnCount = std::max(columnCount, static_cast<uint8_t>(r.cells.size()));
  // colSpan-aware column count: the max effective columns across rows.
  for (const TableRow& r : block.rows) {
    uint8_t eff = 0;
    for (const TableCell& c : r.cells) eff = static_cast<uint8_t>(eff + c.colSpan);
    columnCount = std::max(columnCount, eff);
  }
  if (columnCount == 0 || columnCount > MAX_TABLE_COLS) {
    placeTableAsParagraphs(block);
    return;
  }

  const uint16_t totalWidth = viewportWidth_;
  const uint16_t colWidth = totalWidth / columnCount;
  const uint16_t innerColWidth =
      (colWidth > 2 * TABLE_CELL_PADDING) ? static_cast<uint16_t>(colWidth - 2 * TABLE_CELL_PADDING) : 0;
  if (innerColWidth < MIN_COL_INNER_WIDTH) {
    placeTableAsParagraphs(block);
    return;
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(fontId_) * lineCompression_ + 0.5f);
  const bool hasBorder = block.hasBorder;

  std::vector<TableLayoutRow> layoutRows;
  layoutRows.reserve(block.rows.size());
  bool needsParagraphFallback = false;

  for (const TableRow& bufRow : block.rows) {
    const bool isFullWidthSpan = bufRow.cells.size() == 1 && bufRow.cells[0].colSpan == columnCount;
    const uint8_t renderCols = isFullWidthSpan ? 1 : columnCount;
    const uint16_t renderColWidth = totalWidth / renderCols;
    const uint16_t renderInnerWidth =
        (renderColWidth > 2 * TABLE_CELL_PADDING) ? static_cast<uint16_t>(renderColWidth - 2 * TABLE_CELL_PADDING) : 0;

    const bool hasMergedCell =
        std::any_of(bufRow.cells.begin(), bufRow.cells.end(), [](const TableCell& c) { return c.colSpan != 1; });
    if (hasMergedCell && !isFullWidthSpan) {
      needsParagraphFallback = true;
      break;
    }

    TableLayoutRow lr;
    lr.isHeaderRow = bufRow.isHeaderRow;
    lr.renderCols = renderCols;
    lr.cells.reserve(renderCols);
    uint16_t maxContentHeight = 0;

    for (const TableCell& bufCell : bufRow.cells) {
      ::TableCell cell;
      cell.isHeader = bufCell.isHeader;
      if (!bufCell.words.empty()) {
        auto cellText = buildCellText(bufCell);
        cellText->layoutAndExtractLines(renderer_, fontId_, renderInnerWidth,
                                        [&cell](const std::shared_ptr<TextBlock>& tb, bool, bool) {
                                          if (cell.lines.size() < MAX_CELL_LINES) cell.lines.push_back(tb);
                                          return ParsedText::LineProcessResult::Accepted;
                                        });
      }
      // Cell images intentionally not reproduced (see the method comment).
      uint16_t contentHeight = static_cast<uint16_t>(cell.lines.size() * lineHeight);
      if (contentHeight > maxContentHeight) maxContentHeight = contentHeight;
      lr.cells.push_back(std::move(cell));
    }
    if (!isFullWidthSpan) {
      while (lr.cells.size() < columnCount) lr.cells.emplace_back();
    }
    if (maxContentHeight == 0) maxContentHeight = static_cast<uint16_t>(lineHeight);
    lr.height = static_cast<uint16_t>(maxContentHeight + 2 * TABLE_CELL_PADDING);
    layoutRows.push_back(std::move(lr));
  }

  if (needsParagraphFallback) {
    placeTableAsParagraphs(block);
    return;
  }

  struct SinkTableCtx final : TablePageContext {
    LayoutSink* self;
    int currentY() const override { return self->currentPageNextY_; }
    void ensurePage() override {
      if (!self->currentPage_) {
        self->currentPage_.reset(new Page());
        self->currentPageNextY_ = 0;
      }
    }
    void emitPageAndReset() override { self->emitPage(self->lastBodyChildByteOffset_); }
    void advanceY(int delta) override {
      self->currentPageNextY_ = static_cast<int16_t>(self->currentPageNextY_ + delta);
    }
    void pushFragment(uint8_t cols, uint16_t tw, uint16_t th, std::vector<::TableRow>&& rows, int16_t yPos,
                      bool border) override {
      self->currentPage_->elements.push_back(
          std::make_shared<PageTableFragment>(cols, tw, th, std::move(rows), /*xPos=*/0, yPos, border));
    }
    void onOversizeRow(const TableLayoutRow& lr) override {
      // Over-tall row → flatten to paragraphs. Rebuild a minimal compiled block from the wrapped
      // lines' words and route through placeTableAsParagraphs (cell images are already dropped).
      Block fb;
      fb.type = BlockType::Table;
      TableRow r;
      r.isHeaderRow = lr.isHeaderRow;
      for (const auto& c : lr.cells) {
        TableCell cc;
        cc.isHeader = c.isHeader;
        for (const auto& line : c.lines) {
          for (uint16_t i = 0; i < line->wordCount(); ++i) {
            Word w;
            w.textOff = static_cast<uint32_t>(cc.text.size());
            cc.words.push_back(w);
            cc.text.append(line->wordText(i));
            cc.text.push_back('\0');
          }
        }
        r.cells.push_back(std::move(cc));
      }
      fb.rows.push_back(std::move(r));
      self->placeTableAsParagraphs(fb);
    }
  } ctx;
  ctx.self = this;
  packTableFragments(layoutRows, totalWidth, viewportHeight_, hasBorder, ctx);
}

void LayoutSink::placeHr() {
  // A pending empty-merge block before the rule still advances Y by its margins + extra paragraph
  // spacing (e.g. the empty block between two consecutive <hr/>s). Materialize and lay it out (no
  // words → just its spacing) before the rule.
  bool carryBrGap = false;
  if (hasPendingMerge_) {
    currentBlockPreformatted_ = false;
    currentTextBlock_.reset(new (std::nothrow) ParsedText(extraParagraphSpacing_, hyphenationEnabled_,
                                                          pendingMergeStyle_, bionicReadingEnabled_));
    // Laying out the empty block above does NOT clear its fromBrElement flag, so when the empty
    // <br> block preceded the HR, a br-gap is owed to the block that FOLLOWS the rule as well as
    // the one before it. Carry the br-gap forward onto the post-HR block.
    carryBrGap = pendingMergeFromBr_;
    hasPendingMerge_ = false;
    pendingMergeFromBr_ = false;
    if (currentTextBlock_) {
      wordsExtractedInBlock_ = 0;
      makePages();
    }
  }

  if (!currentPage_) {
    currentPage_.reset(new Page());
    currentPageNextY_ = 0;
  }
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(fontId_) * lineCompression_ + 0.5f);
  const int16_t marginV = static_cast<int16_t>(lineHeight / 2);
  currentPageNextY_ += marginV;
  if (currentPageNextY_ + 1 + marginV > viewportHeight_) {
    emitPage(lastBodyChildByteOffset_);
    currentPage_.reset(new Page());
    currentPageNextY_ = 0;
  }
  const int16_t hrWidth = static_cast<int16_t>(viewportWidth_ / 2);
  const int16_t hrX = static_cast<int16_t>(viewportWidth_ / 4);
  currentPage_->elements.push_back(std::make_shared<PageHR>(hrX, currentPageNextY_, hrWidth));
  currentPageNextY_ += 1 + marginV;

  // Carry the owed br-gap forward as a pending fromBr merge, so the next onBlock() adds a blank
  // line's worth of top margin to the following paragraph (the empty-block fold applies the gap
  // when pendingMergeFromBr_). Neutral style — only the br-gap contribution matters here.
  if (carryBrGap) {
    pendingMergeStyle_ = BlockStyle{};
    pendingMergeStyle_.fromBrElement = true;
    pendingMergeFromBr_ = true;
    hasPendingMerge_ = true;
  }
}

void LayoutSink::attachFloatImage(const Block& block, const CssStyle& imgStyle, BlockStyle& bs) {
  if (!currentPage_) currentPage_.reset(new (std::nothrow) Page());

  // The producer transmits INTRINSIC dims; recompute the display dims via the shared helper (the
  // same computation the walk's float-candidate gate used) before using them as the float size.
  int containerWidth = viewportWidth_;
  const int inset = bs.totalHorizontalInset();
  if (inset > 0 && inset < viewportWidth_) containerWidth = viewportWidth_ - inset;
  const float emSize = static_cast<float>(renderer_.getFontAscenderSize(fontId_));
  const ImageDisplaySize ds = computeImageDisplaySize(block.inlineImageWidth, block.inlineImageHeight, imgStyle,
                                                      viewportWidth_, viewportHeight_, containerWidth, emSize);
  const int16_t imgH = static_cast<int16_t>(ds.height);
  const int16_t imgW = static_cast<int16_t>(ds.width);
  const bool imgIsRight = (block.inlineImageSide == 2);

  // A float never crosses a page boundary: break first if it would not fit.
  if (imgH > static_cast<int16_t>(viewportHeight_ - currentPageNextY_) && currentPage_ &&
      !currentPage_->elements.empty()) {
    emitPage(lastBodyChildByteOffset_);
  }

  const int16_t imgX = imgIsRight ? static_cast<int16_t>(viewportWidth_ - imgW) : 0;
  const int16_t top = static_cast<int16_t>(currentPageNextY_);

  const std::string cachePath = nextImageCachePath(block.inlineImageEntryPath);
  auto fullImageBlock = std::make_shared<ImageBlock>(cachePath, imgW, imgH, block.inlineImageAlt, epubFilePath_,
                                                     block.inlineImageEntryPath);
  deferredPageImage_ = std::make_shared<PageImage>(fullImageBlock, imgX, top);
  currentPage_->elements.push_back(deferredPageImage_);

  if (bs.floatZoneCount < BlockStyle::kMaxFloatZones) {
    auto& z = bs.floatZones[bs.floatZoneCount++];
    z.top = top;
    z.bottom = static_cast<int16_t>(top + imgH);
    z.width = static_cast<int16_t>(imgW + 4);
    z.isRight = imgIsRight;
  }
  activeFloatTop_ = top;
  activeFloatBottom_ = static_cast<int16_t>(top + imgH);
  activeFloatWidth_ = static_cast<int16_t>(imgW + 4);
  activeFloatIsRight_ = imgIsRight;
}

// --- BlockSink overrides. ---

void LayoutSink::onBlock(Block&& block, const CssStyle& style) {
  // A TOC-boundary (or CSS page-break-before) block starts a fresh page — but never a blank leading
  // page, so only break when the current page already has content.
  if ((block.flags & kPageBreakBefore) && currentPage_ && !currentPage_->elements.empty()) {
    emitPage(lastBodyChildByteOffset_);
  }

  if (block.type == BlockType::Image) {
    // A standalone (centered) block image. floatSide != 0 would be a float, but the producer
    // emits floats as fields on the following Text block, not as Image blocks — so every Image
    // block here is a centered block image.
    placeBlockImage(block, style);
    return;
  }
  if (block.type == BlockType::Hr) {
    placeHr();
    return;
  }
  if (block.type == BlockType::Table) {
    placeTable(block);
    return;
  }
  if (block.type != BlockType::Text) {
    return;
  }

  const bool isHeading = (block.flags & kStartsChapter) != 0;
  const bool fromBr = (block.flags & kFromBrElement) != 0;

  // A <br> block inherits the alignment CONTEXT of the currently-open block at the <br> — which,
  // when an empty block is being reused, is that empty block's alignment AFTER the empty-block reset
  // (see below). Model it as the live merge context: the pending empty chain's alignment if one is
  // open, else the last laid-out block's. NOT lastBlockAlignment_ alone, which ignores intervening
  // (reset) empty blocks and wrongly carried a heading's Center across the chain (a Moby Dick
  // <p class="toc"> regression this fixed).
  const CssTextAlign contextAlign = hasPendingMerge_ ? pendingMergeStyle_.alignment : lastBlockAlignment_;
  const bool contextAlignDefined = hasPendingMerge_ ? pendingMergeStyle_.textAlignDefined : lastBlockAlignmentDefined_;

  BlockStyle blockStyle;
  if (fromBr) {
    // EVERY <br> block gets a NEUTRAL layout style: only the current alignment context, never the
    // element's CSS margins — those would over-space and (for an empty <br>) mis-place the injected
    // line-gap. This holds whether the block stays empty (a
    // section separator) or later receives text (an inline <br>, or a poem line). The one CSS
    // property a <br> block DOES carry is a span-level text-indent (poem stanza pattern): it is
    // applied to the open <br> block after brStyle, so the producer transmits it via textIndent.
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

  // Empty wrapper / <br> transcript block: don't lay it out. Accumulate its style into a pending
  // merge that folds into the next non-empty block — CSS margin collapsing across empty wrapper
  // elements (e.g. <div style="margin:2em"><h1>…</h1></div>).
  if (block.words.empty()) {
    // Empty-block ALIGNMENT RESET (issue #1026): on closing a header/block tag that stayed empty,
    // clear the block's alignment so a centered empty heading does not bleed its (Center,
    // textAlignDefined=true) alignment through the empty-block-merge chain into the next paragraph.
    // The reset fires for every empty block EXCEPT a <br> (which preserves alignment for text in the
    // same container). This reset alignment also becomes the context a following <br> inherits
    // (above). Alignment only; margins/padding accumulate through the merge untouched. Done here (a
    // tag-close layout mutation) because the producer can't transmit it.
    if (!blockStyle.fromBrElement) {
      blockStyle.textAlignDefined = false;
      blockStyle.alignment = (paragraphAlignment_ == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(paragraphAlignment_);
    }
    if (hasPendingMerge_) {
      // Chain of consecutive empty blocks: combine their styles into the running merge.
      BlockStyle incoming = blockStyle;
      if (pendingMergeFromBr_) {
        const int16_t lh = static_cast<int16_t>(renderer_.getLineHeight(fontId_) * lineCompression_ + 0.5f);
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lh);
      }
      pendingMergeStyle_ = pendingMergeStyle_.getCombinedBlockStyle(incoming);
    } else {
      pendingMergeStyle_ = blockStyle;
    }
    pendingMergeFromBr_ = blockStyle.fromBrElement;
    hasPendingMerge_ = true;
    return;
  }

  // Non-empty block: fold any pending empty-block merge into its style first.
  if (hasPendingMerge_) {
    BlockStyle incoming = blockStyle;
    if (pendingMergeFromBr_) {
      const int16_t lh = static_cast<int16_t>(renderer_.getLineHeight(fontId_) * lineCompression_ + 0.5f);
      incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lh);
    }
    blockStyle = pendingMergeStyle_.getCombinedBlockStyle(incoming);
    hasPendingMerge_ = false;
    pendingMergeFromBr_ = false;
  }

  // Track the alignment context for a subsequent <br> block (which reads it from the current
  // block). Captured from the final merged style actually laid out.
  lastBlockAlignment_ = blockStyle.alignment;
  lastBlockAlignmentDefined_ = blockStyle.textAlignDefined;

  layoutTextBlock(std::move(block), blockStyle);
}

void LayoutSink::onAnchor(const std::string& id) { pendingAnchorId_ = id; }

void LayoutSink::onChapter(uint8_t /*level*/, const std::string& /*title*/) {}

void LayoutSink::onPageBreakLabel(const std::string& label) {
  if (label.empty()) return;
  pageBreakLabels_.emplace_back(static_cast<uint16_t>(completedPageCount_), label);
}

void LayoutSink::onFootnote(int wordIndex, const FootnoteEntry& entry) {
  // Buffered until layout reaches this word position (assigned in addLineToPage). wordIndex is
  // relative to the block currently being built.
  pendingFootnotes_.push_back({wordIndex, entry});
}

void LayoutSink::onXPathAdvance(uint16_t paragraphIndex, uint16_t listItemIndex, uint32_t bodyChildByteOffset) {
  // The walk's counters as of the block about to be laid out; emitPage records them into the LUT.
  xpathParagraphIndex_ = paragraphIndex;
  xpathListItemIndex_ = listItemIndex;
  lastBodyChildByteOffset_ = bodyChildByteOffset;
}

void LayoutSink::onSpineEnd() {
  // Flush the last accumulated text block.
  if (currentTextBlock_) {
    makePages();
    if (layoutFailed_) return;
  }
  // Emit the final page if it has content — independent of whether a text block is open. A spine
  // whose only element is an image/table/HR (e.g. a cover page) has no currentTextBlock_ but still
  // has a page to flush (the sink only creates a text block when a Text block arrives).
  if (currentPage_ && !currentPage_->elements.empty()) {
    emitPage(0u);
  }
}

bool replaySpineBegin(BlockStreamReader& reader, uint32_t spineIndex, SpineReplayCursor& cur) {
  cur = SpineReplayCursor{};
  cur.spineIndex = spineIndex;
  if (!reader.openSpine(spineIndex)) {
    cur.ok = false;
    return false;
  }
  cur.started = true;
  return true;
}

bool replaySpineStep(BlockStreamReader& reader, LayoutSink& sink, uint32_t maxBlocks, SpineReplayCursor& cur) {
  if (!cur.started || cur.finished || !cur.ok) return false;
  // openSpine (via replaySpineBegin) loaded these for the spine; they persist across slices because
  // the reader's per-spine state is unchanged between nextLogicalBlock calls.
  const auto& anchors = reader.spineAnchors();  // keyed by first-record index of a logical block
  const auto& labels = reader.spineLabels();
  const auto& stylePool = reader.spineStylePool();  // v6: this spine's own local pool
  const auto& chapters = reader.spineChapters();    // v6: this spine's own chapter entries

  uint32_t done = 0;
  Block lb;
  while (reader.nextLogicalBlock(lb)) {
    const uint32_t bi = reader.currentFirstRecordIndex();
    // anchors/labels/footnotes/xpath fire BEFORE onBlock (accumulated during the block's build);
    // chapters fire AFTER onBlock. Read lb's fields before the move into onBlock.
    for (const auto& a : anchors)
      if (a.blockIndex == bi) sink.onAnchor(a.id);
    for (const auto& pl : labels)
      if (pl.blockIndex == bi) sink.onPageBreakLabel(pl.label);
    for (const auto& fn : lb.footnotes) sink.onFootnote(static_cast<int>(fn.wordIndex), fn.entry);
    if (lb.hasXPath) sink.onXPathAdvance(lb.xpath.paragraphIndex, lb.xpath.listItemIndex, lb.xpath.bodyChildByteOffset);
    static const CssStyle kEmptyStyle{};
    const CssStyle& style = (lb.styleId < stylePool.size()) ? stylePool[lb.styleId] : kEmptyStyle;
    sink.onBlock(std::move(lb), style);
    for (const auto& ch : chapters)
      if (ch.blockIndex == bi) sink.onChapter(ch.level, ch.title);
    if (maxBlocks != 0 && ++done >= maxBlocks) return true;  // yield mid-spine; more blocks remain
  }
  if (!reader.ok()) {
    cur.ok = false;
    return false;
  }
  sink.onSpineEnd();
  cur.finished = true;
  return false;  // spine complete
}

bool replaySpine(BlockStreamReader& reader, uint32_t spineIndex, LayoutSink& sink) {
  SpineReplayCursor cur;
  if (!replaySpineBegin(reader, spineIndex, cur)) return false;
  // Run to completion in one call (maxBlocks == 0).
  replaySpineStep(reader, sink, /*maxBlocks=*/0, cur);
  return cur.finished && cur.ok;
}

}  // namespace compiled
