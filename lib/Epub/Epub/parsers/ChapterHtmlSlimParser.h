#pragma once

#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../EpubImageManifest.h"
#include "../FontSizeLadder.h"
#include "../FootnoteEntry.h"
#include "../FootnotePreviews.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class Page;
class PageImage;  // forward declaration — Page.h included in .cpp
class PageLine;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser final : public Print {
  std::shared_ptr<Epub> epub;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int skipTextUntilDepth = INT_MAX;  // skip character data inside synthetic zero-height spacer <p>
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int strikethroughUntilDepth = INT_MAX;
  int preUntilDepth = INT_MAX;  // set when inside a <pre> element; enables \n → line-break handling
  int svgDepth = 0;             // nesting counter for <svg> elements; text inside SVG is skipped (path data etc.)
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int16_t lastBlockMarginBottom = 0;  // tracks previous block's marginBottom for CSS margin collapsing

  // Inline image beside paragraph text (CSS float context)
  // Fixed-size arrays — no heap allocation. Float nesting > 4 is pathological in practice.
  static constexpr int kMaxFloatDepth = 4;
  int floatDepth_ = 0;
  int floatOpenDepths_[kMaxFloatDepth] = {};  // parser depth at which each float was opened
  bool floatOpenSides_[kMaxFloatDepth] = {};  // true = right float, false = left float
  struct PendingInlineImage {
    std::string cachedPath;
    std::string epubEntryPath;  // entry path within the EPUB zip
    int16_t width = 0;
    int16_t height = 0;
    std::string alt;
    bool active = false;
    bool isRight = false;  // true when float: right
    // epubFilePath is not stored — epub->getPath() is read at ImageBlock construction time
    // to avoid a redundant heap copy of a constant string.
  };
  PendingInlineImage pendingInlineImage_;         // active=true when a float-context image is deferred
  std::shared_ptr<PageImage> deferredPageImage_;  // the PageImage whose yPos needs updating

  // Drop cap: a left-floated span with a large font-size at the very start of a
  // paragraph (<p><span class="first-letter">A</span>ll ...). The letter is captured
  // while the span is open, then rendered as its own top-aligned PageLine with a
  // FloatZone so the paragraph's first lines wrap beside it — the same mechanism
  // used for left-floated inline images.
  static constexpr float kDropCapMinMultiplier = 1.95f;  // spans below this render inline
  static constexpr float kDropCapMaxMultiplier = 4.0f;   // ≈3 text lines tall
  static constexpr int16_t kDropCapGapPx = 6;            // horizontal gap between cap and text
  struct PendingDropCap {
    bool active = false;      // true while capturing the span's text
    int depth = 0;            // parser depth of the drop-cap span (pre-increment)
    float multiplier = 1.0f;  // composed font-size multiplier relative to the body font
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    char text[16] = {};  // drop caps are 1 glyph, occasionally with a leading quote
    int textLen = 0;
  };
  PendingDropCap pendingDropCap_;
  std::shared_ptr<PageLine> deferredDropCapLine_;  // the cap PageLine whose yPos needs updating
  // Offset from the paragraph's first-line top to the cap PageLine's yPos, so the cap's
  // INK top (not its leading-padded ascender top) aligns with the first line's ink top.
  int16_t dropCapYAdjust_ = 0;

