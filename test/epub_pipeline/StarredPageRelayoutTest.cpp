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
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexer.h"
#include "SaxParser/SaxParser.h"
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
    EXPECT_EQ(StarAnchoring::parseOwed(store, nullptr, -1), !starred.empty()) << "every new star's text to name";
    while (StarAnchoring::step(store, epub, nullptr, -1) == StarAnchoring::Work::Parsed) {
    }
    EXPECT_FALSE(StarAnchoring::parseOwed(store, nullptr, -1));
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

  void reopenUnderAnotherLayout(const std::vector<Starred>& starred, bool offsetsKept);

  // The book's stars as a version 3 file has them: no offset, the XPath alone.
  void forgetOffsets() const {
    BookmarkStore store;
    store.load(epub->getCachePath());
    for (size_t i = 0; i < store.getAll().size(); ++i) store.setOffset(i, StarAnchor::kNoOffset, 0);
    store.save();
  }

  // The book's stars as another firmware's section format would leave them: offsets counted under a format this one
  // does not have, and wrong under this one (`wrongBy` off), so a test can tell a stale offset trusted from one
  // resolved again.
  void staleOffsets(const uint32_t wrongBy) const {
    BookmarkStore store;
    store.load(epub->getCachePath());
    for (size_t i = 0; i < store.getAll().size(); ++i) {
      const StarAnchor& anchor = store.getAnchors()[i];
      store.setOffset(i, anchor.offset + wrongBy, static_cast<uint8_t>(Section::formatVersion() + 1));
    }
    store.save();
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
// A bookmarks.bin as firmware before the anchors wrote it (version 2): spine, page and an empty name each.
void writeVersion2(const std::string& cacheDir, const std::vector<std::pair<uint16_t, uint16_t>>& bookmarks) {
  std::string out(1, '\x02');
  const auto put16 = [&out](const uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>(v >> 8));
  };
  put16(static_cast<uint16_t>(bookmarks.size()));
  for (const auto& [spine, page] : bookmarks) {
    put16(spine);
    put16(page);
    put16(0);
  }
  std::ofstream(cacheDir + "/bookmarks.bin", std::ios::binary)
      .write(out.data(), static_cast<std::streamsize>(out.size()));
}

bool pageHolds(const Section& section, const uint16_t page, const uint32_t offset) {
  const auto start = section.getVisibleTextOffsetForPage(page);
  if (!start || *start > offset) return false;
  return offset < section.getVisibleTextOffsetAfterPage(page).value_or(UINT32_MAX);
}

}  // namespace

// The book opened again under layout B with what the file keeps: every star on the page holding its text. With the
// offsets kept (`offsetsKept`), not one chapter is parsed for it; without them (a version 3 file, or an offset of
// another section format) each named star's XPath is resolved once, and a star with no name keeps its page.
void StarredPageRelayout::reopenUnderAnotherLayout(const std::vector<Starred>& starred, const bool offsetsKept) {
  BookmarkStore store;
  store.load(epub->getCachePath());
  ASSERT_EQ(store.getAll().size(), starred.size());
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutB());
    const bool named = std::any_of(starred.begin(), starred.end(),
                                   [spine](const Starred& star) { return star.spine == spine && star.named; });
    EXPECT_EQ(StarAnchoring::parseOwed(store, section.get(), spine), named && !offsetsKept) << "spine " << spine;
    for (int calls = 0;; ++calls) {
      ASSERT_LT(calls, 100000);
      const StarAnchoring::Work work = StarAnchoring::step(store, epub, section.get(), spine);
      if (work == StarAnchoring::Work::None) break;
      if (offsetsKept) EXPECT_EQ(work, StarAnchoring::Work::Cheap) << "spine " << spine << ": parsed";
    }
    EXPECT_FALSE(StarAnchoring::parseOwed(store, section.get(), spine)) << "spine " << spine;
    for (size_t i = 0; i < starred.size(); ++i) {
      if (starred[i].spine != spine) continue;
      const uint16_t page = store.getAll()[i].pageNumber;
      if (!starred[i].named && !offsetsKept) {
        EXPECT_EQ(page, starred[i].page) << "spine " << spine << ": nothing to find it by, so its page stands";
        continue;
      }
      EXPECT_TRUE(pageHolds(*section, page, starred[i].offset))
          << "spine " << spine << ": the star of page " << starred[i].page << " (text at " << starred[i].offset
          << ") is on page " << page << "/" << section->pageCount << " via " << store.getAnchors()[i].xpath;
      EXPECT_EQ(store.getAnchors()[i].layout, section->layoutFingerprint());
      EXPECT_EQ(store.getAnchors()[i].pageCount, section->pageCount);
      EXPECT_EQ(store.getAnchors()[i].offset, starred[i].offset);
      EXPECT_EQ(store.getAnchors()[i].offsetFormat, Section::formatVersion());
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
}

