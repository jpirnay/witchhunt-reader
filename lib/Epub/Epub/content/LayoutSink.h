#pragma once
// LayoutSink — the Stage-2 (measure + paginate) consumer of the walk's BlockSink stream
// (docs/compiled-book-pipeline-plan.md Phase 3; design in docs/parser-stage1-step5-design.md).
//
// Given the block stream onBlock(Block&&, CssStyle&) / onAnchor / onChapter / onFootnote /
// onPageBreakLabel / onXPathAdvance / onSpineEnd, LayoutSink produces the sequence of Page objects
// for a spine (delivered via completePageFn) and the anchor / page-label / paragraph-LUT tables.
// It owns all the settings-dependent layout state (current text block, current page, float state,
// margin-merge state) and holds a GfxRenderer& for word measurement.
//
// This is the SOLE layout path: the parser's XHTML walk drives a LayoutSink directly on the
// device/section-cache build, and the content.bin replay drives one from persisted records
// (test/epub_pipeline). The stream is settings-independent; all pixel/position resolution is
// LayoutSink's job, so a font/margin/hyphenation/bionic change re-runs only Stage-2.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BlockSink.h"
#include "CompiledContent.h"
#include "Epub/FontSizeLadder.h"
#include "Epub/FootnoteEntry.h"
#include "Epub/blocks/BlockStyle.h"
#include "Epub/css/CssStyle.h"

class GfxRenderer;
class Page;
class PageImage;
class ParsedText;
class TextBlock;

namespace compiled {

// The settings-dependent layout inputs (font, viewport, spacing, alignment, hyphenation, bionic,
// embedded-CSS honoring, size ladder, image/epub paths). Bundled as the sink's ctor args.
struct LayoutParams {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool bionicReadingEnabled = false;
  bool embeddedStyle = true;      // honor publisher CSS text-align (see the alignment resolution)
  FontSizeLadder fontSizeLadder;  // body-font sibling-size ladder (see resolveBlockFont)
  // Cache-path prefix for extracted images: <cache>/img_<spine>_<hash>_. The sink appends a
  // per-image counter + source extension, mirroring Section::getImageBasePath + imageCounter.
  // (Intrinsic dims ride on the compiled Block, so no image manifest is needed here.)
  std::string imageBasePath;
  std::string epubFilePath;  // for ImageBlock lazy extraction (epub->getPath())
};

// Per-page XPath LUT entry: the walk's paragraph/list-item counters + body-child byte offset at
// each page break, for reading-progress and xpath navigation. Field-compatible with
// ChapterHtmlSlimParser::ParagraphLutEntry (the getter adapts between them).
struct LayoutLutEntry {
  uint32_t xhtmlByteOffset = 0;
  uint16_t paragraphIndex = 0;
  uint16_t listItemIndex = 0;
};

class LayoutSink : public BlockSink {
 public:
  LayoutSink(GfxRenderer& renderer, LayoutParams params, std::function<void(std::unique_ptr<Page>)> completePageFn);
  ~LayoutSink() override;

  // BlockSink — driven by the walk during a Section build.
  void onBlock(Block&& block, const CssStyle& style) override;
  void onAnchor(const std::string& id) override;
  void onChapter(uint8_t level, const std::string& title) override;
  void onPageBreakLabel(const std::string& label) override;
  void onFootnote(int wordIndex, const FootnoteEntry& entry) override;
  void onXPathAdvance(uint16_t paragraphIndex, uint16_t listItemIndex, uint32_t bodyChildByteOffset) override;
  void onSpineEnd() override;

  // Side outputs the caller pulls today via the parser getters (getAnchors / etc.).
  const std::vector<std::pair<std::string, uint16_t>>& anchors() const { return anchorData_; }
  const std::vector<std::pair<uint16_t, std::string>>& pageBreakLabels() const { return pageBreakLabels_; }
  const std::vector<LayoutLutEntry>& paragraphLutPerPage() const { return paragraphLutPerPage_; }

