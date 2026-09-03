#include <gtest/gtest.h>

#include "activities/ListRowTap.h"

namespace {

using ListRowTap::Result;

TEST(ListRowTap, FirstTapOnANewRowSelectsItWithoutActivating) {
  int selection = 0;
  EXPECT_EQ(Result::Selected, ListRowTap::apply(3, 10, selection));
  EXPECT_EQ(3, selection);
}

TEST(ListRowTap, SecondTapOnTheSameRowActivates) {
  int selection = 0;
  ASSERT_EQ(Result::Selected, ListRowTap::apply(3, 10, selection));
  EXPECT_EQ(Result::Activate, ListRowTap::apply(3, 10, selection));
  EXPECT_EQ(3, selection);
}

// The point of the whole rule: reaching a row you did not mean costs one more tap, never an
// action. A finger wandering across rows can never activate one it has not settled on.
TEST(ListRowTap, MovingAwayBeforeTheSecondTapNeverActivates) {
  int selection = 0;
  EXPECT_EQ(Result::Selected, ListRowTap::apply(3, 10, selection));
  EXPECT_EQ(Result::Selected, ListRowTap::apply(7, 10, selection));
  EXPECT_EQ(Result::Selected, ListRowTap::apply(2, 10, selection));
  EXPECT_EQ(2, selection);
  EXPECT_EQ(Result::Activate, ListRowTap::apply(2, 10, selection));
}

// A screen is entered with its selection already somewhere — usually 0, or restored from a
// return hint. Tapping that row activates on the FIRST tap, which is correct: it is already
// highlighted, so the reader has the feedback the rule exists to give.
TEST(ListRowTap, TappingTheRowThatIsAlreadySelectedOnEntryActivatesImmediately) {
  int selection = 4;
  EXPECT_EQ(Result::Activate, ListRowTap::apply(4, 10, selection));
  EXPECT_EQ(4, selection);
}

TEST(ListRowTap, OutOfRangeIsRejectedAndLeavesTheSelectionAlone) {
  int selection = 2;
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(-1, 10, selection));
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(10, 10, selection));
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(99, 10, selection));
  EXPECT_EQ(2, selection);
}

// A list that emptied between the render that recorded the band and this tap.
TEST(ListRowTap, AnEmptyListRejectsEverything) {
  int selection = 0;
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(0, 0, selection));
  EXPECT_EQ(0, selection);
}

// A list that SHRANK between the render and the tap: the recorded row may no longer exist, and
// must be rejected rather than clamped onto a different item.
TEST(ListRowTap, ARowThatNoLongerExistsIsRejectedNotClamped) {
  int selection = 1;
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(8, 3, selection));
  EXPECT_EQ(1, selection);
}

TEST(ListRowTap, SingleItemListActivatesOnTheFirstTap) {
  int selection = 0;
  EXPECT_EQ(Result::Activate, ListRowTap::apply(0, 1, selection));
}

// Two taps on the same row are the contract, but three must not do anything surprising: the
// third is another Activate, not a toggle back to Selected.
TEST(ListRowTap, RepeatedTapsOnTheSelectedRowKeepActivating) {
  int selection = 5;
  EXPECT_EQ(Result::Activate, ListRowTap::apply(5, 10, selection));
  EXPECT_EQ(Result::Activate, ListRowTap::apply(5, 10, selection));
}

// SettingsActivity compares in the BAND's frame and then shifts by one; StatusBarSettings
// round-trips a uint8_t. Both hand apply() a plain int by reference, so the update has to be
// visible to the caller — that is what lets those two adapt without duplicating the rule.
TEST(ListRowTap, UpdatesTheSelectionThroughTheReference) {
  int selection = 0;
  ListRowTap::apply(6, 10, selection);
  EXPECT_EQ(6, selection);
  int shifted = 3 - 1;  // SettingsActivity's band frame
  EXPECT_EQ(Result::Activate, ListRowTap::apply(2, 8, shifted));
  EXPECT_EQ(2, shifted);
}

}  // namespace
