// The reader's two margins, Top & Bottom and Left & Right, as a settings file gives them back
// (CrossPointSettings::screenMarginFromStored, read by JsonSettingsIO::loadSettings). Before the two there was one,
// "screenMargin", for all four sides: a file with only that key gives it to both.
#include <gtest/gtest.h>

#include "CrossPointSettings.h"

namespace {

using S = CrossPointSettings;
constexpr int kAbsent = -1;

// The old single margin's default and range, which both rows keep.
TEST(ReaderMargins, TheRowsKeepTheOldRangeAndDefault) {
  EXPECT_EQ(S::DEFAULT_SCREEN_MARGIN, 5);
  EXPECT_EQ(S::SCREEN_MARGIN_MIN, 5);
  EXPECT_EQ(S::SCREEN_MARGIN_MAX, 40);
  EXPECT_EQ(S::SCREEN_MARGIN_STEP, 5);
}

// Neither key in the file, only the old one: each margin is the old value, so nobody's pages move.
TEST(ReaderMargins, AFileFromBeforeGivesItsMarginToBoth) {
  for (int old = S::SCREEN_MARGIN_MIN; old <= S::SCREEN_MARGIN_MAX; old += S::SCREEN_MARGIN_STEP) {
    EXPECT_EQ(S::screenMarginFromStored(kAbsent, old), old) << "from " << old;
  }
}

TEST(ReaderMargins, TheirOwnKeysWin) {
  EXPECT_EQ(S::screenMarginFromStored(30, 10), 30);
  EXPECT_EQ(S::screenMarginFromStored(5, 40), 5);
  EXPECT_EQ(S::screenMarginFromStored(25, kAbsent), 25);
}

TEST(ReaderMargins, NothingStoredIsTheDefault) {
  EXPECT_EQ(S::screenMarginFromStored(kAbsent, kAbsent), S::DEFAULT_SCREEN_MARGIN);
}

// As the settings loop bounds a value row: below the range is its least, above it its most.
TEST(ReaderMargins, OutOfRangeIsClampedToTheRows) {
  EXPECT_EQ(S::screenMarginFromStored(0, kAbsent), S::SCREEN_MARGIN_MIN);
  EXPECT_EQ(S::screenMarginFromStored(300, kAbsent), S::SCREEN_MARGIN_MAX);
  EXPECT_EQ(S::screenMarginFromStored(kAbsent, 2), S::SCREEN_MARGIN_MIN);
  EXPECT_EQ(S::screenMarginFromStored(kAbsent, 45), S::SCREEN_MARGIN_MAX);
}

}  // namespace
