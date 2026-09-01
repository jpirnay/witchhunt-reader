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
// Page.h, not just a forward declaration: the table layout members below hold TableRow/TableCell
// by value. Only ChapterHtmlSlimParser.cpp and Section.cpp include this header, and both already
// pull Page.h in anyway.
#include "../Page.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

// Page, PageImage and PageLine all come from ../Page.h above.
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

// Rough page count for a spine of `inflatedSize` XHTML bytes, used only to pre-size the
// per-page vectors that the build grows one entry at a time (Section's page-offset LUT and
// the parser's paragraph LUT). Deliberately an UNDER-estimate: the point is to remove the
// long doubling ladder from the middle of a parse, not to get the count right. Reserving
// too much would take heap the build needs; reserving a bit too little just leaves one or
// two doublings at the end, which is already the cheap case.
//
// 1 KB of XHTML per page, from device measurement (X4, 2026-08-11): a 16340-byte spine laid
// out to 19 pages (860 B/page), and a 551-byte spine to 1. An earlier 4096 guess reserved 3
// entries for that 19-page spine and left most of the ladder in place.
//
// Erring low is cheap here because MAX_RESERVED_PAGES bounds the whole downside: even a spine
// that saturates it reserves only 2 KB (Section's u32 LUT) and 4 KB (the 8-byte paragraph LUT).
// Past the cap the vector grows normally.
inline constexpr size_t estimatePagesForSpine(const size_t inflatedSize) {
  constexpr size_t XHTML_BYTES_PER_PAGE = 1024;
  constexpr size_t MAX_RESERVED_PAGES = 512;
  const size_t pages = inflatedSize / XHTML_BYTES_PER_PAGE;
  if (pages > MAX_RESERVED_PAGES) return MAX_RESERVED_PAGES;
  return pages;
}

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
  // Streaming table model. A row is buffered only until its </tr>, where it is laid out, packed
  // into the pending fragment and freed. The whole table was buffered until </table> once, because
  // grid layout needs the column count before the first row can be placed -- but our columns are
  // viewportWidth/N regardless of content, so the only thing that ever needed every row was the
  // maximum column count. Widening rows flush the fragment and start a new one at the wider count
  // instead; narrower rows still pad, which keeps a ragged row aligned with its neighbours.
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
  // One buffered row wrapped into grid cells, sized but not yet placed on a page.
  struct LayoutRow {
    std::vector<TableCell> cells;
    uint16_t height = 0;  // content height + 2 x TABLE_CELL_PADDING
    bool isHeaderRow = false;
    uint8_t renderCols = 0;  // grid columns this row was laid out on (the table's column count)
  };
  // Rows accumulated for the PageTableFragment currently being packed. A fragment carries a single
  // column count and must fit the viewport, so a change in either forces a flush.
  struct TableFragmentPacker {
    std::vector<TableRow> rows;
    uint16_t height = 0;
    uint8_t cols = 0;
    uint16_t totalWidth = 0;
    int16_t xInset = 0;  // left edge of the table box; non-zero when the <table> carries a left inset
    bool hasBorder = true;
  };
  struct BufferedTable {
    // The row currently being filled. Cleared at every <tr> and freed again at </tr> once the row
    // has been laid out, so a table's resident cost is one row, not one table.
    BufferedTableRow pendingRow;
    TableFragmentPacker packer;
    // Column count for the table as a whole, widened by any row that needs more. Distinct from
    // packer.cols, which is per-fragment and resets on every flush: without this a ragged row
    // landing just after a page break would narrow the table's second half.
    uint8_t columnCount = 0;
    // The table's own box, from the <table> element's resolved CSS margins and padding. Columns
    // divide contentWidth rather than the full viewport, so an indented table stays indented
    // instead of being stretched edge to edge.
    uint16_t contentWidth = 0;
    int depth = 0;          // nesting depth; > 1 means we're inside a nested table
    bool hasBorder = true;  // false when border="0" on the <table> element
    // This table can never be a grid (nested table, rowspan, low heap): every cell from here on
    // is emitted as a paragraph at </td>. Latched -- degrading is one-way.
    bool degraded = false;
    // This ROW has already emitted part of itself as paragraphs, so the rest of it must follow
    // in document order rather than going back into the grid. Cleared at the next <tr>, which is
    // what makes a bad row cost one row instead of the whole table.
    bool rowDegraded = false;
    // Set when a cell pushes the row past MAX_TABLE_COLS. The cell is open and about to receive
    // text, so the degrade is deferred to </td> rather than run with a cell mid-fill.
    bool rowOverflowed = false;
    // Attributed resident bytes held by pendingRow's cells (see MAX_TABLE_ROW_BUFFER_BYTES).
    size_t pendingRowBytes = 0;
    // The table's leading header row, re-emitted at the top of every continuation fragment so a
    // table that spans a page break keeps its column labels. Held in its BUFFERED form and laid
    // out again per fragment, NOT as a laid-out LayoutRow whose TextBlocks are shared between
    // fragments. Sharing them was cheaper but it couples the header's lifetime to every page the
    // table touches, which rules out ever scoping line storage to a page (see the table scratch
    // arena). Re-layout costs one row of work per page break and keeps each fragment's lines
    // owned solely by that fragment. Bounded: one row, captured only if it opens the table, and
    // only while it is short enough to be worth the space it costs each page.
    std::unique_ptr<BufferedTableRow> repeatHeader;
    uint16_t repeatHeaderHeight = 0;    // laid-out height, measured once at capture
    bool repeatHeaderResolved = false;  // set after the table's first row; only that row qualifies
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

  // Horizontal insets (margin + padding, per side) of the block-level elements currently open.
  // Every block nested inside them starts at their sum: layout has no box model, so without
  // this a wrapper's inset would reach only its FIRST child -- the empty-block merge in
  // startNewTextBlock() -- and every later sibling would jump back to the viewport edge. With a
  // hanging indent on the children (verse: div{margin-left:2em} > p{text-indent:-1em}) those
  // siblings then start left of the panel and lose their first glyph (issue #198).
  // depth = parser depth at push (pre-increment); popped in endElement when that scope closes.
  struct BlockInsetEntry {
    int depth;
    int16_t left;
    int16_t right;
  };
  // Insets nested deeper than this are dropped rather than tracked: the total is capped at 4em
  // either way, and a book that nests inset wrappers this deep is pathological, not typographic.
  static constexpr size_t kMaxBlockInsetDepth = 8;
  std::vector<BlockInsetEntry> blockInsetStack_;

  // Fold the insets of every enclosing block-level element into `style`, capping each side at
  // MAX_HORIZONTAL_INSET_EM so deep nesting cannot squeeze the text column away.
  void addAncestorInsets(BlockStyle& style, float emSize) const;

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
  // Set when the heap gate refused an image-header read (see imageHeaderDegraded()).
  bool imageHeaderSkippedForHeap = false;
  uint32_t streamStartTimeMs = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  size_t currentFootnoteLinkTextLen = 0;
  // Non-owning; the Section's BuildState keeps the lookup alive across build slices.
  // Membership in the book's preview store is the sole expansion gate: it already encodes
  // "this link points at a real note", so no epub:type/same-file checks here. The store is
  // filled for this spine before the parse starts (Section::resolveInlineFootnotePreviews).
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
  // Whether the heap can afford the ~32 KB inflate ring a ZIP image-header read needs. Checked at
  // the call site, never latched: the heap recovers between pages, and a single dip must not
  // disable images for the rest of the chapter (that result gets baked into the section cache).
  bool heapAllowsImageHeaderRead() const;
  void startNewTextBlock(const BlockStyle& blockStyle);
  void clearSpentBlockHeadingStyle();
  bool heapAllowsTableRowLayout() const;
  bool flushPartWordBuffer();
  void makePages();
  // Called at </tr>: lay the pending row out, pack it into the fragment, and free its cells.
  // Degrades the row to paragraphs if it cannot be a grid row. No-op for an empty row.
  void commitPendingRow();
  // Wrap one buffered row into grid cells at `columnCount` columns. Returns false when the row
  // cannot be a grid row at all -- an unsupported colspan, a cell needing more lines than the grid
  // carries, or a cell taller than the viewport -- leaving `out` unusable and the row's cell text
  // untouched, so the caller can still emit it as paragraphs. The caller chooses how far to fall
  // back; nothing is written to a page from here.
  bool layoutTableRow(BufferedTableRow& bufRow, uint8_t columnCount, LayoutRow& out);
  // Emit the packed rows as one PageTableFragment, page-breaking first if it does not fit, and
  // reset the packer. No-op when nothing is packed.
  void flushTableFragment(TableFragmentPacker& packer);
  // Emit every cell of `row` as a sequential paragraph, releasing each cell's ParsedText as it is
  // laid out. Returns false when the heap guard stopped the parse.
  bool emitRowAsParagraphs(BufferedTableRow& row);
  // Paragraph path only: emit each cell's image as a full-width block image, then drop the cells.
  void emitRowImagesAsBlocks(BufferedTableRow& row);
  // Emit one buffered cell as a paragraph (plus its image, when emitImage) and free the cell's
  // ParsedText before returning. Shared by the row fallback and the degraded path so both
  // produce identical output. Returns false when the heap guard stopped the parse.
  bool emitCellAsParagraph(BufferedTableCell& cell, bool emitImage);
  // Give up the grid for the pending ROW: flush whatever is packed so the fragment closes before
  // the paragraphs, then emit the row's buffered cells (text then images) and latch rowDegraded so
  // the rest of the row streams at </td>. The next <tr> clears it and the grid resumes.
  void degradeRow(const char* reason);
  // Give up the grid for the whole TABLE. degradeRow plus a latch, for triggers that cannot
  // recover on the next row: a nested table, a rowspan, or a heap too low to lay rows out.
  void degradeTable(const char* reason);
  // Degrade the row while a <td> is still open: lifts the open cell out, degrades, then emits the
  // words accumulated so far and hands the cell back empty so parsing continues into it. Needed
  // because degradeRow() clears the row and so can only run between cells -- which never happens
  // in a one-cell row. Returns false when the drain stopped the parse (low heap).
  bool degradeRowAtOpenCell(const char* reason);
  // Called on </td> once degraded: emit and release the just-closed cell immediately.
  void streamClosedCell(BufferedTableRow& row);
  // Resolve an image src to a sized ImageBlock (lazy-extracted from the EPUB), scaled to fit
  // maxWidth/maxHeight. Returns nullptr when the image is unsupported or its dimensions
  // cannot be resolved. The cache path is derived from the archive entry, not from parse order.
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
  // True when at least one image was dropped to alt text because the heap gate refused its
  // header read — a transient condition, unlike an unreadable or unsupported image. The pages
  // are usable but incomplete, and the caller must not keep them: see the latch site in
  // startElement's image branch.
  [[nodiscard]] bool imageHeaderDegraded() const { return imageHeaderSkippedForHeap; }
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
