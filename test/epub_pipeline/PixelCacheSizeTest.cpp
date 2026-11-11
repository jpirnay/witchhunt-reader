// An image's pixel caches are keyed by the size it is drawn at (ImageBlock::bwCachePath, grayscaleCachePath). Keyed on
// the image alone, one image a book draws at two sizes found the other size's cache on every draw: renderFromCache
// refused it ("Cache width mismatch"), the image was decoded again, and the cache it wrote was the next draw's
// mismatch. The inline-images fixture's star does that on the X3 (2026-11-11): 18 px at its own size, 31 px where its
// CSS sets 1em. The host's em is its 18 px ascender, the star's own size, so the fixture's run here reaches the same
// fault through its tall image, drawn at 30 by 44 in a line and at a column's em down chapter 4 under vertical text.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/blocks/ImageBlock.h"
#include "GfxRenderer.h"
#include "LogObserver.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_inline_images.epub";
const char* kStar = "OEBPS/images/star.png";  // 18 by 18

std::vector<std::string> gMismatches;
void noteMismatch(const char* /*level*/, const char* /*origin*/, const char* message) {
  if (std::strstr(message, "Cache width mismatch") != nullptr) gMismatches.emplace_back(message);
}

// The width a pixel cache says it holds: its header is magic, width, height.
int cachedWidth(const std::string& cache) {
  std::ifstream in(cache, std::ios::binary);
  uint16_t header[2] = {0, 0};
  in.read(reinterpret_cast<char*>(header), sizeof(header));
  return in ? header[1] : -1;
}

// Every pixel cache under `dir`, with when it was last written.
using Written = std::map<std::string, fs::file_time_type>;
Written pixelCaches(const fs::path& dir) {
  Written caches;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pxc") {
      caches[entry.path().filename().string()] = entry.last_write_time();
    }
  }
  return caches;
}

// The caches of `after` that are new or written again since `before`.
std::vector<std::string> writtenSince(const Written& before, const Written& after) {
  std::vector<std::string> names;
  for (const auto& [name, time] : after) {
    const auto was = before.find(name);
    if (was == before.end() || was->second != time) names.push_back(name);
  }
  return names;
}

struct PixelCacheSizeFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() / (std::string("pixel_cache_size_") + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    gMismatches.clear();
    setLogObserver(noteMismatch);
  }
  void TearDown() override {
    setLogObserver(nullptr);
    fs::remove_all(work);
  }
};

// The device's case: the star at its own 18 px and at 31 px, drawn by turns as a chapter with both draws them. Each
// size decodes once into a cache of its own and replays from it after.
TEST_F(PixelCacheSizeFixture, AnImageDrawnAtTwoSizesKeepsACacheForEach) {
  const std::string path = (work / "img_star.png").string();
  ImageBlock ownSize(path, 18, 18, "", kBook, kStar);
  ImageBlock oneEm(path, 31, 31, "", kBook, kStar);
  GfxRenderer renderer;
  const auto drawBoth = [&] {
    for (const bool monochrome : {true, false}) {
      ownSize.render(renderer, 0, 0, /*forceLoad=*/true, monochrome);
      oneEm.render(renderer, 0, 0, /*forceLoad=*/true, monochrome);
    }
  };
  drawBoth();
  EXPECT_NE(ownSize.bwCachePath(), oneEm.bwCachePath());
  EXPECT_NE(ownSize.grayscaleCachePath(), oneEm.grayscaleCachePath());
  ASSERT_TRUE(ownSize.hasPixelCache() && ownSize.hasGrayscaleCache());
  ASSERT_TRUE(oneEm.hasPixelCache() && oneEm.hasGrayscaleCache());
  EXPECT_EQ(18, cachedWidth(ownSize.bwCachePath()));
  EXPECT_EQ(31, cachedWidth(oneEm.bwCachePath()));
  EXPECT_EQ(18, cachedWidth(ownSize.grayscaleCachePath()));
  EXPECT_EQ(31, cachedWidth(oneEm.grayscaleCachePath()));

  const Written written = pixelCaches(work);
  ASSERT_EQ(4u, written.size()) << "a 1-bit and a 4-level cache per size";
  drawBoth();
  drawBoth();
  const auto again = writtenSince(written, pixelCaches(work));
  EXPECT_TRUE(again.empty()) << "decoded again: " << again.front();
  EXPECT_TRUE(gMismatches.empty()) << gMismatches.size() << " mismatches, the first: " << gMismatches.front();
}

