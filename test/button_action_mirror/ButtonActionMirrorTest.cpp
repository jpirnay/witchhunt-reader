#include <gtest/gtest.h>

#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "ButtonActionNames.h"
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

// settings.json stores an action by name (ButtonActionNames.h): every action has one, its own, and reads back as
// itself.
TEST(StoredButtonAction, EveryActionHasItsOwnName) {
  std::set<std::string> seen;
  for (uint8_t action = 0; action < S::BUTTON_ACTION_COUNT; ++action) {
    const char* name = ButtonActionNames::name(static_cast<S::BUTTON_ACTION>(action));
    ASSERT_NE(nullptr, name) << int(action);
    EXPECT_TRUE(seen.insert(name).second) << name;
    uint8_t read = S::BUTTON_ACTION_COUNT;
    EXPECT_TRUE(ButtonActionNames::fromName(name, read)) << name;
    EXPECT_EQ(action, read) << name;
  }
}

// Names are what a file holds, so none already written may change, or move to another action.
TEST(StoredButtonAction, NamesAreStable) {
  const struct {
    S::BUTTON_ACTION action;
    const char* name;
  } STORED[] = {
      {S::BTN_DEFAULT, "default"},
      {S::BTN_PAGE_FORWARD, "pageForward"},
      {S::BTN_PAGE_BACK, "pageBack"},
      {S::BTN_PAGE_FORWARD_10, "pageForward10"},
      {S::BTN_PAGE_BACK_10, "pageBack10"},
      {S::BTN_GO_HOME, "goHome"},
      {S::BTN_SLEEP, "sleep"},
      {S::BTN_FORCE_REFRESH, "forceRefresh"},
      {S::BTN_FORCE_FAST_REFRESH, "forceFastRefresh"},
      {S::BTN_OPEN_TOC, "openToc"},
      {S::BTN_OPEN_BOOKMARKS, "openBookmarks"},
      {S::BTN_STAR_PAGE, "starPage"},
      {S::BTN_FOOTNOTES, "footnotes"},
      {S::BTN_NEXT_SECTION, "nextSection"},
      {S::BTN_PREV_SECTION, "prevSection"},
      {S::BTN_EXIT_READER, "exitReader"},
      {S::BTN_READER_MENU, "readerMenu"},
      {S::BTN_TOGGLE_BIONIC_READING, "toggleBionicReading"},
      {S::BTN_KOREADER_SYNC, "koreaderSync"},
      {S::BTN_CYCLE_FONT_SIZE, "cycleFontSize"},
      {S::BTN_CYCLE_ORIENTATION, "cycleOrientation"},
      {S::BTN_QUICK_OVERRIDES, "quickOverrides"},
      {S::BTN_IGNORE, "ignore"},
      {S::BTN_DICTIONARY, "dictionary"},
      {S::BTN_FONT_SIZE_SMALLER, "fontSizeSmaller"},
      {S::BTN_FONT_SIZE_LARGER, "fontSizeLarger"},
      {S::BTN_TOGGLE_TOUCH_UI, "toggleTouchUi"},
      {S::BTN_CYCLE_ORIENTATION_BACK, "cycleOrientationBack"},
      {S::BTN_BACK, "back"},
      {S::BTN_READER_BARS, "readerBars"},
      {S::BTN_CONFIRM, "confirm"},
      {S::BTN_TOGGLE_DARK_MODE, "toggleDarkMode"},
      {S::BTN_AUTO_PAGE_TURN, "autoPageTurn"},
      {S::BTN_SCREENSHOT, "screenshot"},
      {S::BTN_TOGGLE_TOUCH_LOCK, "toggleTouchLock"},
      {S::BTN_LIGHT_TOGGLE, "lightToggle"},
      {S::BTN_LIGHT_BRIGHTER, "lightBrighter"},
      {S::BTN_LIGHT_DIMMER, "lightDimmer"},
      {S::BTN_LIGHT_PANEL, "lightPanel"},
      {S::BTN_LIGHT_WARMER, "lightWarmer"},
      {S::BTN_LIGHT_COOLER, "lightCooler"},
      {S::BTN_TOGGLE_BLE_REMOTE, "toggleBleRemote"},
  };
  for (const auto& entry : STORED) {
    EXPECT_STREQ(entry.name, ButtonActionNames::name(entry.action)) << int(entry.action);
  }
}

// A name from a newer build, or none, is not an action here: the loader keeps the row's default.
TEST(StoredButtonAction, AnUnknownNameIsNoAction) {
  uint8_t read = S::BTN_PAGE_BACK;
  EXPECT_FALSE(ButtonActionNames::fromName("teleport", read));
  EXPECT_FALSE(ButtonActionNames::fromName("", read));
  EXPECT_FALSE(ButtonActionNames::fromName(nullptr, read));
  EXPECT_FALSE(ButtonActionNames::fromName("PageForward", read)) << "names are case-sensitive";
  EXPECT_EQ(S::BTN_PAGE_BACK, read) << "untouched";
}

// Files up to version 5 stored numbers. Each version that inserted actions moved the ones after them up; a file from
// before keeps its meaning. Positions are version 5's (ButtonActionNames::V5_ORDER), frozen, so these hold whatever
// the enum does next.
using ButtonActionNames::v5Number;