TEST_P(StarredPageRelayout, EveryStarredPageFindsItsTextInAnotherLayoutWithoutAParse) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  reopenUnderAnotherLayout(starred, /*offsetsKept=*/true);
}

// An offset counted under another section format is not trusted: the XPath finds the text and the offset is counted
// again, under this format. A star with no XPath keeps its page rather than follow such an offset.
TEST_P(StarredPageRelayout, AnOffsetOfAnotherSectionFormatGivesWayToTheXPath) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  staleOffsets(/*wrongBy=*/7);
  {
    BookmarkStore store;
    store.load(epub->getCachePath());
    for (const StarAnchor& anchor : store.getAnchors()) EXPECT_FALSE(StarAnchoring::knownOffset(anchor).has_value());
  }
  reopenUnderAnotherLayout(starred, /*offsetsKept=*/false);
}

TEST_P(StarredPageRelayout, WithoutItsOffsetAStarFindsItsTextByItsXPath) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  forgetOffsets();
  reopenUnderAnotherLayout(starred, /*offsetsKept=*/false);
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

// A bookmark from firmware before the anchors (bookmarks.bin version 2) is anchored the first time its chapter is
// shown complete: its page taken as counted in that layout, its text named as a new bookmark's is. From then on it
// follows its text like any other, and a second open parses nothing.
TEST_P(StarredPageRelayout, AnOlderBookmarkIsAnchoredOnFirstOpenAndThenFollowsItsText) {
  struct Older {
    uint16_t spine;
    uint16_t page;
    uint32_t offset;
    bool nameable;
  };
  std::vector<Older> older;
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutA());
    for (int page = 0; page < section->pageCount; ++page) {
      const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
      if (!offset) continue;
      const bool nameable = !ChapterXPathIndexer::findXPathForVisibleOffset(epub, spine, *offset).empty();
      older.push_back({static_cast<uint16_t>(spine), static_cast<uint16_t>(page), *offset, nameable});
    }
  }
  if (older.empty()) GTEST_SKIP() << "no page with text";
  std::vector<std::pair<uint16_t, uint16_t>> pages;
  for (const Older& bookmark : older) pages.emplace_back(bookmark.spine, bookmark.page);
  writeVersion2(epub->getCachePath(), pages);

  // The first open, in layout A: one naming parse a bookmark, nothing else.
  {
    BookmarkStore store;
    store.load(epub->getCachePath());
    ASSERT_EQ(store.getAll().size(), older.size());
    for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
      const auto section = build(spine, layoutA());
      const auto inSpine = static_cast<int>(std::count_if(
          older.begin(), older.end(), [spine](const Older& bookmark) { return bookmark.spine == spine; }));
      int parses = 0;
      for (StarAnchoring::Work work;
           (work = StarAnchoring::step(store, epub, section.get(), spine)) != StarAnchoring::Work::None;) {
        if (work == StarAnchoring::Work::Parsed) ++parses;
        ASSERT_LE(parses, inSpine) << "spine " << spine;
      }
      EXPECT_EQ(parses, inSpine) << "spine " << spine << ": one naming parse a bookmark";
      for (size_t i = 0; i < older.size(); ++i) {
        if (older[i].spine != spine) continue;
        const StarAnchor& anchor = store.getAnchors()[i];
        EXPECT_EQ(store.getAll()[i].pageNumber, older[i].page);
        EXPECT_EQ(anchor.layout, section->layoutFingerprint());
        EXPECT_EQ(StarAnchoring::knownOffset(anchor), older[i].offset);
        EXPECT_EQ(anchor.xpath.empty(), !older[i].nameable) << "spine " << spine << " page " << older[i].page;
      }
    }
    store.save();
  }
  // A second open in layout A: nothing to do.
  {
    BookmarkStore store;
    store.load(epub->getCachePath());
    for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
      Section section(epub, spine, renderer);
      ASSERT_TRUE(section.loadSectionFile(layoutA()));
      EXPECT_EQ(StarAnchoring::step(store, epub, &section, spine), StarAnchoring::Work::None) << "spine " << spine;
    }
  }
  // Layout B: every bookmark on its text, with no parse.
  BookmarkStore store;
  store.load(epub->getCachePath());
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutB());
    for (StarAnchoring::Work work;
         (work = StarAnchoring::step(store, epub, section.get(), spine)) != StarAnchoring::Work::None;) {
      ASSERT_EQ(work, StarAnchoring::Work::Cheap) << "spine " << spine << ": parsed";
    }
    for (size_t i = 0; i < older.size(); ++i) {
      if (older[i].spine != spine) continue;
      EXPECT_TRUE(pageHolds(*section, store.getAll()[i].pageNumber, older[i].offset))
          << "spine " << spine << ": the bookmark of page " << older[i].page << " is on page "
          << store.getAll()[i].pageNumber;
    }
  }
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

