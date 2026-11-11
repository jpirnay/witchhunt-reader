// Beside a section build the image lane may wait for the lane after the build (image_scratch::canWait), and there the
// first image refused for memory ends the page's warm (Page::warmImageCaches). On the X3 (2026-11-11) a page of icons
// took 26 refused decodes in 3.5 s, each image twice, before it was left for the lane after the build all the same.
// Any other pass still tries every image.
#include <BuildArena.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <functional>
#include <string>

#include "Epub/Page.h"
#include "Epub/blocks/ImageBlock.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

const char* kInlineBook = CORPUS_DIR "/test_inline_images.epub";
// Chapter 5: an image in a table cell, then a block image.
const char* kTableBook = CORPUS_DIR "/test_extensionless_images.epub";

// The X3's build tail, which serves the extraction ring (Epub::EXTRACT_RING_BYTES) as the lane's does.
constexpr size_t kBuildTail = 35 * 1024;
// Under any image decode's gate: every decode is refused for memory.
constexpr uint32_t kTooLittleHeap = 8 * 1024;

int imagesOn(const Page& page) {
  int images = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() == TAG_PageImage) ++images;
  }
  return images;
}

struct WarmRefusalStopFixture : testing::Test {
  fs::path work;
  uint32_t savedHeap = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / (std::string("warm_refusal_stop_") + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    savedHeap = ESP.getFreeHeap();
  }
  void TearDown() override {
    ESP.setFreeHeap(savedHeap);
    fs::remove_all(work);
  }

  // The image lane's warm of `page` with every decode refused: the refusals it counted (image_scratch::heapShortCount).
  static uint32_t refusalsWarming(const Page& page, GfxRenderer& renderer, const bool canWait) {
    BuildArena tail(kBuildTail);
    const image_scratch::ScopedArena scratch(&tail);
    const image_scratch::ScopedCanWait wait(canWait);
    ESP.setFreeHeap(kTooLittleHeap);
    const uint32_t before = image_scratch::heapShortCount();
    page.warmImageCaches(renderer, 0, 0, /*forceLoadLargeImages=*/true, /*monochromeOutput=*/true,
                         /*alsoWarmGrayscale=*/true);
    ESP.setFreeHeap(200 * 1024);
    return image_scratch::heapShortCount() - before;
  }

  // Runs `check` on the first page of `book` whose images (block or inline) number at least `minImages`, or whose
  // first table holds one when `table` is set.
  bool onFirstPage(const char* book, const bool table, const int minImages,
                   const std::function<void(const Page&, GfxRenderer&)>& check) const {
    bool found = false;
    const bool built = pipeline_harness::forEachPage(
        book, (work / "cache").string(), pipeline_harness::Profile{},
        [&](int, const Section&, const Page& page, GfxRenderer& renderer) {
          if (found) return;
          bool hasTable = false;
          for (const auto& element : page.elements) {
            if (element->getTag() == TAG_PageTable && static_cast<const PageTableFragment&>(*element).hasImages()) {
              hasTable = true;
            }
          }
          if (table ? !hasTable : imagesOn(page) < minImages) return;
          found = true;
          check(page, renderer);
        });
    return built && found;
  }
};

TEST_F(WarmRefusalStopFixture, BesideABuildTheFirstRefusalEndsThePagesWarm) {
  ASSERT_TRUE(onFirstPage(kInlineBook, /*table=*/false, 3, [](const Page& page, GfxRenderer& renderer) {
    EXPECT_EQ(1u, refusalsWarming(page, renderer, /*canWait=*/true)) << "one refused decode, no image after it tried";
    EXPECT_TRUE(page.hasUncachedImages(true, true, true)) << "left for the lane after the build";

    // That lane, with the memory back: every image decodes.
    BuildArena tail(kBuildTail);
    const image_scratch::ScopedArena scratch(&tail);
    const image_scratch::ScopedCanWait wait(true);
    page.warmImageCaches(renderer, 0, 0, true, true, true);
    EXPECT_FALSE(page.hasUncachedImages(true, true, true));
  }));
}

TEST_F(WarmRefusalStopFixture, AnyOtherPassTriesEveryImage) {
  ASSERT_TRUE(onFirstPage(kInlineBook, /*table=*/false, 3, [](const Page& page, GfxRenderer& renderer) {
    const auto images = static_cast<uint32_t>(imagesOn(page));
    EXPECT_GE(refusalsWarming(page, renderer, /*canWait=*/false), images) << "each image refused for itself";
  }));
}

// An image in a table cell is the page's too: refused, it ends the warm before the block image after the table.
TEST_F(WarmRefusalStopFixture, ARefusalInATableCellEndsThePagesWarmToo) {
  ASSERT_TRUE(onFirstPage(kTableBook, /*table=*/true, 0, [](const Page& page, GfxRenderer& renderer) {
    ASSERT_GE(imagesOn(page), 1) << "the block image after the table";
    EXPECT_EQ(1u, refusalsWarming(page, renderer, /*canWait=*/true));
    EXPECT_GE(refusalsWarming(page, renderer, /*canWait=*/false), 2u) << "the cell's image and the block's";
  }));
}

}  // namespace
