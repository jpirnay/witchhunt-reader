// Windowing for variable-height list rows (long file names wrapped over up to three lines).
// Pure geometry: no renderer, no theme — heights come from the test's own table.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "components/ListLayout.h"

namespace {

// Row heights by index; anything past the end is a plain single-line row.
std::function<int(int)> heights(std::vector<int> table, int fallback = 40) {
  return [table = std::move(table), fallback](const int index) {
    return index < static_cast<int>(table.size()) ? table[index] : fallback;
  };
}

int lastIndex(const ListLayout::Window& w) { return w.first + w.count - 1; }

}  // namespace

TEST(ListLayout, UniformRowsFillTheRectFromTheAnchor) {
  int anchor = 0;
  const auto w = ListLayout::computeWindow(100, 0, 400, anchor, heights({}));

  EXPECT_EQ(anchor, 0);
  EXPECT_EQ(w.first, 0);
  EXPECT_EQ(w.count, 10);  // 400 / 40
  EXPECT_EQ(w.top[0], 0);
  EXPECT_EQ(w.top[9], 360);
  EXPECT_EQ(w.height[3], 40);
}

TEST(ListLayout, PartialRowIsNeverDrawn) {
  int anchor = 0;
  // 410 px holds ten 40 px rows with 10 px to spare — the eleventh must not be half-drawn.
  const auto w = ListLayout::computeWindow(100, 0, 410, anchor, heights({}));
  EXPECT_EQ(w.count, 10);
}

TEST(ListLayout, TallRowsPushItemsOffTheScreen) {
  int anchor = 0;
  // Three-line rows (40 + 2 * 20) for the first four items.
  const auto w = ListLayout::computeWindow(100, 0, 400, anchor, heights({80, 80, 80, 80}));

  EXPECT_EQ(w.count, 6);  // 4 * 80 = 320, then two 40 px rows
  EXPECT_EQ(w.top[4], 320);
  EXPECT_EQ(w.height[0], 80);
  EXPECT_EQ(w.height[4], 40);
}

TEST(ListLayout, ScrollsDownOneRowAtATimeToKeepTheSelectionVisible) {
  int anchor = 0;
  auto window = ListLayout::computeWindow(100, 9, 400, anchor, heights({}));
  EXPECT_EQ(anchor, 0) << "selection on the last visible row must not scroll";
  EXPECT_EQ(lastIndex(window), 9);

  window = ListLayout::computeWindow(100, 10, 400, anchor, heights({}));
  EXPECT_EQ(anchor, 1) << "one step down surrenders exactly one row at the top";
  EXPECT_EQ(window.first, 1);
  EXPECT_EQ(lastIndex(window), 10);
}

TEST(ListLayout, TallSelectedRowCanCostSeveralRowsAtTheTop) {
  int anchor = 0;
  // Item 10 is a three-line row: making room for it takes two 40 px rows off the top.
  const auto heightOf = heights({40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 80});
  const auto window = ListLayout::computeWindow(100, 10, 400, anchor, heightOf);

  EXPECT_EQ(anchor, 2);
  EXPECT_EQ(window.first, 2);
  EXPECT_EQ(lastIndex(window), 10);
}

TEST(ListLayout, SelectionAboveTheWindowPullsTheAnchorToIt) {
  int anchor = 40;
  const auto w = ListLayout::computeWindow(100, 12, 400, anchor, heights({}));

  EXPECT_EQ(anchor, 12);
  EXPECT_EQ(w.first, 12);
  EXPECT_EQ(w.count, 10);
}

TEST(ListLayout, EndOfListIsBackfilledSoTheScreenStaysFull) {
  int anchor = 95;  // would leave five rows of blank space below
  const auto w = ListLayout::computeWindow(100, 99, 400, anchor, heights({}));

  EXPECT_EQ(anchor, 90);
  EXPECT_EQ(lastIndex(w), 99);
  EXPECT_EQ(w.count, 10);
}

TEST(ListLayout, BackfillNeverScrollsTheSelectionAway) {
  int anchor = 98;
  const auto w = ListLayout::computeWindow(100, 98, 400, anchor, heights({}));

  EXPECT_LE(w.first, 98);
  EXPECT_GE(lastIndex(w), 98);
  EXPECT_EQ(lastIndex(w), 99);
}