 private:
  // --- Layout helpers, copied verbatim from ChapterHtmlSlimParser (renderer-local). ---
  void resolveBlockFont(BlockStyle& bs);
  int effectiveFontId(const BlockStyle& bs) const { return bs.headingFontId != 0 ? bs.headingFontId : fontId_; }
  int effectiveLineHeight(const BlockStyle& bs) const;
  void emitPage(uint32_t xhtmlByteOffset);
  // Layout the current block into pages (port of ChapterHtmlSlimParser::makePages, text path).
  void makePages();
  // Append one laid-out line to the current page (port of addLineToPage).
  int addLineToPage(const std::shared_ptr<TextBlock>& line, bool lineEndsWithHyphenatedWord,
                    bool suppressHyphenationRetry);
  // Reconstruct the px BlockStyle from the block's pre-px CssStyle. Alignment stays sink-side
  // (it depends on paragraphAlignment, a user setting): headings default to Center, blocks to
  // paragraphAlignment, and publisher text-align overrides when embeddedStyle. The heading
  // font-size multiplier is already folded into `style` by the producer (settings-independent).
  BlockStyle buildBlockStyle(const CssStyle& style, bool isHeading) const;
  // Rebuild a ParsedText from a materialized text block, adding words through the same
  // ParsedText::addWord path, replaying the >96-word mid-block flush.
  void layoutTextBlock(Block&& block, const BlockStyle& blockStyle);
  // Abbreviate each inline footnote preview run in `block` to the viewport width, in place.
  // Stage-1 stores the FULL preview text (settings-independent); the width-based abbreviation
  // (viewport*2 px budget, trailing ellipsis) is font/viewport-dependent so it happens here.
  void abbreviatePreviewRuns(Block& block) const;
  // Place a standalone (centered, full-width) block image: resolve display dims via the shared
  // helper, apply the pending-block spacing, page-break, and push a PageImage.
  void placeBlockImage(const Block& block, const CssStyle& imgStyle);
  // Emit a horizontal rule (centered 25%->75%, half-line margins above/below).
  void placeHr();
  // Lay out a Table block: choose grid vs paragraph fallback, wrap cells, pack fragments via the
  // shared compiled::packTableFragments.
  void placeTable(const Block& block);
  // Paragraph fallback: each cell's words become a sequential paragraph.
  void placeTableAsParagraphs(const Block& block);
  // Build a wrapped ParsedText for a compiled table cell's words at the given wrap width.
  std::unique_ptr<ParsedText> buildCellText(const TableCell& cell) const;
  // Allocate the next image cache path (imageBasePath + counter + ext).
  std::string nextImageCachePath(const std::string& entryPath);
  // Attach a block's inline float image (its inlineImage* fields) beside the paragraph text:
  // page-break if it won't fit, push a deferred PageImage, set the block's float zone and the
  // active-float state.
  void attachFloatImage(const Block& block, const CssStyle& imgStyle, BlockStyle& bs);

  GfxRenderer& renderer_;
  std::function<void(std::unique_ptr<Page>)> completePageFn_;

  // Cached from params_ for the hot layout math (mirrors the parser's scalar members).
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
  int imageCounter_ = 0;  // per-spine image counter

  // Empty-block merge (CSS margin collapsing across wrapper elements). The producer emits empty
  // wrapper / <br> blocks as a 1:1 transcript; we hold the accumulated merged style across a run
  // of empty blocks and fold it into the next non-empty block's style, collapsing their margins.
  bool hasPendingMerge_ = false;
  BlockStyle pendingMergeStyle_;
  bool pendingMergeFromBr_ = false;
  // Alignment context a following <br> block inherits (the current block's alignment at the <br>).
  CssTextAlign lastBlockAlignment_ = CssTextAlign::Justify;
  bool lastBlockAlignmentDefined_ = false;

  // Layout state.
  std::unique_ptr<ParsedText> currentTextBlock_;
  std::unique_ptr<Page> currentPage_;
  int16_t currentPageNextY_ = 0;
  int16_t lastBlockMarginBottom_ = 0;
  int32_t auxFontId_ = 0;
  int completedPageCount_ = 0;
  int wordsExtractedInBlock_ = 0;
  bool layoutFailed_ = false;
  bool currentBlockPreformatted_ = false;  // block is inside <pre> → suppress extra paragraph spacing

  // Float / deferred-image zone state.
  std::shared_ptr<PageImage> deferredPageImage_;
  int16_t activeFloatTop_ = 0;
  int16_t activeFloatBottom_ = 0;
  int16_t activeFloatWidth_ = 0;
  bool activeFloatIsRight_ = false;

  // Footnotes anchored to a word position in the block currently being laid out. onFootnote
  // buffers them (it fires while the block is built, before that block's onBlock); addLineToPage
  // assigns each to the page once layout reaches its word index, with a makePages tail fallback.
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes_;

  // Side-output tables (pulled by the caller after onSpineEnd).
  std::vector<std::pair<std::string, uint16_t>> anchorData_;
  std::string pendingAnchorId_;
  std::vector<std::pair<uint16_t, std::string>> pageBreakLabels_;
  std::vector<LayoutLutEntry> paragraphLutPerPage_;

  // XPath counters at page-break time (fed by onXPathAdvance), recorded into the per-page LUT.
  uint16_t xpathParagraphIndex_ = 0;
  uint16_t xpathListItemIndex_ = 0;
  uint32_t lastBodyChildByteOffset_ = 0;
};

}  // namespace compiled
