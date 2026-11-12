// DirectPixelWriter in Dark Mode (GfxRenderer::keepsImagePolarity): every picture the reader draws from its pixel
// cache or a decoder goes through this writer, and the panel shows the frame inverted. So the writer stores each
// pixel inverted, white ones included, and the picture reaches the glass as it is, not as its negative.
#include <gtest/gtest.h>

#include <cstdint>

#include "DirectPixelWriter.h"

namespace {

constexpr int kW = 64;
constexpr int kH = 40;

// One picture row of the four 2-bit values: black, dark gray, light gray, white.
void drawRow(GfxRenderer& renderer, const int x, const int y) {
  DirectPixelWriter pw;
  pw.init(renderer);
  pw.beginRow(y);
  for (int i = 0; i < 4; ++i) pw.writePixel(x + i, static_cast<uint8_t>(i));
}

// What the panel shows at logical (x, y): the frame's bit, inverted when the output is.
bool glassIsBlack(const GfxRenderer& renderer, const bool outputInverted, const int x, const int y) {
  int px = 0;
  int py = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      px = y;
      py = kH - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      px = kW - 1 - x;
      py = kH - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      px = kW - 1 - y;
      py = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      px = x;
      py = y;
      break;
  }
  return renderer.isBlackAt(px, py) != outputInverted;
}

class PolarityOrientation : public ::testing::TestWithParam<GfxRenderer::Orientation> {};

// The same picture on a light page and on a dark one looks the same on the glass: black where the B/W pass draws
// black (values 0-2), white where it leaves white (3).
TEST_P(PolarityOrientation, ThePictureLooksTheSameOnTheGlassInBothModes) {
  for (const bool dark : {false, true}) {
    GfxRenderer renderer(kW, kH, GetParam());
    renderer.setOutputInverted(dark);
    drawRow(renderer, 5, 7);
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(glassIsBlack(renderer, dark, 5 + i, 7), i < 3) << "dark=" << dark << " value " << i;
    }
  }
}

// The page under a picture is black in Dark Mode (the frame's white): a picture's white pixels must replace it, or a
// photo shows black holes wherever it is white.
TEST_P(PolarityOrientation, WhitePixelsAreWrittenOverWhateverIsUnder) {
  GfxRenderer renderer(kW, kH, GetParam());
  renderer.setOutputInverted(true);
  renderer.fill(0x00);  // the frame's black: white on the glass, the opposite of what a dark page holds
  drawRow(renderer, 2, 3);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(glassIsBlack(renderer, true, 2 + i, 3), i < 3) << "value " << i;
}

INSTANTIATE_TEST_SUITE_P(DirectPixelWriter, PolarityOrientation,
                         ::testing::Values(GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                           GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise));

// A picture replayed twice (a redraw over the page on screen, a resumed decode) is the same picture: the writes are
// absolute, unlike an XOR over its box.
TEST(DirectPixelWriterPolarity, DrawingTwiceChangesNothing) {
  GfxRenderer renderer(kW, kH, GfxRenderer::LandscapeCounterClockwise);
  renderer.setOutputInverted(true);
  drawRow(renderer, 9, 4);
  bool once[4];
  for (int i = 0; i < 4; ++i) once[i] = renderer.isBlackAt(9 + i, 4);
  drawRow(renderer, 9, 4);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(renderer.isBlackAt(9 + i, 4), once[i]) << "value " << i;
}

// The gray planes never reach the panel inverted, and keep their encoding: Dark Mode changes only the B/W pass.
TEST(DirectPixelWriterPolarity, GrayPlanesAreUnchanged) {
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    GfxRenderer light(kW, kH, GfxRenderer::LandscapeCounterClockwise);
    GfxRenderer dark(kW, kH, GfxRenderer::LandscapeCounterClockwise);
    dark.setOutputInverted(true);
    for (GfxRenderer* r : {&light, &dark}) {
      r->fill(0x00);
      r->setRenderMode(mode);
      drawRow(*r, 1, 1);
    }
    for (int i = 0; i < 4; ++i) EXPECT_EQ(light.isBlackAt(1 + i, 1), dark.isBlackAt(1 + i, 1)) << "value " << i;
  }
}

}  // namespace
