// Where the dots of a reader image land in the B/W pass.
//
// The B/W pass draws every reader image from its 1-bit pixel cache, an Atkinson dither of the decoded grays made at
// the size the image is drawn (ImageBlock::render, monochromeOutput). These tests pin that it is shown as it was
// dithered: at the drawn size, one cached pixel to one screen pixel, never rescaled after the dither; and a tall image
// cut across pages draws each piece's own rows of the one dither.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/PixelCache.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_png_images.epub";
const char* kEntry = "OEBPS/images/scaling_test.png";
constexpr int kWidth = 120;
constexpr int kHeight = 160;
// Where the image is drawn. Landscape counter-clockwise is the identity transform on the stub's panel, so a screen
// pixel is the framebuffer pixel of the same coordinates.
constexpr int kX = 10;
constexpr int kY = 20;
constexpr int kStride = 60;  // the stub's panel: 480 px wide
constexpr int kRows = 800;

struct CachedImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;  // 2 bits a pixel, 4 to a byte, MSB first
  uint8_t at(const int col, const int row) const {
    const int bytesPerRow = (width + 3) / 4;
    return (pixels[static_cast<size_t>(row) * bytesPerRow + (col >> 2)] >> (6 - (col & 3) * 2)) & 0x03;
  }
};

struct ImageDitherPlacementFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() / ("dither_placement_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  }
  void TearDown() override { fs::remove_all(work); }

  ImageBlock block() const { return ImageBlock((work / "img.png").string(), kWidth, kHeight, "", kBook, kEntry); }

  void fill(const uint8_t byte) const { std::memset(renderer.getWriteTarget(), byte, kStride * kRows); }
  bool black(const int x, const int y) const {
    return (renderer.getWriteTarget()[y * kStride + (x >> 3)] & (1 << (7 - (x & 7)))) == 0;
  }

  static CachedImage read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CachedImage image;
    if (bytes.size() < PixelCache::PXC_HEADER_BYTES) return image;
    uint16_t header[3];
    std::memcpy(header, bytes.data(), sizeof(header));
    if (header[0] != PixelCache::PXC_MAGIC) return image;
    image.width = header[1];
    image.height = header[2];
    image.pixels.assign(bytes.begin() + PixelCache::PXC_HEADER_BYTES, bytes.end());
    return image;
  }
  // The decode the warm pass does: both caches, as with Display and AA on.
  CachedImage decodeBoth(ImageBlock& image) {
    image.render(renderer, kX, kY, /*forceLoad=*/true, /*monochromeOutput=*/true, /*alsoCacheOtherVariant=*/true);
    return read(work / "img.1bit.pxc");
  }
};

// The 1-bit cache is the drawn size and holds only black and white.
TEST_F(ImageDitherPlacementFixture, TheOneBitCacheIsADitherAtTheDrawnSize) {
  ImageBlock image = block();
  const CachedImage bw = decodeBoth(image);
  ASSERT_EQ(bw.width, kWidth);
  ASSERT_EQ(bw.height, kHeight);
  int blacks = 0;
  for (int row = 0; row < bw.height; ++row) {
    for (int col = 0; col < bw.width; ++col) {
      const uint8_t v = bw.at(col, row);
      ASSERT_TRUE(v == 0 || v == 3) << "a gray level in the 1-bit cache at " << col << "," << row;
      blacks += v == 0;
    }
  }
  EXPECT_GT(blacks, 0);
  EXPECT_LT(blacks, kWidth * kHeight) << "a dither, not a solid block";
}

// The B/W pass puts each cached pixel on its own screen pixel at the image's place, and draws nothing outside it.
TEST_F(ImageDitherPlacementFixture, TheBwPassDrawsTheCacheOnePixelToOne) {
  ImageBlock image = block();
  const CachedImage bw = decodeBoth(image);
  ASSERT_EQ(bw.width, kWidth);

  fill(0xFF);
  image.render(renderer, kX, kY, /*forceLoad=*/true, /*monochromeOutput=*/true);  // from the cache now
  int mismatches = 0;
  for (int row = 0; row < kHeight; ++row) {
    for (int col = 0; col < kWidth; ++col) {
      mismatches += black(kX + col, kY + row) != (bw.at(col, row) == 0);
    }
  }
  EXPECT_EQ(mismatches, 0);
  int outside = 0;
  for (int y = 0; y < kRows; ++y) {
    for (int x = 0; x < kStride * 8; ++x) {
      const bool inBox = x >= kX && x < kX + kWidth && y >= kY && y < kY + kHeight;
      outside += !inBox && black(x, y);
    }
  }
  EXPECT_EQ(outside, 0);
}

// A tall image cut across pages draws each piece from the one cache, from the row the piece starts at: the dither runs
// on across the cut instead of starting again at each piece.
TEST_F(ImageDitherPlacementFixture, ASplitImageContinuesTheSameDither) {
  ImageBlock image = block();
  const CachedImage bw = decodeBoth(image);
  ASSERT_EQ(bw.height, kHeight);

  constexpr int kFrom = 60;
  constexpr int kRowsDrawn = 50;
  const auto piece = image.makeCrop(kFrom, kRowsDrawn);
  fill(0xFF);
  piece->render(renderer, kX, kY, /*forceLoad=*/true, /*monochromeOutput=*/true);
  int mismatches = 0;
  for (int row = 0; row < kRowsDrawn; ++row) {
    for (int col = 0; col < kWidth; ++col) {
      mismatches += black(kX + col, kY + row) != (bw.at(col, kFrom + row) == 0);
    }
  }
  EXPECT_EQ(mismatches, 0);
  for (int col = 0; col < kWidth; ++col) {
    EXPECT_FALSE(black(kX + col, kY + kRowsDrawn)) << "drawn past the piece at column " << col;
  }
}

}  // namespace
