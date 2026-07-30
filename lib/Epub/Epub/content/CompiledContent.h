#pragma once
// Compiled content format (content.bin, magic "WBC1") — Phase 3 of
// docs/compiled-book-pipeline-plan.md; layout defined in docs/compiled-content-format.md.
//
// This is the SETTINGS-INDEPENDENT product of a one-time Stage-1 compile: parsed,
// CSS-resolved blocks with text runs and image refs, keyed only by the book's ZIP
// content fingerprint. Stage-2 pagination (word measurement + line/page breaking)
// reads this and produces the per-settings section caches, so a font/margin change
// never re-runs ZIP/XML/CSS.
//
// This header + CompiledContent.cpp define the in-memory model and the content.bin writer/reader
// (with a host round-trip test). ContentSink fills the model from the walk; LayoutSink lays it out.
// content.bin is produced/consumed by the host harness today; wiring it into the device Section
// build is a pending step (see docs/parser-stage1-content-bin-device-wiring-design-2026-07-26.md).

#include <HalStorage.h>  // FsFile

#include <cstdint>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/css/CssStyle.h"

namespace compiled {

inline constexpr char kMagic[4] = {'W', 'B', 'C', '1'};
// v2 added Block::footnotePreviews (inline footnote preview runs; abbreviation deferred to Stage-2).
// v3 completes the Stage-1 artifact so a content.bin read-back reproduces the layout byte-identically:
// Block::footnotes (per-block footnote anchors), Block::xpath (LUT counters), and a per-spine
// page-break label table — the pieces ContentSink previously dropped.
// v4 adds the source book's ZIP content fingerprint to the header, so a reader can reject a
// content.bin that does not match the book on disk (stale cache → recompile).
// v5 makes the file STREAMABLE + RANDOM-ACCESS: a fixed header with back-patched section offsets
// (style pool, spine-offset index, chapters) so ContentBinWriter can append one block at a time and
// BlockStreamReader can seek to any spine and read one block at a time — neither ever holds a whole
// spine (let alone the whole book) in RAM. See docs/stage1-revised-plan-v2-2026-07-26.md.
// v6 makes the file READABLE WHILE STILL BEING WRITTEN (Increment E producer/consumer, see
// docs/stage1-incr-E-producer-consumer-design-2026-07-26.md): each spine is SELF-CONTAINED — it
// carries its OWN local style table + its own chapter entries inside its section — so a committed
// spine can be replayed the instant its offset commits, with no dependency on any later-written
// book-global structure. The spine-offset INDEX is pre-allocated at a fixed location right after the
// header (spineCount slots, all 0) and each spine's start offset is committed into its slot as the
// spine finishes; a 0 slot means "not compiled yet". There is no book-global style pool or chapters
// section any more. No device shipped v5 content.bin → clean break, no migration.
// v7 bakes a per-spine BLOCK-OFFSET table into each spine's aux region (after the chapter table):
// blockCount x BlockOffset{fileOffset, charOffset}, one entry per LOGICAL block. This gives the
// reader O(1) seekToBlock(i) with no content scan -- the piece live pagination needs to start
// mid-spine and lay out backward (docs/stage1-single-source-live-pagination-2026-07-27.md). It is
// microreader's baked-descriptor-table approach (MrbChapterSource bulk-reads its offsets at open).
// content.bin is a rebuildable cache; a v6 file just fails the version check and is recompiled.
inline constexpr uint8_t kVersion = 7;

// v6 fixed header: magic(4) + version(1) + fingerprint(8) + spineCount(4). Immediately followed by
// the pre-allocated spine-offset index: spineCount × u32, all 0 at begin(), each committed as its
// spine finishes. A reader validates magic+version, reads spineCount, then reads the index that
// starts at kHeaderSize; a 0 offset = spine not yet available. Per-spine style tables + chapters
// live inside each spine's section (self-contained), so there are no book-level pool/chapter offsets.
inline constexpr uint32_t kHeaderSize = 4 + 1 + 8 + 4;  // = 17 bytes; spine index begins here

// Word::styleSpan bits. Bits 0-6 = inline font style (the EpdFontFamily::Style set,
// remapped to a stable on-disk layout); bit 7 = word attaches to the previous word
// with no space before it (inline-element boundary, e.g. <b>foo</b>bar). All
// layout-independent, so they live in Stage-1.
enum WordStyleSpan : uint8_t {
  kSpanBold = 1 << 0,
  kSpanItalic = 1 << 1,
  kSpanUnderline = 1 << 2,
  kSpanStrikethrough = 1 << 3,
  kSpanSuper = 1 << 4,
  kSpanSub = 1 << 5,
  kSpanSmallCaps = 1 << 6,
  kSpanAttachPrev = 1 << 7,  // no leading space (ParsedText wordContinues)
};

// Per-word settings-independent data — the slice of today's TextBlock that survives
// a relayout. No xpos: Stage-2 measurement computes it. bidiLevel is 0 for LTR books
// and reserved for RTL (see docs/compiled-content-format.md "RTL / BiDi").
struct Word {
  uint32_t textOff = 0;   // byte offset of the word's text within Block::text
  uint8_t styleSpan = 0;  // WordStyleSpan bitmask (style + attach-to-previous)
  uint8_t sizePct = 100;  // per-word font-size percent; 100 = inherit block size
  uint8_t bidiLevel = 0;  // Unicode embedding level; 0 = LTR
};

enum class BlockType : uint8_t { Text = 0, Image = 1, Table = 2, Hr = 3 };

// A run of words inside a Text block that make up an inline footnote preview ("(note text)").
// The full preview text is stored settings-independently; Stage-2 abbreviates the run to the
// viewport width at layout time (the abbreviation is font/viewport-dependent, so it must NOT be
// baked into content.bin). startWord indexes into Block::words; count words follow.
struct PreviewRun {
  uint32_t startWord = 0;
  uint32_t count = 0;
};

// A footnote link anchored to a word position within a Text block. Stage-2 (LayoutSink) buffers it
// and assigns it to the page that lays out word `wordIndex`, exactly as the walk's onFootnote did.
struct FootnoteRef {
  uint32_t wordIndex = 0;  // word offset within the block (== stage1BlockWordCount at emit time)
  FootnoteEntry entry;
};

// The walk's XPath counters as of a block, needed to rebuild the per-page paragraph LUT (progress /
// xpath navigation) when Stage-2 replays from content.bin. Settings-independent (pure XML position).
struct XPathCounters {
  uint16_t paragraphIndex = 0;
  uint16_t listItemIndex = 0;
  uint32_t bodyChildByteOffset = 0;
};

// One table cell: text runs (a mini text block) and/or a single cell image. Settings-
// independent — Stage-2 reproduces today's grid-or-paragraph decision (which is
// font-dependent) from this structure, so grid tables survive relayout unchanged.
struct TableCell {
  std::vector<Word> words;
  std::string text;            // words back-to-back, each NUL-terminated
  std::string imageEntryPath;  // optional cell image (empty = none)
  int16_t imageWidth = 0;      // intrinsic dims, pre-probed at compile
  int16_t imageHeight = 0;
  std::string imageAlt;
  bool isHeader = false;  // <th> cell
  uint8_t colSpan = 1;
};

struct TableRow {
  std::vector<TableCell> cells;
  bool isHeaderRow = false;  // all cells are <th>
};

// Block::flags bits (docs/compiled-content-format.md).
enum BlockFlags : uint8_t {
  kStartsChapter = 1 << 0,
  kPageBreakBefore = 1 << 1,
  kPageBreakAfter = 1 << 2,
  // bits 3-4: base direction (0 auto, 1 LTR, 2 RTL) — reserved for RTL.
  kDirectionShift = 3,
  kDirectionMask = 0b11 << 3,
  // This (empty) block came from a <br> section separator: Stage-2 injects a blank
  // line's worth of top margin when merging it into the following paragraph.
  kFromBrElement = 1 << 5,
  // Continuation of the previous TEXT block, produced by the writer's 8 KB
  // split-at-write; Stage-2 treats the run as one logical paragraph.
  kContinuation = 1 << 6,
  // Block is inside a <pre> element: Stage-2 suppresses the extra inter-paragraph spacing,
  // so preformatted lines are single-spaced.
  kPreformatted = 1 << 7,
};

// One content block. Text fields are used when type==Text, image fields when
// type==Image; the unused set stays empty/zero.
struct Block {
  BlockType type = BlockType::Text;
  uint16_t styleId = 0;  // index into CompiledContent::stylePool
  uint8_t flags = 0;
  uint32_t charOffset = 0;  // absolute char offset of this block's first char (reading progress)

