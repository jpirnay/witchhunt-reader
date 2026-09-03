#include <gtest/gtest.h>

#include "components/ListLayout.h"
#include "components/themes/ListTouchBand.h"

namespace {

using ListTouchBand::Band;
using ListTouchBand::Builder;

// The band header states its row cap independently so it stays free of every include, so the
// two have to be checked against each other somewhere. The direction matters: the band is a
// CAPACITY and may be larger, but it must never be smaller than the window a wrapped list can
// paint, or that list would draw rows the band cannot record and they would answer no tap.
static_assert(ListLayout::kMaxRows <= ListTouchBand::kMaxRows, "ListLayout's window must fit in a ListTouchBand::Band");

// What BaseTheme::drawList records on a plain X4 settings page: 30px rows filling a 480px
// content band that starts 60px down, first item on the page is index 0.
Band uniformBand(const int firstIndex = 0, const int rowCount = 16) {
  Builder b;
  b.begin(/*rectX=*/0, /*rectWidth=*/480, firstIndex);
  for (int r = 0; r < rowCount; ++r) b.addRow(60 + r * 30, 30, /*selectable=*/true);
  return b.band;
}

TEST(ListTouchBand, HitsEachRowAtItsCentre) {
  const Band b = uniformBand();
  for (int r = 0; r < b.count; ++r) {
    EXPECT_EQ(r, ListTouchBand::hitTestIn(b, 240, 60 + r * 30 + 15)) << "row " << r;
  }
}

TEST(ListTouchBand, ResolvesThroughThePageOffset) {
  // Second page of a paged list: row 0 on screen is item 16, so a tap there means 16.
  const Band b = uniformBand(/*firstIndex=*/16);
  EXPECT_EQ(16, ListTouchBand::hitTestIn(b, 240, 60 + 15));
  EXPECT_EQ(20, ListTouchBand::hitTestIn(b, 240, 60 + 4 * 30 + 15));
}

TEST(ListTouchBand, RowBoundariesBelongToTheRowBelow) {
  const Band b = uniformBand();
  // A row owns [top, top+height): its last pixel is its own, the next one starts the next row.
  EXPECT_EQ(0, ListTouchBand::hitTestIn(b, 240, 60));
  EXPECT_EQ(0, ListTouchBand::hitTestIn(b, 240, 89));
  EXPECT_EQ(1, ListTouchBand::hitTestIn(b, 240, 90));
}

TEST(ListTouchBand, MissesAboveAndBelowTheBand) {
  const Band b = uniformBand();
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b, 240, 59));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b, 240, 60 + 16 * 30));
}

TEST(ListTouchBand, MissesOutsideTheHorizontalExtent) {
  const Band b = uniformBand();
  const int y = 60 + 15;
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b, -1, y));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b, 480, y));
  EXPECT_EQ(0, ListTouchBand::hitTestIn(b, 479, y));
}

// The remainder strip a uniform list leaves when rect.height is not a multiple of rowHeight
// belongs to no row. Dividing by the step would round it into the last one.
TEST(ListTouchBand, RemainderStripBelowTheLastRowIsAMiss) {
  Builder b;
  b.begin(0, 480, 0);
  for (int r = 0; r < 3; ++r) b.addRow(0 + r * 30, 30, true);  // 90px of rows in a 100px rect
  EXPECT_EQ(2, ListTouchBand::hitTestIn(b.band, 240, 89));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, 90));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, 99));
}

TEST(ListTouchBand, SeparatorRowsAreNotSelectable) {
  Builder b;
  b.begin(0, 480, 0);
  b.addRow(0, 30, /*selectable=*/false);  // section heading
  b.addRow(30, 30, true);
  b.addRow(60, 30, false);  // another heading
  b.addRow(90, 30, true);
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, 15));
  EXPECT_EQ(1, ListTouchBand::hitTestIn(b.band, 240, 45));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, 75));
  EXPECT_EQ(3, ListTouchBand::hitTestIn(b.band, 240, 105));
}

// drawWrappedList's rows differ in height, which is the case rowTouch()'s uniform-step
// arithmetic cannot express at all.
TEST(ListTouchBand, HandlesVariableRowHeights) {
  Builder b;
  b.begin(0, 480, /*firstIndex=*/5);
  b.addRow(0, 30, true);   // one line
  b.addRow(30, 46, true);  // wrapped to two
  b.addRow(76, 62, true);  // wrapped to three
  EXPECT_EQ(5, ListTouchBand::hitTestIn(b.band, 240, 29));
  EXPECT_EQ(6, ListTouchBand::hitTestIn(b.band, 240, 30));
  EXPECT_EQ(6, ListTouchBand::hitTestIn(b.band, 240, 75));
  EXPECT_EQ(7, ListTouchBand::hitTestIn(b.band, 240, 76));
  EXPECT_EQ(7, ListTouchBand::hitTestIn(b.band, 240, 137));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, 138));
}

TEST(ListTouchBand, EmptyBandMissesEverything) {
  const Band b{};
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b, 0, 0));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b, 240, 100));
}