  // Active float occupying the current page. A floated image never crosses a page
  // boundary (attachPendingFloatImage page-breaks first if it would not fit), so the
  // float lives entirely within one page. While currentPageNextY is above
  // activeFloatBottom_, makePages() injects this zone into every text block so the
  // caption AND the following paragraphs wrap beside the image — not just the one
  // block the image was attached to. Cleared once layout passes activeFloatBottom_
  // or on a page break. activeFloatBottom_ == 0 means no active float.
  int16_t activeFloatTop_ = 0;
  int16_t activeFloatBottom_ = 0;
  int16_t activeFloatWidth_ = 0;  // image width + gap
  bool activeFloatIsRight_ = false;
  int fontId;
  // Default heading multipliers (index 0=h1, 1=h2, 2=h3) applied when a heading has no
  // explicit CSS font-size; resolveBlockFont() then snaps them to the size ladder.
  static constexpr float kHeadingMultiplier[3] = {1.6f, 1.4f, 1.2f};
  // Sibling-size ladder of the body font (see FontSizeLadder). resolveBlockFont() snaps a
  // block's effective font size to the nearest real font on it; empty = scale-only fallback.
  FontSizeLadder fontSizeLadder_;
  // One non-body font per section: body regular/bold/italic plus one auxiliary regular is
  // exactly the FontDecompressor's four page slots. The first block to resolve off-body
  // claims the slot; blocks that would need a different font keep the scale fallback.
  int32_t auxFontId_ = 0;
  // EPUBs often set their running prose to a nominal CSS size such as 10pt,
  // 87%, or 0.875em. The reader's selected font size is our body size. Root
  // (html/body) sizes are normalized as inherited context; the main-text
  // baseline comes from tag-level paragraph/list rules so prose maps to 1.0
  // and headings/notes remain proportional to that prose size.
  float rootFontSizeBaseline_ = 1.0f;
  bool hasRootFontSizeBaseline_ = false;
  float mainTextFontSizeBaseline_ = 1.0f;
  bool hasMainTextFontSizeBaseline_ = false;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  // When true, widen the per-word font-size dead zone (±10% snaps publisher <span font-size:0.92em>
  // body wrappers back to native size); when false, only a tight ±3% dead zone is applied.
  bool fontSizeNormalization;
  const CssParser* cssParser;
  EpubImageManifest* imageManifest;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;
  bool lowMemoryImageFallback = false;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasSmallCaps = false, smallCaps = false;
    bool hasMarginLeft = false;
    int16_t marginLeftPx = 0;  // margin-left in pixels, for span-level poem indents
    // Inline font-size as a percent of the PARENT element's size (em semantics).
    // Nested entries compose multiplicatively in updateEffectiveInlineStyle().
    bool hasFontSize = false;
    uint8_t fontSizePct = 100;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  bool effectiveSup = false;
  bool effectiveSub = false;
  bool effectiveSmallCaps = false;
  int16_t effectiveInlineMarginLeft = 0;  // accumulated margin-left from inline span stack
  // Composed inline font-size percent (relative to the block font size) for the
  // words currently being flushed. 100 outside sized spans; clamped to the
  // ParsedText per-word range so it always fits the uint8_t word-size channel.
  uint8_t effectiveSizePct = 100;
  // Buffered table model — populated while inside <table>, emitted on </table>
  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    std::string imageSrc;  // first image found in this cell (empty if none)
    std::string imageAlt;
    bool isHeader = false;
    uint8_t colSpan = 1;
  };
  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool isHeaderRow = false;   // true when all cells in this row are <th>
    uint8_t effectiveCols = 0;  // sum of colSpan values; tracks actual column footprint
  };
  struct BufferedTable {
    std::vector<BufferedTableRow> rows;
    int depth = 0;             // nesting depth; > 1 means we're inside a nested table
    bool unsupported = false;  // true → emit as paragraphs instead of grid
    bool hasBorder = true;     // false when border="0" on the <table> element
    uint8_t maxCols = 0;       // max effectiveCols across all rows
    // Streaming mode: once a table can no longer become a grid (unsupported) or the heap has
    // run low, cells are emitted as paragraphs from </td> and freed immediately, so `rows`
    // never holds more than the cell currently being filled. Buffering the whole table only
    // exists because grid layout needs every cell measured before column widths are known;
    // when that layout is off the table, holding the cells is pure cost. Set only via
    // beginTableStreaming(), which first drains whatever was already buffered.
    bool streaming = false;
  };
  std::unique_ptr<BufferedTable> currentTable;
  BufferedTableCell* currentTableCell = nullptr;  // non-null while inside <td>/<th>

  struct ListEntry {
    int depth;
    bool isOrdered;
    int counter;
    bool suppressMarker = false;  // true when list-style-type: none
  };
  std::vector<ListEntry> listStack;

  // Ancestor block widths set via an explicit CSS `width` (e.g. a
  // <div style="width:100px"> wrapper). A percentage image width resolves against the
  // innermost such width, so a width:100% image inside a narrow box stays small
  // (matches KOReader) instead of filling the viewport. depth = parser depth at push
  // (pre-increment); popped in endElement when that scope closes.
  struct ContainerWidthEntry {
    int depth;
    int16_t width;
  };
  std::vector<ContainerWidthEntry> containerWidthStack_;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;

  // External printed-page labels sourced from NCX <pageList> or EPUB 3 nav page-list.
  // Keyed by HTML id (anchor fragment). When the parser encounters an element whose id
  // matches one of these, it records the label as if the element were an inline
  // doc-pagebreak marker. Anchors already labeled this way are not re-recorded if the
  // same element also carries an inline pagebreak attribute.
  std::vector<std::pair<std::string, std::string>> externalPageBreakAnchors;
  // Optional label for the start of this XHTML file (NCX entries with no fragment).
  std::string topOfFilePageLabel;
  bool topOfFilePageLabelEmitted = false;

  // Page break label mapping: stores the printed page label from EPUB pagebreak markers
  // and the section page index where that printed page begins.
  std::vector<std::pair<uint16_t, std::string>> pageBreakLabels;

  // Paragraph index tracking for XPath-to-page lookup table.
  // Counts <p> sibling indices (1-based, matching XPath convention) during page building.
  // Stored per page in the section cache so that XPath p[N] can be resolved to a page
  // without reparsing, and current page can generate an XPath without reparsing.
  uint16_t xpathParagraphIndex = 0;  // current <p> sibling index (1-based)
  // Running count of <li> elements opened anywhere in the chapter (1-based, any depth).
  // Used by the section LUT so KOReader-supplied list-item XPaths can snap to the exact
  // page on download, the same way <p>-anchored XPaths use xpathParagraphIndex.
  uint16_t xpathListItemIndex = 0;
  int xpathBodyDepth = -1;  // depth of the <body> element (-1 = not yet seen)
  // Byte offset of the most recent direct-body-child element start (any tag at xpathBodyDepth+1).
  // Recorded at the same depth condition that increments xpathParagraphIndex, so the stored
  // offset is guaranteed to land on a body-child element boundary. This keeps the XPath forward
  // mapper's partial-parse heuristic reliable for wrapped chapters: without this, the offset
  // could point mid-way into a nested <div>/<section>, which confuses partialBaseDepth.
  uint32_t lastBodyChildByteOffset = 0;

  struct ParagraphLutEntry {
    uint32_t xhtmlByteOffset;  // byte offset of most recent body-child element start at page break
    uint16_t paragraphIndex;   // 1-based <p> index at page completion
    uint16_t listItemIndex;    // running <li> count at page completion (any depth)
  };
  std::vector<ParagraphLutEntry> paragraphLutPerPage;  // deep LUT: one entry per page

  // Active parser for streaming. Stored as a member so page-break sites (addLineToPage,
  // image breaks) can call saxParser_.byteOffset() without threading the parser through
  // every call site.
  SaxParser saxParser_;

  // Streaming state for the Print-derived parsing API.
  size_t totalStreamSize = 0;
  size_t bytesStreamed = 0;
  int lastReportedProgress = -1;
  int progressStepPercent = 0;
  bool progressUiEnabled = true;
  bool streamFailed = false;
  uint32_t streamStartTimeMs = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  size_t currentFootnoteLinkTextLen = 0;
  // Non-owning; the Section's BuildState keeps the lookup alive across build slices.
  // Membership in the book-level preview cache is the sole expansion gate: it already
  // encodes "this link points at a real note", so no epub:type/same-file checks here.
  FootnotePreviews::Lookup* inlineFootnotePreviews = nullptr;
  std::string pendingInlineFootnotePreview;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;
  bool bionicReadingEnabled = false;
  bool layoutFailed = false;

  // Per-chapter caches: resolveStyle and parseInlineStyle are called for every HTML element;
  // caching by (tag|classAttr) and styleAttr avoids repeated string operations and hash lookups.
  std::unordered_map<std::string, CssStyle> cssStyleCache_;
  std::unordered_map<std::string, CssStyle> inlineStyleCache_;

  // Default size for superscript/subscript text, percent of the surrounding size.
  // Sup/sub scaling flows through the ordinary per-word size channel (the SUP/SUB
  // style bits only shift the baseline at render time); an explicit CSS font-size
  // on the element overrides this default.
  static constexpr uint8_t kSupSubDefaultSizePct = 50;

  void updateEffectiveInlineStyle();
  // Fold an element's CSS font-size (multiplier relative to its parent) into an inline
  // style-stack entry.
  static void applyCssFontSizeToEntry(StyleStackEntry& entry, const CssStyle& cssStyle);
  // Apply kSupSubDefaultSizePct when the entry resolves to sup/sub. Call BEFORE
  // applyCssFontSizeToEntry so publisher CSS (e.g. `.sup { font-size: 0.7em }`) wins.
  static void applySupSubDefaultSize(StyleStackEntry& entry);
  void initializeFontSizeBaseline();
  void observeFontSizeBaseline(const char* tagName, const CssStyle& cssStyle);
  CssStyle normalizeFontSizeForElement(const char* tagName, const CssStyle& cssStyle) const;
  bool ensureHeapForTextLayout(const char* phase);
  void startNewTextBlock(const BlockStyle& blockStyle);
  bool flushPartWordBuffer();
  void makePages();
  void emitBufferedTable();
  void emitTableAsFragments(BufferedTable& table);
  void emitTableAsParagraphs(BufferedTable& table);
  // Fallback path: emit each cell's image as a full-width block image below the table.
  void emitCellImagesAsBlocks(BufferedTable& table);
  // Emit one buffered cell as a paragraph (plus its image, when emitImage) and free the cell's
  // ParsedText before returning. Shared by the batch fallback and the streaming path so both
  // produce identical output. Returns false when the heap guard stopped the parse.
  bool emitCellAsParagraph(BufferedTableCell& cell, bool emitImage);
  // Switch an in-flight table to streaming mode: emit everything buffered so far as paragraphs,
  // free the row storage, and mark the table so later cells emit directly from </td> instead of
  // accumulating. Grid layout is already off the table by the time this is called.
  void beginTableStreaming(const char* reason);
  // Called on </td> while streaming: emit and release the just-closed cell immediately.
  void streamClosedCell(BufferedTableRow& row);
  // Resolve an image src to a sized ImageBlock (lazy-extracted from the EPUB), scaled to fit
  // maxWidth/maxHeight. Returns nullptr when the image is unsupported or its dimensions
  // cannot be resolved. Advances imageCounter to allocate a unique cache path.
  std::shared_ptr<ImageBlock> buildCellImage(const std::string& src, const std::string& alt, uint16_t maxWidth,
                                             uint16_t maxHeight);
  // Place an already-built ImageBlock as a centered, full-width block element, page-breaking if needed.
  void placeImageBlockAsBlock(const std::shared_ptr<ImageBlock>& image);
  // Emit currentPage to the consumer while keeping paragraphLutPerPage and completedPageCount
  // in lockstep. Every page break MUST go through this helper; open-coded completePageFn
  // calls risk desynchronising paragraphLutPerPage and failing the size check in Section.cpp.
  void emitPage(uint32_t xhtmlByteOffset);
  void recordPageBreakLabel(const std::string& label);
  // Attach the pending inline float image to `bs` and place it on the current page.
  // Clears pendingInlineImage_ on return.  No-op if pendingInlineImage_ is not active.
  void attachPendingFloatImage(BlockStyle& bs);
  // Begin drop-cap capture when a left-floated large-font inline element opens at the
  // start of an empty paragraph. Returns true when capture started (the element must
  // then not push an inline style entry).
  bool tryStartDropCapCapture(const CssStyle& cssStyle);
  // Place the captured drop cap: one-word PageLine on the current page plus a FloatZone
  // on the paragraph's block style. Falls back to an inline word when the cap is unusable.
  void finalizePendingDropCap();
  // XML callbacks
  static void startElement(void* userData, const char* name, const char** atts);
  static void characterData(void* userData, const char* s, int len);
  static void defaultHandlerExpand(void* userData, const char* s, int len);
  static void endElement(void* userData, const char* name);
  std::string abbreviateInlineFootnote(const char* text) const;

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const bool fontSizeNormalization, const bool bionicReadingEnabled,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 std::vector<std::string> tocAnchors = {},
                                 const std::function<void(int)>& progressFn = nullptr,
                                 const CssParser* cssParser = nullptr, EpubImageManifest* imageManifest = nullptr)

      : epub(epub),
        renderer(renderer),
        completePageFn(completePageFn),
        progressFn(progressFn),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        fontSizeNormalization(fontSizeNormalization),
        cssParser(cssParser),
        imageManifest(imageManifest),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)),
        bionicReadingEnabled(bionicReadingEnabled) {}

  ~ChapterHtmlSlimParser() override;

  // Streaming parse lifecycle. Caller flow:
  //   parser.setup(totalInflatedSize);
  //   epub->readItemContentsToStream(href, parser, ...);
  //   parser.finalize();
  // Returns false from setup() on parser allocation failure; check streamSucceeded()
  // after finalize() to detect a parse error mid-stream.
  bool setup(size_t totalInflatedSize);
  bool finalize();
  [[nodiscard]] bool streamSucceeded() const { return !streamFailed; }
  void setInlineFootnotePreviews(FootnotePreviews::Lookup* lookup) { inlineFootnotePreviews = lookup; }

  // Print interface — fed by Epub::readItemContentsToStream.
  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;

  ParsedText::LineProcessResult addLineToPage(std::shared_ptr<TextBlock> line, bool lineEndsWithHyphenatedWord,
                                              bool suppressHyphenationRetry);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  const std::vector<std::pair<uint16_t, std::string>>& getPageBreakLabels() const { return pageBreakLabels; }
  const std::vector<ParagraphLutEntry>& getParagraphLutPerPage() const { return paragraphLutPerPage; }

  // Supplies printed-page labels from NCX <pageList> for this chapter. `anchors` maps
  // HTML id -> label; an entry with an empty id applies to the first page of this file.
  void setExternalPageBreakAnchors(std::vector<std::pair<std::string, std::string>> anchors);

  // Supplies the body font's sibling-size ladder (see FontSizeLadder). Blocks whose
  // effective font size differs from the body resolve to the nearest real font on it.
  void setFontSizeLadder(const FontSizeLadder& ladder) { fontSizeLadder_ = ladder; }

 private:
  // Snap a completed block's effective font size (block multiplier, after uniform per-word
  // folding) to the size ladder: sets headingFontId to the chosen real font and reduces
  // fontSizeMultiplier to the residual. Applies the one-aux-font-per-section budget and is
  // idempotent via bs.fontResolved. Must run before the block's first layout so measured
  // widths, line heights and rendering all use the same (fontId, scale) pair.
  void resolveBlockFont(BlockStyle& bs);
  // Effective fontId for a block: its heading font when set, else the body fontId.
  int effectiveFontId(const BlockStyle& bs) const { return bs.headingFontId != 0 ? bs.headingFontId : fontId; }
  // Line height for a block, honoring the taller heading font (residual multiplier on top)
  // or the body-font scale path. Centralizes the layout-time sizing. Defined in the .cpp
  // because it dereferences GfxRenderer, which is only forward-declared here.
  int effectiveLineHeight(const BlockStyle& bs) const;
};
