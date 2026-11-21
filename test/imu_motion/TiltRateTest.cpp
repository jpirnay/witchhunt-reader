#include <gtest/gtest.h>

#include "TiltRate.h"

namespace {

using TiltRate::Axis;

// A rotation in the panel's native frame (x along the long side, y along the short side, z into the screen) as the
// gyro reports it: the rate about each axis, right-hand rule.
struct Turn {
  int32_t gx;  // mdps
  int32_t gy;
};

// The edge of the text that moves away from the reader, for each orientation. The text's edges sit, in the native
// frame (GfxRenderer's transforms): Portrait top -x, right -y; LandscapeCw top +y, right -x; Inverted top +x,
// right +y; LandscapeCcw top -y, right +x. Turning the edge at `+axis` into +z is a turn about the third axis: +x into
// +z is a negative turn about y, -x into +z positive; +y into +z positive about x, -y into +z negative.
constexpr Turn kRightAway[] = {{-100, 0}, {0, 100}, {100, 0}, {0, -100}};
constexpr Turn kTopAway[] = {{0, 100}, {100, 0}, {0, -100}, {-100, 0}};

}  // namespace

TEST(TiltRateTest, RightEdgeAwayIsNegativeInEveryOrientation) {
  for (uint8_t o = 0; o < 4; ++o) {
    EXPECT_LT(TiltRate::rate(kRightAway[o].gx, kRightAway[o].gy, o, Axis::Sideways), 0) << "orientation " << int(o);
    // The other gesture's axis does not leak in.
    EXPECT_EQ(TiltRate::rate(kRightAway[o].gx, kRightAway[o].gy, o, Axis::ForwardBack), 0) << "orientation " << int(o);
  }
}

TEST(TiltRateTest, TopEdgeAwayIsNegativeInEveryOrientation) {
  for (uint8_t o = 0; o < 4; ++o) {
    EXPECT_LT(TiltRate::rate(kTopAway[o].gx, kTopAway[o].gy, o, Axis::ForwardBack), 0) << "orientation " << int(o);
    EXPECT_EQ(TiltRate::rate(kTopAway[o].gx, kTopAway[o].gy, o, Axis::Sideways), 0) << "orientation " << int(o);
  }
}

// The sideways mapping is the one tilt page turns had before the forward/back axis: unchanged for existing users.
TEST(TiltRateTest, SidewaysKeepsTheEarlierMapping) {
  EXPECT_EQ(TiltRate::rate(7, 3, 0, Axis::Sideways), 7);
  EXPECT_EQ(TiltRate::rate(7, 3, 2, Axis::Sideways), -7);
  EXPECT_EQ(TiltRate::rate(7, 3, 1, Axis::Sideways), -3);
  EXPECT_EQ(TiltRate::rate(7, 3, 3, Axis::Sideways), 3);
}
