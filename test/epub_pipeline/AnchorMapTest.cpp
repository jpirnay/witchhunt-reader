// Cover for the section cache's ANCHOR MAP — the id-to-page table every fragment jump lands on
// (`notes.xhtml#en1`, a TOC entry pointing mid-chapter, a cross-reference).
//
// The golden layout dumps cannot see this table: they compare what dumpPage prints, which is the
// text of each page and nothing about its anchors. A build that wrote an empty anchor map, or one
// missing every entry but the last, passes every golden — so the map needs cover of its own.
//
// The map is produced by streaming: the parser appends each anchor to a spill file as it finds
// one and the finalizer copies that file into the cache, so that a chapter with hundreds of
// anchors costs O(1) RAM instead of the ~28 KB (and a 43 KB doubling spike) that holding them
// used to. The tests below pin what that streaming has to preserve — every anchor, its page, and
// in particular the LAST one, which is recorded during finalize and so is the one an
// off-by-one-lifecycle bug singles out.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

// Spine 0 wraps its paragraphs in a div (id "ch01") and carries p1..p24; spine 2 is the note
// document, whose single note body is the one anchor in it.
constexpr int kWrappedSpine = 0;
constexpr int kNotesSpine = 2;

// test/epubs/make_test_nested_tables.py: its fifth chapter's inner table holds TARGETLONG's cell, then the cell with
// the id "inner-target" and the word TARGETWORD, then TARGETB's; the first two are each longer than a page.
const std::string kNestedTablesFixture = std::string(CORPUS_DIR) + "/test_nested_tables.epub";
constexpr int kNestedTargetSpine = 4;

// test/epubs/make_test_inline_anchors.py: one case of an inline id per chapter, its docstring lists them.
const std::string kInlineAnchorsFixture = std::string(CORPUS_DIR) + "/test_inline_anchors.epub";
constexpr int kInlineLongSpine = 0;
constexpr int kInlineShortSpine = 1;
constexpr int kInlineBeforeFirstSpine = 2;
constexpr int kInlineTableSpine = 3;
constexpr int kInlineHyphenSpine = 4;
constexpr int kInlineTailSpine = 5;
constexpr int kInlineWrapSpine = 6;
constexpr int kInlineBreakSpine = 7;

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "anchor_map_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// The book's CSS (inline styles too) only where a case forces a page break or sets a margin.
Section::BuildParams params(const bool hyphenate = false, const bool embeddedStyle = false) {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = TextScale();
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = hyphenate;
  p.fontSizeNormalization = false;
  p.embeddedStyle = embeddedStyle;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

struct BuiltSection {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
};

BuiltSection build(const std::string& tag, int spineIndex, const std::string& fixture = kFixture,
                   const bool hyphenate = false, const bool embeddedStyle = false) {
  BuiltSection out;
  out.epub = std::make_shared<Epub>(fixture, freshCacheDir(tag));
  EXPECT_TRUE(out.epub->load(true));
  out.epub->setupCacheDir();

  GfxRenderer renderer;
  out.section = std::make_unique<Section>(out.epub, spineIndex, renderer);
  EXPECT_TRUE(out.section->createSectionFile(params(hyphenate, embeddedStyle), {}, /*skipEviction=*/true));
  EXPECT_TRUE(out.section->loadSectionFile(params(hyphenate, embeddedStyle)));
  return out;
}

// The first page that holds `word` as a whole token, if any.
std::optional<int> pageWithWord(Section& section, const std::string_view word) {
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    if (!page) continue;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
      if (!block || !block->valid()) continue;
      for (uint16_t w = 0; w < block->wordCount(); ++w) {
        if (std::string_view(block->wordText(w), block->wordTextLen(w)) == word) return pg;
      }
    }
  }
  return std::nullopt;
}

}  // namespace

