// Where GfxRenderer::drawBitmap() puts each row of a BMP (BitmapRowTarget.h). The rows come off the
// card in file order, and an ordinary BMP stores them bottom-up, so the first rows read are the
// image's bottom ones.
//
// The bug this pins (crosspoint-reader PR #4472, Justin Mitchell): a Crop-mode sleep image taller
// than the screen's shape has its bottom rows below the screen. Read first, the first of them
// ended the loop, and the screen stayed blank.

#include <gtest/gtest.h>

#include <vector>

#include "BitmapRowTarget.h"

namespace {

using bitmap_rows::RowFate;
using bitmap_rows::rowTarget;

struct Drawn {
  std::vector<int> timesPerScreenRow;  // how often each screen row was drawn
  int rowsRead = 0;
};

// drawBitmap's row loop: rows in file order, a Stop ends it before the read. `cropPixY` rows are cut from
// each end, as drawBitmap does for a visible height of height - 2 * cropPixY.
Drawn drawRows(const int height, const bool topDown, const int cropPixY, const ImageScale scale, const int y,
               const int screenHeight) {
  Drawn drawn;
  drawn.timesPerScreenRow.assign(static_cast<size_t>(screenHeight), 0);
  for (int bmpY = 0; bmpY < height; ++bmpY) {
    const auto target = rowTarget(bmpY, height, topDown, cropPixY, height - 2 * cropPixY, scale, y, screenHeight);
    if (target.fate == RowFate::Stop) break;
    ++drawn.rowsRead;
    if (target.fate == RowFate::Skip) continue;
    EXPECT_GE(target.screenY, 0);
    EXPECT_LT(target.screenY, screenHeight);
    ++drawn.timesPerScreenRow[static_cast<size_t>(target.screenY)];
  }
  return drawn;
}

void expectEveryRowOnce(const Drawn& drawn) {
  for (size_t row = 0; row < drawn.timesPerScreenRow.size(); ++row) {
    ASSERT_EQ(drawn.timesPerScreenRow[row], 1) << "screen row " << row;
  }
}

// #4472's case: 480x1000 on the X4's 800 rows, cut to the screen's shape. With 99 rows cut a side (what
// the float crop came to), 802 rows are left and the first one read lands below the screen.
TEST(BitmapRowTarget, ABottomUpImageCroppedTallerThanTheScreenFillsIt) {
  const Drawn drawn = drawRows(1000, /*topDown=*/false, /*cropPixY=*/99, ImageScale(), 0, 800);
  expectEveryRowOnce(drawn);
}

// The X3 one: 480x800 on 792 rows, only the overflow cut, 4 rows a side.
TEST(BitmapRowTarget, ABottomUpImageCutToTheOverflowFillsTheScreen) {
  const Drawn drawn = drawRows(800, /*topDown=*/false, /*cropPixY=*/4, ImageScale(), 0, 792);
  expectEveryRowOnce(drawn);
}

// An odd overflow leaves one row more than the screen holds: it is read and left off.
TEST(BitmapRowTarget, ABottomUpRowBelowTheScreenIsReadAndSkipped) {
  const auto first = rowTarget(0, 801, /*topDown=*/false, /*cropTop=*/4, /*shownRows=*/793, ImageScale(), 0, 792);
  EXPECT_EQ(first.fate, RowFate::Skip);
  const Drawn drawn = drawRows(801, /*topDown=*/false, /*cropPixY=*/4, ImageScale(), 0, 792);
  expectEveryRowOnce(drawn);
}

// Top-down rows walk down the screen: the first one past its bottom ends the read.
TEST(BitmapRowTarget, ATopDownImageStopsAtTheBottomOfTheScreen) {
  const Drawn drawn = drawRows(1000, /*topDown=*/true, /*cropPixY=*/99, ImageScale(), 0, 800);
  expectEveryRowOnce(drawn);
  EXPECT_EQ(drawn.rowsRead, 99 + 800) << "the top crop, then the screen's rows";
}

// Bottom-up rows walk up the screen: once one is above its top, so is every later one.
TEST(BitmapRowTarget, ABottomUpImageStopsAboveTheTopOfTheScreen) {
  const Drawn drawn = drawRows(1000, /*topDown=*/false, /*cropPixY=*/0, ImageScale(), /*y=*/-200, 800);
  expectEveryRowOnce(drawn);
  EXPECT_EQ(drawn.rowsRead, 800);
}

// A scaled image (Fit, or Crop on an image larger than the screen both ways) keeps its rows: 1584
// rows at half size fill 792.
TEST(BitmapRowTarget, AScaledBottomUpImageFillsTheScreen) {
  const Drawn drawn = drawRows(1584, /*topDown=*/false, /*cropPixY=*/0, ImageScale(1, 2), 0, 792);
  for (size_t row = 0; row < drawn.timesPerScreenRow.size(); ++row) {
    ASSERT_GE(drawn.timesPerScreenRow[row], 1) << "screen row " << row;
  }
}

// The cropped rows at either end are never drawn, whichever way the file stores them.
TEST(BitmapRowTarget, CroppedRowsAreNotDrawn) {
  for (const bool topDown : {false, true}) {
    const Drawn drawn = drawRows(100, topDown, /*cropPixY=*/10, ImageScale(), /*y=*/10, 200);
    for (int row = 0; row < 200; ++row) {
      EXPECT_EQ(drawn.timesPerScreenRow[static_cast<size_t>(row)], (row >= 10 && row < 90) ? 1 : 0)
          << "row " << row << (topDown ? " top-down" : " bottom-up");
    }
  }
}

// A visible height that leaves an odd number of rows over: one more is cut below than above, and exactly the
// visible rows are drawn whichever way the file stores them.
TEST(BitmapRowTarget, AnOddCropCutsTheExtraRowBelow) {
  for (const bool topDown : {false, true}) {
    std::vector<int> times(200, 0);
    for (int bmpY = 0; bmpY < 101; ++bmpY) {
      const auto target = rowTarget(bmpY, 101, topDown, /*cropTop=*/10, /*shownRows=*/80, ImageScale(), 0, 200);
      if (target.fate == RowFate::Stop) break;
      if (target.fate == RowFate::Draw) ++times[static_cast<size_t>(target.screenY)];
    }
    for (int row = 0; row < 200; ++row) {
      EXPECT_EQ(times[static_cast<size_t>(row)], row < 80 ? 1 : 0) << "row " << row << (topDown ? " top-down" : "");
    }
  }
}

}  // namespace
