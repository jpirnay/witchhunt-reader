// Epub's book.bin memos (getBookSize, getCumulativeSpineItemSize, getTocIndexForSpineIndex, getTocItem): asked in
// any order, they answer what book.bin says, as the uncached reads do.
#include <Arduino.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_text_rendering.epub";

class EpubMetadataMemo : public ::testing::Test {
 protected:
  void SetUp() override {
    cacheRoot_ = fs::temp_directory_path() / ("epub_metadata_memo_" + std::to_string(getpid()));
    fs::remove_all(cacheRoot_);
    fs::create_directories(cacheRoot_);
    epub_ = std::make_unique<Epub>(kFixture, cacheRoot_.string());
    ASSERT_TRUE(epub_->load(true));
    ASSERT_GE(epub_->getSpineItemsCount(), 4);
    ASSERT_GE(epub_->getTocItemsCount(), 3);
  }
  void TearDown() override {
    epub_.reset();
    fs::remove_all(cacheRoot_);
  }

  fs::path cacheRoot_;
  std::unique_ptr<Epub> epub_;
};

}  // namespace

TEST_F(EpubMetadataMemo, CumulativeSizesMatchTheSpineInAnyOrder) {
  const int count = epub_->getSpineItemsCount();
  const int order[] = {2, 1, 2, 1, 0, count - 1, 3, 2, 3, 0, 0, count - 1, 1};
  for (const int spine : order) {
    EXPECT_EQ(epub_->getCumulativeSpineItemSize(spine), epub_->getSpineItem(spine).cumulativeSize) << spine;
  }
}

TEST_F(EpubMetadataMemo, BookSizeIsTheLastCumulativeSize) {
  const int last = epub_->getSpineItemsCount() - 1;
  EXPECT_EQ(epub_->getBookSize(), epub_->getSpineItem(last).cumulativeSize);
  EXPECT_EQ(epub_->getBookSize(), epub_->getSpineItem(last).cumulativeSize);  // and again, from the memo
}

TEST_F(EpubMetadataMemo, ProgressIsTheSameAskedTwice) {
  const float first = epub_->calculateProgress(2, 0.5f);
  EXPECT_GT(first, 0.0f);
  EXPECT_LT(first, 1.0f);
  EXPECT_FLOAT_EQ(epub_->calculateProgress(2, 0.5f), first);
  EXPECT_LT(epub_->calculateProgress(1, 0.5f), first);
}

TEST_F(EpubMetadataMemo, TocEntriesAndIndexesMatchInAnyOrder) {
  const int tocCount = epub_->getTocItemsCount();
  for (const int toc : {1, 1, 0, 2, 1, tocCount - 1, 0}) {
    const auto memo = epub_->getTocItem(toc);
    const auto again = epub_->getTocItem(toc);
    EXPECT_EQ(memo.title, again.title) << toc;
    EXPECT_EQ(memo.href, again.href) << toc;
    EXPECT_EQ(memo.spineIndex, again.spineIndex) << toc;
  }
  EXPECT_NE(epub_->getTocItem(0).href, epub_->getTocItem(1).href);
  for (const int spine : {2, 2, 0, 3, 2}) {
    EXPECT_EQ(epub_->getTocIndexForSpineIndex(spine), epub_->getSpineItem(spine).tocIndex) << spine;
  }
}