// A list longer than the cap must drop its tail rather than mis-map it: recording stops, and
// the rows that were recorded still answer correctly.
TEST(ListTouchBand, StopsAtTheRowCapWithoutCorruptingWhatFits) {
  Builder b;
  b.begin(0, 480, 0);
  for (int r = 0; r < ListTouchBand::kMaxRows + 5; ++r) b.addRow(r * 20, 20, true);
  EXPECT_EQ(ListTouchBand::kMaxRows, b.band.count);
  EXPECT_EQ(0, ListTouchBand::hitTestIn(b.band, 240, 10));
  EXPECT_EQ(ListTouchBand::kMaxRows - 1,
            ListTouchBand::hitTestIn(b.band, 240, (ListTouchBand::kMaxRows - 1) * 20 + 10));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, ListTouchBand::kMaxRows * 20 + 10));
}

// The shared-state half: record/snapshot/invalidate round-trip and the hasBand() gate that
// lets a screen with no list leave the tap for whoever else wants it.
TEST(ListTouchBand, RecordAndSnapshotRoundTrip) {
  ListTouchBand::record(uniformBand(/*firstIndex=*/8));
  Band out;
  ASSERT_TRUE(ListTouchBand::snapshot(out));
  EXPECT_EQ(8, out.firstIndex);
  EXPECT_EQ(16, out.count);
  EXPECT_TRUE(ListTouchBand::hasBand());
  EXPECT_EQ(8, ListTouchBand::hitTest(240, 60 + 15));

  ListTouchBand::invalidate();
  EXPECT_FALSE(ListTouchBand::hasBand());
  EXPECT_EQ(-1, ListTouchBand::hitTest(240, 60 + 15));
}

// Rows in the top half of the mask must stay distinguishable from rows in the bottom half.
// A 32-bit shift silently wraps (row 32 lands on row 0's bit), which would make a separator in
// the second half read as selectable and a real row read as a heading.
TEST(ListTouchBand, SelectabilityIsTrackedAboveTheThirtySecondRow) {
  ASSERT_GT(ListTouchBand::kMaxRows, 32) << "this test is about the high half of the mask";
  Builder b;
  b.begin(0, 480, 0);
  for (int r = 0; r < ListTouchBand::kMaxRows; ++r) {
    // Row 0 selectable, row 32 a separator: the pair that collides under a 32-bit shift.
    b.addRow(r * 20, 20, /*selectable=*/r != 32);
  }
  EXPECT_EQ(0, ListTouchBand::hitTestIn(b.band, 240, 10));
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 240, 32 * 20 + 10)) << "separator row 32";
  EXPECT_EQ(33, ListTouchBand::hitTestIn(b.band, 240, 33 * 20 + 10));
  EXPECT_EQ(ListTouchBand::kMaxRows - 1,
            ListTouchBand::hitTestIn(b.band, 240, (ListTouchBand::kMaxRows - 1) * 20 + 10));
}

// The screenful that overran the old cap of 24: the EPUB reader menu on the LilyGo T5S3.
// Its portrait frame is 540x960, getContentRect() takes the 40 px hint strip off the bottom and
// the menu starts its list 75 px down, so BaseTheme::drawList paints (960 - 40 - 75) / 30 = 28
// rows. Every one of them has to be recordable, or the rows past the cap are painted rows that
// answer to no tap -- which is exactly how the menu's bottom four rows went dead on that board.
TEST(ListTouchBand, RecordsAFullReaderMenuScreenfulOnA960PxPanel) {
  constexpr int kListTop = 75;
  constexpr int kRowHeight = 30;
  constexpr int kRows = (960 - 40 - kListTop) / kRowHeight;
  static_assert(kRows == 28, "T5S3 reader-menu geometry");
  ASSERT_LE(kRows, ListTouchBand::kMaxRows);

  Builder b;
  b.begin(/*rectX=*/0, /*rectWidth=*/540, /*firstIndex=*/0);
  for (int r = 0; r < kRows; ++r) b.addRow(kListTop + r * kRowHeight, kRowHeight, /*selectable=*/true);
  EXPECT_EQ(kRows, b.band.count);

  // The last row is the one that used to be dead, so name it rather than trusting the loop.
  EXPECT_EQ(kRows - 1, ListTouchBand::hitTestIn(b.band, 270, kListTop + (kRows - 1) * kRowHeight + 15));
  for (int r = 0; r < kRows; ++r) {
    EXPECT_EQ(r, ListTouchBand::hitTestIn(b.band, 270, kListTop + r * kRowHeight + 15)) << "row " << r;
  }
  // Below the last row is the gap above the hint strip, and belongs to no row.
  EXPECT_EQ(-1, ListTouchBand::hitTestIn(b.band, 270, kListTop + kRows * kRowHeight + 5));
}

TEST(ListTouchBand, ABandOfOnlySeparatorsDoesNotCountAsALiveList) {
  Builder b;
  b.begin(0, 480, 0);
  b.addRow(0, 30, false);
  b.addRow(30, 30, false);
  ListTouchBand::record(b.band);
  EXPECT_FALSE(ListTouchBand::hasBand());
  ListTouchBand::invalidate();
}

}  // namespace
