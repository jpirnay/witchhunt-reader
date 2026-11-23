#include <gtest/gtest.h>

#include <iterator>
#include <vector>

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

// A settings file stores actions by number. Each version that inserted actions moved the ones after them up; a file
// from before keeps its meaning (JsonSettingsIO::loadSettings).
//
// The light block as versions 0 to 3 numbered it, in today's names, and as version 4 did: it put the light panel into
// the block, before the warm pair.
constexpr uint8_t OLD_LIGHT_BLOCK[] = {S::BTN_LIGHT_TOGGLE, S::BTN_LIGHT_BRIGHTER, S::BTN_LIGHT_DIMMER,
                                       S::BTN_LIGHT_WARMER, S::BTN_LIGHT_COOLER};
constexpr uint8_t V4_LIGHT_BLOCK[] = {S::BTN_LIGHT_TOGGLE, S::BTN_LIGHT_BRIGHTER, S::BTN_LIGHT_DIMMER,
                                      S::BTN_LIGHT_PANEL,  S::BTN_LIGHT_WARMER,   S::BTN_LIGHT_COOLER};

TEST(StoredButtonAction, TheLightActionsKeepTheirMeaningFromEveryVersion) {
  // Where each version's light block began. Version 0 had nothing between Cycle Orientation Back and it, so it began
  // at Back's place; each later version's began past its own insertion.
  const struct {
    int version;
    uint8_t start;
  } FILES[] = {{0, S::BTN_BACK}, {1, S::BTN_READER_BARS}, {2, S::BTN_CONFIRM}, {3, S::BTN_TOGGLE_DARK_MODE}};
  for (const auto& file : FILES) {
    for (uint8_t i = 0; i < std::size(OLD_LIGHT_BLOCK); ++i) {
      EXPECT_EQ(OLD_LIGHT_BLOCK[i], S::upgradeStoredAction(file.start + i, file.version))
          << "v" << file.version << " light " << int(i);
    }
  }
  // Version 4's began after Take Screenshot, where version 5 put Touch Lock.
  for (uint8_t i = 0; i < std::size(V4_LIGHT_BLOCK); ++i) {
    EXPECT_EQ(V4_LIGHT_BLOCK[i], S::upgradeStoredAction(S::BTN_TOGGLE_TOUCH_LOCK + i, 4)) << "v4 light " << int(i);
  }
  // Version 1 had Back, version 2 Reader Bars, version 3 Confirm, version 4 Dark Mode to Screenshot; they stay.
  EXPECT_EQ(S::BTN_BACK, S::upgradeStoredAction(S::BTN_BACK, 1));
  EXPECT_EQ(S::BTN_READER_BARS, S::upgradeStoredAction(S::BTN_READER_BARS, 2));
  EXPECT_EQ(S::BTN_CONFIRM, S::upgradeStoredAction(S::BTN_CONFIRM, 3));
  for (const uint8_t action : {S::BTN_TOGGLE_DARK_MODE, S::BTN_AUTO_PAGE_TURN, S::BTN_SCREENSHOT}) {
    EXPECT_EQ(action, S::upgradeStoredAction(action, 4)) << int(action);
  }
}

// An action is new in the version that inserted it: no value a file from before can hold becomes it, and the first
// value past such a file's last action is still past today's.
TEST(StoredButtonAction, NoStoredValueBecomesALaterAction) {
  const struct {
    int version;
    uint8_t count;
    std::vector<uint8_t> added;
  } FILES[] = {
      {3,
       static_cast<uint8_t>(S::BTN_TOGGLE_DARK_MODE + std::size(OLD_LIGHT_BLOCK)),
       {S::BTN_TOGGLE_DARK_MODE, S::BTN_AUTO_PAGE_TURN, S::BTN_SCREENSHOT, S::BTN_LIGHT_PANEL,
        S::BTN_TOGGLE_TOUCH_LOCK}},
      {4, static_cast<uint8_t>(S::BTN_TOGGLE_TOUCH_LOCK + std::size(V4_LIGHT_BLOCK)), {S::BTN_TOGGLE_TOUCH_LOCK}},
  };
  for (const auto& file : FILES) {
    for (uint8_t stored = 0; stored < file.count; ++stored) {
      const uint8_t now = S::upgradeStoredAction(stored, file.version);
      EXPECT_LT(now, S::BUTTON_ACTION_COUNT) << "v" << file.version << " " << int(stored);
      for (const uint8_t added : file.added) {
        EXPECT_NE(added, now) << "v" << file.version << " " << int(stored);
      }
    }
    EXPECT_EQ(S::BUTTON_ACTION_COUNT, S::upgradeStoredAction(file.count, file.version)) << "v" << file.version;
  }
}

TEST(StoredButtonAction, TheActionsBeforeTheInsertionsStay) {
  for (uint8_t action = 0; action < S::BTN_BACK; ++action) {
    for (const int version : {0, 1, 2, 3, 4}) {
      EXPECT_EQ(action, S::upgradeStoredAction(action, version)) << int(action) << " v" << version;
    }
  }
  // Version 4 inserted after Confirm, version 5 after Take Screenshot.
  for (uint8_t action = 0; action < S::BTN_TOGGLE_DARK_MODE; ++action) {
    EXPECT_EQ(action, S::upgradeStoredAction(action, 3)) << int(action);
  }
  for (uint8_t action = 0; action < S::BTN_TOGGLE_TOUCH_LOCK; ++action) {
    EXPECT_EQ(action, S::upgradeStoredAction(action, 4)) << int(action);
  }
}

TEST(StoredButtonAction, TheCurrentVersionIsUntouched) {
  for (uint8_t action = 0; action < S::BUTTON_ACTION_COUNT; ++action) {
    EXPECT_EQ(action, S::upgradeStoredAction(action, S::BUTTON_ACTIONS_VERSION)) << int(action);
  }
}

}  // namespace