// The last anchor in a document is recorded during finalize, after the parse has ended, which
// makes it the one entry a lifecycle mistake drops (or keeps alone). Pinned separately from the
// sweep below so a failure says which case broke.
TEST(AnchorMap, TheLastAnchorSurvivesFinalize) {
  const auto built = build("last_anchor", kWrappedSpine);

  // ind_tail is an empty <a> after the final paragraph, so it opens no text block and is still
  // pending when the parse ends -- the finalize-time recording, and the only anchor in the
  // corpus that reaches it. An earlier version of the spill closed before this point, sending
  // this one anchor down the resident fallback, whose count then replaced the 544 already on
  // disk: the whole chapter's map became this single entry.
  const auto tail = built.section->getPageForAnchor("ind_tail");
  ASSERT_TRUE(tail.has_value()) << "the document's final anchor was lost at finalize";
  EXPECT_LT(*tail, built.section->pageCount);

  // And it did not arrive at the cost of the rest.
  EXPECT_TRUE(built.section->getPageForAnchor("p1").has_value())
      << "only the trailing anchor survived — the map was rewritten rather than appended to";
  EXPECT_TRUE(built.section->getPageForAnchor("p24").has_value());
}

// Every id in the document resolves, to the page it actually sits on, in order. Not "the map is
// non-empty" — a map that kept only some of its entries still answers a lookup, it just silently
// strands the fragments it dropped, which on a notes document means the footnotes that no longer
// go anywhere. And a map whose pages were all zero (or all the last page) would still pass a
// has_value() sweep.
TEST(AnchorMap, AnchorPagesAdvanceThroughTheChapter) {
  const auto built = build("pages_advance", kWrappedSpine);
  ASSERT_GT(built.section->pageCount, 1) << "fixture must span several pages to be meaningful";

  EXPECT_TRUE(built.section->getPageForAnchor("ch01").has_value()) << "the wrapping div's id";
  uint16_t previous = 0;
  bool sawLaterPage = false;
  for (int i = 1; i <= 24; ++i) {
    const auto page = built.section->getPageForAnchor("p" + std::to_string(i));
    ASSERT_TRUE(page.has_value()) << "p" << i << " is missing from the anchor map";
    EXPECT_GE(*page, previous) << "p" << i << " maps before the paragraph ahead of it";
    if (*page > previous) sawLaterPage = true;
    previous = *page;
  }
  EXPECT_TRUE(sawLaterPage) << "every anchor mapped to the same page";
}

// A chapter with exactly one anchor: the degenerate case at the other end, and the shape of a
// note document that holds a single note.
TEST(AnchorMap, SingleAnchorChapter) {
  const auto built = build("single", kNotesSpine);
  EXPECT_TRUE(built.section->getPageForAnchor("n1").has_value());
}

// An id the document does not contain must miss rather than resolve to page 0 — the caller's
// fallback depends on being able to tell the difference.
TEST(AnchorMap, UnknownAnchorMisses) {
  const auto built = build("unknown", kWrappedSpine);
  EXPECT_FALSE(built.section->getPageForAnchor("no_such_id").has_value());
}

// An id on a cell of a table inside a table names the page that cell's text is on. The inner table's words were dropped
// once, and its id went with them to whatever paragraph followed.
TEST(AnchorMap, NestedTableCellIdNamesThePageOfItsText) {
  const auto built = build("nested_table_cell", kNestedTargetSpine, kNestedTablesFixture);
  const auto textPage = pageWithWord(*built.section, "TARGETWORD");
  ASSERT_TRUE(textPage.has_value()) << "the inner cell's text is on no page";
  // The pages an id taken at the wrong moment would name: the outer cell's line, the inner cell before, the one after,
  // the paragraph after the table. The fixture must keep each of them off the target's page.
  for (const char* neighbour : {"TARGETOUTER", "TARGETLONG", "TARGETB", "TARGETAFTER"}) {
    const auto page = pageWithWord(*built.section, neighbour);
    ASSERT_TRUE(page.has_value()) << neighbour << " is on no page";
    EXPECT_NE(*page, *textPage) << neighbour << " shares the target's page: the fixture no longer tells them apart";
  }
  const auto anchored = built.section->getPageForAnchor("inner-target");
  ASSERT_TRUE(anchored.has_value()) << "the inner cell's id is missing from the anchor map";
  EXPECT_EQ(static_cast<int>(*anchored), *textPage);
}

