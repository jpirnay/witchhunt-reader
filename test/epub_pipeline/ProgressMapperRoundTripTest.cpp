// KOReader sync round trips over the whole test corpus: every page of every chapter is uploaded
// (CrossPoint -> KOReader) and pulled back (KOReader -> CrossPoint), then snapped to a page the
// way the reader does it, and must come back on the page it left or shortly before it.
//
// Two directions, two parsers, two ways of counting the chapter's text; the corpus has the
// shapes that break such pairs (wrapped paragraphs, headings, lists, tables, hidden text,
// image-only chapters, entities, ligatures). What has to hold:
//   1. The pull never lands AHEAD of the pushed page. Behind costs a re-read; ahead skips text.
//   2. It lands at most one page behind, the known cost of the upload naming the paragraph
//      rather than the character: a paragraph that starts on the previous page pulls the
//      reader back to it.
// Two books are allowed more, below, each for a measured reason. Anything else is a mismatch
// between the two directions.
//
// The reader's snap (EpubReaderActivity::NavigationTarget::resolveInto) is mirrored here: a
// list-item index goes through the li LUT, then the paragraph LUT; a paragraph index through the
// paragraph LUT; a miss keeps the mapper's page estimate.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexer.h"
#include "KOReaderSync/ProgressMapper.h"

namespace fs = std::filesystem;

namespace {

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

std::vector<std::string> corpusBooks() {
  std::vector<std::string> books;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") books.push_back(entry.path().string());
  }
  std::sort(books.begin(), books.end());
  return books;
}

// How far from the pushed page a pull may land, per book. One page behind is the paragraph-start
// cost every book pays; ahead is never right. The two listed books do worse, and the numbers pin
// what they do today so a change in either direction is noticed:
//   test_table_cell_overflow: its paragraphs run three pages each, so the start of the one the
//     reader is in can be two pages back.
//   test_spine_toc_edges: the upload picks the paragraph by byte fraction of the chapter's text,
//     and these chapters' density is uneven. The appendix is densest on its first pages, so page
//     3's position is named as p[6], which the layout put on page 1 (two behind). The front
//     matter and chapter 3 start with headings and a blockquote, many lines for few bytes, so
//     half the bytes are reached a page before half the pages, and the paragraph named is the
//     one on the NEXT page (one ahead: the reader skips a page). An upload taken from the
//     section cache's paragraph LUT instead of the byte scan would remove both.
struct Allowance {
  int behind;
  int ahead;
};

Allowance allowedDrift(const std::string& book) {
  static const std::map<std::string, Allowance> known = {
      {"test_table_cell_overflow.epub", {2, 0}},
      {"test_spine_toc_edges.epub", {2, 1}},
  };
  const auto it = known.find(fs::path(book).filename().string());
  return it == known.end() ? Allowance{1, 0} : it->second;
}

// The page the reader would open after applying `pos` to the chapter `section` is built for.
int snappedPage(const CrossPointPosition& pos, const Section& section) {
  if (pos.hasListItemIndex) {
    if (const auto p = section.getPageForListItemIndex(pos.listItemIndex)) return *p;
    if (const auto p = section.getPageForParagraphIndex(pos.listItemIndex)) return *p;
    return pos.pageNumber;
  }
  if (pos.hasParagraphIndex) {
    if (const auto p = section.getPageForParagraphIndex(pos.paragraphIndex)) return *p;
  }
  return pos.pageNumber;
}

struct RoundTripFixture : testing::TestWithParam<std::string> {
  fs::path work;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    // One directory per test AND book: ctest runs the three tests of a book in parallel
    // processes, and they must not delete each other's cache.
    std::string tag = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : tag) {
      if (c == '/') c = '_';
    }
    work = fs::temp_directory_path() / "progress_mapper_roundtrip" / tag;
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(GetParam(), (work / "cache").string());
    ASSERT_TRUE(epub->load(true)) << GetParam();
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }

  std::unique_ptr<Section> build(const int spine) {
    auto section = std::make_unique<Section>(epub, spine, renderer);
    EXPECT_TRUE(section->createSectionFile(params(), {}, /*skipEviction=*/true)) << "spine " << spine;
    EXPECT_TRUE(section->loadSectionFile(params())) << "spine " << spine;
    return section;
  }

  float spineStartPercentage(const int spine) const {
    const float start = spine > 0 ? static_cast<float>(epub->getCumulativeSpineItemSize(spine - 1)) : 0.0f;
    return start / static_cast<float>(epub->getBookSize());
  }
};

TEST_P(RoundTripFixture, EveryPageComesBackOnOrShortlyBeforeItself) {
  const Allowance allowed = allowedDrift(GetParam());
  const int spineCount = epub->getSpineItemsCount();
  ASSERT_GT(spineCount, 0);
  int worstBehind = 0;
  int worstAhead = 0;
  for (int spine = 0; spine < spineCount; ++spine) {
    const auto section = build(spine);
    const int pages = section->pageCount;
    ASSERT_GT(pages, 0) << "spine " << spine;
    for (int page = 0; page < pages; ++page) {
      const auto ko = ProgressMapper::toKOReader(epub, {spine, page, pages});
      const auto back = ProgressMapper::toCrossPoint(epub, ko, spine, pages);
      EXPECT_EQ(back.spineIndex, spine) << "page " << page << " via " << ko.xpath;
      if (back.spineIndex != spine) continue;
      const int landed = snappedPage(back, *section);
      worstBehind = std::max(worstBehind, page - landed);
      worstAhead = std::max(worstAhead, landed - page);
      EXPECT_LE(landed - page, allowed.ahead)
          << "spine " << spine << " page " << page << "/" << pages << " came back AHEAD, as " << landed << " (estimate "
          << back.pageNumber << ") via " << ko.xpath;
      EXPECT_LE(page - landed, allowed.behind)
          << "spine " << spine << " page " << page << "/" << pages << " came back as " << landed << " (estimate "
          << back.pageNumber << ") via " << ko.xpath;
    }
  }
  RecordProperty("worstPagesBehind", worstBehind);
  RecordProperty("worstPagesAhead", worstAhead);
}