TEST(ListLayout, ShortListIsShownWholeAndNeverScrolls) {
  int anchor = 0;
  const auto w = ListLayout::computeWindow(3, 2, 400, anchor, heights({}));

  EXPECT_EQ(anchor, 0);
  EXPECT_EQ(w.first, 0);
  EXPECT_EQ(w.count, 3);
}

TEST(ListLayout, ARowTallerThanTheRectIsStillDrawn) {
  int anchor = 0;
  // Nothing fits; dropping it would render an empty list with an unreachable selection.
  const auto w = ListLayout::computeWindow(5, 0, 50, anchor, heights({500}));

  EXPECT_EQ(w.count, 1);
  EXPECT_EQ(w.first, 0);
  EXPECT_EQ(w.height[0], 500);
}

TEST(ListLayout, DegenerateInputsYieldAnEmptyWindow) {
  int anchor = 7;
  EXPECT_EQ(ListLayout::computeWindow(0, 0, 400, anchor, heights({})).count, 0);
  EXPECT_EQ(anchor, 0) << "an empty list has no scroll position";

  anchor = 7;
  EXPECT_EQ(ListLayout::computeWindow(10, 0, 0, anchor, heights({})).count, 0);

  anchor = 7;
  EXPECT_EQ(ListLayout::computeWindow(10, 0, 400, anchor, nullptr).count, 0);
}

TEST(ListLayout, AnchorPastTheEndIsClampedBackIntoTheList) {
  int anchor = 500;
  const auto w = ListLayout::computeWindow(20, 19, 400, anchor, heights({}));

  EXPECT_EQ(lastIndex(w), 19);
  EXPECT_EQ(w.count, 10);
  EXPECT_EQ(anchor, 10);
}

// The reason computeWindow re-seats the window at the selection instead of walking towards it:
// heightOf() reads a name from the SD index and measures wrapped text, and a long press jumps
// straight to the end of the list. Walking there would measure every name in between.
TEST(ListLayout, JumpToTheEndOfALongListMeasuresOnlyAScreenful) {
  int calls = 0;
  const auto counted = [&calls](int) {
    calls++;
    return 40;
  };

  int anchor = 0;
  const auto w = ListLayout::computeWindow(2000, 1999, 400, anchor, counted);

  EXPECT_EQ(lastIndex(w), 1999);
  EXPECT_EQ(w.count, 10);
  EXPECT_EQ(anchor, 1990);
  EXPECT_LT(calls, 4 * ListLayout::kMaxRows) << "measured " << calls << " rows for a 10-row screen";
}

TEST(ListLayout, JumpToTheStartOfALongListMeasuresOnlyAScreenful) {
  int calls = 0;
  const auto counted = [&calls](int) {
    calls++;
    return 40;
  };

  int anchor = 1990;
  const auto w = ListLayout::computeWindow(2000, 0, 400, anchor, counted);

  EXPECT_EQ(anchor, 0);
  EXPECT_EQ(w.first, 0);
  EXPECT_LT(calls, 4 * ListLayout::kMaxRows) << "measured " << calls << " rows for a 10-row screen";
}

TEST(ListLayout, RepeatedHeightsAreMeasuredOnce) {
  std::vector<int> seen;
  const auto recording = [&seen](const int index) {
    seen.push_back(index);
    return 40;
  };

  int anchor = 50;
  ListLayout::computeWindow(100, 50, 400, anchor, recording);

  std::vector<int> unique = seen;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  EXPECT_EQ(seen.size(), unique.size()) << "a row was measured more than once in one call";
}

// What a page turn relies on: the caller moves the selection AND the anchor to the same index, and
// the window then starts there — a full screen of unseen rows below it. Without honouring the
// anchor the layout would only scroll far enough to keep the selection visible, which after a page
// jump means one new row at the bottom.
TEST(ListLayout, AnchorOnTheSelectionStartsTheWindowThere) {
  int anchor = 20;
  const auto w = ListLayout::computeWindow(100, 20, 400, anchor, heights({}));

  EXPECT_EQ(anchor, 20);
  EXPECT_EQ(w.first, 20);
  EXPECT_EQ(w.count, 10);
  EXPECT_EQ(lastIndex(w), 29);
}

TEST(ListLayout, WindowNeverExceedsTheRowCap) {
  int anchor = 0;
  // 4000 px of 1 px rows: the cap is what keeps Window a fixed-size stack object.
  const auto w = ListLayout::computeWindow(4000, 0, 4000, anchor, heights({}, 1));
  EXPECT_EQ(w.count, ListLayout::kMaxRows);
}
