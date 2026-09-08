// Cover for the section cache's ANCHOR MAP — the id-to-page table every fragment jump lands on
// (`notes.xhtml#en1`, a TOC entry pointing mid-chapter, a cross-reference).
//
// The golden layout dumps cannot see this table: they compare what dumpPage prints, which is the
// text of each page and nothing about its anchors. A build that wrote an empty anchor map, or one
// missing every entry but the last, passes all 41 goldens — so the map needs cover of its own.
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
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

// Spine 0 wraps its paragraphs in a div (id "ch01") and carries p1..p24; spine 2 is the note
// document, whose single note body is the one anchor in it.
constexpr int kWrappedSpine = 0;
constexpr int kNotesSpine = 2;

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

BuiltSection build(const std::string& tag, int spineIndex) {
  BuiltSection out;
  out.epub = std::make_shared<Epub>(kFixture, freshCacheDir(tag));
  EXPECT_TRUE(out.epub->load(true));
  out.epub->setupCacheDir();

  GfxRenderer renderer;
  out.section = std::make_unique<Section>(out.epub, spineIndex, renderer);
  EXPECT_TRUE(out.section->createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_TRUE(out.section->loadSectionFile(params()));
  return out;
}

}  // namespace

// Every id in the document resolves. Not "the map is non-empty" — a map that kept only some of
// its entries still answers a lookup, it just silently strands the fragments it dropped, which
// on a notes document means the footnotes that no longer go anywhere.
TEST(AnchorMap, EveryAnchorInTheChapterResolves) {
  const auto built = build("all_anchors", kWrappedSpine);

  EXPECT_TRUE(built.section->getPageForAnchor("ch01").has_value()) << "the wrapping div's id";
  for (int i = 1; i <= 24; ++i) {
    const std::string id = "p" + std::to_string(i);
    EXPECT_TRUE(built.section->getPageForAnchor(id).has_value()) << id << " is missing from the anchor map";
  }
}

// The last anchor in a document is recorded during finalize, after the parse has ended, which
// makes it the one entry a lifecycle mistake drops (or keeps alone). Pinned separately from the
// sweep above so a failure says which case broke.
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

// Anchors map to the page they actually sit on, in order. A map whose pages were all zero (or all
// the last page) would still pass a has_value() sweep.
TEST(AnchorMap, AnchorPagesAdvanceThroughTheChapter) {
  const auto built = build("pages_advance", kWrappedSpine);
  ASSERT_GT(built.section->pageCount, 1) << "fixture must span several pages to be meaningful";

  uint16_t previous = 0;
  bool sawLaterPage = false;
  for (int i = 1; i <= 24; ++i) {
    const auto page = built.section->getPageForAnchor("p" + std::to_string(i));
    ASSERT_TRUE(page.has_value()) << "p" << i;
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
