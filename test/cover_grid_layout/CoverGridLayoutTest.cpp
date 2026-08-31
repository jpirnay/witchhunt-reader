// Cover-grid geometry: columns and rows are derived from the panel, never hard-coded, so a
// higher-resolution device gets more cells rather than a few oversized ones. Pure arithmetic —
// the theme metrics below are the real ones, restated here so the expectations are readable.

#include <gtest/gtest.h>

#include "components/CoverGridLayout.h"

namespace {

// Cover box ceiling used by RecentBooksActivity: the stored 220x240 thumbnail plus its 1 px frame.
constexpr int kMaxCell = 242;

// Content area left to the grid, computed the way RecentBooksActivity does it:
//   width  = panel width - side button hints
//   height = panel height - button hints - (topPadding + header + spacing) - spacing
struct Theme {
  int topPadding, header, spacing, buttonHints, sideHints;
};
constexpr Theme kClassic{5, 45, 10, 40, 30};
constexpr Theme kLyra{5, 84, 16, 40, 30};  // the default theme, and the one with the tall header

CoverGridLayout::Input portrait(int panelW, int panelH, const Theme& t, bool isX3) {
  const int contentWidth = panelW - (isX3 ? 2 * t.sideHints : t.sideHints);
  const int contentTop = t.topPadding + t.header + t.spacing;
  const int contentHeight = (panelH - t.buttonHints) - contentTop - t.spacing;
  return {contentWidth, contentHeight, isX3 ? 12 : 24, kMaxCell};
}

}  // namespace

TEST(CoverGridLayout, X4PortraitIsTwoByTwoWithFullSizeCells) {
  for (const auto& theme : {kClassic, kLyra}) {
    const auto l = CoverGridLayout::compute(portrait(480, 800, theme, /*isX3=*/false));
    EXPECT_EQ(l.cols, 2);
    EXPECT_EQ(l.rows, 2);
    EXPECT_EQ(l.cellWidth, 210);
    EXPECT_EQ(l.cellHeight, kMaxCell);  // stored thumb draws 1:1, never resampled
  }
}

TEST(CoverGridLayout, X3PortraitIsTwoByTwoWithFullSizeCells) {
  for (const auto& theme : {kClassic, kLyra}) {
    const auto l = CoverGridLayout::compute(portrait(528, 792, theme, /*isX3=*/true));
    EXPECT_EQ(l.cols, 2);
    EXPECT_EQ(l.rows, 2);
    EXPECT_EQ(l.cellWidth, 219);
    EXPECT_EQ(l.cellHeight, kMaxCell);
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

TEST(CoverGridLayout, HigherResolutionPanelGetsMoreCellsNotBiggerOnes) {
  // A 1072x1448 300 dpi panel: nothing changes but the numbers handed in.
  const auto l = CoverGridLayout::compute(portrait(1072, 1448, kLyra, /*isX3=*/false));
  EXPECT_EQ(l.cols, 4);
  EXPECT_EQ(l.rows, 4);
  EXPECT_EQ(l.cellHeight, kMaxCell);  // still capped by the stored thumbnail
  EXPECT_GE(l.cellWidth, CoverGridLayout::kMinCellWidth);
}

TEST(CoverGridLayout, ColumnsNeverDropBelowTheMinimumCellWidth) {
  for (int panelW = 300; panelW <= 2000; panelW += 7) {
    const auto l = CoverGridLayout::compute(portrait(panelW, 1000, kClassic, /*isX3=*/false));
    EXPECT_GE(l.cellWidth, CoverGridLayout::kMinCellWidth) << "panel width " << panelW;
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
// The inverse of the cell placement RecentBooksActivity::renderGridView does:
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
  y = kOriginY + row * l.rowStride + (l.cellHeight + CoverGridLayout::kLabelHeight) / 2;
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
  const int cellBottom = kOriginY + l.cellHeight + CoverGridLayout::kLabelHeight;
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
