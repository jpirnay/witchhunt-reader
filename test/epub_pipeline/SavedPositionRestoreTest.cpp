// A saved position -- progress.bin on a reopen, a link Back entry on Back -- is a page, the paragraph
// under it and the chapter's page count at the time (EpubNavigationTarget::makeSavedPage). This pins
// how one resolves against a real built chapter.
//
// Issue #604: Back from a footnote landed about four pages before the page the reader had left. The
// Back stack resolved its entry through the paragraph, and a paragraph maps to the FIRST page its
// index covers: where a paragraph running over page breaks starts, or the first of a run of pages
// sharing one index, which is what a chapter with only some body-child <p>s gives. Moby-Dick on the
// host: 73 of 870 pages came back early, by up to 19 pages. The saved page is exact while the chapter
// keeps its layout, which coming back from a note always does.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "src/activities/reader/EpubNavigationTarget.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSpine = 1;  // the unwrapped chapter: its pages carry paragraph anchors
const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

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

struct Built {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
};

Built build(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "saved_position_restore_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  Built out;
  out.epub = std::make_shared<Epub>(kFixture, dir.string());
  EXPECT_TRUE(out.epub->load(true));
  out.epub->setupCacheDir();
  GfxRenderer renderer;
  out.section = std::make_unique<Section>(out.epub, kSpine, renderer);
  EXPECT_TRUE(out.section->createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_TRUE(out.section->loadSectionFile(params()));
  return out;
}

// A page whose paragraph anchor resolves to an EARLIER page: the case the bug needs. The fixture
// has one (a paragraph running over a page break); asserting it keeps the tests below meaningful.
struct EarlyAnchor {
  int page = -1;
  uint16_t paragraph = 0;
  int paragraphPage = -1;
};

EarlyAnchor findEarlyAnchor(const Section& section) {
  for (uint16_t page = 1; page < static_cast<uint16_t>(section.pageCount); ++page) {
    const auto paragraph = section.getParagraphIndexForPage(page);
    if (!paragraph) continue;
    const auto back = section.getPageForParagraphIndex(*paragraph);
    if (back && *back < page) return {page, *paragraph, *back};
  }
  return {};
}

int resolve(const EpubNavigationTarget& target, Section& section) {
  section.currentPage = 0;
  target.resolveInto(section, kSpine);
  return section.currentPage;
}

}  // namespace

// The regression: the chapter is laid out as it was when the position was saved, so Back lands on
// the saved page itself, not where its paragraph starts.
TEST(SavedPositionRestore, AnUnchangedChapterLandsOnTheSavedPage) {
  auto built = build("unchanged");
  const auto early = findEarlyAnchor(*built.section);
  ASSERT_GE(early.page, 0) << "fixture must have a page whose paragraph starts on an earlier page";

  const auto target =
      EpubNavigationTarget::makeSavedPage(kSpine, early.page, built.section->pageCount, early.paragraph);
  EXPECT_EQ(resolve(target, *built.section), early.page)
      << "p[" << early.paragraph << "] starts on page " << early.paragraphPage << "; Back must not go there";
}

// A chapter laid out anew since the save (another font, a firmware that moved the layout) has a
// different page count, and the saved page number no longer means anything: the paragraph does.
TEST(SavedPositionRestore, ARelaidOutChapterLandsOnTheSavedParagraph) {
  auto built = build("relaid_out");
  const auto early = findEarlyAnchor(*built.section);
  ASSERT_GE(early.page, 0);

  const auto target =
      EpubNavigationTarget::makeSavedPage(kSpine, early.page, built.section->pageCount + 3, early.paragraph);
  EXPECT_EQ(resolve(target, *built.section), early.paragraphPage);
}

// With no paragraph (a page none of whose paragraphs is counted, or a position saved mid-build) a
// relaid-out chapter rescales the page by the page counts instead.
TEST(SavedPositionRestore, ARelaidOutChapterWithoutAParagraphRescalesThePage) {
  auto built = build("rescaled");
  const int pages = built.section->pageCount;
  ASSERT_GT(pages, 2);

  const int savedPage = pages - 1;
  const int savedPageCount = pages * 2;
  const auto target = EpubNavigationTarget::makeSavedPage(kSpine, savedPage, savedPageCount, 0);
  // resolveInto's own arithmetic, so float rounding cannot make the two disagree by a page.
  const int rescaled =
      static_cast<int>(static_cast<float>(savedPage) / static_cast<float>(savedPageCount) * static_cast<float>(pages));
  ASSERT_NE(rescaled, savedPage);
  EXPECT_EQ(resolve(target, *built.section), rescaled);
}
