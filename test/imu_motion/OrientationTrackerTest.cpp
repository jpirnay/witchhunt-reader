#include <gtest/gtest.h>

#include <cstdio>

#include "OrientationTracker.h"

namespace {

constexpr uint8_t PORTRAIT = 0;
constexpr uint8_t LANDSCAPE_CW = 1;
constexpr uint8_t INVERTED = 2;
constexpr uint8_t LANDSCAPE_CCW = 3;
constexpr uint8_t NONE = OrientationTracker::kNone;

// Holds one sample (mg) from `fromMs` to `toMs` at the reader's 200 ms poll; true if any update reported a change.
bool hold(OrientationTracker& t, const int32_t axMg, const int32_t ayMg, const uint32_t fromMs, const uint32_t toMs) {
  bool changed = false;
  for (uint32_t ms = fromMs; ms <= toMs; ms += 200) changed |= t.update(axMg, ayMg, ms);
  return changed;
}

}  // namespace

// The Picco's measured poses (CMD:IMU, 2026-11-16), az left out: the tracker reads the panel's plane only.
TEST(OrientationTrackerTest, ClassifiesTheMeasuredPiccoPoses) {
  EXPECT_EQ(OrientationTracker::classify(-1030, 20), PORTRAIT);
  EXPECT_EQ(OrientationTracker::classify(80, 900), LANDSCAPE_CW);  // a quarter clockwise, leant back 35 degrees
  EXPECT_EQ(OrientationTracker::classify(1030, -20), INVERTED);
  EXPECT_EQ(OrientationTracker::classify(420, -870), LANDSCAPE_CCW);  // on its way, 26 degrees off the axis
}

TEST(OrientationTrackerTest, FlatOrDiagonalIsNoOrientation) {
  EXPECT_EQ(OrientationTracker::classify(40, 50), NONE);         // flat on the desk
  EXPECT_EQ(OrientationTracker::classify(300, -350), NONE);      // under 0.5 g in the plane: about 27 degrees from flat
  EXPECT_EQ(OrientationTracker::classify(700, 700), NONE);       // the diagonal
  EXPECT_EQ(OrientationTracker::classify(-800, 600), NONE);      // 37 degrees off the axis: inside the dead band
  EXPECT_EQ(OrientationTracker::classify(-800, 500), PORTRAIT);  // 32 degrees off: the axis leads by 1.6
}

TEST(OrientationTrackerTest, ReportsOnlyAfterTheDwell) {
  OrientationTracker t;
  EXPECT_FALSE(t.update(0, 1000, 1000));
  EXPECT_FALSE(t.update(0, 1000, 1000 + OrientationTracker::kDwellMs - 1));
  EXPECT_TRUE(t.update(0, 1000, 1000 + OrientationTracker::kDwellMs));
  EXPECT_EQ(t.settled(), LANDSCAPE_CW);
  // Held on, it does not report again.
  EXPECT_FALSE(hold(t, 0, 1000, 2000, 5000));
}

TEST(OrientationTrackerTest, FirstSettledPoseCountsAsNew) {
  // A reader opened with the device held normally reports portrait once; the reader ignores a no-op.
  OrientationTracker t;
  EXPECT_TRUE(hold(t, -1000, 0, 0, 1000));
  EXPECT_EQ(t.settled(), PORTRAIT);
}

TEST(OrientationTrackerTest, AFlickDoesNotRotate) {
  OrientationTracker t;
  hold(t, -1000, 0, 0, 1000);
  // Half a second through landscape and back: shorter than the dwell.
  EXPECT_FALSE(hold(t, 0, 1000, 1200, 1600));
  EXPECT_FALSE(hold(t, -1000, 0, 1800, 4000));
  EXPECT_EQ(t.settled(), PORTRAIT);
}

TEST(OrientationTrackerTest, LyingFlatKeepsTheLastOrientation) {
  OrientationTracker t;
  hold(t, 0, -1000, 0, 1000);
  ASSERT_EQ(t.settled(), LANDSCAPE_CCW);
  EXPECT_FALSE(hold(t, 20, 30, 1200, 10000));
  EXPECT_EQ(t.settled(), LANDSCAPE_CCW);
  // Picked up again the same way: nothing new to report.
  EXPECT_FALSE(hold(t, 0, -1000, 10200, 12000));
}

TEST(OrientationTrackerTest, AnInterruptedHoldStartsTheDwellAgain) {
  OrientationTracker t;
  hold(t, -1000, 0, 0, 1000);
  EXPECT_FALSE(t.update(1000, 0, 2000));
  EXPECT_FALSE(t.update(700, 700, 2400));  // through the diagonal
  EXPECT_FALSE(t.update(1000, 0, 2800));
  EXPECT_FALSE(t.update(1000, 0, 2800 + OrientationTracker::kDwellMs - 1));
  EXPECT_TRUE(t.update(1000, 0, 2800 + OrientationTracker::kDwellMs));
  EXPECT_EQ(t.settled(), INVERTED);
}

TEST(OrientationTrackerTest, ResetForgetsTheSettledPose) {
  OrientationTracker t;
  hold(t, -1000, 0, 0, 1000);
  t.reset();
  EXPECT_EQ(t.settled(), NONE);
  EXPECT_TRUE(hold(t, -1000, 0, 5000, 6000));
}

TEST(OrientationTrackerTest, MillisWrapDoesNotStallTheDwell) {
  OrientationTracker t;
  const uint32_t start = 0xFFFFFF00u;
  EXPECT_FALSE(t.update(0, 1000, start));
  EXPECT_TRUE(t.update(0, 1000, start + OrientationTracker::kDwellMs));  // wraps past zero
}

namespace {
// The float classifier this one replaced, as it was, for the comparison below.
uint8_t floatClassify(const float ax, const float ay) {
  constexpr float kMinTiltG = 0.5f;
  constexpr float kLeadRatio = 1.5f;
  const auto absf = [](const float v) { return v < 0 ? -v : v; };
  if (ax * ax + ay * ay < kMinTiltG * kMinTiltG) return NONE;
  const float x = absf(ax);
  const float y = absf(ay);
  if (x >= kLeadRatio * y) return ax < 0 ? PORTRAIT : INVERTED;
  if (y >= kLeadRatio * x) return ay > 0 ? LANDSCAPE_CW : LANDSCAPE_CCW;
  return NONE;
}
}  // namespace

// Every whole-mg sample in the plane over the IMUs' ±2 g range, against the float classifier given the same sample in
// g. They may part only on a threshold itself (the 500 mg circle, or a lead of exactly 3:2), where the float answer
// came from how mg / 1000 rounded.
TEST(OrientationTrackerTest, MatchesTheFloatClassifierOffTheThresholds) {
  int parted = 0;
  for (int32_t x = -2000; x <= 2000; ++x) {
    for (int32_t y = -2000; y <= 2000; ++y) {
      if (OrientationTracker::classify(x, y) == floatClassify(x / 1000.0f, y / 1000.0f)) continue;
      const int32_t ax = x < 0 ? -x : x;
      const int32_t ay = y < 0 ? -y : y;
      EXPECT_TRUE(ax * ax + ay * ay == 500 * 500 || 2 * ax == 3 * ay || 2 * ay == 3 * ax) << x << "," << y;
      ++parted;
    }
  }
  std::printf("float and integer classifiers part on %d of 16,008,001 samples\n", parted);
}