  // Text block:
  std::vector<Word> words;
  std::string text;  // words back-to-back, each NUL-terminated
  // Inline footnote preview runs within this block (empty for the vast majority of blocks).
  // The words themselves live in `words`/`text`; these ranges tell Stage-2 which runs to
  // abbreviate to the viewport at layout time. See PreviewRun.
  std::vector<PreviewRun> footnotePreviews;
  // Footnote links anchored to words in this block (empty for most blocks). Replayed as
  // onFootnote(wordIndex, entry) so Stage-2 assigns each to the page laying out its anchor word.
  std::vector<FootnoteRef> footnotes;
  // The walk's XPath counters as of this block's emit (onXPathAdvance). hasXPath=false when the
  // walk emitted no advance before this block (Stage-2 then carries the previous counters forward).
  bool hasXPath = false;
  XPathCounters xpath;
  // Optional inline (float) image rendered beside this paragraph's text. Stage-2 places
  // it; Stage-1 just records the ref (empty entryPath = none). intrinsic dims, pre-probed.
  std::string inlineImageEntryPath;
  int16_t inlineImageWidth = 0;
  int16_t inlineImageHeight = 0;
  uint8_t inlineImageSide = 0;  // 1 left / 2 right (0 = none)
  std::string inlineImageAlt;