// The size is the drawn one, upright: a turned image (a gaiji down a column) is cached at the size it is decoded and
// drawn at, not its box on the page, and a crop of a split image shares its whole image's cache.
TEST_F(PixelCacheSizeFixture, TheCacheIsNamedByTheDrawnSize) {
  const std::string path = (work / "img_star.png").string();
  ImageBlock turned(path, /*width=*/31, /*height=*/18, "", kBook, kStar);  // its box: 31 across the column
  turned.setTurned();
  ASSERT_EQ(18, turned.drawnWidth());
  ASSERT_EQ(31, turned.drawnHeight());
  EXPECT_EQ((work / "img_star.18x31.1bit.pxc").string(), turned.bwCachePath());
  EXPECT_EQ((work / "img_star.18x31.bayer.pxc").string(), turned.grayscaleCachePath());
  EXPECT_EQ((work / "img_star.18x31.ckpt").string(), turned.checkpointPath());

  const ImageBlock whole(path, 18, 36, "", kBook, kStar);
  const auto crop = whole.makeCrop(/*srcYOffset=*/18, /*srcHeight=*/18);
  EXPECT_EQ(whole.bwCachePath(), crop->bwCachePath());
}

// The fixture through the real build, every page drawn as the reader draws it: the image lane's warm (the 1-bit cache
// and the 4-level one beside it), the B/W pass and the gray replay. Then the whole book again, as a reader leafing
// back: nothing is decoded a second time, and no draw finds a cache of another size.
TEST_F(PixelCacheSizeFixture, TheInlineImagesBookLogsNoWidthMismatch) {
  for (const VerticalText vertical : {VerticalText::Off, VerticalText::Automatic}) {
    SCOPED_TRACE(vertical == VerticalText::Off ? "horizontal" : "vertical text");
    pipeline_harness::Profile profile;
    profile.verticalText = vertical;
    const fs::path cacheDir = work / (vertical == VerticalText::Off ? "horizontal" : "vertical");
    int imagePages = 0;
    const auto drawEveryPage = [&] {
      return pipeline_harness::forEachPage(kBook, cacheDir.string(), profile,
                                           [&](int, const Section&, const Page& page, GfxRenderer& renderer) {
                                             if (!page.hasImages()) return;
                                             ++imagePages;
                                             page.warmImageCaches(renderer, 0, 0, /*forceLoadLargeImages=*/true,
                                                                  /*monochromeOutput=*/true,
                                                                  /*alsoWarmGrayscale=*/true);
                                             EXPECT_FALSE(page.hasUncachedImages(true, true, true));
                                             page.render(renderer, profile.fontId, 0, 0, true, true);
                                             page.renderImagesFromGrayscaleCache(renderer, 0, 0);
                                           });
    };
    ASSERT_TRUE(drawEveryPage());
    ASSERT_GT(imagePages, 0);
    const Written written = pixelCaches(cacheDir);
    ASSERT_FALSE(written.empty());
    ASSERT_TRUE(drawEveryPage());
    const auto again = writtenSince(written, pixelCaches(cacheDir));
    EXPECT_TRUE(again.empty()) << "the second reading decoded " << again.front() << " again";
  }
  EXPECT_TRUE(gMismatches.empty()) << gMismatches.size() << " mismatches, the first: " << gMismatches.front();
}

}  // namespace
