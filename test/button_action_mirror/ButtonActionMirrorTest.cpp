#include <gtest/gtest.h>

#include "CrossPointSettings.h"

namespace {

using S = CrossPointSettings;

TEST(ButtonActionMirror, EveryDirectionalPairSwaps) {
  EXPECT_EQ(S::BTN_PAGE_BACK, S::mirroredDirection(S::BTN_PAGE_FORWARD));
  EXPECT_EQ(S::BTN_PAGE_FORWARD, S::mirroredDirection(S::BTN_PAGE_BACK));
  EXPECT_EQ(S::BTN_PAGE_BACK_10, S::mirroredDirection(S::BTN_PAGE_FORWARD_10));
  EXPECT_EQ(S::BTN_PAGE_FORWARD_10, S::mirroredDirection(S::BTN_PAGE_BACK_10));
  EXPECT_EQ(S::BTN_PREV_SECTION, S::mirroredDirection(S::BTN_NEXT_SECTION));
  EXPECT_EQ(S::BTN_NEXT_SECTION, S::mirroredDirection(S::BTN_PREV_SECTION));
}

TEST(ButtonActionMirror, AMirrorOfAMirrorIsTheAction) {
  for (uint8_t a = 0; a < S::BUTTON_ACTION_COUNT; ++a) {
    EXPECT_EQ(a, S::mirroredDirection(S::mirroredDirection(a))) << int(a);
  }
}

TEST(ButtonActionMirror, OtherActionsAreUnchanged) {
  for (const uint8_t a : {S::BTN_DEFAULT, S::BTN_GO_HOME, S::BTN_OPEN_TOC, S::BTN_READER_MENU, S::BTN_READER_BARS,
                          S::BTN_CONFIRM, S::BTN_IGNORE, S::BTN_FONT_SIZE_SMALLER, S::BTN_FONT_SIZE_LARGER,
                          S::BTN_CYCLE_ORIENTATION, S::BTN_CYCLE_ORIENTATION_BACK}) {
    EXPECT_EQ(a, S::mirroredDirection(a)) << int(a);
  }
}

// A settings file stores actions by number. Each version that inserted an action before the board-gated light block
// moved that block up; a file from before keeps its meaning (JsonSettingsIO::loadSettings).
TEST(StoredButtonAction, TheLightActionsKeepTheirMeaningFromEveryVersion) {
  // Version 0 numbered the light actions from BTN_BACK's place: nothing between Cycle Orientation Back and them.
  for (uint8_t light = 0; light < S::BUTTON_ACTION_COUNT - S::BTN_LIGHT_TOGGLE; ++light) {
    EXPECT_EQ(S::BTN_LIGHT_TOGGLE + light, S::upgradeStoredAction(S::BTN_BACK + light, 0)) << int(light);
    EXPECT_EQ(S::BTN_LIGHT_TOGGLE + light, S::upgradeStoredAction(S::BTN_READER_BARS + light, 1)) << int(light);
    EXPECT_EQ(S::BTN_LIGHT_TOGGLE + light, S::upgradeStoredAction(S::BTN_CONFIRM + light, 2)) << int(light);
    EXPECT_EQ(S::BTN_LIGHT_TOGGLE + light, S::upgradeStoredAction(S::BTN_LIGHT_TOGGLE + light, 3)) << int(light);
  }
  // Version 1 had Back, version 2 Reader Bars; they stay.
  EXPECT_EQ(S::BTN_BACK, S::upgradeStoredAction(S::BTN_BACK, 1));
  EXPECT_EQ(S::BTN_READER_BARS, S::upgradeStoredAction(S::BTN_READER_BARS, 2));
}

TEST(StoredButtonAction, TheActionsBeforeTheInsertionsStay) {
  for (uint8_t action = 0; action < S::BTN_BACK; ++action) {
    for (const int version : {0, 1, 2, 3}) {
      EXPECT_EQ(action, S::upgradeStoredAction(action, version)) << int(action) << " v" << version;
    }
  }
}

TEST(StoredButtonAction, TheCurrentVersionIsUntouched) {
  for (uint8_t action = 0; action < S::BUTTON_ACTION_COUNT; ++action) {
    EXPECT_EQ(action, S::upgradeStoredAction(action, S::BUTTON_ACTIONS_VERSION)) << int(action);
  }
}

}  // namespace
