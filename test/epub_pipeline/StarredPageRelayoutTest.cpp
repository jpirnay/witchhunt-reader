// A starred page after a relayout: it stays on its text, not on its page number.
//
// A star used to keep only spine and page, so after a font, size, margin or spacing change it named whatever text the
// new layout put on that page number. Now it also keeps the KOReader XPath of the page's first text (StarAnchoring,
// the sync's own mappers), and a section of another layout moves it onto the page that holds that text. What has to
// hold, for every page of every chapter of the corpus starred under one layout, after the book is closed (the XPath is
// all that is left) and opened under another:
//   1. the star is on the page whose window of visible text holds the text its page started with;
//   2. it now counts in the new layout, so the work is done once;
//   3. a star with no anchor (older firmware) keeps its page, as before.
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexer.h"
#include "src/BookmarkStore.h"
#include "src/StarAnchoring.h"

namespace fs = std::filesystem;

namespace {

// The layout the pages are starred in.
Section::BuildParams layoutA() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  return p;
}

// Another font at a larger size, as this harness can have it: its stub metrics are the same for every font, so the
// lines are made shorter and further apart instead, which is what a larger size does to them.
Section::BuildParams layoutB() {
  Section::BuildParams p = layoutA();
  p.fontId = 1;
  p.lineCompression = 1.35f;
  p.viewportWidth = 352;
  p.viewportHeight = 590;
  return p;
}

// Smaller again, for the second relayout of one session.
Section::BuildParams layoutC() {
  Section::BuildParams p = layoutA();
  p.fontId = 2;
  p.lineCompression = 0.9f;
  p.viewportWidth = 560;
  p.viewportHeight = 780;
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

// A star made in layout A, the text its page started with, and whether that text has a name: a chapter with no text
// at or after the page's start (a cover, a picture) has none, and its star keeps its page.
struct Starred {
  uint16_t spine;
  uint16_t page;
  uint32_t offset;
  bool named;
};

struct StarredPageRelayout : testing::TestWithParam<std::string> {
  fs::path work;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    std::string tag = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : tag) {
      if (c == '/') c = '_';
    }
    work = fs::temp_directory_path() / ("starred_page_relayout_" + std::to_string(::getpid())) / tag;
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(GetParam(), (work / "cache").string());
    ASSERT_TRUE(epub->load(true)) << GetParam();
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work.parent_path());
  }

  std::unique_ptr<Section> build(const int spine, const Section::BuildParams& p) {
    auto section = std::make_unique<Section>(epub, spine, renderer);
    EXPECT_TRUE(section->createSectionFile(p, {}, /*skipEviction=*/true)) << "spine " << spine;
    EXPECT_TRUE(section->loadSectionFile(p)) << "spine " << spine;
    return section;
  }

  // Stars every page of every chapter in layout A that has an offset, names their text as the reader's idle time
  // does, and closes the book.
  std::vector<Starred> starEveryPage() {
    BookmarkStore store;
    store.load(epub->getCachePath());
    std::vector<Starred> starred;
    for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
      const auto section = build(spine, layoutA());
      for (int page = 0; page < section->pageCount; ++page) {
        const StarAnchor anchor = StarAnchoring::forPage(*section, page);
        if (anchor.offset == StarAnchor::kNoOffset) continue;
        EXPECT_TRUE(store.toggle(static_cast<uint16_t>(spine), static_cast<uint16_t>(page), anchor));
        starred.push_back({static_cast<uint16_t>(spine), static_cast<uint16_t>(page), anchor.offset, false});
      }
    }
    while (StarAnchoring::step(store, epub, nullptr, -1) == StarAnchoring::Work::Parsed) {
    }
    for (size_t i = 0; i < starred.size(); ++i) {
      starred[i].named = !store.getAnchors()[i].xpath.empty();
      if (!starred[i].named) {
        EXPECT_TRUE(ChapterXPathIndexer::findXPathForVisibleOffset(epub, starred[i].spine, starred[i].offset).empty())
            << "spine " << starred[i].spine << " page " << starred[i].page << ": the push names it, the star did not";
      }
    }
    store.save();
    return starred;
  }

  // Settles the spine's stars in `section` as the reader's idle time does.
  static void settle(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section& section, const int spine) {
    for (int calls = 0; StarAnchoring::step(store, epub, &section, spine) != StarAnchoring::Work::None; ++calls) {
      ASSERT_LT(calls, 100000);
    }
  }
};

// Whether `page` of `section` holds the text at `offset`: its start is at or before it, and the next page that starts
// further on starts after it.
bool pageHolds(const Section& section, const uint16_t page, const uint32_t offset) {
  const auto start = section.getVisibleTextOffsetForPage(page);
  if (!start || *start > offset) return false;
  return offset < section.getVisibleTextOffsetAfterPage(page).value_or(UINT32_MAX);
}

}  // namespace

