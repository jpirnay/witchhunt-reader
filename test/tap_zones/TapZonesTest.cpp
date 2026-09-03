#include <gtest/gtest.h>

#include "components/TapZones.h"

namespace {

using TapZones::Zone;
using TapZones::zoneFor;

// The T5S3 in portrait, and the X4 in portrait, so the arithmetic is exercised
// on two aspect ratios rather than one.
constexpr int W = 540;
constexpr int H = 960;

TEST(TapZones, OuterThirdsAreLeftAndRightOverTheirWholeHeight) {
  // This is the property that matters most: the page-turn zones must not have
  // been narrowed by the arrival of Top and Bottom.
  for (int y = 0; y < H; y += 40) {
    EXPECT_EQ(Zone::Left, zoneFor(0, y, W, H)) << "y=" << y;
    EXPECT_EQ(Zone::Left, zoneFor(W / 3 - 1, y, W, H)) << "y=" << y;
    EXPECT_EQ(Zone::Right, zoneFor(W - W / 3, y, W, H)) << "y=" << y;
    EXPECT_EQ(Zone::Right, zoneFor(W - 1, y, W, H)) << "y=" << y;
  }
}

TEST(TapZones, TheCentreColumnSplitsIntoTopCentreBottom) {
  const int x = W / 2;
  EXPECT_EQ(Zone::Top, zoneFor(x, 0, W, H));
  EXPECT_EQ(Zone::Top, zoneFor(x, H / 3 - 1, W, H));
  EXPECT_EQ(Zone::Centre, zoneFor(x, H / 3, W, H));
  EXPECT_EQ(Zone::Centre, zoneFor(x, H - H / 3 - 1, W, H));
  EXPECT_EQ(Zone::Bottom, zoneFor(x, H - H / 3, W, H));
  EXPECT_EQ(Zone::Bottom, zoneFor(x, H - 1, W, H));
}

TEST(TapZones, CentreMatchesTheReaderMenuRectangleItReplaces) {
  // ReaderUtils::isTouchMenuTap accepted x in [w/3, w - w/3) and y in
  // [h/3, h - h/3). Centre must be exactly that set, or a centre tap stops
  // opening the reader menu on a device that has always had it.
  const int zw = W / 3;
  const int zh = H / 3;
  for (int x = 0; x < W; x += 7) {
    for (int y = 0; y < H; y += 11) {
      const bool wasMenuTap = x >= zw && x < W - zw && y >= zh && y < H - zh;
      EXPECT_EQ(wasMenuTap, zoneFor(x, y, W, H) == Zone::Centre) << "x=" << x << " y=" << y;
    }
  }
}

TEST(TapZones, LandscapeKeepsTheSameShape) {
  // Zones are computed in logical pixels, so rotating the reader swaps width and
  // height and the bands follow the page rather than the panel.
  EXPECT_EQ(Zone::Left, zoneFor(10, 200, H, W));
  EXPECT_EQ(Zone::Right, zoneFor(H - 10, 200, H, W));
  EXPECT_EQ(Zone::Top, zoneFor(H / 2, 10, H, W));
  EXPECT_EQ(Zone::Bottom, zoneFor(H / 2, W - 10, H, W));
  EXPECT_EQ(Zone::Centre, zoneFor(H / 2, W / 2, H, W));
}

TEST(TapZones, DegenerateSizesFallThroughToCentre) {
  // A one- or two-pixel screen makes every third zero-wide, so all four outer
  // tests are unreachable (x < 0 and x >= width both fail for an in-range x, and
  // likewise for y) and the point lands in Centre. Asserted because zoneFor is
  // called with whatever the renderer reports and must stay total: the worst
  // outcome of a nonsense size should be a dull answer, not an undefined one.
  EXPECT_EQ(Zone::Centre, zoneFor(0, 0, 1, 1));
  EXPECT_EQ(Zone::Centre, zoneFor(1, 1, 2, 2));
}

}  // namespace