namespace {

// What the inline-id cases share: the page an id's map entry names against the page of the word it sits before.
void expectIdOnPageOf(Section& section, const char* id, const char* word) {
  const auto wordPage = pageWithWord(section, word);
  ASSERT_TRUE(wordPage.has_value()) << word << " is on no page";
  const auto anchored = section.getPageForAnchor(id);
  ASSERT_TRUE(anchored.has_value()) << id << " is missing from the anchor map";
  EXPECT_EQ(static_cast<int>(*anchored), *wordPage) << id << " should be on the page of " << word;
}

// The fixture has to put `later` on a later page than `earlier`, or a case cannot tell an id's own line from the next
// block's.
void assertLaterPage(Section& section, const char* earlier, const char* later) {
  const auto first = pageWithWord(section, earlier);
  const auto second = pageWithWord(section, later);
  ASSERT_TRUE(first && second) << earlier << " or " << later << " is on no page";
  ASSERT_LT(*first, *second) << earlier << " and " << later << " share a page: the fixture no longer tells them apart";
}

}  // namespace

// An id on an inline element names the page of ITS line, not the page the paragraph began on (a <span id> was recorded
// with the block's first line, an <a id> with the next block), and an id after the block's last word names the page
// that word is on. A block id keeps its block's first line. The paragraph has three pages and the ids sit on the middle
// one, so the page of the id, of the block's first line and of its last line all differ.
TEST(AnchorMap, InlineIdNamesThePageOfItsOwnLine) {
  const auto built = build("inline_ids", kInlineLongSpine, kInlineAnchorsFixture);
  Section& section = *built.section;
  const auto firstPage = pageWithWord(section, "BLOCKFIRST");
  const auto linkPage = pageWithWord(section, "w560");
  const auto spanPage = pageWithWord(section, "w700");
  const auto lastPage = pageWithWord(section, "w1100");
  ASSERT_TRUE(firstPage && linkPage && spanPage && lastPage);
  ASSERT_LT(*firstPage, *linkPage) << "w560 is on the paragraph's first page: lengthen the fixture";
  ASSERT_LT(*linkPage, *lastPage) << "w560 is on the paragraph's last page: the ids no longer tell block end apart";
  ASSERT_LT(*spanPage, *lastPage);

  expectIdOnPageOf(section, "blk-first", "BLOCKFIRST");
  expectIdOnPageOf(section, "inl-a", "w560");
  expectIdOnPageOf(section, "inl-dfn", "w600");
  expectIdOnPageOf(section, "inl-s", "w700");
  expectIdOnPageOf(section, "inl-tail", "w1100");
  expectIdOnPageOf(section, "blk-next", "NEXTBLOCK");
}

// A paragraph of under 96 words (one layout window) that crosses a page: the id in its second half is on the second
// page. It was recorded with the block's first line.
TEST(AnchorMap, InlineIdInAShortPageCrossingParagraphNamesItsPage) {
  const auto built = build("inline_short", kInlineShortSpine, kInlineAnchorsFixture);
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(*built.section, "s001", "s055"));
  expectIdOnPageOf(*built.section, "short-s", "s055");
}

// An id before its block's first word is on that word's line. It used to wait in the single slot for the next block:
// the next block's id overwrote it (bf1, bf2, bf-h, bf-n, bf-b were lost), or it named the next block's page (bf-solo
// a page late, and bf-br, after a <br>, the page of the line after the next <br>).
TEST(AnchorMap, InlineIdsBeforeABlocksFirstWordAreKept) {
  const auto built = build("inline_before_first", kInlineBeforeFirstSpine, kInlineAnchorsFixture);
  Section& section = *built.section;
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(section, "BFWORD", "NEXTA"));
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(section, "SOLOWORD", "SOLONEXT"));
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(section, "BRWORD", "BRNEXT"));
  expectIdOnPageOf(section, "bf1", "BFWORD");
  expectIdOnPageOf(section, "bf2", "BFWORD");
  expectIdOnPageOf(section, "bf-p", "BFWORD");
  expectIdOnPageOf(section, "bf-next", "NEXTA");
  expectIdOnPageOf(section, "bf-h", "BFTITLE");
  expectIdOnPageOf(section, "bf-hp", "HPWORD");
  expectIdOnPageOf(section, "bf-n", "NOTEWORD");
  expectIdOnPageOf(section, "bf-after", "AFTERN");
  expectIdOnPageOf(section, "bf-b", "BOLDWORD");
  expectIdOnPageOf(section, "bf-last", "LASTN");
  expectIdOnPageOf(section, "bf-solo", "SOLOWORD");
  expectIdOnPageOf(section, "bf-br", "BRWORD");
}

