#include <gtest/gtest.h>

#include "components/themes/ButtonHintStrip.h"

namespace {

using ButtonHintStrip::Strip;

// A strip shaped like the one BaseTheme paints on an X4: four 106px boxes at the tuned
// positions, in a 42px band at the bottom of an 800px-tall portrait screen.
Strip x4Strip() {
  Strip s;
  s.y = 800 - 42;
  s.height = 42;
  s.width = 106;
  s.x[0] = 25;
  s.x[1] = 130;
  s.x[2] = 245;
  s.x[3] = 350;
  for (bool& a : s.active) a = true;
  return s;
}

TEST(ButtonHintStrip, HitsEachBoxAtItsCentre) {
  const Strip s = x4Strip();
  const int y = s.y + s.height / 2;
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(i, ButtonHintStrip::hitTestIn(s, s.x[i] + s.width / 2, y)) << "box " << i;
  }
}

// The index IS the raw hardware button (BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT), which
// is what lets the caller skip any mapping. Pin the order so a reshuffle of mapLabels()
// cannot silently start firing the wrong button.
TEST(ButtonHintStrip, LeftmostBoxIsRawButtonZero) {
  const Strip s = x4Strip();
  EXPECT_EQ(0, ButtonHintStrip::hitTestIn(s, s.x[0], s.y));
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, s.x[3] + s.width - 1, s.y + s.height - 1));
}

TEST(ButtonHintStrip, BoundariesAreHalfOpen) {
  const Strip s = x4Strip();
  const int y = s.y + 1;
  // First px of a box hits; the px one past its right edge does not. Uses box 2, the one
  // pair with real space either side of it (see TunedBoxesOverlapByOnePixel).
  EXPECT_EQ(2, ButtonHintStrip::hitTestIn(s, s.x[2], y));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2] - 1, y));
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, s.x[2] + s.width, y));  // = x[3], box 3 starts here
  // Same on the vertical axis: the row above the band and the row past it both miss.
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2], s.y - 1));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2], s.y + s.height));
}

// The hand-tuned X4 positions {25,130,245,350} at width 106 are not disjoint: box 0 spans
// 25..130 and box 1 starts at 130, so one column belongs to both. hitTestIn scans in order
// and awards it to the lower index. Immaterial to a finger, but pinned here so the tie-break
// is a decision on record rather than an accident of loop order -- and so that anyone
// re-tuning the positions sees that these boxes were never disjoint to begin with.
TEST(ButtonHintStrip, TunedBoxesOverlapByOnePixel) {
  const Strip s = x4Strip();
  const int y = s.y + s.height / 2;
  ASSERT_EQ(s.x[0] + s.width, s.x[1] + 1) << "x4 box 0/1 overlap assumption no longer holds";
  EXPECT_EQ(0, ButtonHintStrip::hitTestIn(s, s.x[1], y));
  EXPECT_EQ(1, ButtonHintStrip::hitTestIn(s, s.x[1] + 1, y));
}

TEST(ButtonHintStrip, MissesOutsideTheBoxes) {
  const Strip s = x4Strip();
  const int y = s.y + s.height / 2;
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, 0, y));                     // left of box 0
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[1] + s.width + 2, y));  // real gap, 1 -> 2
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[3] + s.width + 1, y));  // right of box 3
}

// An empty label paints no full-size box (BaseTheme) or a decorative stub (LyraTheme).
// Either way there is no action behind it, so the band must stay dead there.
TEST(ButtonHintStrip, InactiveBoxIsNotTappable) {
  Strip s = x4Strip();
  s.active[2] = false;
  const int y = s.y + s.height / 2;
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2] + s.width / 2, y));
  // Its neighbours still work — an inactive box must not blank the whole strip.
  EXPECT_EQ(1, ButtonHintStrip::hitTestIn(s, s.x[1] + s.width / 2, y));
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, s.x[3] + s.width / 2, y));
}

// invalidate() leaves a default Strip. Every tap must miss it, which is what stops a screen
// that draws no hints from inheriting the previous screen's boxes.
TEST(ButtonHintStrip, DefaultStripSwallowsNothing) {
  const Strip s;
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, 0, 0));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, 240, 780));
}

// The shared-state path: record/hasStrip/hitTest against the seqlock.
TEST(ButtonHintStrip, RecordThenHitTestThroughSharedState) {
  ButtonHintStrip::invalidate();
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  EXPECT_EQ(-1, ButtonHintStrip::hitTest(25, 780));

  const Strip s = x4Strip();
  ButtonHintStrip::record(s);
  EXPECT_TRUE(ButtonHintStrip::hasStrip());
  EXPECT_EQ(0, ButtonHintStrip::hitTest(s.x[0] + 1, s.y + 1));
  EXPECT_EQ(2, ButtonHintStrip::hitTest(s.x[2] + 1, s.y + 1));

  ButtonHintStrip::invalidate();
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  EXPECT_EQ(-1, ButtonHintStrip::hitTest(s.x[0] + 1, s.y + 1));
}

// A strip whose labels are all empty is not a strip: skipping the tap queue entirely on
// such a screen leaves the tap for whoever else wants it.
TEST(ButtonHintStrip, AllInactiveCountsAsNoStrip) {
  Strip s = x4Strip();
  for (bool& a : s.active) a = false;
  ButtonHintStrip::record(s);
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  ButtonHintStrip::invalidate();
}

}  // namespace