// A jump to a star of a chapter not laid out yet goes by its text: the offset the file keeps, without a parse; without
// one, the XPath resolved once, and known from then on.
TEST_P(StarredPageRelayout, AJumpFindsTheText) {
  const std::vector<Starred> starred = starEveryPage();
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  for (const bool offsetsKept : {true, false}) {
    if (!offsetsKept) forgetOffsets();
    BookmarkStore store;
    store.load(epub->getCachePath());
    for (size_t i = 0; i < starred.size(); ++i) {
      if (offsetsKept) EXPECT_EQ(store.getAnchors()[i].offset, starred[i].offset);
      const auto offset = StarAnchoring::textOffset(store, i, epub);
      if (!starred[i].named && !offsetsKept) {
        EXPECT_FALSE(offset.has_value());
        continue;
      }
      ASSERT_TRUE(offset.has_value()) << "spine " << starred[i].spine << " page " << starred[i].page;
      EXPECT_EQ(*offset, starred[i].offset);
    }
    // What the jump lands on, in layout B, is the page holding that text.
    for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
      const auto section = build(spine, layoutB());
      for (size_t i = 0; i < starred.size(); ++i) {
        if (starred[i].spine != spine || (!starred[i].named && !offsetsKept)) continue;
        const auto page = section->getPageForVisibleTextOffset(starred[i].offset);
        ASSERT_TRUE(page.has_value());
        EXPECT_TRUE(pageHolds(*section, *page, starred[i].offset)) << "spine " << spine << " page " << starred[i].page;
      }
    }
  }
}

// The reader lends the secondary framebuffer for these parses: the same stars, the same pages, and the region given
// back after each.
TEST_P(StarredPageRelayout, TheMappingsWorkInALentRegion) {
  std::vector<uint8_t> region(48000);  // an X4's 800 x 480 framebuffer
  BuildArena arena(region.data(), region.size());
  BookmarkStore store;
  store.load(epub->getCachePath());
  std::vector<Starred> starred;
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutA());
    for (int page = 0; page < section->pageCount; ++page) {
      const StarAnchor anchor = StarAnchoring::forPage(*section, page);
      if (anchor.offset == StarAnchor::kNoOffset) continue;
      ASSERT_TRUE(store.toggle(static_cast<uint16_t>(spine), static_cast<uint16_t>(page), anchor));
      starred.push_back({static_cast<uint16_t>(spine), static_cast<uint16_t>(page), anchor.offset, false});
    }
  }
  if (starred.empty()) GTEST_SKIP() << "no page with text";
  while (StarAnchoring::step(store, epub, nullptr, -1, &arena) == StarAnchoring::Work::Parsed) {
    ASSERT_EQ(arena.used(), 0u);
  }
  store.save();
  BookmarkStore reopened;
  reopened.load(epub->getCachePath());
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine, layoutB());
    while (StarAnchoring::step(reopened, epub, section.get(), spine, &arena) != StarAnchoring::Work::None) {
      ASSERT_EQ(arena.used(), 0u);
    }
    for (size_t i = 0; i < starred.size(); ++i) {
      if (starred[i].spine != spine || reopened.getAnchors()[i].xpath.empty()) continue;
      EXPECT_TRUE(pageHolds(*section, reopened.getAll()[i].pageNumber, starred[i].offset))
          << "spine " << spine << " page " << starred[i].page;
    }
  }
  EXPECT_GT(arena.highWater(), SaxParser::stateBytes(SaxParser::Profile::Lean));
}