// Ids in a table cell are kept, at the row's page (they were lost to the single slot, a later id overwriting them). The
// row is laid out as a grid, whose words no PageLine holds: the chapter is one page.
TEST(AnchorMap, InlineIdsInATableCellAreKept) {
  const auto built = build("inline_cell", kInlineTableSpine, kInlineAnchorsFixture);
  ASSERT_EQ(built.section->pageCount, 1);
  for (const char* id : {"cell-1", "cell-2", "cell-after"}) {
    const auto anchored = built.section->getPageForAnchor(id);
    ASSERT_TRUE(anchored.has_value()) << id << " is missing from the anchor map";
    EXPECT_EQ(static_cast<int>(*anchored), 0) << id;
  }
}

namespace {

// Chapter 4 of the fixture: an id before every fourth marker word M001..M159 of a paragraph of long words, 8 ids in a
// layout window of 96 tokens, under the 16 the parser's queue holds (past them an id is recorded at the page reached
// so far, which an id every third token did reach, and that, not hyphenation, made most of the drift once measured).
struct HyphenDrift {
  int ids = 0;
  int early = 0;  // ids that name a page before their word's
  int late = 0;   // ids that name a page after it
  int maxPagesOff = 0;
  int splitWords = 0;  // words the layout split at a line's end: the first part ends in the hyphen it was given
};

void measureHyphenDrift(const bool hyphenate, HyphenDrift& out) {
  const auto built =
      build(hyphenate ? "inline_hyph_on" : "inline_hyph_off", kInlineHyphenSpine, kInlineAnchorsFixture, hyphenate);
  Section& section = *built.section;
  ASSERT_GT(section.pageCount, 1);
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page) << "page " << pg;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
      if (!block || !block->valid()) continue;
      for (uint16_t w = 0; w < block->wordCount(); ++w) {
        const std::string_view text(block->wordText(w), block->wordTextLen(w));
        if (text.size() > 1 && text.back() == '-') ++out.splitWords;
      }
    }
  }
  for (int i = 1; i < 160; i += 4) {
    char id[16];
    char word[16];
    std::snprintf(id, sizeof(id), "hy%03d", i);
    std::snprintf(word, sizeof(word), "M%03d", i);
    const auto wordPage = pageWithWord(section, word);
    ASSERT_TRUE(wordPage.has_value()) << word;
    const auto anchored = section.getPageForAnchor(id);
    ASSERT_TRUE(anchored.has_value()) << id << " is missing from the anchor map";
    const int off = static_cast<int>(*anchored) - *wordPage;
    ++out.ids;
    if (off < 0) ++out.early;
    if (off > 0) ++out.late;
    out.maxPagesOff = std::max(out.maxPagesOff, std::abs(off));
  }
  std::printf("[ HYPH-DRIFT ] hyphenation %s: %d words split; of %d ids, %d a page before their word's, %d after\n",
              hyphenate ? "on" : "off", out.splitWords, out.ids, out.early, out.late);
}

}  // namespace

// Without hyphenation the lines count the parser's tokens exactly: every id names its word's page.
TEST(AnchorMap, InlineIdsNameTheirWordsPageWithHyphenationOff) {
  HyphenDrift drift;
  ASSERT_NO_FATAL_FAILURE(measureHyphenDrift(/*hyphenate=*/false, drift));
  EXPECT_EQ(drift.ids, 40);
  EXPECT_EQ(drift.early, 0);
  EXPECT_EQ(drift.late, 0);
}

