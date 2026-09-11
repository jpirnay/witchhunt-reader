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

// --- Corners ------------------------------------------------------------------
// Consulted only for LONG taps, so the five zones above -- and with them the page-turn
// thirds -- are untouched. See TapZones::cornerFor.

using TapZones::Corner;
using TapZones::cornerFor;

TEST(TapZones, CornersAreSquareAndSizedOffTheShorterEdge) {
  // Square on purpose, and a deliberate divergence from KOReader's w/8 x h/8: on a
  // 540x960 frame that formula gives 67x120, which is a stripe rather than a corner.
  const int side = W / 8;  // W is the shorter edge in portrait
  EXPECT_EQ(Corner::TopLeft, cornerFor(0, 0, W, H));
  EXPECT_EQ(Corner::TopLeft, cornerFor(side - 1, side - 1, W, H));
  EXPECT_EQ(Corner::TopRight, cornerFor(W - 1, 0, W, H));
  EXPECT_EQ(Corner::BottomLeft, cornerFor(0, H - 1, W, H));
  EXPECT_EQ(Corner::BottomRight, cornerFor(W - 1, H - 1, W, H));

  // One pixel past the square on either axis is no longer a corner.
  EXPECT_EQ(Corner::None, cornerFor(side, side - 1, W, H));
  EXPECT_EQ(Corner::None, cornerFor(side - 1, side, W, H));
}

TEST(TapZones, OnlyTheFourSquaresAreCorners) {
  // The mirror of CentreMatchesTheReaderMenuRectangleItReplaces: sweep the frame and
  // assert cornerFor is non-None on exactly the set the geometry describes, so a
  // corner cannot quietly grow into the page-turn zone.
  const int side = W / 8;
  for (int x = 0; x < W; x += 7) {
    for (int y = 0; y < H; y += 11) {
      const bool left = x < side;
      const bool right = x >= W - side;
      const bool top = y < side;
      const bool bottom = y >= H - side;
      const bool inCorner = (left || right) && (top || bottom);
      EXPECT_EQ(inCorner, cornerFor(x, y, W, H) != Corner::None) << "x=" << x << " y=" << y;
    }
  }
}

TEST(TapZones, CornersDoNotDisturbTheTapZones) {
  // The property that makes this change safe to ship: zoneFor must answer for every
  // pixel exactly as it did before corners existed. Recomputed here from the rule
  // rather than from cornerFor, so the two cannot agree by sharing a bug.
  const int zw = W / 3;
  const int zh = H / 3;
  for (int x = 0; x < W; x += 5) {
    for (int y = 0; y < H; y += 7) {
      Zone want;
      if (x < zw) {
        want = Zone::Left;
      } else if (x >= W - zw) {
        want = Zone::Right;
      } else if (y < zh) {
        want = Zone::Top;
      } else if (y >= H - zh) {
        want = Zone::Bottom;
      } else {
        want = Zone::Centre;
      }
      EXPECT_EQ(want, zoneFor(x, y, W, H)) << "x=" << x << " y=" << y;
    }
  }
}

TEST(TapZones, CornersFollowThePageInLandscape) {
  // Sized off the shorter edge, which is H once the frame is rotated.
  const int side = W / 8;  // W is still the shorter of the two values
  EXPECT_EQ(Corner::TopLeft, cornerFor(0, 0, H, W));
  EXPECT_EQ(Corner::BottomRight, cornerFor(H - 1, W - 1, H, W));
  EXPECT_EQ(Corner::None, cornerFor(H / 2, W / 2, H, W));
  EXPECT_EQ(Corner::None, cornerFor(side + 1, W / 2, H, W));
}

TEST(TapZones, DegenerateSizesHaveNoCorners) {
  // side collapses to 0, so every corner test is unreachable and a long press
  // falls through to the five zones -- the same "dull answer, not an undefined
  // one" contract zoneFor keeps.
  EXPECT_EQ(Corner::None, cornerFor(0, 0, 1, 1));
  EXPECT_EQ(Corner::None, cornerFor(0, 0, 7, 7));
  EXPECT_EQ(Corner::None, cornerFor(0, 0, 0, 0));
  EXPECT_EQ(Corner::None, cornerFor(0, 0, -5, -5));
}

TEST(TapZones, CornersOverlapTheZonesTheyAreCarvedFrom) {
  // Not a behaviour assertion so much as a statement of the design: a corner sits
  // INSIDE Left or Right, and the two are resolved by the caller asking cornerFor
  // first for a long press and never for a tap. If this ever stops holding, the
  // page-turn zones have been narrowed and the test above will say so.
  EXPECT_EQ(Zone::Left, zoneFor(0, 0, W, H));
  EXPECT_EQ(Corner::TopLeft, cornerFor(0, 0, W, H));
}

// --- Edge columns (vertical swipe anchoring) ----------------------------------

using TapZones::EdgeColumn;
using TapZones::edgeColumnFor;

TEST(TapZones, EdgeColumnsAreTheOuterQuartersOfTheWidth) {
  const int column = W * 25 / 100;
  const int y = H / 2;  // clear of both bands
  EXPECT_EQ(EdgeColumn::Left, edgeColumnFor(0, y, W, H));
  EXPECT_EQ(EdgeColumn::Left, edgeColumnFor(column - 1, y, W, H));
  EXPECT_EQ(EdgeColumn::None, edgeColumnFor(column, y, W, H));
  EXPECT_EQ(EdgeColumn::None, edgeColumnFor(W - column - 1, y, W, H));
  EXPECT_EQ(EdgeColumn::Right, edgeColumnFor(W - column, y, W, H));
  EXPECT_EQ(EdgeColumn::Right, edgeColumnFor(W - 1, y, W, H));
}

