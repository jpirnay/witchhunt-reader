// details.bin: the Details view's record of a never-opened EPUB's metadata. It must come back
// exactly as written, and must refuse to answer for a different book or an edited sidecar.

#include <BookDetails.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class BookDetailsCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / ("book_details_test_" + std::string(info->name()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = (dir_ / "details.bin").string();
  }
  void TearDown() override { fs::remove_all(dir_); }

  static BookDetails sample() {
    BookDetails d;
    d.title = "Der Zauberberg";
    d.author = "Thomas Mann";
    d.primaryAuthor = "Thomas Mann";
    d.authorSort = "Mann, Thomas";
    d.series = "Werke";
    d.seriesIndex = "3";
    return d;
  }

  fs::path dir_;
  std::string path_;
};

TEST_F(BookDetailsCacheTest, RoundTrips) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 12345, 77, sample()));
  BookDetails out;
  ASSERT_TRUE(BookDetailsCache::read(path_, 12345, 77, out));
  EXPECT_EQ(out.title, "Der Zauberberg");
  EXPECT_EQ(out.author, "Thomas Mann");
  EXPECT_EQ(out.primaryAuthor, "Thomas Mann");
  EXPECT_EQ(out.authorSort, "Mann, Thomas");
  EXPECT_EQ(out.series, "Werke");
  EXPECT_EQ(out.seriesIndex, "3");
}

TEST_F(BookDetailsCacheTest, EmptyRecordOfAFailedParseIsStillAnAnswer) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 10, 0, BookDetails{}));
  BookDetails out = sample();
  ASSERT_TRUE(BookDetailsCache::read(path_, 10, 0, out));
  EXPECT_TRUE(out.title.empty());
  EXPECT_TRUE(out.author.empty());
}

TEST_F(BookDetailsCacheTest, ADifferentBookSizeIsNotAnswered) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 12345, 0, sample()));
  BookDetails out;
  EXPECT_FALSE(BookDetailsCache::read(path_, 54321, 0, out));
  EXPECT_TRUE(out.title.empty());
}

TEST_F(BookDetailsCacheTest, AnUnknownBookSizeSkipsTheSizeCheck) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 12345, 0, sample()));
  BookDetails out;
  EXPECT_TRUE(BookDetailsCache::read(path_, 0, 0, out));
}

TEST_F(BookDetailsCacheTest, AnEditedSidecarIsNotAnswered) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 12345, 77, sample()));
  BookDetails out;
  EXPECT_FALSE(BookDetailsCache::read(path_, 12345, 78, out));
  EXPECT_FALSE(BookDetailsCache::read(path_, 12345, 0, out));  // sidecar since removed
}

TEST_F(BookDetailsCacheTest, MissingOrTruncatedFileIsNotAnswered) {
  BookDetails out;
  EXPECT_FALSE(BookDetailsCache::read(path_, 1, 0, out));

  ASSERT_TRUE(BookDetailsCache::write(path_, 1, 0, sample()));
  fs::resize_file(path_, fs::file_size(path_) - 3);
  EXPECT_FALSE(BookDetailsCache::read(path_, 1, 0, out));
}

TEST_F(BookDetailsCacheTest, AnotherFormatVersionIsNotAnswered) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 1, 0, sample()));
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.put(static_cast<char>(99));
  }
  BookDetails out;
  EXPECT_FALSE(BookDetailsCache::read(path_, 1, 0, out));
}

// A version 1 record has no primary author. Answering from it would group the book under "Unknown
// author" for good, so it is parsed again instead.
TEST_F(BookDetailsCacheTest, AVersionOneRecordIsNotAnswered) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 1, 0, sample()));
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.put(static_cast<char>(1));
  }
  BookDetails out;
  EXPECT_FALSE(BookDetailsCache::read(path_, 1, 0, out));
}

}  // namespace
