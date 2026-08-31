#include <gtest/gtest.h>

#include "components/themes/TapTargets.h"

namespace {

using TapTargets::Recorder;
using TapTargets::Set;

// A vertical home menu the way BaseTheme/LyraTheme record one: full-width rows, menu-local
// values, a gap between rows.
Set verticalMenu(const int rows = 4) {
  Recorder::Builder b;
  for (int i = 0; i < rows; ++i) b.add(10, 100 + i * 60, 460, 50, i);
  return b.set();
}

TEST(TapTargets, HitsEachRectAtItsCentre) {
  const Set s = verticalMenu();
  for (int i = 0; i < s.count; ++i) {
    EXPECT_EQ(i, TapTargets::hitTestIn(s, 240, 100 + i * 60 + 25)) << "row " << i;
  }
}

TEST(TapTargets, TheGapBetweenRectsIsAMiss) {
  const Set s = verticalMenu();
  // Row 0 spans y 100..149; 150..159 is the gap before row 1.
  EXPECT_EQ(0, TapTargets::hitTestIn(s, 240, 149));
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 240, 150));
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 240, 159));
  EXPECT_EQ(1, TapTargets::hitTestIn(s, 240, 160));
}

TEST(TapTargets, MissesOutsideTheHorizontalExtent) {
  const Set s = verticalMenu();
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 9, 125));
  EXPECT_EQ(0, TapTargets::hitTestIn(s, 10, 125));
  EXPECT_EQ(0, TapTargets::hitTestIn(s, 469, 125));
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 470, 125));
}

// The carousel's menu is a horizontal icon strip, which is the case no y-band model covers and
// the reason this recorder exists alongside ListTouchBand.
TEST(TapTargets, HandlesAHorizontalStrip) {
  Recorder::Builder b;
  const int tileW = 120;
  for (int slot = 0; slot < 4; ++slot) b.add(slot * tileW, 700, tileW, 60, slot + 2);  // windowed
  const Set s = b.set();
  EXPECT_EQ(2, TapTargets::hitTestIn(s, 60, 730));
  EXPECT_EQ(3, TapTargets::hitTestIn(s, 180, 730));
  EXPECT_EQ(5, TapTargets::hitTestIn(s, 3 * tileW + 60, 730));
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 4 * tileW, 730));
}

// The value is what the hit MEANS, not the slot it was recorded in: the carousel's sliding
// window makes those differ whenever the icons overflow.
TEST(TapTargets, ReturnsTheValueNotTheSlot) {
  Recorder::Builder b;
  b.add(0, 0, 100, 100, 7);
  EXPECT_EQ(7, TapTargets::hitTestIn(b.set(), 50, 50));
}

// The carousel's side covers slide behind the centre one and the draw puts the centre on top,
// so the recorder adds it first and first-match-wins has to agree.
TEST(TapTargets, OverlappingRectsResolveToTheOneRecordedFirst) {
  Recorder::Builder b;
  b.add(100, 0, 200, 300, 5);  // centre, drawn last = on top, recorded first
  b.add(40, 20, 120, 260, 4);  // left side cover, overlaps the centre's left edge
  const Set s = b.set();
  EXPECT_EQ(5, TapTargets::hitTestIn(s, 120, 150));  // inside the overlap
  EXPECT_EQ(4, TapTargets::hitTestIn(s, 60, 150));   // clear of the centre
}

TEST(TapTargets, ZeroSizedRectsAreSkipped) {
  Recorder::Builder b;
  b.add(0, 0, 0, 100, 1);
  b.add(0, 0, 100, 0, 2);
  b.add(0, 0, 100, 100, 3);
  EXPECT_EQ(3, TapTargets::hitTestIn(b.set(), 50, 50));
}

TEST(TapTargets, EmptySetMissesEverything) {
  const Set s{};
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 0, 0));
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 100, 100));
}

TEST(TapTargets, StopsAtTheCapWithoutCorruptingWhatFits) {
  Recorder::Builder b;
  for (int i = 0; i < TapTargets::kMaxTargets + 4; ++i) b.add(0, i * 10, 100, 10, i);
  const Set s = b.set();
  EXPECT_EQ(TapTargets::kMaxTargets, s.count);
  EXPECT_EQ(0, TapTargets::hitTestIn(s, 50, 5));
  EXPECT_EQ(TapTargets::kMaxTargets - 1, TapTargets::hitTestIn(s, 50, (TapTargets::kMaxTargets - 1) * 10 + 5));
  EXPECT_EQ(-1, TapTargets::hitTestIn(s, 50, TapTargets::kMaxTargets * 10 + 5));
}

TEST(TapTargets, RecordSnapshotAndInvalidateRoundTrip) {
  Recorder r;
  EXPECT_FALSE(r.hasTargets());
  EXPECT_EQ(-1, r.hitTest(240, 125));

  r.record(verticalMenu());
  EXPECT_TRUE(r.hasTargets());
  EXPECT_EQ(1, r.hitTest(240, 160 + 25));

  r.invalidate();
  EXPECT_FALSE(r.hasTargets());
  EXPECT_EQ(-1, r.hitTest(240, 160 + 25));
}

// The home screen keeps two live sets at once (covers and menu); they must be independent.
TEST(TapTargets, TheHomeCoverAndMenuSetsAreIndependent) {
  // The cover tile sits above the menu (which verticalMenu() starts at y=100), as it does on
  // the real screen, so each coordinate belongs to exactly one of the two sets.
  Recorder::Builder covers;
  covers.add(0, 0, 480, 90, 0);
  TapTargets::homeCovers().record(covers);
  TapTargets::homeMenu().record(verticalMenu());

  EXPECT_EQ(0, TapTargets::homeCovers().hitTest(240, 45));
  EXPECT_EQ(-1, TapTargets::homeMenu().hitTest(240, 45));
  EXPECT_EQ(1, TapTargets::homeMenu().hitTest(240, 185));
  EXPECT_EQ(-1, TapTargets::homeCovers().hitTest(240, 185));

  TapTargets::homeCovers().invalidate();
  EXPECT_FALSE(TapTargets::homeCovers().hasTargets());
  EXPECT_TRUE(TapTargets::homeMenu().hasTargets());
  TapTargets::homeMenu().invalidate();
}

}  // namespace
