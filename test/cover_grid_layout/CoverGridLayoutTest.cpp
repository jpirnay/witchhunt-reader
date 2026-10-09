// Cover-grid geometry: columns and rows are derived from the panel, never hard-coded, so a
// higher-resolution device gets more cells rather than a few oversized ones. Pure arithmetic —
// the theme metrics below are the real ones, restated here so the expectations are readable.

#include <gtest/gtest.h>

#include "components/CoverGridLayout.h"

namespace {

// Cover box ceilings (CoverGridLayout::kMaxCellHeight/Width): the full-size 196x240 thumbnail plus its
// 1 px frame.
constexpr int kMaxCell = 242;
constexpr int kMaxCellWidth = 198;

// Content area left to the grid, computed the way FileBrowserActivity::coverGrid does it:
//   width  = panel width - side button hints
//   height = panel height - button hints - (topPadding + header + spacing) - spacing
struct Theme {
  int topPadding, header, spacing, buttonHints, sideHints;
};
constexpr Theme kClassic{5, 45, 10, 40, 30};
constexpr Theme kLyra{5, 84, 16, 40, 30};  // the default theme, and the one with the tall header
// Lyra at the Large UI font size (UiFontLadder): the header grows by title + small (+5 +2), the
// button hints by a body line (+5).
constexpr Theme kLyraLarge{5, 91, 16, 45, 30};

// The Library's tab bar (ListTabBar::HEIGHT), between the header and the grid.
constexpr int kTabBar = 54;

CoverGridLayout::Input portrait(int panelW, int panelH, const Theme& t, bool isX3, int tabBar = 0) {
  const int contentWidth = panelW - (isX3 ? 2 * t.sideHints : t.sideHints);
  const int contentTop = t.topPadding + t.header + t.spacing + tabBar;
  const int contentHeight = (panelH - t.buttonHints) - contentTop - t.spacing;
  return {contentWidth, contentHeight, 12, kMaxCell, kMaxCellWidth};
}

}  // namespace

TEST(CoverGridLayout, X4PortraitIsTwoByTwoWithFullSizeCells) {
  for (const auto& theme : {kClassic, kLyra, kLyraLarge}) {
    const auto l = CoverGridLayout::compute(portrait(480, 800, theme, /*isX3=*/false));
    EXPECT_EQ(l.cols, 2);
    EXPECT_EQ(l.rows, 2);
    EXPECT_EQ(l.cellWidth, 210);
    EXPECT_EQ(l.cellHeight, kMaxCell);  // stored thumb draws 1:1, never resampled
    EXPECT_EQ(l.thumbWidth, 196);       // the full-size thumbnail: the covers already on the card
    EXPECT_EQ(l.thumbHeight, 240);
  }
}

TEST(CoverGridLayout, X3PortraitIsTwoByTwoWithFullSizeCells) {
  for (const auto& theme : {kClassic, kLyra, kLyraLarge}) {
    const auto l = CoverGridLayout::compute(portrait(528, 792, theme, /*isX3=*/true));
    EXPECT_EQ(l.cols, 2);
    EXPECT_EQ(l.rows, 2);
    EXPECT_EQ(l.cellWidth, 219);
    EXPECT_EQ(l.cellHeight, kMaxCell);
    EXPECT_EQ(l.thumbWidth, 196);
    EXPECT_EQ(l.thumbHeight, 240);
  }
}

// The LilyGo T5S3's 540x960 panel: two full-size rows left 211 px blank, more than half a row, so it
// takes a third row of slightly smaller cells -- and at that cover width, a third column.
TEST(CoverGridLayout, T5S3PortraitIsThreeByThreeWithShrunkCells) {
  const auto l = CoverGridLayout::compute(portrait(540, 960, kLyra, /*isX3=*/false));
  EXPECT_EQ(l.cols, 3);
  EXPECT_EQ(l.rows, 3);
  EXPECT_EQ(l.cellWidth, 156);
  EXPECT_EQ(l.cellHeight, 216);
  EXPECT_EQ(l.thumbWidth, 154);  // the thumbnail is made at the cell's size, so it still draws 1:1
  EXPECT_EQ(l.thumbHeight, 214);
}

// The Large UI font takes 12 px from the grid; on a panel whose cells are already shrunk, that is
// a smaller thumbnail.
TEST(CoverGridLayout, T5S3AtTheLargeUiFontShrinksTheCellsAgain) {
  const auto l = CoverGridLayout::compute(portrait(540, 960, kLyraLarge, /*isX3=*/false));
  EXPECT_EQ(l.cols, 3);
  EXPECT_EQ(l.rows, 3);
  EXPECT_EQ(l.thumbWidth, 154);
  EXPECT_EQ(l.thumbHeight, 210);
}

