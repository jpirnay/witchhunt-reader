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
