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
#include "../content/CompiledContent.h"  // compiled::Block (unique_ptr member needs the complete type)
#include "../content/TeeBlockSink.h"     // compiled::TeeBlockSink (unique_ptr member; header-only)
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class Page;  // completePageFn signature only; full type not needed here
class GfxRenderer;
class Epub;
namespace compiled {
struct BlockSink;
class LayoutSink;
}  // namespace compiled

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

  // Inline image beside paragraph text (CSS float context). The walk uses the float nesting to
  // decide the block TYPE it emits (inline float vs centered block); the actual float placement
  // lives in LayoutSink. Fixed-size arrays — no heap. Float nesting > 4 is pathological.
  static constexpr int kMaxFloatDepth = 4;
  int floatDepth_ = 0;
  int floatOpenDepths_[kMaxFloatDepth] = {};  // parser depth at which each float was opened
  bool floatOpenSides_[kMaxFloatDepth] = {};  // true = right float, false = left float
  int fontId;
  // Default heading multipliers (index 0=h1, 1=h2, 2=h3) applied when a heading has no
  // explicit CSS font-size; the multiplier is transmitted to Stage-1 and Stage-2 (LayoutSink)
  // snaps it to the size ladder.
  static constexpr float kHeadingMultiplier[3] = {1.6f, 1.4f, 1.2f};
  // Sibling-size ladder of the body font (see FontSizeLadder), forwarded to LayoutSink, which
  // snaps a block's effective font size to the nearest real font on it; empty = scale-only.
  FontSizeLadder fontSizeLadder_;
  // One non-body font per section: body regular/bold/italic plus one auxiliary regular is
  // exactly the FontDecompressor's four page slots. The first block to resolve off-body
  // claims the slot; blocks that would need a different font keep the scale fallback.
  int32_t auxFontId_ = 0;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
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

  // Active parser for streaming. Stored as a member so walk sites can call
  // saxParser_.byteOffset() without threading the parser through
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
  bool bionicReadingEnabled = false;
  bool layoutFailed = false;

  // Stage-1 (settings-independent content compile) producer tap. When stage1Sink_ is set,
  // the walk ALSO emits a materialized compiled::Block per text block through the sink, with
  // the fused layout path untouched (see docs/stage1-extraction-design.md). Null by default:
  // every hook below is a no-op, so the shipping fused parse is byte-identical. The block is
  // accumulated as words are flushed (stage1AddWord) and handed off at the block boundary
  // (stage1OpenBlock flushes the prior one; finalize() flushes the last). Text is stored raw
  // Unicode in logical order so RTL shaping/reordering stays a Stage-2 concern.
  compiled::BlockSink* stage1Sink_ = nullptr;
  // Increment F: when true, stage1Sink_ runs ALONGSIDE the internal layout sink (a tee), not instead
  // of it — the SAME walk feeds both the LayoutSink (Stage-2 pages, the transient current-spine page
  // store) and stage1Sink_ (a ContentBinWriter → content.bin). When false (the legacy mode used by
  // the host whole-book compile), stage1Sink_ REPLACES the layout sink (content-only, no pages). See
  // effectiveSink() + setup().
  bool stage1SinkTee_ = false;
  // Step 6 (unify): the parser's own layout consumer. Constructed in setup() from the parser's
  // settings members. Drives EITHER the external stage1Sink_ (content-only compile) OR this internal
  // layout sink, OR — in tee mode (stage1SinkTee_) — BOTH via effectiveSink()'s TeeBlockSink. Its
  // emitPage routes pages through completePageFn and its getters back the parser's
  // getAnchors()/getPageBreakLabels()/getParagraphLutPerPage().
  std::unique_ptr<compiled::LayoutSink> layoutSink_;
  // The tee that fans the walk to layoutSink_ + stage1Sink_ in tee mode. Owned; built in setup().
  std::unique_ptr<compiled::TeeBlockSink> stage1TeeSink_;
  // Rebuilt by getParagraphLutPerPage() from the internal sink's LayoutLutEntry vector, so the
  // getter can return the parser's ParagraphLutEntry type (field-identical) without changing
  // Section.cpp's reader. Mutable: the getter is const.
  mutable std::vector<ParagraphLutEntry> lutAdapter_;
  // The sink the producer emits to this build: the external content sink if attached, else the
  // internal layout sink. Null only before setup() constructs the internal sink.
  compiled::BlockSink* effectiveSink() const;
  std::unique_ptr<compiled::Block> stage1Block_;  // current accumulator (null when closed)
  CssStyle stage1BlockCssStyle_;                  // the block's pre-px style, captured at open
  uint32_t stage1CharOffset_ = 0;                 // running codepoint offset within the spine
  uint8_t stage1PendingHeadingLevel_ = 0;         // set by a heading tag, consumed at the next block open
  uint8_t stage1BlockHeadingLevel_ = 0;           // heading level (1-6) of the open block; 0 = not a heading
  bool stage1PendingFromBr_ = false;              // incoming block came from a <br> separator
  std::string stage1PendingAnchor_;               // element id awaiting the block it precedes
  bool stage1PendingPageBreak_ = false;           // the stashed anchor is a TOC boundary → kPageBreakBefore
  // Deferred float image awaiting the paragraph it floats beside — recorded on the Stage-1 block
  // when its first word arrives; LayoutSink places it. Intrinsic dims.
  std::string stage1InlineImagePath_;
  std::string stage1InlineImageAlt_;
  int16_t stage1InlineImageW_ = 0;
  int16_t stage1InlineImageH_ = 0;
  uint8_t stage1InlineImageSide_ = 0;  // 1 left / 2 right
  bool stage1InlineImagePending_ = false;

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
  void startNewTextBlock(const BlockStyle& blockStyle);
  bool flushPartWordBuffer();
  // Emit the settings-independent Table block to the producer. LayoutSink does the grid-vs-
  // paragraph layout, cell wrapping, and PageTableFragment/fallback placement.
  void emitBufferedTable();
  void recordPageBreakLabel(const std::string& label);
  // XML callbacks
  static void startElement(void* userData, const char* name, const char** atts);
  static void characterData(void* userData, const char* s, int len);
  static void defaultHandlerExpand(void* userData, const char* s, int len);
  static void endElement(void* userData, const char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, GfxRenderer& renderer, const int fontId, const float lineCompression,
      const bool extraParagraphSpacing, const uint8_t paragraphAlignment, const uint16_t viewportWidth,
      const uint16_t viewportHeight, const bool hyphenationEnabled, const bool bionicReadingEnabled,
      const std::function<void(std::unique_ptr<Page>)>& completePageFn, const bool embeddedStyle,
      const std::string& contentBase, const std::string& imageBasePath, const uint8_t imageRendering = 0,
      std::vector<std::string> tocAnchors = {}, const std::function<void(int)>& progressFn = nullptr,
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

  // Step 6 unify: when the internal LayoutSink drives output, these proxy its tables (the fused
  // anchorData/pageBreakLabels/paragraphLutPerPage become unread fused scratch). Otherwise (external
  // ContentSink compile) they return the fused tables. Defined out-of-line where LayoutSink is
  // complete. getParagraphLutPerPage rebuilds lutAdapter_ from the sink's LayoutLutEntry vector
  // (field-identical to ParagraphLutEntry, distinct type).
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const;
  const std::vector<std::pair<uint16_t, std::string>>& getPageBreakLabels() const;
  const std::vector<ParagraphLutEntry>& getParagraphLutPerPage() const;

  // Supplies printed-page labels from NCX <pageList> for this chapter. `anchors` maps
  // HTML id -> label; an entry with an empty id applies to the first page of this file.
  void setExternalPageBreakAnchors(std::vector<std::pair<std::string, std::string>> anchors);

  // Supplies the body font's sibling-size ladder (see FontSizeLadder). Blocks whose
  // effective font size differs from the body resolve to the nearest real font on it.
  void setFontSizeLadder(const FontSizeLadder& ladder) { fontSizeLadder_ = ladder; }

  // Attach a Stage-1 content sink in CONTENT-ONLY mode: the walk emits materialized blocks through
  // `sink` and produces NO pages (replaces the internal layout sink). Used by the host whole-book
  // compile (compileBookToContentBin). Null clears it. See docs/stage1-extraction-design.md.
  void setStage1Sink(compiled::BlockSink* sink) {
    stage1Sink_ = sink;
    stage1SinkTee_ = false;
  }

  // Attach a Stage-1 content sink in TEE mode (Increment F): the walk feeds BOTH the internal layout
  // sink (Stage-2 pages) AND `sink` (a ContentBinWriter) from one walk, so the section build also
  // emits content.bin. Null clears it (reverts to plain layout). See effectiveSink().
  void setStage1TeeSink(compiled::BlockSink* sink) {
    stage1Sink_ = sink;
    stage1SinkTee_ = (sink != nullptr);
  }

 private:
  // Stage-1 producer tap (no-ops when stage1Sink_ is null). Defined in the .cpp where
  // compiled::Block is complete.
  // Flush the prior block (INCLUDING empty wrapper/spacer/<br> blocks — the emitted
  // sequence is a 1:1 transcript of the layout's block opens, so Stage-2 can replay the
  // exact same empty-block margin merges), then start a fresh accumulator.
  void stage1OpenBlock(const CssStyle& style);
  void stage1AddWord(const char* text, EpdFontFamily::Style style, uint8_t sizePct, bool attachToPrevious);
  void stage1FlushBlock();         // emit the accumulated block through the sink, if non-empty
  void stage1EmitPendingAnchor();  // emit a stashed anchor id against the block about to be emitted
  // Flush any pending text, then emit a standalone image block (intrinsic dims, EPUB entry path).
  // imgStyle carries the image element's resolved CSS (width/height only matter) so Stage-2
  // can reproduce the settings-dependent display-dimension scaling from the intrinsic dims.
  void stage1EmitImageBlock(const std::string& entryPath, int16_t width, int16_t height, uint8_t floatSide,
                            const std::string& alt, const CssStyle& imgStyle);
  // Flush any pending text, then emit a Table block reconstructed from the buffered table
  // (settings-independent rows/cells; Stage-2 reproduces grid-or-paragraph).
  void stage1EmitTableBlock(const BufferedTable& table);
  // Flush any pending text, then emit a bare HR marker block. Stage-2 derives the centered
  // rule geometry + surrounding half-line margins from the viewport at layout time.
  void stage1EmitHrBlock();
  // Producer-side substitutes for the walk's currentTextBlock queries, so the walk no longer
  // depends on the fused layout block. "Empty" == no words accumulated in the open transcript
  // block; the count == words emitted to Stage-1 for the current block so far (footnote anchor).
  bool stage1BlockIsEmpty() const { return !stage1Block_ || stage1Block_->words.empty(); }
  int stage1BlockWordCount() const { return stage1Block_ ? static_cast<int>(stage1Block_->words.size()) : 0; }
};
