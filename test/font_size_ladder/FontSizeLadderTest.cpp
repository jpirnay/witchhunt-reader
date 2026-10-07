#include <gtest/gtest.h>

#include <set>
#include <string>

#include "CrossPointSettings.h"

namespace {

using S = CrossPointSettings;

// The invariant the settings UI depends on: a font-size row's option INDEX is its stored value,
// so the enum has to run in ascending pixel order or the picker lists the sizes out of order.
// That was tolerable while the labels read "Small / Medium / Tiny"; it is not once they read
// "12pt / 14pt / 10pt". FONT_SIZE was renumbered to make it true -- see FONT_SIZE_ORDER_VERSION.
TEST(FontSizeLadder, EnumValueIsLadderPosition) {
  for (int i = 0; i < S::FONT_SIZE_RUNG_COUNT; ++i) {
    EXPECT_EQ(i, S::FONT_SIZE_RUNGS[i].size) << "rung " << i << " does not sit at its own enum value";
  }
}

TEST(FontSizeLadder, LadderCoversEverySize) {
  EXPECT_EQ(static_cast<int>(S::FONT_SIZE_COUNT), S::FONT_SIZE_RUNG_COUNT);
  for (int v = 0; v < S::FONT_SIZE_COUNT; ++v) {
    EXPECT_NE(0, S::fontSizePoints(static_cast<uint8_t>(v))) << "FONT_SIZE " << v << " names no rung";
  }
}

// --- the built-in rung a point size is drawn with ----------------------------------------------

TEST(FontSizeLadder, ALadderSizeIsDrawnWithItsOwnRung) {
  for (const auto& rung : S::FONT_SIZE_RUNGS) {
    EXPECT_EQ(rung.size, S::builtinRungForPoints(rung.points)) << +rung.points << "pt";
  }
}

// A size chosen under an SD family has no built-in face of its own; the nearest rung draws it.
TEST(FontSizeLadder, AnOffLadderSizeIsDrawnWithTheNearestRung) {
  const auto& smallest = S::FONT_SIZE_RUNGS[0];
  const auto& largest = S::FONT_SIZE_RUNGS[S::FONT_SIZE_RUNG_COUNT - 1];
  EXPECT_EQ(smallest.size, S::builtinRungForPoints(static_cast<uint8_t>(smallest.points - 3)));
  EXPECT_EQ(largest.size, S::builtinRungForPoints(static_cast<uint8_t>(largest.points + 10)));
  for (int i = 1; i < S::FONT_SIZE_RUNG_COUNT; ++i) {
    const int a = S::FONT_SIZE_RUNGS[i - 1].points;
    const int b = S::FONT_SIZE_RUNGS[i].points;
    if ((a + b) % 2 != 0) continue;
    EXPECT_EQ(S::FONT_SIZE_RUNGS[i - 1].size, S::builtinRungForPoints(static_cast<uint8_t>((a + b) / 2)))
        << "a tie between " << a << " and " << b << " goes to the smaller";
  }
}

// --- the v0 -> v1 renumbering -----------------------------------------------------------------

// What migration has to preserve is not a number but a SIZE: whatever point size a reader had
// chosen before the renumbering, they must still have afterwards. Stated that way the test does
// not restate the mapping it is checking.
TEST(FontSizeLadder, MigrationPreservesThePointSizeTheReaderChose) {
  // The v0 numbering, which is now only recorded here and in remapLegacyFontSize().
  const struct {
    uint8_t stored;
    uint8_t points;
  } legacy[] = {{0, 12}, {1, 14}, {2, 16}, {3, 18}, {4, 10}};

  for (const auto& e : legacy) {
    const uint8_t migrated = S::remapLegacyFontSize(e.stored, 0);
    EXPECT_EQ(e.points, S::fontSizePoints(migrated))
        << "v0 value " << static_cast<int>(e.stored) << " used to mean " << static_cast<int>(e.points) << "pt";
  }
}

TEST(FontSizeLadder, RenumberingIsOnlyAppliedToVersionZeroFiles) {
  // A file at version 1 or later already holds pixel-order values; renumbering them again would
  // shift the reader's size by one every time the settings were rewritten.
  for (int v = 0; v < S::FONT_SIZE_COUNT; ++v) {
    const auto stored = static_cast<uint8_t>(v);
    EXPECT_EQ(stored, S::remapLegacyFontSize(stored, 1));
    EXPECT_EQ(stored, S::remapLegacyFontSize(stored, S::FONT_SIZE_ORDER_VERSION));
  }
}

TEST(FontSizeLadder, MigrationIsAPermutationSoNoTwoSizesCollapse) {
  std::set<uint8_t> seen;
  for (uint8_t stored = 0; stored < 5; ++stored) {
    EXPECT_TRUE(seen.insert(S::remapLegacyFontSize(stored, 0)).second) << "two v0 values migrate to the same size";
  }
}

TEST(FontSizeLadder, MigrationLeavesAValueItDoesNotRecognise) {
  // v0 had no PT_20, and a hand-edited file can hold anything. Either way the caller's own
  // clamp is the right place to deal with it, not a guess here.
  EXPECT_EQ(200, S::remapLegacyFontSize(200, 0));
}

// --- the labels -------------------------------------------------------------------------------

TEST(FontSizeLadder, LabelsReadAsPointSizes) {
  // Every rung's label is its own point size with "pt" appended -- checked against the table for
  // all of them rather than spot-checking three, which is what let a stale "24pt" survive here
  // after PT_20 was redefined as 20 pt.
  for (const auto& rung : S::FONT_SIZE_RUNGS) {
    EXPECT_EQ(std::to_string(rung.points) + "pt", S::fontSizeLabel(rung.size));
  }
  EXPECT_EQ("", S::fontSizeLabel(200)) << "a value off the ladder has no label to show";
}

// enumLabels is indexed by stored value, so a gap would render as a blank but selectable row.
TEST(FontSizeLadder, LabelListHasNoBlanksAndIsInAscendingOrder) {
  const auto labels = S::fontSizeLabels();
  ASSERT_EQ(static_cast<size_t>(S::FONT_SIZE_COUNT), labels.size());
  for (int v = 0; v < S::FONT_SIZE_COUNT; ++v) {
    EXPECT_EQ(S::fontSizeLabel(static_cast<uint8_t>(v)), labels[v]);
    EXPECT_FALSE(labels[v].empty());
  }
  // Against the table, not a literal. These were "10pt" and "24pt" hardcoded, and the second went
  // stale the moment a size was added above it -- the exact failure mode FONT_SIZE_RUNGS exists to
  // stop, reproduced in the test that guards it.
  EXPECT_EQ(S::fontSizeLabel(S::FONT_SIZE_RUNGS[0].size), labels.front());
  EXPECT_EQ(S::fontSizeLabel(S::FONT_SIZE_RUNGS[S::FONT_SIZE_RUNG_COUNT - 1].size), labels.back());
  // And the ladder really is ascending, which is what makes front()/back() meaningful at all.
  for (int i = 1; i < S::FONT_SIZE_RUNG_COUNT; ++i) {
    EXPECT_LT(S::FONT_SIZE_RUNGS[i - 1].points, S::FONT_SIZE_RUNGS[i].points);
  }
}

TEST(FontSizeLadder, DefaultEntryShiftsEveryValueByOne) {
  const auto labels = S::fontSizeLabels("Default");
  ASSERT_EQ(static_cast<size_t>(S::FONT_SIZE_COUNT) + 1, labels.size());
  EXPECT_EQ("Default", labels[0]);
  for (int v = 0; v < S::FONT_SIZE_COUNT; ++v) {
    EXPECT_EQ(S::fontSizeLabel(static_cast<uint8_t>(v)), labels[v + 1]);
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
// top -- which would renumber the enum -- fails here instead of silently resizing old files.
TEST(FontSizeLadder, VersionOneValuesMeanTheseSizes) {
  const struct {
    uint8_t stored;
    uint8_t points;
  } v1[] = {{0, 10}, {1, 12}, {2, 14}, {3, 16}, {4, 18}, {5, 20}, {6, 22}, {7, 24}, {8, 26}};
  for (const auto& e : v1) {
    EXPECT_EQ(int{e.points}, int{S::fontPointSizeFromStored(e.stored, 1)}) << "v1 value " << +e.stored;
  }
}

// The overlap the stamp exists for: 8 is PT_26 in a version-1 file and 8 pt in a version-2 one.
TEST(FontSizeLadder, TheStampDecidesWhatAnAmbiguousValueMeans) {
  EXPECT_EQ(int{S::fontSizePoints(S::PT_26)}, int{S::fontPointSizeFromStored(S::PT_26, 1)});
  EXPECT_EQ(int{S::PT_26}, int{S::fontPointSizeFromStored(S::PT_26, S::FONT_SIZE_ORDER_VERSION)});
}

}  // namespace