TEST(StoredButtonAction, VersionFiveNumbersAreItsOrder) {
  std::set<int> seen;
  for (uint8_t number = 0; number < std::size(ButtonActionNames::V5_ORDER); ++number) {
    uint8_t read = S::BUTTON_ACTION_COUNT;
    ASSERT_TRUE(ButtonActionNames::fromLegacyNumber(number, 5, read)) << int(number);
    EXPECT_EQ(ButtonActionNames::V5_ORDER[number], read) << int(number);
    EXPECT_TRUE(seen.insert(read).second) << "V5_ORDER names " << int(read) << " twice";
  }
  uint8_t read = S::BTN_PAGE_BACK;
  EXPECT_FALSE(ButtonActionNames::fromLegacyNumber(std::size(ButtonActionNames::V5_ORDER), 5, read));
  EXPECT_EQ(S::BTN_PAGE_BACK, read) << "untouched";
}

TEST(StoredButtonAction, TheLightActionsKeepTheirMeaningFromEveryVersion) {
  // The light block as versions 0 to 3 numbered it, and as version 4 did: it put the light panel into the block,
  // before the warm pair.
  constexpr S::BUTTON_ACTION OLD_LIGHT_BLOCK[] = {S::BTN_LIGHT_TOGGLE, S::BTN_LIGHT_BRIGHTER, S::BTN_LIGHT_DIMMER,
                                                  S::BTN_LIGHT_WARMER, S::BTN_LIGHT_COOLER};
  constexpr S::BUTTON_ACTION V4_LIGHT_BLOCK[] = {S::BTN_LIGHT_TOGGLE, S::BTN_LIGHT_BRIGHTER, S::BTN_LIGHT_DIMMER,
                                                 S::BTN_LIGHT_PANEL,  S::BTN_LIGHT_WARMER,   S::BTN_LIGHT_COOLER};
  // Where each version's light block began. Version 0 had nothing between Cycle Orientation Back and it, so it began
  // at Back's place; each later version's began past its own insertion.
  const struct {
    int version;
    uint8_t start;
  } FILES[] = {{0, v5Number(S::BTN_BACK)},
               {1, v5Number(S::BTN_READER_BARS)},
               {2, v5Number(S::BTN_CONFIRM)},
               {3, v5Number(S::BTN_TOGGLE_DARK_MODE)}};
  for (const auto& file : FILES) {
    for (uint8_t i = 0; i < std::size(OLD_LIGHT_BLOCK); ++i) {
      uint8_t read = S::BUTTON_ACTION_COUNT;
      ASSERT_TRUE(ButtonActionNames::fromLegacyNumber(file.start + i, file.version, read));
      EXPECT_EQ(OLD_LIGHT_BLOCK[i], read) << "v" << file.version << " light " << int(i);
    }
  }
  for (uint8_t i = 0; i < std::size(V4_LIGHT_BLOCK); ++i) {
    uint8_t read = S::BUTTON_ACTION_COUNT;
    ASSERT_TRUE(ButtonActionNames::fromLegacyNumber(v5Number(S::BTN_TOGGLE_TOUCH_LOCK) + i, 4, read));
    EXPECT_EQ(V4_LIGHT_BLOCK[i], read) << "v4 light " << int(i);
  }
}

// An action is new in the version that inserted it: no number a file from before can hold reads as it, and the
// first number past such a file's last action reads as nothing.
TEST(StoredButtonAction, NoStoredNumberBecomesALaterAction) {
  const struct {
    int version;
    uint8_t count;
    std::vector<S::BUTTON_ACTION> added;
  } FILES[] = {
      {3,
       static_cast<uint8_t>(v5Number(S::BTN_TOGGLE_DARK_MODE) + 5),
       {S::BTN_TOGGLE_DARK_MODE, S::BTN_AUTO_PAGE_TURN, S::BTN_SCREENSHOT, S::BTN_LIGHT_PANEL,
        S::BTN_TOGGLE_TOUCH_LOCK}},
      {4, static_cast<uint8_t>(v5Number(S::BTN_TOGGLE_TOUCH_LOCK) + 6), {S::BTN_TOGGLE_TOUCH_LOCK}},
  };
  for (const auto& file : FILES) {
    for (uint8_t stored = 0; stored < file.count; ++stored) {
      uint8_t read = S::BUTTON_ACTION_COUNT;
      ASSERT_TRUE(ButtonActionNames::fromLegacyNumber(stored, file.version, read)) << int(stored);
      for (const auto added : file.added) {
        EXPECT_NE(added, read) << "v" << file.version << " " << int(stored);
      }
    }
    uint8_t read = S::BTN_PAGE_BACK;
    EXPECT_FALSE(ButtonActionNames::fromLegacyNumber(file.count, file.version, read)) << "v" << file.version;
  }
}

TEST(StoredButtonAction, TheActionsBeforeTheInsertionsStay) {
  const auto readAs = [](const uint8_t stored, const int version) {
    uint8_t read = S::BUTTON_ACTION_COUNT;
    EXPECT_TRUE(ButtonActionNames::fromLegacyNumber(stored, version, read));
    return read;
  };
  for (uint8_t number = 0; number < v5Number(S::BTN_BACK); ++number) {
    for (const int version : {0, 1, 2, 3, 4, 5}) {
      EXPECT_EQ(ButtonActionNames::V5_ORDER[number], readAs(number, version)) << int(number) << " v" << version;
    }
  }
  // Version 4 inserted after Confirm, version 5 after Take Screenshot.
  for (uint8_t number = 0; number < v5Number(S::BTN_TOGGLE_DARK_MODE); ++number) {
    EXPECT_EQ(ButtonActionNames::V5_ORDER[number], readAs(number, 3)) << int(number);
  }
  for (uint8_t number = 0; number < v5Number(S::BTN_TOGGLE_TOUCH_LOCK); ++number) {
    EXPECT_EQ(ButtonActionNames::V5_ORDER[number], readAs(number, 4)) << int(number);
  }
}

}  // namespace