TEST(CoverGridLayout, LessThanHalfARowLeftKeepsFullSizeCells) {
  constexpr int stride = kMaxCell + CoverGridLayout::kLabelHeight + CoverGridLayout::kMargin;
  const auto l = CoverGridLayout::compute({.contentWidth = 450,
                                           .contentHeight = 2 * stride + stride / 2 - 1,
                                           .bottomReserve = 0,
                                           .maxCellHeight = kMaxCell,
                                           .maxCellWidth = kMaxCellWidth});
  EXPECT_EQ(l.rows, 2);
  EXPECT_EQ(l.cellHeight, kMaxCell);
}

TEST(CoverGridLayout, HalfARowLeftTakesAnotherRow) {
  constexpr int stride = kMaxCell + CoverGridLayout::kLabelHeight + CoverGridLayout::kMargin;
  const auto l = CoverGridLayout::compute({.contentWidth = 450,
                                           .contentHeight = 2 * stride + stride / 2,
                                           .bottomReserve = 0,
                                           .maxCellHeight = kMaxCell,
                                           .maxCellWidth = kMaxCellWidth});
  EXPECT_EQ(l.rows, 3);
  EXPECT_LT(l.cellHeight, kMaxCell);
  EXPECT_LE(l.rows * l.rowStride, 2 * stride + stride / 2);
}

TEST(CoverGridLayout, ThumbnailAlwaysFitsItsCell) {
  for (const auto& theme : {kClassic, kLyra}) {
    for (int panelH = 500; panelH <= 1600; panelH += 13) {
      for (int panelW = 300; panelW <= 1200; panelW += 37) {
        const auto l = CoverGridLayout::compute(portrait(panelW, panelH, theme, /*isX3=*/false));
        EXPECT_LE(l.thumbWidth, l.cellWidth - 2) << panelW << "x" << panelH;
        EXPECT_LE(l.thumbHeight, l.cellHeight - 2) << panelW << "x" << panelH;
        EXPECT_LE(l.thumbWidth, kMaxCellWidth - 2);
        EXPECT_LE(l.thumbHeight, kMaxCell - 2);
      }
    }
  }
}

TEST(CoverGridLayout, EveryPageFitsTheContentArea) {
  for (const auto& theme : {kClassic, kLyra}) {
    for (const auto& in : {portrait(480, 800, theme, false), portrait(528, 792, theme, true)}) {
      const auto l = CoverGridLayout::compute(in);
      EXPECT_LE(l.rows * l.rowStride, in.contentHeight - in.bottomReserve);
      EXPECT_LE((l.cols + 1) * CoverGridLayout::kMargin + l.cols * l.cellWidth, in.contentWidth);
    }
  }
}

// The height a page leaves below its last row goes to the rows equally, and where each row's share
// is a label line, the label takes a third line: on the X4's Classic theme under the tab bar, 48 px
// spare, 24 a row.
TEST(CoverGridLayout, SpareHeightGoesToTheRowsAndMakesRoomForAThirdLabelLine) {
  const auto in = portrait(480, 800, kClassic, /*isX3=*/false, kTabBar);
  const auto l = CoverGridLayout::compute(in);
  const int usable = in.contentHeight - in.bottomReserve;
  EXPECT_EQ(l.rows, 2);
  EXPECT_EQ(l.cellHeight, kMaxCell);  // the covers are the same size: only the rows spread
  EXPECT_EQ(l.labelLines, 3);
  EXPECT_EQ(l.labelHeight, CoverGridLayout::kLabelHeight + CoverGridLayout::kLabelLineHeight);
  EXPECT_EQ(l.rowStride, usable / l.rows);
  EXPECT_LE(l.cellHeight + l.labelHeight + CoverGridLayout::kMargin, l.rowStride);
}

// Less than a line to spare per row -- Lyra's taller header under the tab bar leaves none -- keeps
// two label lines, as before.
TEST(CoverGridLayout, WithoutALineToSpareTheLabelKeepsTwoLines) {
  const auto in = portrait(480, 800, kLyra, /*isX3=*/false, kTabBar);
  const auto l = CoverGridLayout::compute(in);
  EXPECT_EQ(l.labelLines, 2);
  EXPECT_EQ(l.labelHeight, CoverGridLayout::kLabelHeight);
  EXPECT_EQ(l.rowStride, (in.contentHeight - in.bottomReserve) / l.rows);
}

