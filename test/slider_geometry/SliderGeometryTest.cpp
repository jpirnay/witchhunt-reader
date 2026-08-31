#include <gtest/gtest.h>

#include "components/SliderGeometry.h"

namespace {

// The real slider: a 360 px bar over a 0..100 range, as EpubReaderPercentSelectionActivity
// configures it. barX varies with the panel, so a non-zero one is used throughout to catch an
// origin dropped somewhere in the arithmetic.
constexpr int kBarX = 60;
constexpr int kBarW = 360;
constexpr int kMin = 0;
constexpr int kMax = 100;

int valueAt(const int px) { return SliderGeometry::valueForX(px, kBarX, kBarW, kMin, kMax); }

TEST(SliderGeometry, TrackIsInsetInsideTheBar) {
  EXPECT_EQ(kBarX + 2, SliderGeometry::trackX(kBarX));
  EXPECT_EQ(kBarW - 4, SliderGeometry::trackWidth(kBarW));
}

TEST(SliderGeometry, TrackStartIsMinAndTrackEndIsMax) {
  EXPECT_EQ(kMin, valueAt(SliderGeometry::trackX(kBarX)));
  EXPECT_EQ(kMax, valueAt(SliderGeometry::trackX(kBarX) + SliderGeometry::trackWidth(kBarW)));
}

// The reason valueForX rounds instead of truncating: with truncation only the very last pixel
// of the track yields the maximum, so the top of the range is effectively untappable.
TEST(SliderGeometry, TheTopOfTheRangeIsReachableWellBeforeTheLastPixel) {
  const int track = SliderGeometry::trackWidth(kBarW);
  const int nearEnd = SliderGeometry::trackX(kBarX) + track - 1;
  EXPECT_EQ(kMax, valueAt(nearEnd));
}

TEST(SliderGeometry, MidTrackIsMidRange) {
  const int mid = SliderGeometry::trackX(kBarX) + SliderGeometry::trackWidth(kBarW) / 2;
  EXPECT_EQ(50, valueAt(mid));
}

// Contacts outside the drawn bar clamp rather than miss: the activity's touch band is wider
// than the bar on purpose, so a finger just past an end means "go to that end".
TEST(SliderGeometry, OutsideTheTrackClampsToTheEnds) {
  EXPECT_EQ(kMin, valueAt(kBarX - 40));
  EXPECT_EQ(kMin, valueAt(0));
  EXPECT_EQ(kMax, valueAt(kBarX + kBarW + 40));
}

// The property that actually matters: the pixel the knob is drawn at must map back to the value
// it was drawn for, or tapping the knob makes it jump.
TEST(SliderGeometry, DrawAndHitTestRoundTripForEveryValue) {
  for (int v = kMin; v <= kMax; ++v) {
    const int fill = SliderGeometry::fillWidthFor(v, kBarW, kMin, kMax);
    const int knobPixel = SliderGeometry::trackX(kBarX) + fill;
    EXPECT_EQ(v, valueAt(knobPixel)) << "value " << v << " drawn at fill " << fill;
  }
}

TEST(SliderGeometry, FillIsZeroAtMinAndFullTrackAtMax) {
  EXPECT_EQ(0, SliderGeometry::fillWidthFor(kMin, kBarW, kMin, kMax));
  EXPECT_EQ(SliderGeometry::trackWidth(kBarW), SliderGeometry::fillWidthFor(kMax, kBarW, kMin, kMax));
}

TEST(SliderGeometry, FillClampsForValuesOutsideTheRange) {
  EXPECT_EQ(0, SliderGeometry::fillWidthFor(kMin - 10, kBarW, kMin, kMax));
  EXPECT_EQ(SliderGeometry::trackWidth(kBarW), SliderGeometry::fillWidthFor(kMax + 10, kBarW, kMin, kMax));
}

// Sliders in this firmware are not all 0..100: KOReader's timeouts and the reader's page-step
// settings use small ranges with a non-zero minimum, where a fencepost error is easiest to make.
TEST(SliderGeometry, HandlesASmallRangeWithANonZeroMinimum) {
  constexpr int lo = 5;
  constexpr int hi = 12;
  const int track = SliderGeometry::trackWidth(kBarW);
  EXPECT_EQ(lo, SliderGeometry::valueForX(SliderGeometry::trackX(kBarX), kBarX, kBarW, lo, hi));
  EXPECT_EQ(hi, SliderGeometry::valueForX(SliderGeometry::trackX(kBarX) + track, kBarX, kBarW, lo, hi));
  for (int v = lo; v <= hi; ++v) {
    const int fill = SliderGeometry::fillWidthFor(v, kBarW, lo, hi);
    EXPECT_EQ(v, SliderGeometry::valueForX(SliderGeometry::trackX(kBarX) + fill, kBarX, kBarW, lo, hi))
        << "value " << v;
  }
}

// A degenerate config must not divide by zero.
TEST(SliderGeometry, ZeroRangeIsSafe) {
  EXPECT_EQ(7, SliderGeometry::valueForX(kBarX + 100, kBarX, kBarW, 7, 7));
  EXPECT_EQ(0, SliderGeometry::fillWidthFor(7, kBarW, 7, 7));
}

TEST(SliderGeometry, ZeroWidthBarIsSafe) {
  EXPECT_EQ(kMin, SliderGeometry::valueForX(kBarX, kBarX, 0, kMin, kMax));
  EXPECT_EQ(0, SliderGeometry::fillWidthFor(50, 0, kMin, kMax));
}

}  // namespace
