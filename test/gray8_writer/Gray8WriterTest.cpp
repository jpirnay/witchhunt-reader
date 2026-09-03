// DirectGray8Writer geometry.
//
// The byte writer exists so a 16-level panel can be painted without the 2-bit
// plane encoding, and it borrows DirectPixelWriter's orientation transform to do
// it. These tests pin that borrowing down: for every orientation, a logical pixel
// must land on the SAME physical pixel through both writers. A divergence here is
// a rotated or mirrored sleep screen, and it would only ever be visible on the
// one board that takes this path.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "DirectPixelWriter.h"

namespace {

constexpr uint8_t kUnset = 0x7F;

// Paint one logical pixel through both writers and report where each landed.
// Returns the physical coordinates the byte writer used, or {-1,-1} if it
// dropped the pixel.
struct Landing {
  int x = -1;
  int y = -1;
};

Landing gray8Landing(const std::vector<uint8_t>& canvas, const int stride, const int width, const int height) {
  Landing hit;
  int found = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      if (canvas[static_cast<size_t>(y) * stride + x] != kUnset) {
        hit = {x, y};
        found++;
      }
    }
  }
  EXPECT_LE(found, 1) << "a single writePixel must touch at most one canvas byte";
  return hit;
}

Landing bwLanding(const GfxRenderer& renderer) {
  Landing hit;
  int found = 0;
  for (int y = 0; y < renderer.getDisplayHeight(); y++) {
    for (int x = 0; x < renderer.getDisplayWidth(); x++) {
      if (renderer.isBlackAt(x, y)) {
        hit = {x, y};
        found++;
      }
    }
  }
  EXPECT_LE(found, 1) << "a single writePixel must touch at most one framebuffer bit";
  return hit;
}

class OrientationTest : public ::testing::TestWithParam<GfxRenderer::Orientation> {};

TEST_P(OrientationTest, ByteWriterLandsWhereBitWriterDoes) {
  // Deliberately not square and not a multiple of 8 in both axes, so a
  // transposed or byte-rounded mapping cannot pass by coincidence.
  constexpr int kPanelW = 96;
  constexpr int kPanelH = 40;
  constexpr int kStride = kPanelW + 5;  // stride > width: the canvas may be padded

  for (int logicalY = 0; logicalY < 40; logicalY += 7) {
    for (int logicalX = 0; logicalX < 40; logicalX += 5) {
      GfxRenderer renderer(kPanelW, kPanelH, GetParam());
      if (logicalX >= renderer.getScreenWidth() || logicalY >= renderer.getScreenHeight()) continue;

      std::vector<uint8_t> canvas(static_cast<size_t>(kStride) * kPanelH, kUnset);
      DirectGray8Writer g8;
      g8.init(renderer, canvas.data(), kStride);
      g8.beginRow(logicalY);
      g8.writePixel(logicalX, 0x42);

      DirectPixelWriter pw;
      pw.init(renderer);
      pw.beginRow(logicalY);
      pw.writePixel(logicalX, 0);  // value < 3 draws black in BW mode

      const Landing gray = gray8Landing(canvas, kStride, kPanelW, kPanelH);
      const Landing bw = bwLanding(renderer);
      EXPECT_EQ(gray.x, bw.x) << "orientation " << GetParam() << " logical (" << logicalX << "," << logicalY << ")";
      EXPECT_EQ(gray.y, bw.y) << "orientation " << GetParam() << " logical (" << logicalX << "," << logicalY << ")";
      EXPECT_NE(gray.x, -1) << "pixel dropped at logical (" << logicalX << "," << logicalY << ")";
      EXPECT_EQ(canvas[static_cast<size_t>(gray.y) * kStride + gray.x], 0x42) << "sample must be stored unquantised";
    }
  }
}

INSTANTIATE_TEST_SUITE_P(AllOrientations, OrientationTest,
                         ::testing::Values(GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                           GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise));

TEST(Gray8Writer, StoresTheFullEightBitRange) {
  // The reason this writer exists: a level the 2-bit path cannot express must
  // survive to the canvas untouched, for the panel to quantise later.
  GfxRenderer renderer(64, 8, GfxRenderer::LandscapeCounterClockwise);
  std::vector<uint8_t> canvas(64 * 8, kUnset);
  DirectGray8Writer g8;
  g8.init(renderer, canvas.data(), 64);
  g8.beginRow(0);
  for (int x = 0; x < 64; x++) g8.writePixel(x, static_cast<uint8_t>(x * 4));
  for (int x = 0; x < 64; x++) EXPECT_EQ(canvas[x], static_cast<uint8_t>(x * 4)) << "at x=" << x;
}

TEST(Gray8Writer, DropsPixelsOutsideThePanel) {
  GfxRenderer renderer(64, 8, GfxRenderer::LandscapeCounterClockwise);
  std::vector<uint8_t> canvas(64 * 8, kUnset);
  DirectGray8Writer g8;
  g8.init(renderer, canvas.data(), 64);

  g8.beginRow(0);
  g8.writePixel(-1, 0x00);  // left of the panel
  g8.writePixel(64, 0x00);  // right of it
  g8.writePixel(9999, 0x00);
  g8.beginRow(8);  // below the last row
  g8.writePixel(0, 0x00);
  g8.beginRow(-1);  // above the first
  g8.writePixel(0, 0x00);

  for (const uint8_t byte : canvas) EXPECT_EQ(byte, kUnset);
}

TEST(Gray8Writer, HonoursStridePaddingBetweenRows) {
  // A canvas whose stride exceeds its width must not have row N+1 written into
  // row N's padding -- the panel buffer this borrows is the driver's own.
  constexpr int kStride = 70;
  GfxRenderer renderer(64, 4, GfxRenderer::LandscapeCounterClockwise);
  std::vector<uint8_t> canvas(static_cast<size_t>(kStride) * 4, kUnset);
  DirectGray8Writer g8;
  g8.init(renderer, canvas.data(), kStride);
  for (int y = 0; y < 4; y++) {
    g8.beginRow(y);
    for (int x = 0; x < 64; x++) g8.writePixel(x, static_cast<uint8_t>(0x10 + y));
  }
  for (int y = 0; y < 4; y++) {
    EXPECT_EQ(canvas[static_cast<size_t>(y) * kStride], static_cast<uint8_t>(0x10 + y));
    for (int pad = 64; pad < kStride; pad++) {
      EXPECT_EQ(canvas[static_cast<size_t>(y) * kStride + pad], kUnset) << "row " << y << " pad " << pad;
    }
  }
}

}  // namespace
