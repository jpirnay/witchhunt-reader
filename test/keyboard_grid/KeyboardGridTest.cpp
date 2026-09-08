// Keyboard key hit test: the inverse of the draw's `origin + index * (size + spacing)` walk.
//
// The round trip is the gate: for every key, compute where the draw puts it, tap the middle of
// that rect, and assert the same key comes back. Two themes are exercised because one of them
// lays the character rows out with NO spacing, where a hit test that assumes a gutter would
// reject every tap on a column boundary.

#include <gtest/gtest.h>

#include <string>

#include "components/KeyboardGrid.h"

namespace {

// The character block, as the four shipped themes lay it out. Classic keeps a 2 px gutter;
// Lyra's is zero, so cells touch.
constexpr KeyboardGrid::Rows kSpaced{/*originX=*/24, /*originY=*/400, /*keyWidth=*/40, /*keyHeight=*/50,
                                     /*spacing=*/2,  /*cols=*/10,     /*rows=*/4};
constexpr KeyboardGrid::Rows kTight{/*originX=*/24, /*originY=*/400, /*keyWidth=*/31, /*keyHeight=*/50,
                                    /*spacing=*/0,  /*cols=*/10,     /*rows=*/4};
// The five-key bottom row: shift, mode, space, del, ok.
constexpr KeyboardGrid::Rows kBottom{/*originX=*/24, /*originY=*/660, /*keyWidth=*/62, /*keyHeight=*/35,
                                     /*spacing=*/5,  /*cols=*/5,      /*rows=*/1};

// Where the draw puts key (r, c).
int keyX(const KeyboardGrid::Rows& g, const int c) { return g.originX + c * (g.keyWidth + g.spacing); }
int keyY(const KeyboardGrid::Rows& g, const int r) { return g.originY + r * (g.keyHeight + g.spacing); }

}  // namespace

TEST(KeyboardGrid, EveryKeyRoundTripsFromItsCentre) {
  for (const auto& g : {kSpaced, kTight, kBottom}) {
    for (int r = 0; r < g.rows; ++r) {
      for (int c = 0; c < g.cols; ++c) {
        int hitRow = -1;
        int hitCol = -1;
        const std::string where = " r=" + std::to_string(r) + " c=" + std::to_string(c);
        ASSERT_TRUE(hitTest(g, keyX(g, c) + g.keyWidth / 2, keyY(g, r) + g.keyHeight / 2, hitRow, hitCol)) << where;
        EXPECT_EQ(hitRow, r) << where;
        EXPECT_EQ(hitCol, c) << where;
      }
    }
  }
}

// The corners of a key belong to that key: the top-left pixel is inside, the bottom-right one is
// the last pixel before the next cell starts.
TEST(KeyboardGrid, KeyRectsAreHalfOpen) {
  for (const auto& g : {kSpaced, kTight}) {
    int row = -1;
    int col = -1;
    EXPECT_TRUE(hitTest(g, keyX(g, 3), keyY(g, 2), row, col));
    EXPECT_EQ(row, 2);
    EXPECT_EQ(col, 3);

    EXPECT_TRUE(hitTest(g, keyX(g, 3) + g.keyWidth - 1, keyY(g, 2) + g.keyHeight - 1, row, col));
    EXPECT_EQ(row, 2);
    EXPECT_EQ(col, 3);
  }
}

// A tight grid has no gutter, so the pixel after a key is the next key -- not a miss.
TEST(KeyboardGrid, TightGridHasNoGutterBetweenColumns) {
  int row = -1;
  int col = -1;
  ASSERT_TRUE(hitTest(kTight, keyX(kTight, 3) + kTight.keyWidth, keyY(kTight, 0) + 5, row, col));
  EXPECT_EQ(col, 4);
}

// A spaced grid does, and a tap that lands in it types nothing rather than guessing a neighbour.
TEST(KeyboardGrid, SpacedGridRejectsTheGutter) {
  int row = 0;
  int col = 0;
  EXPECT_FALSE(hitTest(kSpaced, keyX(kSpaced, 3) + kSpaced.keyWidth, keyY(kSpaced, 0) + 5, row, col));
  EXPECT_FALSE(hitTest(kSpaced, keyX(kSpaced, 3) + 5, keyY(kSpaced, 0) + kSpaced.keyHeight, row, col));
}

TEST(KeyboardGrid, OutsideTheBlockIsAMiss) {
  const KeyboardGrid::Rows& g = kSpaced;
  int row = 0;
  int col = 0;
  EXPECT_FALSE(hitTest(g, g.originX - 1, g.originY + 10, row, col));
  EXPECT_FALSE(hitTest(g, g.originX + 10, g.originY - 1, row, col));
  // Past the last column, and past the last row.
  EXPECT_FALSE(hitTest(g, keyX(g, g.cols - 1) + g.keyWidth + g.spacing, g.originY + 10, row, col));
  EXPECT_FALSE(hitTest(g, g.originX + 10, keyY(g, g.rows - 1) + g.keyHeight + g.spacing, row, col));
}

// A grid the activity has not laid out yet (no render has happened) must answer nothing rather
// than divide by a zero step.
TEST(KeyboardGrid, DegenerateGridIsInert) {
  int row = 0;
  int col = 0;
  EXPECT_FALSE(hitTest(KeyboardGrid::Rows{}, 0, 0, row, col));
  EXPECT_FALSE(hitTest(KeyboardGrid::Rows{10, 10, 0, 20, 0, 4, 4}, 12, 12, row, col));
  EXPECT_FALSE(hitTest(KeyboardGrid::Rows{10, 10, 20, 20, 0, 0, 4}, 12, 12, row, col));
}
