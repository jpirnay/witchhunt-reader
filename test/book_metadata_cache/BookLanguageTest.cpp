// BookMetadataCache::readLanguage: the book's language from book.bin without a load(). The reader asks it before the
// book opens, so a Noto CJK reader font is installed once as the sibling the book's language asks for
// (SdCardFontSystem::effectiveReaderFamily) instead of once as the configured face and again as the sibling.

#include <HalStorage.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "BigSpineBook.h"
#include "BookMetadataCache.h"

namespace fs = std::filesystem;

namespace {

class BookLanguageTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = (fs::temp_directory_path() / ("book_language_" + std::to_string(getpid()))).string();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    cacheDir_ = dir_ + "/cache";
    fs::create_directories(cacheDir_);
    book_ = big_spine_book::write(dir_ + "/book.epub", 3, 0);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // book.bin as Epub::load builds it, with `metadata`.
  void build(const BookMetadataCache::BookMetadata& metadata) {
    BookMetadataCache cache(cacheDir_);
    ASSERT_TRUE(cache.beginWrite());
    ASSERT_TRUE(cache.beginContentOpfPass());
    for (const std::string& href : book_.spineHrefs) cache.createSpineEntry(href);
    ASSERT_TRUE(cache.endContentOpfPass());
    ASSERT_TRUE(cache.beginTocPass());
    ASSERT_TRUE(cache.endTocPass());
    ASSERT_TRUE(cache.endWrite());
    ASSERT_TRUE(cache.buildBookBin(book_.path, metadata));
  }

  std::string dir_;
  std::string cacheDir_;
  big_spine_book::Book book_;
};

TEST_F(BookLanguageTest, ReadsTheLanguageLoadReads) {
  BookMetadataCache::BookMetadata metadata;
  metadata.title = "北京故事";
  metadata.author = std::string(3000, 'a');  // a long string before the language must be skipped whole
  metadata.language = "zh-Hant";
  metadata.description = "after the language";
  build(metadata);

  std::string language;
  ASSERT_TRUE(BookMetadataCache::readLanguage(cacheDir_, language));
  BookMetadataCache loaded(cacheDir_);
  ASSERT_TRUE(loaded.load());
  EXPECT_EQ(loaded.coreMetadata.language, "zh-Hant");
  EXPECT_EQ(language, loaded.coreMetadata.language);
}

TEST_F(BookLanguageTest, ABookWithoutALanguageReadsEmpty) {
  BookMetadataCache::BookMetadata metadata;
  metadata.title = "Untagged";
  build(metadata);

  std::string language = "stale";
  ASSERT_TRUE(BookMetadataCache::readLanguage(cacheDir_, language));
  EXPECT_EQ(language, "");
}

TEST_F(BookLanguageTest, NoBookBinIsNoLanguage) {
  std::string language;
  EXPECT_FALSE(BookMetadataCache::readLanguage(cacheDir_, language));
}

TEST_F(BookLanguageTest, ABookBinOfAnotherVersionIsNoLanguage) {
  // load() rebuilds such a book.bin, so the language it would read is not the book's yet.
  BookMetadataCache::BookMetadata metadata;
  metadata.language = "ja";
  build(metadata);
  {
    std::fstream f(cacheDir_ + "/book.bin", std::ios::in | std::ios::out | std::ios::binary);
    f.put(static_cast<char>(0xFF));
  }
  std::string language;
  EXPECT_FALSE(BookMetadataCache::readLanguage(cacheDir_, language));
}

}  // namespace
