#include <gtest/gtest.h>

#include "CrossPointSettings.h"

namespace {

using S = CrossPointSettings;

// The trap this guards: FONT_SIZE is not in visual order. TINY was appended as 4
// rather than inserted before SMALL, because inserting it would have shifted
// every persisted value. So `size + 1` is NOT "one size bigger" — it is
// EXTRA_LARGE -> TINY, the largest jumping to the smallest.
TEST(FontSizeLadder, EnumOrderIsNotVisualOrder) {
  EXPECT_EQ(S::TINY, 4);
  EXPECT_LT(S::SMALL, S::TINY) << "if this ever becomes false the ladder can be replaced by arithmetic";
}

TEST(FontSizeLadder, StepsUpThroughEveryVisualSize) {
  EXPECT_EQ(S::SMALL, S::stepFontSize(S::TINY, 1));
  EXPECT_EQ(S::MEDIUM, S::stepFontSize(S::SMALL, 1));
  EXPECT_EQ(S::LARGE, S::stepFontSize(S::MEDIUM, 1));
  EXPECT_EQ(S::EXTRA_LARGE, S::stepFontSize(S::LARGE, 1));
}

TEST(FontSizeLadder, StepsDownThroughEveryVisualSize) {
  EXPECT_EQ(S::LARGE, S::stepFontSize(S::EXTRA_LARGE, -1));
  EXPECT_EQ(S::MEDIUM, S::stepFontSize(S::LARGE, -1));
  EXPECT_EQ(S::SMALL, S::stepFontSize(S::MEDIUM, -1));
  EXPECT_EQ(S::TINY, S::stepFontSize(S::SMALL, -1));
}

TEST(FontSizeLadder, ClampsRatherThanWrapping) {
  // A pinch that has reached the end should stay there. Wrapping would turn a
  // continued pinch-out into the smallest text on screen.
  EXPECT_EQ(S::EXTRA_LARGE, S::stepFontSize(S::EXTRA_LARGE, 1));
  EXPECT_EQ(S::EXTRA_LARGE, S::stepFontSize(S::EXTRA_LARGE, 99));
  EXPECT_EQ(S::TINY, S::stepFontSize(S::TINY, -1));
  EXPECT_EQ(S::TINY, S::stepFontSize(S::TINY, -99));
}

TEST(FontSizeLadder, ZeroDeltaIsIdentity) {
  for (const uint8_t size : S::FONT_SIZE_LADDER) {
    EXPECT_EQ(size, S::stepFontSize(size, 0));
  }
}

TEST(FontSizeLadder, AnUnknownSizeIsLeftAlone) {
  // A hand-edited settings file, or a value from a future build. It has no place
  // on the ladder, so guessing an end would silently resize the reader's text.
  EXPECT_EQ(200, S::stepFontSize(200, 1));
  EXPECT_EQ(200, S::stepFontSize(200, -1));
}

TEST(FontSizeLadder, LadderCoversEverySize) {
  EXPECT_EQ(static_cast<size_t>(S::FONT_SIZE_COUNT), sizeof(S::FONT_SIZE_LADDER) / sizeof(S::FONT_SIZE_LADDER[0]));
}

}  // namespace
