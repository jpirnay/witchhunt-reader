// Host tests for the panel-native -> logical touch transform.
//
// This is the phase-2 gate from docs/touch-input-migration-2026-08-14.md: the
// orientation transform is pure logic and must be verifiable without hardware.
// Getting a rotation branch wrong is the classic touch bug — taps land mirrored
// or on the wrong axis — and it is exactly the kind of thing that is painful to
// diagnose on device and trivial to catch here.
//
// Panel geometry throughout is the X4 Pro's: 800x480 native (landscape), which
// the reader presents as 480x800 in portrait.

#include <TouchTransform.h>
#include <gtest/gtest.h>

namespace {

constexpr int kPanelW = 800;  // native landscape width
constexpr int kPanelH = 480;  // native landscape height

struct Pt {
  int x;
  int y;
};

Pt map(const int orientation, const float nx, const float ny) {
  Pt p{};
  touchtransform::tapToLogical(orientation, kPanelW, kPanelH, nx, ny, p.x, p.y);
  return p;
}

// LandscapeCounterClockwise is the panel's native frame, so it is the identity
// mapping and the easiest anchor for the other three.
TEST(TouchTransform, LandscapeCcwIsIdentity) {
  const Pt origin = map(touchtransform::LandscapeCounterClockwise, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, 0);
  EXPECT_EQ(origin.y, 0);

  const Pt mid = map(touchtransform::LandscapeCounterClockwise, 0.5f, 0.5f);
  EXPECT_EQ(mid.x, kPanelW / 2);
  EXPECT_EQ(mid.y, kPanelH / 2);

  const Pt far = map(touchtransform::LandscapeCounterClockwise, 1.0f, 1.0f);
  EXPECT_EQ(far.x, kPanelW - 1);
  EXPECT_EQ(far.y, kPanelH - 1);
}

// Portrait renders 480x800: the logical X axis runs along the panel's height
// and is reversed, the logical Y axis runs along the panel's width.
TEST(TouchTransform, PortraitRotatesAndMirrorsX) {
  const Pt origin = map(touchtransform::Portrait, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, kPanelH - 1);
  EXPECT_EQ(origin.y, 0);

  const Pt far = map(touchtransform::Portrait, 1.0f, 1.0f);
  EXPECT_EQ(far.x, 0);
  EXPECT_EQ(far.y, kPanelW - 1);
}

TEST(TouchTransform, PortraitInvertedIsPortraitTurnedAround) {
  const Pt origin = map(touchtransform::PortraitInverted, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, 0);
  EXPECT_EQ(origin.y, kPanelW - 1);

  const Pt far = map(touchtransform::PortraitInverted, 1.0f, 1.0f);
  EXPECT_EQ(far.x, kPanelH - 1);
  EXPECT_EQ(far.y, 0);
}

TEST(TouchTransform, LandscapeClockwiseMirrorsBothAxes) {
  const Pt origin = map(touchtransform::LandscapeClockwise, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, kPanelW - 1);
  EXPECT_EQ(origin.y, kPanelH - 1);

  const Pt far = map(touchtransform::LandscapeClockwise, 1.0f, 1.0f);
  EXPECT_EQ(far.x, 0);
  EXPECT_EQ(far.y, 0);
}

// Every orientation must land inside its own logical screen for any input in
// 0..1 -- the property that actually matters to callers doing hit tests.
TEST(TouchTransform, AllOrientationsStayOnScreen) {
  const int orientations[] = {
      touchtransform::Portrait,
      touchtransform::LandscapeClockwise,
      touchtransform::PortraitInverted,
      touchtransform::LandscapeCounterClockwise,
  };
  for (const int orientation : orientations) {
    // Portrait and PortraitInverted present the panel rotated, so their logical
    // extents are the panel's dimensions swapped.
    const bool portrait = orientation == touchtransform::Portrait || orientation == touchtransform::PortraitInverted;
    const int logicalW = portrait ? kPanelH : kPanelW;
    const int logicalH = portrait ? kPanelW : kPanelH;
    for (float nx = 0.0f; nx <= 1.0f; nx += 0.05f) {
      for (float ny = 0.0f; ny <= 1.0f; ny += 0.05f) {
        const Pt p = map(orientation, nx, ny);
        EXPECT_GE(p.x, 0) << "orientation " << orientation;
        EXPECT_LT(p.x, logicalW) << "orientation " << orientation;
        EXPECT_GE(p.y, 0) << "orientation " << orientation;
        EXPECT_LT(p.y, logicalH) << "orientation " << orientation;
      }
    }
  }
}

// A controller reporting slightly out of range (or exactly 1.0, which would
// otherwise index one pixel past the edge) must be clamped, not wrapped.
TEST(TouchTransform, OutOfRangeInputIsClamped) {
  const Pt low = map(touchtransform::LandscapeCounterClockwise, -0.4f, -1.0f);
  EXPECT_EQ(low.x, 0);
  EXPECT_EQ(low.y, 0);

  const Pt high = map(touchtransform::LandscapeCounterClockwise, 1.7f, 2.5f);
  EXPECT_EQ(high.x, kPanelW - 1);
  EXPECT_EQ(high.y, kPanelH - 1);

  // Clamping happens before rotation, so it holds in a rotated frame too.
  const Pt rotated = map(touchtransform::Portrait, 1.7f, 2.5f);
  EXPECT_EQ(rotated.x, 0);
  EXPECT_EQ(rotated.y, kPanelW - 1);
}

// Opposite orientations must disagree, or a mis-wired transform that ignored
// the orientation argument entirely would still pass everything above.
TEST(TouchTransform, OppositeOrientationsDiffer) {
  const Pt ccw = map(touchtransform::LandscapeCounterClockwise, 0.25f, 0.75f);
  const Pt cw = map(touchtransform::LandscapeClockwise, 0.25f, 0.75f);
  EXPECT_NE(ccw.x, cw.x);
  EXPECT_NE(ccw.y, cw.y);

  const Pt portrait = map(touchtransform::Portrait, 0.25f, 0.75f);
  const Pt inverted = map(touchtransform::PortraitInverted, 0.25f, 0.75f);
  EXPECT_NE(portrait.x, inverted.x);
  EXPECT_NE(portrait.y, inverted.y);
}

// --- Band hit-test (rowTouch / colTouch arithmetic) --------------------------
// The second half of the phase-2 gate. rowTouch and colTouch are the same test
// on perpendicular axes, so these cover both.

namespace {
// Row-shaped call: band along y, bounded on x.
bool rowHit(const int x, const int y, const int top, const int rowStep, const int rowCount, int& row,
            const int xStart = 0, const int xEnd = 480, const int rowHeight = 0) {
  return touchtransform::bandHit(y, x, top, rowStep, rowCount, xStart, xEnd, rowHeight, row);
}
}  // namespace

TEST(BandHit, SelectsTheRowContainingThePoint) {
  int row = -1;
  EXPECT_TRUE(rowHit(100, 200, 200, 40, 5, row));
  EXPECT_EQ(row, 0);

  EXPECT_TRUE(rowHit(100, 239, 200, 40, 5, row));
  EXPECT_EQ(row, 0) << "last pixel of row 0 still belongs to row 0";

  EXPECT_TRUE(rowHit(100, 240, 200, 40, 5, row));
  EXPECT_EQ(row, 1) << "first pixel of row 1";

  EXPECT_TRUE(rowHit(100, 399, 200, 40, 5, row));
  EXPECT_EQ(row, 4) << "last pixel of the last row";
}

TEST(BandHit, RejectsAboveTheBandAndPastTheLastRow) {
  int row = -1;
  EXPECT_FALSE(rowHit(100, 199, 200, 40, 5, row)) << "one pixel above the first row";
  EXPECT_FALSE(rowHit(100, 400, 200, 40, 5, row)) << "one pixel past the last row";
  EXPECT_FALSE(rowHit(100, 10000, 200, 40, 5, row));
}

TEST(BandHit, RejectsOutsideTheCrossAxisBounds) {
  int row = -1;
  EXPECT_TRUE(rowHit(40, 210, 200, 40, 5, row, 40, 300));
  EXPECT_FALSE(rowHit(39, 210, 200, 40, 5, row, 40, 300)) << "xStart is inclusive";
  EXPECT_FALSE(rowHit(300, 210, 200, 40, 5, row, 40, 300)) << "xEnd is exclusive";
  EXPECT_TRUE(rowHit(299, 210, 200, 40, 5, row, 40, 300));
}

// A non-zero extent is what stops a tap in the gap between rows from selecting
// the row above it -- the reason the parameter exists.
TEST(BandHit, ExtentExcludesTheGapBetweenRows) {
  int row = -1;
  // 40px pitch, 30px of drawn row, 10px gap.
  EXPECT_TRUE(rowHit(100, 229, 200, 40, 5, row, 0, 480, 30));
  EXPECT_EQ(row, 0);
  EXPECT_FALSE(rowHit(100, 230, 200, 40, 5, row, 0, 480, 30)) << "first gap pixel selects nothing";
  EXPECT_FALSE(rowHit(100, 239, 200, 40, 5, row, 0, 480, 30)) << "last gap pixel selects nothing";
  EXPECT_TRUE(rowHit(100, 240, 200, 40, 5, row, 0, 480, 30)) << "next row starts again";
  EXPECT_EQ(row, 1);
}

TEST(BandHit, ZeroExtentMeansTheWholeStepIsLive) {
  int row = -1;
  for (int y = 200; y < 240; ++y) {
    EXPECT_TRUE(rowHit(100, y, 200, 40, 5, row)) << "y=" << y;
    EXPECT_EQ(row, 0) << "y=" << y;
  }
}

TEST(BandHit, DegenerateGeometryIsRejected) {
  int row = -1;
  EXPECT_FALSE(rowHit(100, 210, 200, 0, 5, row)) << "zero step";
  EXPECT_FALSE(rowHit(100, 210, 200, -40, 5, row)) << "negative step";
  EXPECT_FALSE(rowHit(100, 210, 200, 40, 0, row)) << "no rows";
  EXPECT_FALSE(rowHit(100, 210, 200, 40, -1, row)) << "negative count";
}

TEST(BandHit, LeavesIndexUntouchedOnMiss) {
  int row = 7;
  EXPECT_FALSE(rowHit(100, 199, 200, 40, 5, row));
  EXPECT_EQ(row, 7) << "a miss must not clobber the caller's selection";
}

// Columns use the identical helper with the axes swapped; verify that mapping
// so colTouch is covered by the same arithmetic.
TEST(BandHit, ColumnShapedCallBandsAlongX) {
  int col = -1;
  // Two 120px-wide buttons starting at x=60, live between y=400 and y=460.
  const auto colHit = [&](const int x, const int y) {
    return touchtransform::bandHit(x, y, 60, 120, 2, 400, 460, 0, col);
  };
  EXPECT_TRUE(colHit(60, 400));
  EXPECT_EQ(col, 0);
  EXPECT_TRUE(colHit(179, 459));
  EXPECT_EQ(col, 0);
  EXPECT_TRUE(colHit(180, 430));
  EXPECT_EQ(col, 1);
  EXPECT_FALSE(colHit(300, 430)) << "past the last column";
  EXPECT_FALSE(colHit(100, 399)) << "above the live band";
  EXPECT_FALSE(colHit(100, 460)) << "below the live band";
}

}  // namespace