TEST_P(RoundTripFixture, LastPageDoesNotFallToTheChapterStart) {
  // The failure that matters here is not a page of drift but a fall to page 0: the two passes
  // of the forward mapper counting the chapter's text differently would make the end of the
  // chapter unreachable, and the upload would name the chapter's root instead.
  const int allowed = allowedDrift(GetParam()).behind;
  const int spineCount = epub->getSpineItemsCount();
  for (int spine = 0; spine < spineCount; ++spine) {
    const auto section = build(spine);
    const int pages = section->pageCount;
    if (pages < 2) continue;
    const auto ko = ProgressMapper::toKOReader(epub, {spine, pages - 1, pages});
    const auto back = ProgressMapper::toCrossPoint(epub, ko, spine, pages);
    EXPECT_EQ(back.spineIndex, spine);
    EXPECT_GE(snappedPage(back, *section), pages - 1 - allowed) << "spine " << spine << " via " << ko.xpath;
  }
}

TEST_P(RoundTripFixture, ChapterStartStaysOnFirstPageWhateverThePercentageSays) {
  // What a push from the start of a chapter on any device looks like, with KOReader's percentage
  // skewed either way by up to 5% of the book (issue #268 was +1.5%). The skew may even point
  // into the neighbouring chapter: the XPath still wins.
  constexpr float kSkews[] = {-0.05f, -0.011f, -0.01f, -0.001f, 0.0f, 0.001f, 0.01f, 0.011f, 0.05f};
  const int spineCount = epub->getSpineItemsCount();
  for (int spine = 0; spine < spineCount; ++spine) {
    const auto section = build(spine);
    const int pages = section->pageCount;
    const auto ko = ProgressMapper::toKOReader(epub, {spine, 0, pages});
    for (const float skew : kSkews) {
      const float percentage = std::clamp(spineStartPercentage(spine) + skew, 0.0f, 1.0f);
      const auto back = ProgressMapper::toCrossPoint(epub, {ko.xpath, percentage}, spine, pages);
      EXPECT_EQ(back.spineIndex, spine) << "skew " << skew << " via " << ko.xpath;
      if (back.spineIndex != spine) continue;
      EXPECT_EQ(snappedPage(back, *section), 0) << "spine " << spine << " skew " << skew << " via " << ko.xpath;
    }
  }
}

// A page start that pushes as a text point comes back on exactly that offset; one that pushes as
// an element path resolves on or before it. This is the counting agreement between the parser's
// LUT and the two mappers, with no allowance. An element match is anchored at its first direct
// text or its END tag, which is no page start, so toCrossPoint does not hand its offset to the
// reader (the paragraph LUT or the estimate lands it); its anchor is checked at the resolver.
TEST_P(RoundTripFixture, EveryTextPointComesBackExactly) {
  const int spineCount = epub->getSpineItemsCount();
  int textPoints = 0;
  for (int spine = 0; spine < spineCount; ++spine) {
    const auto section = build(spine);
    const int pages = section->pageCount;
    for (int page = 0; page < pages; ++page) {
      const auto start = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
      if (!start) continue;
      CrossPointPosition pos{};
      pos.spineIndex = spine;
      pos.pageNumber = page;
      pos.totalPages = pages;
      pos.visibleTextOffset = *start;
      pos.hasVisibleTextOffset = true;
      const auto ko = ProgressMapper::toKOReader(epub, pos);
      if (ko.xpath.find("/text()[") == std::string::npos) {
        float intra = 0.0f;
        bool exact = false;
        uint32_t anchor = 0;
        ASSERT_TRUE(ChapterXPathIndexer::findProgressForXPath(epub, spine, ko.xpath, intra, exact, nullptr, &anchor))
            << "spine " << spine << " page " << page << " via " << ko.xpath;
        EXPECT_LE(anchor, *start) << "spine " << spine << " page " << page << " via " << ko.xpath;
        EXPECT_FALSE(ProgressMapper::toCrossPoint(epub, ko).hasVisibleTextOffset)
            << "spine " << spine << " page " << page << " via " << ko.xpath;
        continue;
      }
      ++textPoints;
      const auto back = ProgressMapper::toCrossPoint(epub, ko);
      ASSERT_TRUE(back.hasVisibleTextOffset) << "spine " << spine << " page " << page << " via " << ko.xpath;
      EXPECT_EQ(back.visibleTextOffset, *start) << "spine " << spine << " page " << page << " via " << ko.xpath;
    }
  }
  EXPECT_GT(textPoints, 0);
}

INSTANTIATE_TEST_SUITE_P(Corpus, RoundTripFixture, testing::ValuesIn(corpusBooks()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           std::string name = fs::path(info.param).stem().string();
                           for (char& c : name) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return name;
                         });

}  // namespace