  // Image block:
  std::string entryPath;  // EPUB-internal path (e.g. OEBPS/images/x.jpg)
  int16_t width = 0;      // intrinsic dimensions, pre-probed at compile
  int16_t height = 0;
  uint8_t floatSide = 0;  // 0 none / 1 left / 2 right
  std::string alt;

  // Table block:
  std::vector<TableRow> rows;
  bool hasBorder = true;  // border="0" clears it (affects Stage-2 grid rendering)
};

// Named position for anchor navigation (TOC targets, in-book links). Resolves to a
// (block, char-offset) pair; Stage-2 maps that to a page. Id stored as a string
// (not a hash) so a lookup can never resolve the wrong target.
struct Anchor {
  std::string id;           // element id / fragment
  uint32_t blockIndex = 0;  // block within this spine
  uint32_t charOffsetInBlock = 0;
};

// Book-level chapter/heading entry (drives the TOC and heading navigation).
struct Chapter {
  uint16_t spineIndex = 0;
  uint32_t blockIndex = 0;
  uint8_t level = 0;  // heading level 1..6; 0 = non-heading chapter boundary
  std::string title;
};

// A printed-page label (EPUB pagebreak marker / NCX pageList) anchored to a block position.
// Stage-2 replays onPageBreakLabel at the block boundary so the label lands on the right page.
struct PageBreakLabel {
  std::string label;
  uint32_t blockIndex = 0;  // block index within the spine at which the label occurs
};

// v7: per-LOGICAL-block descriptor, baked into the spine's aux region so a reader can seek to any
// block in O(1) (no scan) — the piece that lets live pagination start mid-spine and go backward.
// Mirrors microreader's MrbChapterSource descriptor table (file offset + cumulative char offset per
// paragraph, bulk-read at chapter open). One entry per LOGICAL block (kContinuation split records
// share their base block's entry), so index i here == the i-th block nextLogicalBlock() returns.
struct BlockOffset {
  uint32_t fileOffset = 0;    // absolute content.bin offset of this logical block's FIRST record
  uint32_t charOffset = 0;    // absolute char offset of this block's first char (reading progress)
  uint32_t recordIndex = 0;   // RECORD index of this logical block's first record (anchors/labels/
                              // chapters are keyed on record index; seekToBlock restores it so the
                              // replay cross-reference in LayoutSink keeps resolving after a seek).
};

// Per-spine content, in document order.
struct SpineContent {
  std::vector<Block> blocks;
  std::vector<Anchor> anchors;
  std::vector<PageBreakLabel> pageBreakLabels;
  uint32_t firstCharOffset = 0;  // absolute char offset of the spine's first char (progress)
};

// A whole book's compiled content.
struct CompiledContent {
  // Source book's ZIP content fingerprint (Epub::zipFingerprint). 0 = unset (older/anonymous
  // compiles). A reader compares this to the book on disk and treats a mismatch as a stale cache.
  uint64_t sourceFingerprint = 0;
  std::vector<CssStyle> stylePool;  // deduped block styles; blocks reference by index
  std::vector<SpineContent> spines;
  std::vector<Chapter> chapters;
};

// Whether two block styles are identical for pooling purposes (all rendering-relevant
// fields + the explicit-set flags). Two blocks that resolve to equal styles share a pool id.
bool styleEquals(const CssStyle& a, const CssStyle& b);

// Return the pool id for `style`, appending it if not already present (dedup by value). Linear
// scan — the distinct-style set per book is small (tens), and blocks vastly outnumber styles. The
// pool overload lets the streaming writer intern into a standalone pool (no whole CompiledContent).
uint16_t internStyle(std::vector<CssStyle>& pool, const CssStyle& style);
uint16_t internStyle(CompiledContent& content, const CssStyle& style);

// Serialize/deserialize the WBC1 container. Return false on I/O error or a
// version/magic mismatch (caller treats a mismatch like a stale cache: recompile).
bool writeContentBin(FsFile& out, const CompiledContent& content);
bool readContentBin(FsFile& in, CompiledContent& content);

}  // namespace compiled