// A page starred while its chapter was still being built: the build's file has no offset table yet, so the star has
// its layout and page only. Once the build is done, the table gives its offset, its text is named, and it counts.
TEST(StarredPageDuringABuild, GetsItsTextOnceTheBuildIsDone) {
  const auto work = fs::temp_directory_path() / ("starred_page_building_" + std::to_string(::getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub", work.string());
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  constexpr int kSpine = 1;
  Section section(epub, kSpine, renderer);
  ASSERT_TRUE(section.createSectionFile(layoutA(), {}, true));
  ASSERT_TRUE(section.loadSectionFile(layoutA()));
  ASSERT_GT(section.pageCount, 1);
  const auto page = static_cast<uint16_t>(section.pageCount - 1);

  StarAnchor building;  // what StarAnchoring::forPage() gives while the build runs
  building.layout = section.layoutFingerprint();
  building.nameOwed = true;
  BookmarkStore store;
  ASSERT_TRUE(store.toggle(kSpine, page, building));
  ASSERT_TRUE(StarAnchoring::parseOwed(store, &section, kSpine) == false) << "the offset comes from the table";
  while (StarAnchoring::step(store, epub, &section, kSpine) != StarAnchoring::Work::None) {
  }
  EXPECT_EQ(store.getAll()[0].pageNumber, page);
  EXPECT_EQ(store.getAnchors()[0].offset, section.getVisibleTextOffsetForPage(page));
  EXPECT_FALSE(store.getAnchors()[0].xpath.empty());
  EXPECT_EQ(store.getAnchors()[0].pageCount, section.pageCount);
  fs::remove_all(work);
}

namespace {
// One book of the corpus opened as the reader opens it, with its cache under a folder of this process's own.
struct OpenedBook {
  fs::path work;
  std::shared_ptr<Epub> epub;
  explicit OpenedBook(const std::string& name) {
    work = fs::temp_directory_path() / ("older_bookmark_" + std::to_string(::getpid()) + "_" + name);
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/" + name, work.string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
  }
  ~OpenedBook() {
    epub.reset();
    fs::remove_all(work);
  }
};
}  // namespace

// A cover has no text to name: the bookmark on it is tried once, on the first open, and goes by its offset from then
// on. No later open parses it again.
TEST(OlderBookmark, InAChapterWithNoTextIsTriedOnce) {
  const OpenedBook book("test_tables.epub");
  GfxRenderer renderer;
  constexpr int kCover = 0;
  Section section(book.epub, kCover, renderer);
  ASSERT_TRUE(section.createSectionFile(layoutA(), {}, true));
  ASSERT_TRUE(section.loadSectionFile(layoutA()));
  const auto offset = section.getVisibleTextOffsetForPage(0);
  ASSERT_TRUE(offset.has_value());
  ASSERT_TRUE(ChapterXPathIndexer::findXPathForVisibleOffset(book.epub, kCover, *offset).empty()) << "not a cover";
  writeVersion2(book.epub->getCachePath(), {{kCover, 0}});

  BookmarkStore store;
  store.load(book.epub->getCachePath());
  EXPECT_EQ(StarAnchoring::step(store, book.epub, &section, kCover), StarAnchoring::Work::Cheap);   // anchored
  EXPECT_EQ(StarAnchoring::step(store, book.epub, &section, kCover), StarAnchoring::Work::Parsed);  // naming tried
  EXPECT_EQ(StarAnchoring::step(store, book.epub, &section, kCover), StarAnchoring::Work::None);
  EXPECT_TRUE(store.getAnchors()[0].xpath.empty());
  EXPECT_NE(store.getAnchors()[0].layout, 0u);
  store.save();

  BookmarkStore again;
  again.load(book.epub->getCachePath());
  EXPECT_FALSE(StarAnchoring::parseOwed(again, &section, kCover));
  EXPECT_EQ(StarAnchoring::step(again, book.epub, &section, kCover), StarAnchoring::Work::None);
}

// A page past the chapter's end in the layout it is first shown in is not this layout's page: the bookmark is left as
// it was, and the first layout that has the page anchors it there.
TEST(OlderBookmark, PastTheChaptersEndWaitsForALayoutThatHasThePage) {
  const OpenedBook book("test_wrapped_paragraphs.epub");
  GfxRenderer renderer;
  constexpr int kSpine = 1;
  Section a(book.epub, kSpine, renderer);
  ASSERT_TRUE(a.createSectionFile(layoutA(), {}, true));
  ASSERT_TRUE(a.loadSectionFile(layoutA()));
  Section b(book.epub, kSpine, renderer);
  ASSERT_TRUE(b.createSectionFile(layoutB(), {}, true));
  ASSERT_TRUE(b.loadSectionFile(layoutB()));
  ASSERT_GT(b.pageCount, a.pageCount) << "layout B must lay the chapter out in more pages";
  const auto page = static_cast<uint16_t>(b.pageCount - 1);
  writeVersion2(book.epub->getCachePath(), {{kSpine, page}});

  BookmarkStore store;
  store.load(book.epub->getCachePath());
  EXPECT_EQ(StarAnchoring::step(store, book.epub, &a, kSpine), StarAnchoring::Work::None);
  EXPECT_EQ(store.getAll()[0].pageNumber, page);
  EXPECT_EQ(store.getAnchors()[0].layout, 0u);

  while (StarAnchoring::step(store, book.epub, &b, kSpine) != StarAnchoring::Work::None) {
  }
  EXPECT_EQ(store.getAll()[0].pageNumber, page);
  EXPECT_EQ(store.getAnchors()[0].layout, b.layoutFingerprint());
  EXPECT_EQ(StarAnchoring::knownOffset(store.getAnchors()[0]), b.getVisibleTextOffsetForPage(page));
  EXPECT_FALSE(store.getAnchors()[0].xpath.empty());
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
