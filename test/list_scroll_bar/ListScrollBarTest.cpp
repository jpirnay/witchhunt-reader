#include <gtest/gtest.h>

#include "components/themes/ListScrollBar.h"

namespace {

using ListScrollBar::Bar;
using ListScrollBar::fromPaintedBar;
using ListScrollBar::Hit;
using ListScrollBar::hitTestIn;
using ListScrollBar::kMinHitWidth;

// A list occupying x in [20, 520) with the painted bar at its right edge, matching
// BaseTheme's scrollBarWidth = 4 / scrollBarRightOffset = 5 on a 540 px portrait frame.
constexpr int kListLeft = 20;
constexpr int kPaintedWidth = 4;
constexpr int kPaintedX = 511;  // 520 - 5 - 4
constexpr int kTop = 100;
constexpr int kHeight = 600;

Bar makeBar(const int thumbTop, const int thumbHeight) {
  return fromPaintedBar(kPaintedX, kPaintedWidth, kListLeft, kTop, kHeight, thumbTop, thumbHeight);
}

TEST(ListScrollBar, TheHitStripIsWiderThanThePaintedBar) {
  // The whole point: 4 px is not a touch target. The strip reaches LEFT from the bar's
  // right edge, so the painted bar stays visually where it is.
  const Bar bar = makeBar(kTop, 60);
  EXPECT_EQ(kMinHitWidth, bar.hitWidth);
  EXPECT_EQ(kPaintedX + kPaintedWidth - kMinHitWidth, bar.hitX);
  EXPECT_LT(bar.hitX, kPaintedX) << "the strip must extend inwards past the painted bar";
}

TEST(ListScrollBar, AboveTheThumbPagesBackAndBelowPagesForward) {
  const Bar bar = makeBar(300, 80);
  const int x = kPaintedX;  // on the painted bar itself
  EXPECT_EQ(Hit::PageBack, hitTestIn(bar, x, kTop));
  EXPECT_EQ(Hit::PageBack, hitTestIn(bar, x, 299));
  EXPECT_EQ(Hit::PageForward, hitTestIn(bar, x, 380));
  EXPECT_EQ(Hit::PageForward, hitTestIn(bar, x, kTop + kHeight - 1));
}

TEST(ListScrollBar, TheThumbItselfIsNeitherDirection) {
  // Neither back nor forward, and on e-paper there is no drag to begin -- so a tap on the
  // thumb must do nothing rather than pick a direction arbitrarily.
  const Bar bar = makeBar(300, 80);
  for (int y = 300; y < 380; y += 7) {
    EXPECT_EQ(Hit::None, hitTestIn(bar, kPaintedX, y)) << "y=" << y;
  }
}

TEST(ListScrollBar, TapsAnywhereInTheStripWidthCount) {
  // Forgiving horizontally, which is the reason the strip exists.
  const Bar bar = makeBar(300, 80);
  for (int x = bar.hitX; x < bar.hitX + bar.hitWidth; x += 3) {
    EXPECT_EQ(Hit::PageBack, hitTestIn(bar, x, 200)) << "x=" << x;
  }
}

TEST(ListScrollBar, OutsideTheStripIsNobodys) {
  const Bar bar = makeBar(300, 80);
  EXPECT_EQ(Hit::None, hitTestIn(bar, bar.hitX - 1, 200));             // left of the strip
  EXPECT_EQ(Hit::None, hitTestIn(bar, bar.hitX + bar.hitWidth, 200));  // right of it
  EXPECT_EQ(Hit::None, hitTestIn(bar, kPaintedX, kTop - 1));           // above the track
  EXPECT_EQ(Hit::None, hitTestIn(bar, kPaintedX, kTop + kHeight));     // below the track
}

TEST(ListScrollBar, TheStripNeverEscapesTheList) {
  // A narrow list must not get a strip wider than itself, or the bar would start claiming
  // taps in whatever is drawn to its left.
  const Bar bar = fromPaintedBar(/*paintedX=*/30, kPaintedWidth, /*listLeft=*/28, kTop, kHeight, kTop, 50);
  EXPECT_EQ(28, bar.hitX);
  EXPECT_EQ(30 + kPaintedWidth - 28, bar.hitWidth);
  EXPECT_EQ(Hit::None, hitTestIn(bar, 27, 200));
}

TEST(ListScrollBar, AnUnrecordedBarAnswersNothing) {
  // A screen that painted no bar leaves this zeroed, and every tap must miss.
  const Bar bar{};
  EXPECT_EQ(Hit::None, hitTestIn(bar, 0, 0));
  EXPECT_EQ(Hit::None, hitTestIn(bar, 500, 300));
}

TEST(ListScrollBar, AThumblessBarStillSplits) {
  // thumbHeight 0 means the draw reported no thumb; the track still has to answer rather
  // than treat every tap as a hit on a zero-height thumb.
  const Bar bar = makeBar(/*thumbTop=*/400, /*thumbHeight=*/0);
  EXPECT_EQ(Hit::PageBack, hitTestIn(bar, kPaintedX, 399));
  EXPECT_EQ(Hit::PageForward, hitTestIn(bar, kPaintedX, 400));
}

}  // namespace