// Hyphenation on, the same paragraph: its long words split at the lines' ends (84 of them in this layout). A split's
// remainder is a token of its own in the lines' count but not in the parser's (ParsedText::hyphenateWordAtIndex), so
// the lines run ahead of an id's index and can record it a line or two early: 2 of the 40 ids name the page before
// their word's here (docs/future_work/memory-and-parsing.md). Never late, never more than a page off (the link Back
// stack's tolerance); tighten this to the word's page once a split counts as one token.
TEST(AnchorMap, InlineIdsHoldWithHyphenationOn) {
  HyphenDrift drift;
  ASSERT_NO_FATAL_FAILURE(measureHyphenDrift(/*hyphenate=*/true, drift));
  ASSERT_GT(drift.splitWords, 0) << "no word was split: the case does not test hyphenation";
  EXPECT_EQ(drift.ids, 40);
  EXPECT_EQ(drift.late, 0);
  EXPECT_LE(drift.maxPagesOff, 1);
}

// An id after a paragraph's last word (a space between: no word of the block reaches it) is on the page of that last
// word when the next block starts a new page. It was on the next block's.
TEST(AnchorMap, TrailingInlineIdStaysOnTheLastLinesPage) {
  const auto built = build("inline_tail", kInlineTailSpine, kInlineAnchorsFixture, false, /*embeddedStyle=*/true);
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(*built.section, "TAILFIRST", "t400"));
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(*built.section, "t400", "TAILNEXT"));
  expectIdOnPageOf(*built.section, "tail-id", "t400");
}

// Ids on elements with no text that end the chapter get no line: they name the last page. Recorded after the last page
// was emitted, the block's and the span's id named the page past it (and the <a id> too, once queued as inline).
TEST(AnchorMap, IdsEndingTheChapterNameItsLastPage) {
  const auto built = build("inline_end", kInlineTailSpine, kInlineAnchorsFixture, false, /*embeddedStyle=*/true);
  const int lastPage = built.section->pageCount - 1;
  ASSERT_EQ(pageWithWord(*built.section, "TAILNEXT"), lastPage);
  for (const char* id : {"end-a", "end-s", "end-p"}) {
    const auto anchored = built.section->getPageForAnchor(id);
    ASSERT_TRUE(anchored.has_value()) << id << " is missing from the anchor map";
    EXPECT_EQ(static_cast<int>(*anchored), lastPage) << id;
  }
}

// An <a id> opened after a div's last word that wraps a block names the wrapped block's first line, not the div's last
// line (a page earlier here).
TEST(AnchorMap, AnInlineIdWrappingABlockNamesTheBlocksLine) {
  const auto built = build("inline_wrap", kInlineWrapSpine, kInlineAnchorsFixture, false, /*embeddedStyle=*/true);
  ASSERT_NO_FATAL_FAILURE(assertLaterPage(*built.section, "x400", "WRAPPED"));
  expectIdOnPageOf(*built.section, "wrap-id", "WRAPPED");
}

// An id after a line's last word and before its <br> is on that line's page, where a page ends at the <br>. A <br>
// splits the paragraph's block, as a block element does inside a div, but the id is not the next line's.
TEST(AnchorMap, AnInlineIdBeforeABreakStaysOnItsLine) {
  const auto built = build("inline_br", kInlineBreakSpine, kInlineAnchorsFixture, false, /*embeddedStyle=*/true);
  Section& section = *built.section;
  int checked = 0;
  for (int i = 1; i <= 150; ++i) {
    char id[16];
    char left[16];
    char right[16];
    snprintf(id, sizeof(id), "br-%03d", i);
    snprintf(left, sizeof(left), "L%03d", i);
    snprintf(right, sizeof(right), "R%03d", i);
    const auto leftPage = pageWithWord(section, left);
    const auto rightPage = pageWithWord(section, right);
    ASSERT_TRUE(leftPage && rightPage) << left;
    if (*leftPage == *rightPage) continue;
    ++checked;
    expectIdOnPageOf(section, id, left);
  }
  EXPECT_GT(checked, 0) << "no page of the fixture's chapter 7 ends at a <br>: vary its margins";
}