TEST_P(StarredPageRelayout, EveryStarredPageFindsItsTextInAnotherLayout) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";

  // The book opened again under layout B: only what the file keeps.
  BookmarkStore store;
  store.load(epub->getCachePath());
  ASSERT_EQ(store.getAll().size(), starred.size());
  int moved = 0;
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutB());
    settle(store, epub, *section, spine);
    for (size_t i = 0; i < starred.size(); ++i) {
      if (starred[i].spine != spine) continue;
      const uint16_t page = store.getAll()[i].pageNumber;
      if (!starred[i].named) {
        EXPECT_EQ(page, starred[i].page) << "spine " << spine << ": nothing to find it by, so its page stands";
        continue;
      }
      EXPECT_TRUE(pageHolds(*section, page, starred[i].offset))
          << "spine " << spine << ": the star of page " << starred[i].page << " (text at " << starred[i].offset
          << ") is on page " << page << "/" << section->pageCount << " via " << store.getAnchors()[i].xpath;
      EXPECT_EQ(store.getAnchors()[i].layout, section->layoutFingerprint());
      EXPECT_EQ(store.getAnchors()[i].pageCount, section->pageCount);
      if (page != starred[i].page) moved++;
    }
  }
  // Saved, it stands under B on the next open: nothing left to do.
  store.save();
  BookmarkStore again;
  again.load(epub->getCachePath());
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    Section section(epub, spine, renderer);
    ASSERT_TRUE(section.loadSectionFile(layoutB()));
    EXPECT_EQ(StarAnchoring::step(again, epub, &section, spine), StarAnchoring::Work::None) << "spine " << spine;
  }
  RecordProperty("moved", moved);
}

// The drift the anchor is for: on the corpus's longer chapters a page number names other text in layout B.
TEST(StarredPageRelayoutDrift, APageNumberAloneNamesOtherTextAfterARelayout) {
  const auto work = fs::temp_directory_path() / ("starred_page_drift_" + std::to_string(::getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub", work.string());
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  int drifted = 0;
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    Section a(epub, spine, renderer);
    ASSERT_TRUE(a.createSectionFile(layoutA(), {}, true));
    ASSERT_TRUE(a.loadSectionFile(layoutA()));
    Section b(epub, spine, renderer);
    ASSERT_TRUE(b.createSectionFile(layoutB(), {}, true));
    ASSERT_TRUE(b.loadSectionFile(layoutB()));
    for (int page = 0; page < a.pageCount && page < b.pageCount; ++page) {
      const auto start = a.getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
      if (start && !pageHolds(b, static_cast<uint16_t>(page), *start)) drifted++;
    }
  }
  EXPECT_GT(drifted, 0) << "the two layouts must put different text on a page number for the tests above to mean "
                           "anything";
  fs::remove_all(work);
}

TEST_P(StarredPageRelayout, AStarWithoutAnAnchorKeepsItsPage) {
  // As older firmware and the TXT reader make them.
  BookmarkStore store;
  store.load(epub->getCachePath());
  ASSERT_TRUE(store.toggle(0, 0));
  store.save();
  store.load(epub->getCachePath());
  const auto section = build(0, layoutB());
  EXPECT_EQ(StarAnchoring::step(store, epub, section.get(), 0), StarAnchoring::Work::None);
  EXPECT_EQ(store.getAll()[0].pageNumber, 0);
  EXPECT_EQ(store.getAnchors()[0].layout, 0u);
}

// What the first relayout resolved, the session keeps: a second one moves the stars without a parse.
TEST_P(StarredPageRelayout, ASecondRelayoutInOneSessionParsesNothing) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  BookmarkStore store;
  store.load(epub->getCachePath());
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto b = build(spine, layoutB());
    settle(store, epub, *b, spine);
    const auto c = build(spine, layoutC());
    for (StarAnchoring::Work work;
         (work = StarAnchoring::step(store, epub, c.get(), spine)) != StarAnchoring::Work::None;) {
      ASSERT_EQ(work, StarAnchoring::Work::Cheap) << "spine " << spine;
    }
    for (size_t i = 0; i < starred.size(); ++i) {
      if (starred[i].spine != spine || !starred[i].named) continue;
      EXPECT_TRUE(pageHolds(*c, store.getAll()[i].pageNumber, starred[i].offset))
          << "spine " << spine << " page " << starred[i].page;
    }
  }
}

// A jump to a star of a chapter not laid out yet goes by its text: the XPath resolved once, then known.
TEST_P(StarredPageRelayout, AJumpFindsTheTextFromTheXPath) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  BookmarkStore store;
  store.load(epub->getCachePath());
  for (size_t i = 0; i < starred.size(); ++i) {
    const auto offset = StarAnchoring::textOffset(store, i, epub);
    if (!starred[i].named) {
      EXPECT_FALSE(offset.has_value());
      continue;
    }
    ASSERT_TRUE(offset.has_value()) << "spine " << starred[i].spine << " page " << starred[i].page;
    EXPECT_EQ(store.getAnchors()[i].offset, *offset);
  }
  // What the jump lands on, in layout B, is the page holding that text.
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutB());
    for (size_t i = 0; i < starred.size(); ++i) {
      if (starred[i].spine != spine || !starred[i].named) continue;
      const auto page = section->getPageForVisibleTextOffset(store.getAnchors()[i].offset);
      ASSERT_TRUE(page.has_value());
      EXPECT_TRUE(pageHolds(*section, *page, starred[i].offset)) << "spine " << spine << " page " << starred[i].page;
    }
  }
}

// Stars made under other layouts can come to rest on one page: unstarring it takes them all.
TEST(StarredPageStore, UnstarringAPageTakesEveryStarOnIt) {
  BookmarkStore store;
  StarAnchor a;
  a.layout = 1;
  StarAnchor b;
  b.layout = 2;
  ASSERT_TRUE(store.toggle(0, 3, a));
  ASSERT_TRUE(store.toggle(0, 4, b));
  store.movePage(1, 3, 1, 10);  // the second one settled onto the first one's page
  EXPECT_FALSE(store.toggle(0, 3, StarAnchor{}));
  EXPECT_TRUE(store.isEmpty());
  EXPECT_TRUE(store.getAnchors().empty());
}

INSTANTIATE_TEST_SUITE_P(Corpus, StarredPageRelayout, testing::ValuesIn(corpusBooks()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           std::string name = fs::path(info.param).stem().string();
                           for (char& c : name) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return name;
                         });
