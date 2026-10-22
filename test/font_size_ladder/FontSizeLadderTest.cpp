#include <gtest/gtest.h>

#include <string>

#include "CrossPointSettings.h"

namespace {

using S = CrossPointSettings;

// builtinRungForPoints() and ReaderSizeList::builtin() both assume the table runs smallest first.
TEST(FontSizeLadder, LadderIsAscending) {
  for (int i = 1; i < S::FONT_SIZE_RUNG_COUNT; ++i) {
    EXPECT_LT(S::FONT_SIZE_RUNGS[i - 1].points, S::FONT_SIZE_RUNGS[i].points);
  }
}

// --- point sizes (FONT_SIZE_ORDER_VERSION 2) -----------------------------------------------------

// A file from before point sizes holds FONT_SIZE values, and each must come back as the point size
// it meant -- under both older numberings.
TEST(FontSizeLadder, StoredEnumValuesConvertToThePointSizeTheyMeant) {
  for (const auto& rung : S::FONT_SIZE_RUNGS) {
    EXPECT_EQ(int{rung.points}, int{S::fontPointSizeFromStored(rung.size, 1)}) << "v1 value " << +rung.size;
  }
  const struct {
    uint8_t stored;
    uint8_t points;
  } v0[] = {{0, 12}, {1, 14}, {2, 16}, {3, 18}, {4, 10}};
  for (const auto& e : v0) {
    EXPECT_EQ(int{e.points}, int{S::fontPointSizeFromStored(e.stored, 0)}) << "v0 value " << +e.stored;
  }
}

// Sizes off the ladder are the point of storing points: an SD family built at 7 pt.
TEST(FontSizeLadder, StoredPointSizesAreKeptAsTheyAre) {
  for (int pt = S::MIN_FONT_POINT_SIZE; pt <= S::MAX_FONT_POINT_SIZE; ++pt) {
    EXPECT_EQ(pt, int{S::fontPointSizeFromStored(pt, S::FONT_SIZE_ORDER_VERSION)});
  }
}

// 0 hands the choice of fallback to the caller: a setting takes DEFAULT_FONT_POINT_SIZE, a book's
// override goes back to Default.
TEST(FontSizeLadder, UnusableStoredSizesConvertToZero) {
  constexpr uint8_t v2 = S::FONT_SIZE_ORDER_VERSION;
  EXPECT_EQ(0, S::fontPointSizeFromStored(-1, v2)) << "an absent key";
  EXPECT_EQ(0, S::fontPointSizeFromStored(0, v2));
  EXPECT_EQ(0, S::fontPointSizeFromStored(S::MAX_FONT_POINT_SIZE + 1, v2)) << "would not fit a book's int8_t";
  EXPECT_EQ(0, S::fontPointSizeFromStored(1000, v2));
  EXPECT_EQ(0, S::fontPointSizeFromStored(-1, 1));
  EXPECT_EQ(0, S::fontPointSizeFromStored(S::FONT_SIZE_COUNT, 1)) << "a FONT_SIZE no rung carries";
  // v0 had no PT_20, and a hand-edited file can hold anything. The caller's own fallback deals with it.
  EXPECT_EQ(0, S::fontPointSizeFromStored(200, 0));
}

// The stamps are history. Version 2 is the first that holds point sizes whatever the current version
// is, so a later bump cannot turn a version-2 file back into enum values: 8 pt would read as 26 pt.
TEST(FontSizeLadder, VersionTwoIsTheFirstPointSizeFile) {
  EXPECT_EQ(2, int{S::FIRST_POINT_SIZE_VERSION});
  EXPECT_EQ(8, int{S::fontPointSizeFromStored(8, 2)});
  EXPECT_EQ(12, int{S::fontPointSizeFromStored(12, 2)});
  EXPECT_EQ(0, int{S::fontPointSizeFromStored(0, 2)});
}

// The version-1 numbering written out rather than read off the table, so a rung inserted below the
// top -- which would renumber the enum -- fails here instead of silently resizing old files. The
// {8, 26} entry is the value the stamp exists for: 8 is PT_26 in a version-1 file and 8 pt in a
// version-2 one (StoredPointSizesAreKeptAsTheyAre).
TEST(FontSizeLadder, VersionOneValuesMeanTheseSizes) {
  const struct {
    uint8_t stored;
    uint8_t points;
  } v1[] = {{0, 10}, {1, 12}, {2, 14}, {3, 16}, {4, 18}, {5, 20}, {6, 22}, {7, 24}, {8, 26}};
  for (const auto& e : v1) {
    EXPECT_EQ(int{e.points}, int{S::fontPointSizeFromStored(e.stored, 1)}) << "v1 value " << +e.stored;
  }
}

}  // namespace
