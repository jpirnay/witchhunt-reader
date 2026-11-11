// Dark mode's picture boxes (ImagePolarity.h): the frame a box is inverted in must be exactly the pixels the picture
// was drawn on, in every orientation, down to a box that starts or ends inside a byte. A pixel too many is a white line
// beside a cover on a black page; a pixel too few, a black one inside it.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ImagePolarity.h"

namespace {

using image_polarity::PanelRect;

// GfxRenderer's rotateCoordinates(), pixel by pixel: the model the box mapping is checked against.
void rotate(const int orientation, const int x, const int y, const int w, const int h, int& px, int& py) {
  switch (orientation) {
    case touchtransform::Portrait:
      px = y;
      py = h - 1 - x;
      break;
    case touchtransform::LandscapeClockwise:
      px = w - 1 - x;
      py = h - 1 - y;
      break;
    case touchtransform::PortraitInverted:
      px = w - 1 - y;
      py = x;
      break;
    default:
      px = x;
      py = y;
      break;
  }
}

struct Panel {
  int width;
  int height;
  int widthBytes;
  std::vector<uint8_t> frame;
  Panel(const int w, const int h) : width(w), height(h), widthBytes((w + 7) / 8), frame(widthBytes * h, 0xFF) {}
  bool white(const int x, const int y) const { return frame[y * widthBytes + (x >> 3)] & (0x80 >> (x & 7)); }
  int logicalWidth(const int orientation) const {
    return orientation == touchtransform::Portrait || orientation == touchtransform::PortraitInverted ? height : width;
  }
  int logicalHeight(const int orientation) const {
    return orientation == touchtransform::Portrait || orientation == touchtransform::PortraitInverted ? width : height;
  }
};

// Inverts a logical box and checks every panel pixel: flipped inside the box's image, untouched outside it.
void expectBoxInverted(Panel& panel, const int orientation, const int x, const int y, const int w, const int h) {
  std::vector<bool> inBox(panel.width * panel.height, false);
  for (int ly = y; ly < y + h; ++ly) {
    for (int lx = x; lx < x + w; ++lx) {
      if (lx < 0 || ly < 0 || lx >= panel.logicalWidth(orientation) || ly >= panel.logicalHeight(orientation)) continue;
      int px = 0;
      int py = 0;
      rotate(orientation, lx, ly, panel.width, panel.height, px, py);
      inBox[py * panel.width + px] = true;
    }
  }
  PanelRect box{};
  const bool any = image_polarity::panelRectOf(orientation, panel.width, panel.height, x, y, w, h, box);
  if (any) image_polarity::invertPanelRect(panel.frame.data(), panel.widthBytes, box);
  for (int py = 0; py < panel.height; ++py) {
    for (int px = 0; px < panel.width; ++px) {
      ASSERT_EQ(panel.white(px, py), !inBox[py * panel.width + px])
          << "orientation " << orientation << " box " << x << "," << y << " " << w << "x" << h << " at panel " << px
          << "," << py;
    }
  }
}

class EveryOrientation : public ::testing::TestWithParam<int> {};

TEST_P(EveryOrientation, BoxLandsOnTheRotatedPixels) {
  // 37 x 21 on a 64 x 40 panel: no edge of the box falls on a byte boundary in any orientation.
  Panel panel(64, 40);
  expectBoxInverted(panel, GetParam(), 3, 5, 11, 13);
}

TEST_P(EveryOrientation, PartialBytesAtBothEndsAndWithinOneByte) {
  for (const int x : {0, 1, 7, 8, 9, 13}) {
    for (const int w : {1, 2, 6, 7, 8, 9, 15, 17}) {
      Panel panel(48, 48);
      expectBoxInverted(panel, GetParam(), x, x / 2, w, w);
    }
  }
}

TEST_P(EveryOrientation, ABoxOverTheEdgeIsClippedToThePanel) {
  Panel panel(40, 24);
  expectBoxInverted(panel, GetParam(), -5, -3, 20, 12);
  Panel far(40, 24);
  expectBoxInverted(far, GetParam(), far.logicalWidth(GetParam()) - 4, far.logicalHeight(GetParam()) - 2, 30, 30);
}

TEST_P(EveryOrientation, TwiceGivesTheFrameBack) {
  Panel panel(56, 32);
  for (size_t i = 0; i < panel.frame.size(); ++i) panel.frame[i] = static_cast<uint8_t>(i * 37 + 11);
  const std::vector<uint8_t> before = panel.frame;
  PanelRect box{};
  ASSERT_TRUE(image_polarity::panelRectOf(GetParam(), panel.width, panel.height, 2, 3, 19, 9, box));
  image_polarity::invertPanelRect(panel.frame.data(), panel.widthBytes, box);
  EXPECT_NE(panel.frame, before);
  image_polarity::invertPanelRect(panel.frame.data(), panel.widthBytes, box);
  EXPECT_EQ(panel.frame, before);
}

INSTANTIATE_TEST_SUITE_P(ImagePolarity, EveryOrientation,
                         ::testing::Values(touchtransform::Portrait, touchtransform::LandscapeClockwise,
                                           touchtransform::PortraitInverted,
                                           touchtransform::LandscapeCounterClockwise));

TEST(ImagePolarity, NothingOnThePanelIsNoBox) {
  PanelRect box{};
  EXPECT_FALSE(image_polarity::panelRectOf(touchtransform::Portrait, 800, 480, 0, 0, 0, 10, box));
  EXPECT_FALSE(image_polarity::panelRectOf(touchtransform::Portrait, 800, 480, 0, 0, 10, -1, box));
  EXPECT_FALSE(image_polarity::panelRectOf(touchtransform::LandscapeCounterClockwise, 800, 480, 800, 0, 10, 10, box));
  EXPECT_FALSE(image_polarity::panelRectOf(touchtransform::Portrait, 800, 480, -20, -20, 10, 10, box));
}

// The X3's 792-pixel rows are 99 bytes: a box ending in the last byte of a row must not spill into the next.
TEST(ImagePolarity, LastByteOfAnX3RowStaysInItsRow) {
  Panel panel(792, 4);
  ASSERT_EQ(panel.widthBytes, 99);
  expectBoxInverted(panel, touchtransform::LandscapeCounterClockwise, 780, 1, 12, 2);
}

}  // namespace
