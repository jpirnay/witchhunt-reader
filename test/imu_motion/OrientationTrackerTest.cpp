#include <gtest/gtest.h>

#include "OrientationTracker.h"

namespace {

constexpr uint8_t PORTRAIT = 0;
constexpr uint8_t LANDSCAPE_CW = 1;
constexpr uint8_t INVERTED = 2;
constexpr uint8_t LANDSCAPE_CCW = 3;
constexpr uint8_t NONE = OrientationTracker::kNone;

// Holds one sample from `fromMs` to `toMs` at the reader's 200 ms poll; true if any update reported a change.
bool hold(OrientationTracker& t, const float ax, const float ay, const uint32_t fromMs, const uint32_t toMs) {
  bool changed = false;
  for (uint32_t ms = fromMs; ms <= toMs; ms += 200) changed |= t.update(ax, ay, ms);
  return changed;
}

}  // namespace

// The Picco's measured poses (CMD:IMU, 2026-11-16), az left out: the tracker reads the panel's plane only.
TEST(OrientationTrackerTest, ClassifiesTheMeasuredPiccoPoses) {
  EXPECT_EQ(OrientationTracker::classify(-1.03f, 0.02f), PORTRAIT);
  EXPECT_EQ(OrientationTracker::classify(0.08f, 0.90f), LANDSCAPE_CW);  // a quarter clockwise, leant back 35 degrees
  EXPECT_EQ(OrientationTracker::classify(1.03f, -0.02f), INVERTED);
  EXPECT_EQ(OrientationTracker::classify(0.42f, -0.87f), LANDSCAPE_CCW);  // on its way, 26 degrees off the axis
}

TEST(OrientationTrackerTest, FlatOrDiagonalIsNoOrientation) {
  EXPECT_EQ(OrientationTracker::classify(0.04f, 0.05f), NONE);   // flat on the desk
  EXPECT_EQ(OrientationTracker::classify(0.30f, -0.35f), NONE);  // under 0.5 g in the plane: about 27 degrees from flat
  EXPECT_EQ(OrientationTracker::classify(0.70f, 0.70f), NONE);   // the diagonal
  EXPECT_EQ(OrientationTracker::classify(-0.80f, 0.60f), NONE);  // 37 degrees off the axis: inside the dead band
  EXPECT_EQ(OrientationTracker::classify(-0.80f, 0.50f), PORTRAIT);  // 32 degrees off: the axis leads by 1.6
}

TEST(OrientationTrackerTest, ReportsOnlyAfterTheDwell) {
  OrientationTracker t;
  EXPECT_FALSE(t.update(0.0f, 1.0f, 1000));
  EXPECT_FALSE(t.update(0.0f, 1.0f, 1000 + OrientationTracker::kDwellMs - 1));
  EXPECT_TRUE(t.update(0.0f, 1.0f, 1000 + OrientationTracker::kDwellMs));
  EXPECT_EQ(t.settled(), LANDSCAPE_CW);
  // Held on, it does not report again.
  EXPECT_FALSE(hold(t, 0.0f, 1.0f, 2000, 5000));
}

TEST(OrientationTrackerTest, FirstSettledPoseCountsAsNew) {
  // A reader opened with the device held normally reports portrait once; the reader ignores a no-op.
  OrientationTracker t;
  EXPECT_TRUE(hold(t, -1.0f, 0.0f, 0, 1000));
  EXPECT_EQ(t.settled(), PORTRAIT);
}

TEST(OrientationTrackerTest, AFlickDoesNotRotate) {
  OrientationTracker t;
  hold(t, -1.0f, 0.0f, 0, 1000);
  // Half a second through landscape and back: shorter than the dwell.
  EXPECT_FALSE(hold(t, 0.0f, 1.0f, 1200, 1600));
  EXPECT_FALSE(hold(t, -1.0f, 0.0f, 1800, 4000));
  EXPECT_EQ(t.settled(), PORTRAIT);
}

TEST(OrientationTrackerTest, LyingFlatKeepsTheLastOrientation) {
  OrientationTracker t;
  hold(t, 0.0f, -1.0f, 0, 1000);
  ASSERT_EQ(t.settled(), LANDSCAPE_CCW);
  EXPECT_FALSE(hold(t, 0.02f, 0.03f, 1200, 10000));
  EXPECT_EQ(t.settled(), LANDSCAPE_CCW);
  // Picked up again the same way: nothing new to report.
  EXPECT_FALSE(hold(t, 0.0f, -1.0f, 10200, 12000));
}

TEST(OrientationTrackerTest, AnInterruptedHoldStartsTheDwellAgain) {
  OrientationTracker t;
  hold(t, -1.0f, 0.0f, 0, 1000);
  EXPECT_FALSE(t.update(1.0f, 0.0f, 2000));
  EXPECT_FALSE(t.update(0.7f, 0.7f, 2400));  // through the diagonal
  EXPECT_FALSE(t.update(1.0f, 0.0f, 2800));
  EXPECT_FALSE(t.update(1.0f, 0.0f, 2800 + OrientationTracker::kDwellMs - 1));
  EXPECT_TRUE(t.update(1.0f, 0.0f, 2800 + OrientationTracker::kDwellMs));
  EXPECT_EQ(t.settled(), INVERTED);
}

TEST(OrientationTrackerTest, ResetForgetsTheSettledPose) {
  OrientationTracker t;
  hold(t, -1.0f, 0.0f, 0, 1000);
  t.reset();
  EXPECT_EQ(t.settled(), NONE);
  EXPECT_TRUE(hold(t, -1.0f, 0.0f, 5000, 6000));
}

TEST(OrientationTrackerTest, MillisWrapDoesNotStallTheDwell) {
  OrientationTracker t;
  const uint32_t start = 0xFFFFFF00u;
  EXPECT_FALSE(t.update(0.0f, 1.0f, start));
  EXPECT_TRUE(t.update(0.0f, 1.0f, start + OrientationTracker::kDwellMs));  // wraps past zero
}
