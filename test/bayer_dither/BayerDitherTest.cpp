// applyBayerDither4Level(): the reader's grey pictures and the GIF and sleep pictures are dithered to four levels with
// a 4 x 4 ordered dither. However it is computed, every grey in every cell of the matrix must get the level the
// definition gives: the grey plus the cell's offset, clamped to 0..255, quantised at the midpoints.
#include <gtest/gtest.h>

#include <cstdint>

#include "DitherUtils.h"

namespace {

uint8_t definition(const uint8_t gray, const int x, const int y) {
  static const uint8_t kMatrix[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };
  int adjusted = gray + (kMatrix[y & 3][x & 3] - 8) * 5;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return adjusted >= 213 ? 3 : static_cast<uint8_t>((adjusted + 42) / 85);
}

}  // namespace

// Negative and large coordinates too: a picture placed partly off the screen passes them.
TEST(BayerDither4Level, EveryGreyInEveryCellGetsTheLevelOfItsClampedSum) {
  int checked = 0;
  for (int gray = 0; gray < 256; gray++) {
    for (int y = -8; y < 12; y++) {
      for (int x = -8; x < 12; x++) {
        ASSERT_EQ(definition(static_cast<uint8_t>(gray), x, y),
                  applyBayerDither4Level(static_cast<uint8_t>(gray), x, y))
            << "gray " << gray << " at " << x << "," << y;
        checked++;
      }
    }
  }
  EXPECT_EQ(256 * 20 * 20, checked);
  EXPECT_EQ(definition(200, 799, 479), applyBayerDither4Level(200, 799, 479));
  EXPECT_EQ(definition(1, 1000003, 70001), applyBayerDither4Level(1, 1000003, 70001));
}

// Black and white stay black and white in every cell, and a mid grey takes more than one level across the matrix.
TEST(BayerDither4Level, KeepsTheRailsAndDithersTheGreysBetween) {
  bool sawLevel1 = false;
  bool sawLevel2 = false;
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      EXPECT_EQ(0, applyBayerDither4Level(0, x, y));
      EXPECT_EQ(3, applyBayerDither4Level(255, x, y));
      const uint8_t mid = applyBayerDither4Level(128, x, y);
      sawLevel1 |= mid == 1;
      sawLevel2 |= mid == 2;
    }
  }
  EXPECT_TRUE(sawLevel1 && sawLevel2);
}
