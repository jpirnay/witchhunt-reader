// The size list behind every reader font-size control. The settings rows, the reader menu, the
// size buttons and the font a page is drawn with all go through it, so a stored size is shown,
// stepped from and rendered as the same size.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ReaderFontSizes.h"

namespace {

using S = CrossPointSettings;

std::vector<int> asVector(const ReaderSizeList& list) { return {list.points, list.points + list.count}; }

std::vector<int> ladderPoints() {
  std::vector<int> pts;
  for (const auto& rung : S::FONT_SIZE_RUNGS) pts.push_back(rung.points);
  return pts;
}

TEST(ReaderSizeList, BuiltinIsTheLadderInAscendingOrder) {
  EXPECT_EQ(ladderPoints(), asVector(ReaderSizeList::builtin()));
}

TEST(ReaderSizeList, AnOfferedSizeSnapsToItself) {
  const auto list = ReaderSizeList::builtin();
  for (const auto& rung : S::FONT_SIZE_RUNGS) EXPECT_EQ(int{rung.points}, int{list.snap(rung.points)});
}

// A size chosen under an SD family (8 pt, say) read while Bookerly is selected.
TEST(ReaderSizeList, AnUnofferedSizeSnapsToTheNearest) {
  const auto list = ReaderSizeList::builtin();
  const int smallest = list.points[0];
  const int largest = list.points[list.count - 1];
  EXPECT_EQ(smallest, list.snap(static_cast<uint8_t>(smallest - 2)));
  EXPECT_EQ(smallest, list.snap(1));
  EXPECT_EQ(largest, list.snap(static_cast<uint8_t>(largest + 9)));
  EXPECT_EQ(largest, list.snap(255));
}

// Halfway between two sizes goes to the smaller. Built from the table: where a gap is even,
// (a + b) / 2 is a true tie.
TEST(ReaderSizeList, ATieGoesToTheSmallerSize) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 1; i < list.count; ++i) {
    const int a = list.points[i - 1];
    const int b = list.points[i];
    if ((a + b) % 2 != 0) continue;
    EXPECT_EQ(a, list.snap(static_cast<uint8_t>((a + b) / 2))) << "between " << a << " and " << b;
  }
}

// Built-in faces are chosen by builtinRungForPoints(), the settings row by snap(). If the two
// ever disagreed, the row would read one size while the page was drawn in another.
TEST(ReaderSizeList, BuiltinSnapAgreesWithTheBuiltinFaceChosen) {
  const auto list = ReaderSizeList::builtin();
  for (int pt = 1; pt <= 255; ++pt) {
    const auto p = static_cast<uint8_t>(pt);
    EXPECT_EQ(int{list.snap(p)}, int{S::fontSizePoints(S::builtinRungForPoints(p))}) << pt << "pt";
  }
}

TEST(ReaderSizeList, IndexOfAnOfferedSizeIsItsPosition) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 0; i < list.count; ++i) EXPECT_EQ(int{i}, int{list.indexOf(list.points[i])});
}

TEST(ReaderSizeList, StepsUpAndDownThroughEverySize) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 1; i < list.count; ++i) {
    EXPECT_EQ(int{list.points[i]}, int{list.step(list.points[i - 1], 1)});
    EXPECT_EQ(int{list.points[i - 1]}, int{list.step(list.points[i], -1)});
  }
}

// A pinch that has reached the end should stay there. Wrapping would turn a continued pinch-out
// into the smallest text on screen.
TEST(ReaderSizeList, StepClampsRatherThanWrapping) {
  const auto list = ReaderSizeList::builtin();
  const uint8_t smallest = list.points[0];
  const uint8_t largest = list.points[list.count - 1];
  EXPECT_EQ(int{largest}, int{list.step(largest, 1)});
  EXPECT_EQ(int{largest}, int{list.step(largest, 99)});
  EXPECT_EQ(int{smallest}, int{list.step(smallest, -1)});
  EXPECT_EQ(int{smallest}, int{list.step(smallest, -99)});
  for (uint8_t i = 0; i < list.count; ++i) EXPECT_EQ(int{list.points[i]}, int{list.step(list.points[i], 0)});
}

// From a stored size the list does not offer, steps count from the size SHOWN for it.
TEST(ReaderSizeList, StepsFromAnUnofferedSizeStartAtItsNearest) {
  const auto list = ReaderSizeList::builtin();
  const auto below = static_cast<uint8_t>(list.points[0] - 2);
  EXPECT_EQ(int{list.points[1]}, int{list.step(below, 1)});
  EXPECT_EQ(int{list.points[0]}, int{list.step(below, -1)}) << "the smallest is already what is shown";
}

TEST(ReaderSizeList, NextWrapsFromTheLargestToTheSmallest) {
  const auto list = ReaderSizeList::builtin();
  for (uint8_t i = 1; i < list.count; ++i) EXPECT_EQ(int{list.points[i]}, int{list.next(list.points[i - 1])});
  EXPECT_EQ(int{list.points[0]}, int{list.next(list.points[list.count - 1])});
}

}  // namespace