TEST(TapZones, TheBandsAreExcludedFromBothColumns) {
  // The whole reason this helper exists rather than a bare x test: a swipe starting in a
  // bottom corner must NOT be both "open the reader menu" and "brighten the light".
  const int band = H * 14 / 100;
  for (int x = 0; x < W; x += 9) {
    EXPECT_EQ(EdgeColumn::None, edgeColumnFor(x, 0, W, H)) << "x=" << x;
    EXPECT_EQ(EdgeColumn::None, edgeColumnFor(x, band - 1, W, H)) << "x=" << x;
    EXPECT_EQ(EdgeColumn::None, edgeColumnFor(x, H - 1, W, H)) << "x=" << x;
    EXPECT_EQ(EdgeColumn::None, edgeColumnFor(x, H - band, W, H)) << "x=" << x;
  }
  // One pixel inside the band boundary the columns are live again.
  EXPECT_EQ(EdgeColumn::Left, edgeColumnFor(0, band, W, H));
  EXPECT_EQ(EdgeColumn::Right, edgeColumnFor(W - 1, H - band - 1, W, H));
}

TEST(TapZones, MidPageVerticalSwipesBelongToNobody) {
  // The deliberate capability change in re-anchoring: a vertical swipe down the middle of
  // the page used to be brightness (whichever half it fell in) and now means nothing, so a
  // reader resting a thumb mid-page cannot dim the screen by accident.
  for (int y = H * 14 / 100; y < H - H * 14 / 100; y += 23) {
    EXPECT_EQ(EdgeColumn::None, edgeColumnFor(W / 2, y, W, H)) << "y=" << y;
  }
}

TEST(TapZones, EdgeColumnsFollowThePageInLandscape) {
  const int column = H * 25 / 100;  // width is H once rotated
  EXPECT_EQ(EdgeColumn::Left, edgeColumnFor(0, W / 2, H, W));
  EXPECT_EQ(EdgeColumn::Right, edgeColumnFor(H - 1, W / 2, H, W));
  EXPECT_EQ(EdgeColumn::None, edgeColumnFor(column, W / 2, H, W));
}

TEST(TapZones, DegenerateSizesHaveNoEdgeColumns) {
  EXPECT_EQ(EdgeColumn::None, edgeColumnFor(0, 0, 0, 0));
  EXPECT_EQ(EdgeColumn::None, edgeColumnFor(0, 0, -5, -5));
  // A tiny frame collapses the band to 0, so y is never excluded, but the column test still
  // has to answer rather than misbehave.
  EXPECT_NE(EdgeColumn::Right, edgeColumnFor(0, 0, 1, 1));
}

}  // namespace

// --- Bounded band split -------------------------------------------------------
// The paged text views that are not the reading surface (a book description, a dictionary
// entry) turn pages by tapping the halves of the text itself. Bounded, because those screens
// draw a button-hint strip that must keep receiving its own taps.

TEST(TapZones, BandSplitsIntoTwoHalves) {
  // A 300x400 band at (40, 100): x < 190 is back, x >= 190 is forward.
  EXPECT_EQ(TapZones::halfOfBand(40, 100, 40, 100, 300, 400), TapZones::Half::Previous);
  EXPECT_EQ(TapZones::halfOfBand(189, 300, 40, 100, 300, 400), TapZones::Half::Previous);
  EXPECT_EQ(TapZones::halfOfBand(190, 300, 40, 100, 300, 400), TapZones::Half::Next);
  EXPECT_EQ(TapZones::halfOfBand(339, 499, 40, 100, 300, 400), TapZones::Half::Next);
}

// The regression this bounding exists to prevent: a tap below the band is the hint strip's, and
// must not be read as a page turn.
TEST(TapZones, OutsideTheBandIsNobodys) {
  EXPECT_EQ(TapZones::halfOfBand(200, 501, 40, 100, 300, 400), TapZones::Half::None);  // below
  EXPECT_EQ(TapZones::halfOfBand(200, 99, 40, 100, 300, 400), TapZones::Half::None);   // above
  EXPECT_EQ(TapZones::halfOfBand(39, 300, 40, 100, 300, 400), TapZones::Half::None);   // left of
  EXPECT_EQ(TapZones::halfOfBand(340, 300, 40, 100, 300, 400), TapZones::Half::None);  // right of
}

// A band that was never laid out answers nothing rather than dividing by zero or claiming the
// whole screen.
TEST(TapZones, DegenerateBandIsInert) {
  EXPECT_EQ(TapZones::halfOfBand(10, 10, 0, 0, 0, 400), TapZones::Half::None);
  EXPECT_EQ(TapZones::halfOfBand(10, 10, 0, 0, 300, 0), TapZones::Half::None);
  EXPECT_EQ(TapZones::halfOfBand(10, 10, 0, 0, -5, -5), TapZones::Half::None);
}

// A one-pixel band still resolves, and to the forward half: `px < x + width / 2` with width 1 is
// `px < x`, which the bounds check already excluded.
TEST(TapZones, SinglePixelBandGoesForward) {
  EXPECT_EQ(TapZones::halfOfBand(50, 50, 50, 50, 1, 1), TapZones::Half::Next);
}
