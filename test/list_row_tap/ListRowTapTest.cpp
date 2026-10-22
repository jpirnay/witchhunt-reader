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
// Another tap on it is another Activate, not a toggle back to Selected, and a one-item list
// activates on its first tap. apply() takes the selection by reference, so SettingsActivity (which
// compares in the band's frame and shifts by one) and StatusBarSettings (a uint8_t round trip) see
// the update without duplicating the rule.
TEST(ListRowTap, TappingTheRowThatIsAlreadySelectedOnEntryActivatesImmediately) {
  int selection = 4;
  EXPECT_EQ(Result::Activate, ListRowTap::apply(4, 10, selection));
  EXPECT_EQ(Result::Activate, ListRowTap::apply(4, 10, selection));
  EXPECT_EQ(4, selection);

  int only = 0;
  EXPECT_EQ(Result::Activate, ListRowTap::apply(0, 1, only));

  int shifted = 3 - 1;  // SettingsActivity's band frame
  EXPECT_EQ(Result::Activate, ListRowTap::apply(2, 8, shifted));
  EXPECT_EQ(2, shifted);
}

// Including a list that emptied, or SHRANK, between the render that recorded the band and this
// tap: the recorded row may no longer exist, and is rejected rather than clamped onto another item.
TEST(ListRowTap, OutOfRangeIsRejectedAndLeavesTheSelectionAlone) {
  int selection = 2;
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(-1, 10, selection));
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(10, 10, selection));
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(99, 10, selection));
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(0, 0, selection));
  EXPECT_EQ(Result::Rejected, ListRowTap::apply(8, 3, selection));
  EXPECT_EQ(2, selection);
}

TEST(ListRowTap, SelectThenActivatePreferenceKeepsFirstTapAsSelection) {
  ListRowTap::ActivationState state;
  EXPECT_EQ(Result::Selected, state.applyPreference(3, Result::Selected, false));
  EXPECT_EQ(Result::Activate, state.applyPreference(3, Result::Activate, false));
}

TEST(ListRowTap, ImmediatePreferencePromotesSelectionToActivation) {
  ListRowTap::ActivationState state;
  EXPECT_EQ(Result::Activate, state.applyPreference(3, Result::Selected, true));
  EXPECT_EQ(Result::Activate, state.applyPreference(3, Result::Activate, true));
  EXPECT_EQ(Result::Rejected, state.applyPreference(3, Result::Rejected, true));
}

TEST(ListRowTap, InitiallySelectedRowStillRequiresTwoTaps) {
  ListRowTap::ActivationState state;
  EXPECT_EQ(Result::Selected, state.applyPreference(4, Result::Activate, false));
  EXPECT_EQ(Result::Activate, state.applyPreference(4, Result::Activate, false));
}

TEST(ListRowTap, ChangingRowsRestartsTheTwoTapSequence) {
  ListRowTap::ActivationState state;
  EXPECT_EQ(Result::Selected, state.applyPreference(3, Result::Selected, false));
  EXPECT_EQ(Result::Selected, state.applyPreference(7, Result::Selected, false));
  EXPECT_EQ(Result::Activate, state.applyPreference(7, Result::Activate, false));
}

TEST(ListRowTap, ResetRequiresAFirstTapAgain) {
  ListRowTap::ActivationState state;
  EXPECT_EQ(Result::Selected, state.applyPreference(3, Result::Selected, false));
  state.reset();
  EXPECT_EQ(Result::Selected, state.applyPreference(3, Result::Activate, false));
}

}  // namespace