// Every device and theme: what the label takes plus the cover always fits the row it is in.
TEST(CoverGridLayout, TheLabelAlwaysFitsItsRow) {
  for (const auto& theme : {kClassic, kLyra, kLyraLarge}) {
    for (const int bar : {0, kTabBar}) {
      for (const auto& in : {portrait(480, 800, theme, false, bar), portrait(528, 792, theme, true, bar),
                             portrait(540, 960, theme, false, bar)}) {
        const auto l = CoverGridLayout::compute(in);
        EXPECT_GE(l.labelLines, 2);
        EXPECT_LE(l.labelLines, 3);
        EXPECT_LE(l.cellHeight + l.labelHeight + CoverGridLayout::kMargin, l.rowStride);
        EXPECT_LE(l.rows * l.rowStride, in.contentHeight - in.bottomReserve);
      }
    }
  }
}

TEST(CoverGridLayout, HigherResolutionPanelGetsMoreCellsNotBiggerOnes) {
  // A 1072x1448 300 dpi panel: nothing changes but the numbers handed in.
  const auto l = CoverGridLayout::compute(portrait(1072, 1448, kLyra, /*isX3=*/false));
  EXPECT_EQ(l.cols, 6);
  EXPECT_EQ(l.rows, 4);
  EXPECT_EQ(l.cellHeight, kMaxCell);  // still capped by the full-size thumbnail
}

// A column holds a 2:3 cover at the cell's height -- the usual shape, and what a fitted one fills.
TEST(CoverGridLayout, EveryColumnHoldsAPortraitCover) {
  for (int panelW = 300; panelW <= 2000; panelW += 7) {
    const auto l = CoverGridLayout::compute(portrait(panelW, 1000, kClassic, /*isX3=*/false));
    EXPECT_GE(l.cellWidth, (l.cellHeight - 2) * 2 / 3 + 2) << "panel width " << panelW;
  }
}

TEST(CoverGridLayout, NarrowPanelStillYieldsOneColumn) {
  const auto l = CoverGridLayout::compute(
      {.contentWidth = 150, .contentHeight = 600, .bottomReserve = 24, .maxCellHeight = kMaxCell});
  EXPECT_EQ(l.cols, 1);
  EXPECT_EQ(l.cellWidth, 130);  // squeezed rather than nothing at all
}

TEST(CoverGridLayout, ShortPanelKeepsOneRowAndClampsTheCell) {
  // Not even one full-size row fits: one row survives, shrunk but never below the floor.
  const auto l = CoverGridLayout::compute(
      {.contentWidth = 450, .contentHeight = 200, .bottomReserve = 24, .maxCellHeight = kMaxCell});
  EXPECT_EQ(l.rows, 1);
  EXPECT_EQ(l.cellHeight, 130);
  EXPECT_GE(l.cellHeight, CoverGridLayout::kMinCellHeight);
}

TEST(CoverGridLayout, DegenerateInputsDoNotProduceNonsense) {
  const auto l = CoverGridLayout::compute({0, 0, 0, 0});
  EXPECT_EQ(l.cols, 1);
  EXPECT_EQ(l.rows, 1);
  EXPECT_GE(l.cellWidth, 1);
  EXPECT_EQ(l.cellHeight, CoverGridLayout::kMinCellHeight);
}

// --- hitTest ------------------------------------------------------------------------------
//
// The inverse of the cell placement FileBrowserActivity::drawCoverGrid does:
//   cx = originX + kMargin + col * (cellWidth + kMargin)
//   cy = originY + (row - pageStartRow) * rowStride
// Restated here so a change to either side fails loudly rather than silently mis-aiming taps.

namespace {

constexpr int kOriginX = 0;
constexpr int kOriginY = 100;

// The real X4 portrait grid: 2 columns, full-size cells.
CoverGridLayout::Layout x4Grid() { return CoverGridLayout::compute(portrait(480, 800, kLyra, /*isX3=*/false)); }

// Centre of the cell at (row, col) on the current page, in the frame hitTest expects.
void cellCentre(const CoverGridLayout::Layout& l, int row, int col, int& x, int& y) {
  x = kOriginX + CoverGridLayout::kMargin + col * (l.cellWidth + CoverGridLayout::kMargin) + l.cellWidth / 2;
  y = kOriginY + row * l.rowStride + (l.cellHeight + l.labelHeight) / 2;
}

}  // namespace

TEST(CoverGridLayoutHitTest, HitsEveryCellOfTheFirstPageAtItsCentre) {
  const auto l = x4Grid();
  const int count = l.cols * l.rows;
  for (int row = 0; row < l.rows; ++row) {
    for (int col = 0; col < l.cols; ++col) {
      int x = 0;
      int y = 0;
      cellCentre(l, row, col, x, y);
      EXPECT_EQ(row * l.cols + col, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, y))
          << "row " << row << " col " << col;
    }
  }
}

TEST(CoverGridLayoutHitTest, ResolvesThroughThePageOffset) {
  const auto l = x4Grid();
  const int perPage = l.cols * l.rows;
  int x = 0;
  int y = 0;
  cellCentre(l, 0, 0, x, y);
  // Second page: the top-left cell on screen is the first item of that page.
  EXPECT_EQ(l.rows * l.cols, CoverGridLayout::hitTest(l, kOriginX, kOriginY, l.rows, perPage * 3, x, y));
}

