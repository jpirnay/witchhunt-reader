// A clear that keeps the reading state (BookCache::Keep::ReadingState: the Keep answer to Delete
// Book Cache and to Clear Reading Cache) leaves progress.bin and bookmarks.bin in a folder whose
// layout is gone. Both name pages of that layout. The reader reopens on the saved page when the
// rebuilt chapter has the page count it saved, and on the saved paragraph when it has not
// (NavigationTarget::relaidOutRestorePage); a bookmark is a page number and nothing else.
//
// So a reopen after the clear lands where the book was left exactly when the rebuild lays the
// chapter out as it was before. Nothing the clear removes feeds into that layout; this pins it on a
// real book run through the whole pipeline: index, build, read, clear, index again, build again.
#include <Arduino.h>
#include <BookCache.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
// BookmarkStore::toggle() leaves a bookmark's name to its default, which -Wextra reports.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "src/BookmarkStore.h"
#pragma GCC diagnostic pop
#include "src/activities/reader/EpubProgressRecord.h"

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

// One open of the book, as the reader does it: index, then lay out the chapter.
struct OpenBook {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
};

OpenBook open(const std::string& cacheRoot) {
  OpenBook book;
  book.epub = std::make_shared<Epub>(kFixture, cacheRoot);
  EXPECT_TRUE(book.epub->load(true));
  book.epub->setupCacheDir();
  GfxRenderer renderer;
  book.section = std::make_unique<Section>(book.epub, kSpine, renderer);
  EXPECT_TRUE(book.section->createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_TRUE(book.section->loadSectionFile(params()));
  return book;
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(KeptReadingState, AReopenAfterAKeptClearLandsOnTheSavedPageAndBookmark) {
  const auto cacheRoot = fs::temp_directory_path() / "kept_reading_state_test";
  fs::remove_all(cacheRoot);
  fs::create_directories(cacheRoot);

  // Read to the middle of the chapter, star its last page, and close: the progress record the
  // reader's exit writes, paragraph included, and its bookmarks.
  std::string cacheDir;
  EpubProgressRecord saved;
  uint16_t starredPage = 0;
  std::optional<uint16_t> starredParagraph;
  std::optional<uint16_t> savedParagraphPage;
  {
    const auto book = open(cacheRoot.string());
    cacheDir = book.epub->getCachePath();
    ASSERT_GT(book.section->pageCount, 2) << "fixture must span several pages to be meaningful";

    saved.spineIndex = kSpine;
    saved.page = book.section->pageCount / 2;
    saved.pageCount = book.section->pageCount;
    saved.percent = 42;
    saved.paragraph = book.section->getParagraphIndexForPage(static_cast<uint16_t>(saved.page));
    ASSERT_TRUE(saved.paragraph.has_value()) << "the saved page must carry a paragraph anchor";
    savedParagraphPage = book.section->getPageForParagraphIndex(*saved.paragraph);
    uint8_t record[EpubProgressRecord::kMaxSize] = {};
    std::ofstream(cacheDir + "/progress.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(record), static_cast<std::streamsize>(saved.encode(record)));

    starredPage = static_cast<uint16_t>(book.section->pageCount - 1);
    starredParagraph = book.section->getParagraphIndexForPage(starredPage);
    ASSERT_TRUE(starredParagraph.has_value()) << "the starred page must carry a paragraph anchor";
    BookmarkStore bookmarks;
    bookmarks.load(cacheDir);
    ASSERT_TRUE(bookmarks.toggle(kSpine, starredPage));
    bookmarks.save();
  }
  const std::string progressBytes = readFile(cacheDir + "/progress.bin");
  const std::string bookmarkBytes = readFile(cacheDir + "/bookmarks.bin");

  ASSERT_TRUE(BookCache::clear(cacheDir, BookCache::Keep::ReadingState));
  ASSERT_FALSE(fs::exists(cacheDir + "/book.bin"));
  ASSERT_FALSE(fs::exists(cacheDir + "/spines"));

  const auto book = open(cacheRoot.string());
  // Indexing and laying the book out again leave both files as they were.
  EXPECT_EQ(progressBytes, readFile(cacheDir + "/progress.bin"));
  EXPECT_EQ(bookmarkBytes, readFile(cacheDir + "/bookmarks.bin"));

  const auto record =
      EpubProgressRecord::decode(reinterpret_cast<const uint8_t*>(progressBytes.data()), progressBytes.size());
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(saved.spineIndex, record->spineIndex);
  EXPECT_EQ(saved.paragraph, record->paragraph);
  // The page count it saved: the reader resumes on the saved page as it is, no rescale, no remap.
  EXPECT_EQ(record->pageCount, book.section->pageCount);
  EXPECT_LT(record->page, book.section->pageCount);
  // And its paragraph, which a reopen under a changed layout would go by, names the same place.
  EXPECT_EQ(savedParagraphPage, book.section->getPageForParagraphIndex(*record->paragraph));

  BookmarkStore bookmarks;
  bookmarks.load(cacheDir);
  ASSERT_TRUE(bookmarks.has(kSpine, starredPage));
  // The bookmarked page shows what it showed when it was starred.
  EXPECT_EQ(starredParagraph, book.section->getParagraphIndexForPage(starredPage));

  fs::remove_all(cacheRoot);
}
