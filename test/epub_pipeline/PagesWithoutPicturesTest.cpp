// Section's memory of pages without a picture (Section::notePageWithoutPictures): the reader's image lane skips loading
// them again at every turn, so a page it remembers must be one of the section's present pages, never one from before
// the pages were dropped or laid out again.
#include <Arduino.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";
constexpr int kSpine = 1;

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  return p;
}

class PagesWithoutPictures : public ::testing::Test {
 protected:
  void SetUp() override {
    cacheRoot_ = fs::temp_directory_path() / ("pages_without_pictures_" + std::to_string(getpid()));
    fs::remove_all(cacheRoot_);
    fs::create_directories(cacheRoot_);
    epub_ = std::make_shared<Epub>(kFixture, cacheRoot_.string());
    ASSERT_TRUE(epub_->load(true));
    epub_->setupCacheDir();
    section_ = std::make_unique<Section>(epub_, kSpine, renderer_);
    ASSERT_TRUE(section_->createSectionFile(params(), {}, /*skipEviction=*/true));
    ASSERT_TRUE(section_->loadSectionFile(params()));
  }
  void TearDown() override {
    section_.reset();
    epub_.reset();
    fs::remove_all(cacheRoot_);
  }

  fs::path cacheRoot_;
  GfxRenderer renderer_;
  std::shared_ptr<Epub> epub_;
  std::unique_ptr<Section> section_;
};

}  // namespace

TEST_F(PagesWithoutPictures, ANotedPageIsKnownAndAnotherIsNot) {
  EXPECT_FALSE(section_->pageKnownWithoutPictures(0));
  section_->notePageWithoutPictures(0);
  section_->notePageWithoutPictures(3);
  EXPECT_TRUE(section_->pageKnownWithoutPictures(0));
  EXPECT_TRUE(section_->pageKnownWithoutPictures(3));
  EXPECT_FALSE(section_->pageKnownWithoutPictures(1));
  EXPECT_FALSE(section_->pageKnownWithoutPictures(-1));
}

TEST_F(PagesWithoutPictures, TheOldestGoWhenMoreAreNoted) {
  for (int page = 0; page < 21; page++) section_->notePageWithoutPictures(page);
  for (int page = 0; page < 5; page++) EXPECT_FALSE(section_->pageKnownWithoutPictures(page)) << page;
  for (int page = 5; page < 21; page++) EXPECT_TRUE(section_->pageKnownWithoutPictures(page)) << page;
}

TEST_F(PagesWithoutPictures, NotingAPageTwiceKeepsOneSlot) {
  for (int i = 0; i < 20; i++) section_->notePageWithoutPictures(7);
  for (int page = 100; page < 115; page++) section_->notePageWithoutPictures(page);
  EXPECT_TRUE(section_->pageKnownWithoutPictures(7));  // 1 + 15 others fit in 16
}

TEST_F(PagesWithoutPictures, ForgottenWithThePages) {
  section_->notePageWithoutPictures(2);
  ASSERT_TRUE(section_->loadSectionFile(params()));
  EXPECT_FALSE(section_->pageKnownWithoutPictures(2)) << "a reload";

  section_->notePageWithoutPictures(2);
  ASSERT_TRUE(section_->clearCache());
  EXPECT_FALSE(section_->pageKnownWithoutPictures(2)) << "cache cleared";

  section_->notePageWithoutPictures(2);
  ASSERT_TRUE(section_->createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_FALSE(section_->pageKnownWithoutPictures(2)) << "laid out again";
}
