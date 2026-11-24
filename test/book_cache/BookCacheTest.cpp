// BookCache::clear(): what a clear of a book's cache folder removes and what it leaves. Runs over a
// real folder through the stdio storage shim.
#include <BookCache.h>
#include <CacheCipherFile.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class BookCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = (fs::path(BOOK_CACHE_ROOT) / info->name() / "epub_12345").generic_string();
    fs::remove_all(fs::path(dir_).parent_path());
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(fs::path(dir_).parent_path()); }

  void file(const std::string& rel, const std::string& content = "x") const {
    const fs::path path = fs::path(dir_) / rel;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
  }
  bool has(const std::string& rel) const { return fs::exists(fs::path(dir_) / rel); }
  std::string read(const std::string& rel) const {
    std::ifstream in(fs::path(dir_) / rel, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  // What a book's cache folder holds after it was read for a while.
  void fillLikeAReadBook() const {
    file("book.bin");
    file("css_rules.cache");
    file("fingerprint.bin");
    file("pages.bin");
    file("spines/0/3_1a2b3c4d.bin");
    file("spines/0/html_3.bin");
    file("img/0.1bit.pxc");
    file("thumb_400.bmp");
    file("cover.img");
    file("kosync_push.bin");
    file("koreader_docid.txt");
    file("progress.bin", "position");
    file("linkstack.bin", "back stack");
    file("bookmarks.bin", "marks");
    file("koreader_syncmethod.txt", "1");
    file("overrides.bin", "settings");
  }

  std::string dir_;
};

TEST_F(BookCacheTest, KeepNothingRemovesTheFolder) {
  fillLikeAReadBook();
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::Nothing));
  EXPECT_FALSE(fs::exists(dir_));
}

TEST_F(BookCacheTest, KeepReadingStateLeavesOnlyTheReadersFiles) {
  fillLikeAReadBook();
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::ReadingState));

  EXPECT_EQ("position", read("progress.bin"));
  EXPECT_EQ("back stack", read("linkstack.bin"));
  EXPECT_EQ("marks", read("bookmarks.bin"));
  EXPECT_EQ("1", read("koreader_syncmethod.txt"));
  EXPECT_EQ("settings", read("overrides.bin"));
  for (const char* derived : {"book.bin", "css_rules.cache", "fingerprint.bin", "pages.bin", "spines", "img",
                              "thumb_400.bmp", "cover.img", "kosync_push.bin", "koreader_docid.txt"}) {
    EXPECT_FALSE(has(derived)) << derived << " is derived from the book and must go";
  }
}

TEST_F(BookCacheTest, KeepCoversLeavesTheImagesAndNothingElse) {
  fillLikeAReadBook();
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::Nothing, /*keepCovers=*/true));

  EXPECT_TRUE(has("thumb_400.bmp"));
  EXPECT_TRUE(has("cover.img"));
  EXPECT_FALSE(has("progress.bin"));
  EXPECT_FALSE(has("linkstack.bin"));
  EXPECT_FALSE(has("bookmarks.bin"));
  EXPECT_FALSE(has("koreader_syncmethod.txt"));
  EXPECT_FALSE(has("overrides.bin"));
  EXPECT_FALSE(has("spines"));
}

// A protected book's folder is enciphered under its marker's salt (lib/CacheCipher/CacheCipherFile.h). Without the
// marker a kept progress.bin reads as noise, and the book's next open enciphers that noise again under a new salt.
TEST_F(BookCacheTest, TheCipherMarkerStaysWithTheReadingState) {
  fillLikeAReadBook();
  file(cache_cipher::kMarkerName, "WCF1 and its salt");
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::ReadingState));

  EXPECT_EQ("WCF1 and its salt", read(cache_cipher::kMarkerName));
  EXPECT_EQ("position", read("progress.bin"));
  EXPECT_EQ("marks", read("bookmarks.bin"));
  EXPECT_FALSE(has("book.bin"));
  EXPECT_FALSE(has("spines"));
}

// The reader's Delete Book Cache leaves the covers either way, enciphered as the rest of the folder was.
TEST_F(BookCacheTest, TheCipherMarkerStaysWithTheCovers) {
  fillLikeAReadBook();
  file(cache_cipher::kMarkerName, "WCF1 and its salt");
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::Nothing, /*keepCovers=*/true));

  EXPECT_TRUE(has(cache_cipher::kMarkerName));
  EXPECT_TRUE(has("thumb_400.bmp"));
  EXPECT_FALSE(has("progress.bin"));
}

// The marker is kept for the files it reads, not for itself: with nothing else kept the folder goes, marker and all.
TEST_F(BookCacheTest, AMarkerWithNothingElseToKeepGoesWithTheFolder) {
  file("book.bin");
  file("spines/0/0_00000000.bin");
  file(cache_cipher::kMarkerName, "WCF1 and its salt");
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::ReadingState));
  EXPECT_FALSE(fs::exists(dir_));
}

// A book never opened, or opened on a firmware that kept no reading state, leaves no empty folder.
TEST_F(BookCacheTest, AFolderWithNothingToKeepGoes) {
  file("book.bin");
  file("spines/0/0_00000000.bin");
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::ReadingState));
  EXPECT_FALSE(fs::exists(dir_));
}

TEST_F(BookCacheTest, AMissingFolderIsClearAlready) {
  fs::remove_all(dir_);
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::ReadingState));
  EXPECT_TRUE(BookCache::clear(dir_, BookCache::Keep::Nothing));
}

}  // namespace
