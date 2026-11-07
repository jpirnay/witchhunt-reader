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

constexpr int kSpine = 1;         // the unwrapped chapter: its pages carry paragraph anchors
constexpr int kWrappedSpine = 0;  // <body><div><p>, as Calibre writes it: no page has a paragraph anchor
const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

Section::BuildParams params(const uint16_t viewportHeight = 800) {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = viewportHeight;
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

Built build(const std::string& tag, const int spine = kSpine, const uint16_t viewportHeight = 800) {
  const auto dir = fs::temp_directory_path() / "saved_position_restore_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  Built out;
  out.epub = std::make_shared<Epub>(kFixture, dir.string());
  EXPECT_TRUE(out.epub->load(true));
  out.epub->setupCacheDir();
  GfxRenderer renderer;
  out.section = std::make_unique<Section>(out.epub, spine, renderer);
  EXPECT_TRUE(out.section->createSectionFile(params(viewportHeight), {}, /*skipEviction=*/true));
  EXPECT_TRUE(out.section->loadSectionFile(params(viewportHeight)));
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

int resolve(const EpubNavigationTarget& target, Section& section, const int spine = kSpine) {
  section.currentPage = 0;
  target.resolveInto(section, spine);
  return section.currentPage;
}

// `offset` falls on `page`: the page starts at or before it, and the next start is past it.
void expectOffsetOnPage(const Section& section, const int page, const uint32_t offset) {
  const auto start = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
  ASSERT_TRUE(start.has_value());
  EXPECT_LE(*start, offset);
  if (const auto after = section.getVisibleTextOffsetAfterPage(static_cast<uint16_t>(page))) {
    EXPECT_GT(*after, offset);
  }
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

// Another font or size: the page number means nothing, the text does.
TEST(SavedPositionRestore, AnOffsetLandsOnTheSameTextUnderANewLayout) {
  auto before = build("offset_before");
  auto after = build("offset_after", kSpine, /*viewportHeight=*/500);
  ASSERT_NE(before.section->pageCount, after.section->pageCount);

  const int page = before.section->pageCount / 2;
  const auto offset = before.section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
  ASSERT_TRUE(offset.has_value());
  const auto target = EpubNavigationTarget::makeSavedPage(kSpine, page, before.section->pageCount, 0, *offset);
  expectOffsetOnPage(*after.section, resolve(target, *after.section), *offset);
}

// What a Calibre book needs: no page has a paragraph, and the page number was all there was.
TEST(SavedPositionRestore, AnOffsetLandsOnTheSameTextInAChapterWithoutParagraphs) {
  auto before = build("wrapped_before", kWrappedSpine);
  auto after = build("wrapped_after", kWrappedSpine, /*viewportHeight=*/500);
  ASSERT_GT(before.section->pageCount, 1);

  const int page = before.section->pageCount - 1;
  ASSERT_FALSE(before.section->getParagraphIndexForPage(static_cast<uint16_t>(page)).has_value())
      << "the fixture's wrapped chapter must have no paragraph anchors";
  const auto offset = before.section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
  ASSERT_TRUE(offset.has_value());
  const auto target = EpubNavigationTarget::makeSavedPage(kWrappedSpine, page, before.section->pageCount, 0, *offset);
  expectOffsetOnPage(*after.section, resolve(target, *after.section, kWrappedSpine), *offset);
}

// The offset must cost the unchanged case nothing: every page comes back to itself, a page sharing
// its start with the next (an image before the text) included.
TEST(SavedPositionRestore, AnUnchangedChapterKeepsEveryPageWithAnOffset) {
  auto built = build("offset_unchanged");
  for (uint16_t page = 0; page < static_cast<uint16_t>(built.section->pageCount); ++page) {
    const auto offset = built.section->getVisibleTextOffsetForPage(page);
    ASSERT_TRUE(offset.has_value());
    const auto target = EpubNavigationTarget::makeSavedPage(kSpine, page, built.section->pageCount, 0, *offset);
    EXPECT_EQ(resolve(target, *built.section), page);
  }
}

// A new layout can have the old page count by chance. The paragraph path never looks, the offset
// does: the page no longer starts on the saved text.
TEST(SavedPositionRestore, AnOffsetSeesANewLayoutWithTheSamePageCount) {
  auto built = build("offset_same_count");
  const int pages = built.section->pageCount;
  ASSERT_GT(pages, 3);
  const auto offset = built.section->getVisibleTextOffsetForPage(static_cast<uint16_t>(pages - 1));
  ASSERT_TRUE(offset.has_value());
  const auto target = EpubNavigationTarget::makeSavedPage(kSpine, /*page=*/1, pages, 0, *offset);
  EXPECT_EQ(resolve(target, *built.section), pages - 1);
}

// A build shows the page as soon as it knows it, and what it says then has to be what the finished
// cache says, or the reader would land on one page and be moved to another.
TEST(SavedPositionRestore, ALiveBuildAnswersAsTheFinishedChapterWill) {
  // Short pages, so that a page in the middle has plenty after it: an offset is only known once a later page has
  // started.
  constexpr uint16_t kShortPage = 160;
  auto reference = build("offset_reference", kSpine, kShortPage);
  ASSERT_GT(reference.section->pageCount, 8);
  const auto offset =
      reference.section->getVisibleTextOffsetForPage(static_cast<uint16_t>(reference.section->pageCount / 2));
  ASSERT_TRUE(offset.has_value());

  const auto dir = fs::temp_directory_path() / "saved_position_restore_test" / "offset_live";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto epub = std::make_shared<Epub>(kFixture, dir.string());
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  Section section(epub, kSpine, renderer);
  std::optional<uint16_t> answered;
  {
    const host_clock::Ticking tick(1);  // or the budget never runs out, and there is no mid-build to ask in
    for (int i = 0; i < 200000; ++i) {
      const Section::BuildStep step = section.stepSectionBuild(params(kShortPage), /*budgetMs=*/1);
      if (!answered && section.hasActiveBuild()) answered = section.activeBuildPageForVisibleOffset(*offset);
      if (step == Section::BuildStep::Done || step == Section::BuildStep::Failed) break;
    }
  }
  ASSERT_TRUE(answered.has_value()) << "a page in the middle of the chapter must be known before the build ends";
  ASSERT_TRUE(section.loadSectionFile(params(kShortPage)));
  EXPECT_EQ(answered, section.getPageForVisibleTextOffset(*offset));
}
