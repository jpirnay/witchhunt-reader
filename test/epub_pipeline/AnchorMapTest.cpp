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

// test/epubs/make_test_inline_anchors.py: one paragraph of numbered words w001..w700 that crosses pages, with an <a id>
// before w560, a <span id> before w640 and an <a id> after w700, then a block with its own id.
const std::string kInlineAnchorsFixture = std::string(CORPUS_DIR) + "/test_inline_anchors.epub";

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "anchor_map_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

struct BuiltSection {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
};

BuiltSection build(const std::string& tag, int spineIndex, const std::string& fixture = kFixture) {
  BuiltSection out;
  out.epub = std::make_shared<Epub>(fixture, freshCacheDir(tag));
  EXPECT_TRUE(out.epub->load(true));
  out.epub->setupCacheDir();

  GfxRenderer renderer;
  out.section = std::make_unique<Section>(out.epub, spineIndex, renderer);
  EXPECT_TRUE(out.section->createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_TRUE(out.section->loadSectionFile(params()));
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

// An id on an inline element names the page of ITS line, not the page the paragraph began on (a <span id> was recorded
// with the block's first line, an <a id> with the next block), and an id after the block's last word names the page
// that word is on. A block id keeps its block's first line.
TEST(AnchorMap, InlineIdNamesThePageOfItsOwnLine) {
  const auto built = build("inline_ids", 0, kInlineAnchorsFixture);
  Section& section = *built.section;
  const auto firstPage = pageWithWord(section, "BLOCKFIRST");
  const auto linkPage = pageWithWord(section, "w560");
  const auto spanPage = pageWithWord(section, "w640");
  const auto lastPage = pageWithWord(section, "w700");
  const auto nextPage = pageWithWord(section, "NEXTBLOCK");
  ASSERT_TRUE(firstPage && linkPage && spanPage && lastPage && nextPage);
  // The fixture has to tell the places apart: each id's own line is a later page than the block's first.
  ASSERT_LT(*firstPage, *linkPage) << "w560 is on the paragraph's first page: lengthen the fixture";
  ASSERT_LT(*firstPage, *spanPage);

  const auto expectPage = [&](const char* id, const int page) {
    const auto anchored = section.getPageForAnchor(id);
    ASSERT_TRUE(anchored.has_value()) << id << " is missing from the anchor map";
    EXPECT_EQ(static_cast<int>(*anchored), page) << id;
  };
  expectPage("blk-first", *firstPage);
  expectPage("inl-a", *linkPage);
  expectPage("inl-s", *spanPage);
  expectPage("inl-tail", *lastPage);
  expectPage("blk-next", *nextPage);
}