TEST(CoverGridLayoutHitTest, TheGutterBetweenCoversIsAMiss) {
  const auto l = x4Grid();
  const int count = l.cols * l.rows;
  ASSERT_GE(l.cols, 2);
  int x = 0;
  int y = 0;
  cellCentre(l, 0, 0, x, y);
  // Just past the right edge of column 0, inside the margin before column 1.
  const int gutterX = kOriginX + CoverGridLayout::kMargin + l.cellWidth;
  EXPECT_EQ(-1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, gutterX, y));
  EXPECT_EQ(0, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, gutterX - 1, y));
  // And the leading margin before column 0.
  EXPECT_EQ(-1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, kOriginX, y));
}

TEST(CoverGridLayoutHitTest, TheRowGutterBelowTheLabelIsAMiss) {
  const auto l = x4Grid();
  const int count = l.cols * l.rows;
  int x = 0;
  int y = 0;
  cellCentre(l, 0, 0, x, y);
  const int cellBottom = kOriginY + l.cellHeight + l.labelHeight;
  EXPECT_EQ(0, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, cellBottom - 1));
  EXPECT_EQ(-1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, cellBottom));
}

// The last row of a library is usually partial: the empty cells beside the final cover must not
// resolve to an item that does not exist.
TEST(CoverGridLayoutHitTest, EmptyCellsInAPartialLastRowMiss) {
  const auto l = x4Grid();
  ASSERT_GE(l.cols, 2);
  const int count = l.cols * (l.rows - 1) + 1;  // one book alone on the last row
  int x = 0;
  int y = 0;
  cellCentre(l, l.rows - 1, 0, x, y);
  EXPECT_EQ(count - 1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, y));
  cellCentre(l, l.rows - 1, 1, x, y);
  EXPECT_EQ(-1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, y));
}

TEST(CoverGridLayoutHitTest, MissesAboveTheGridAndBelowTheLastRow) {
  const auto l = x4Grid();
  const int count = l.cols * l.rows;
  int x = 0;
  int y = 0;
  cellCentre(l, 0, 0, x, y);
  EXPECT_EQ(-1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, kOriginY - 1));
  EXPECT_EQ(-1, CoverGridLayout::hitTest(l, kOriginX, kOriginY, 0, count, x, kOriginY + l.rows * l.rowStride));
}

// Row steps in the Recent Books grid wrap like every list does (it used to stop dead at both ends).
TEST(CoverGridLayout, RowStepsMoveByAWholeRow) {
  EXPECT_EQ(CoverGridLayout::rowBelow(1, 10, 2), 3);
  EXPECT_EQ(CoverGridLayout::rowAbove(5, 10, 3), 2);
}

TEST(CoverGridLayout, DownFromTheLastRowWrapsToTheSameColumnOfTheFirst) {
  EXPECT_EQ(CoverGridLayout::rowBelow(8, 10, 2), 0);
  EXPECT_EQ(CoverGridLayout::rowBelow(9, 10, 2), 1);
  EXPECT_EQ(CoverGridLayout::rowBelow(7, 8, 3), 1);  // partial last row: its own column, from the top
}

TEST(CoverGridLayout, UpFromTheFirstRowWrapsToTheLastRowThatHasTheColumn) {
  EXPECT_EQ(CoverGridLayout::rowAbove(0, 10, 2), 8);
  EXPECT_EQ(CoverGridLayout::rowAbove(1, 10, 2), 9);
  // 8 items in rows of 3: the last row holds 6 and 7, so column 2 ends a row higher, on 5.
  EXPECT_EQ(CoverGridLayout::rowAbove(2, 8, 3), 5);
  EXPECT_EQ(CoverGridLayout::rowAbove(1, 8, 3), 7);
}

TEST(CoverGridLayout, DownIntoAPartialLastRowLandsOnItsLastCell) {
  // 8 items in rows of 3: from 5 (column 2) there is nothing directly below.
  EXPECT_EQ(CoverGridLayout::rowBelow(5, 8, 3), 7);
}

TEST(CoverGridLayout, RowStepsInASingleRowStayPut) {
  EXPECT_EQ(CoverGridLayout::rowBelow(1, 2, 3), 1);
  EXPECT_EQ(CoverGridLayout::rowAbove(1, 2, 3), 1);
}

TEST(CoverGridLayout, RowStepsOnAnEmptyGridAreHarmless) {
  EXPECT_EQ(CoverGridLayout::rowBelow(0, 0, 3), 0);
  EXPECT_EQ(CoverGridLayout::rowAbove(0, 0, 0), 0);
}
