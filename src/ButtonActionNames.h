#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"

// How settings.json stores a button or gesture action (CrossPointSettings::BUTTON_ACTION): by a name that never
// changes, so an action can go anywhere in the enum without touching a stored file. Files written up to
// BUTTON_ACTIONS_VERSION 5 stored the enum's number instead; fromLegacyNumber() reads those, once, before the file is
// written again with names.
namespace ButtonActionNames {

using S = CrossPointSettings;
using Action = S::BUTTON_ACTION;

// The stored name of each action, by value: the enum's name in camelCase. Never rename one, a file holding the old
// name would lose that binding (test/button_action_mirror pins every one). For storage, a new action needs a name here,
// at its value's place, and nothing more: no version bump, no upgrade step. A table rather than a switch: a switch
// compiles to code for every name, the table to one pointer each.
inline constexpr const char* const NAMES[] = {
    "default",               // BTN_DEFAULT
    "pageForward",           // BTN_PAGE_FORWARD
    "pageBack",              // BTN_PAGE_BACK
    "pageForward10",         // BTN_PAGE_FORWARD_10
    "pageBack10",            // BTN_PAGE_BACK_10
    "goHome",                // BTN_GO_HOME
    "sleep",                 // BTN_SLEEP
    "forceRefresh",          // BTN_FORCE_REFRESH
    "forceFastRefresh",      // BTN_FORCE_FAST_REFRESH
    "openToc",               // BTN_OPEN_TOC
    "openBookmarks",         // BTN_OPEN_BOOKMARKS
    "starPage",              // BTN_STAR_PAGE
    "footnotes",             // BTN_FOOTNOTES
    "nextSection",           // BTN_NEXT_SECTION
    "prevSection",           // BTN_PREV_SECTION
    "exitReader",            // BTN_EXIT_READER
    "readerMenu",            // BTN_READER_MENU
    "toggleBionicReading",   // BTN_TOGGLE_BIONIC_READING
    "koreaderSync",          // BTN_KOREADER_SYNC
    "cycleFontSize",         // BTN_CYCLE_FONT_SIZE
    "cycleOrientation",      // BTN_CYCLE_ORIENTATION
    "quickOverrides",        // BTN_QUICK_OVERRIDES
    "ignore",                // BTN_IGNORE
    "dictionary",            // BTN_DICTIONARY
    "fontSizeSmaller",       // BTN_FONT_SIZE_SMALLER
    "fontSizeLarger",        // BTN_FONT_SIZE_LARGER
    "toggleTouchUi",         // BTN_TOGGLE_TOUCH_UI
    "cycleOrientationBack",  // BTN_CYCLE_ORIENTATION_BACK
    "back",                  // BTN_BACK
    "readerBars",            // BTN_READER_BARS
    "confirm",               // BTN_CONFIRM
    "toggleDarkMode",        // BTN_TOGGLE_DARK_MODE
    "autoPageTurn",          // BTN_AUTO_PAGE_TURN
    "screenshot",            // BTN_SCREENSHOT
    "toggleTouchLock",       // BTN_TOGGLE_TOUCH_LOCK
    "lightToggle",           // BTN_LIGHT_TOGGLE
    "lightBrighter",         // BTN_LIGHT_BRIGHTER
    "lightDimmer",           // BTN_LIGHT_DIMMER
    "lightPanel",            // BTN_LIGHT_PANEL
    "lightWarmer",           // BTN_LIGHT_WARMER
    "lightCooler",           // BTN_LIGHT_COOLER
    "toggleBleRemote",       // BTN_TOGGLE_BLE_REMOTE
};
static_assert(std::size(NAMES) == S::BUTTON_ACTION_COUNT, "every BUTTON_ACTION needs a stored name, at its value");

constexpr const char* name(const Action action) {
  return static_cast<unsigned>(action) < std::size(NAMES) ? NAMES[action] : nullptr;
}

// The action a stored name stands for. False for a name this build does not know, one a newer build wrote.
inline bool fromName(const char* stored, uint8_t& action) {
  if (stored == nullptr) return false;
  for (uint8_t candidate = 0; candidate < std::size(NAMES); ++candidate) {
    if (std::strcmp(stored, NAMES[candidate]) == 0) {
      action = candidate;
      return true;
    }
  }
  return false;
}

// The actions in the order version 5 numbered them, the last numbering a file stored. Frozen with every position
// below: an action added from now on is stored by name only.
// One byte each: the enum itself is an int.
constexpr uint8_t V5_ORDER[] = {
    S::BTN_DEFAULT,
    S::BTN_PAGE_FORWARD,
    S::BTN_PAGE_BACK,
    S::BTN_PAGE_FORWARD_10,
    S::BTN_PAGE_BACK_10,
    S::BTN_GO_HOME,
    S::BTN_SLEEP,
    S::BTN_FORCE_REFRESH,
    S::BTN_FORCE_FAST_REFRESH,
    S::BTN_OPEN_TOC,
    S::BTN_OPEN_BOOKMARKS,
    S::BTN_STAR_PAGE,
    S::BTN_FOOTNOTES,
    S::BTN_NEXT_SECTION,
    S::BTN_PREV_SECTION,
    S::BTN_EXIT_READER,
    S::BTN_READER_MENU,
    S::BTN_TOGGLE_BIONIC_READING,
    S::BTN_KOREADER_SYNC,
    S::BTN_CYCLE_FONT_SIZE,
    S::BTN_CYCLE_ORIENTATION,
    S::BTN_QUICK_OVERRIDES,
    S::BTN_IGNORE,
    S::BTN_DICTIONARY,
    S::BTN_FONT_SIZE_SMALLER,
    S::BTN_FONT_SIZE_LARGER,
    S::BTN_TOGGLE_TOUCH_UI,
    S::BTN_CYCLE_ORIENTATION_BACK,
    S::BTN_BACK,
    S::BTN_READER_BARS,
    S::BTN_CONFIRM,
    S::BTN_TOGGLE_DARK_MODE,
    S::BTN_AUTO_PAGE_TURN,
    S::BTN_SCREENSHOT,
    S::BTN_TOGGLE_TOUCH_LOCK,
    S::BTN_LIGHT_TOGGLE,
    S::BTN_LIGHT_BRIGHTER,
    S::BTN_LIGHT_DIMMER,
    S::BTN_LIGHT_PANEL,
    S::BTN_LIGHT_WARMER,
    S::BTN_LIGHT_COOLER,
};
static_assert(std::size(V5_ORDER) == 41, "version 5 numbered 41 actions; this table is frozen");

// Where version 5 numbered an action.
constexpr uint8_t v5Number(const Action action) {
  for (uint8_t number = 0; number < std::size(V5_ORDER); ++number) {
    if (V5_ORDER[number] == action) return number;
  }
  return static_cast<uint8_t>(std::size(V5_ORDER));
}

// The action a number stood for in a file from BUTTON_ACTIONS_VERSION `fromVersion` (0 to 5). False past that
// version's last action. Each version that inserted actions moved those after them up by as many; the steps lift the
// number into version 5's numbering, each comparing with where its version put its first insertion, in that
// version's numbers.
constexpr bool fromLegacyNumber(const uint8_t stored, const int fromVersion, uint8_t& action) {
  unsigned number = stored;
  if (fromVersion < 1 && number >= v5Number(S::BTN_BACK)) ++number;
  if (fromVersion < 2 && number >= v5Number(S::BTN_READER_BARS)) ++number;
  if (fromVersion < 3 && number >= v5Number(S::BTN_CONFIRM)) ++number;
  // Version 4's three, Toggle Dark Mode to Take Screenshot.
  if (fromVersion < 4 && number >= v5Number(S::BTN_TOGGLE_DARK_MODE)) number += 3;
  // Version 4's Open Light Panel, one below its version 5 number: Touch Lock went in before it.
  if (fromVersion < 4 && number >= v5Number(S::BTN_LIGHT_PANEL) - 1u) ++number;
  if (fromVersion < 5 && number >= v5Number(S::BTN_TOGGLE_TOUCH_LOCK)) ++number;
  if (number >= std::size(V5_ORDER)) return false;
  action = V5_ORDER[number];
  return true;
}

}  // namespace ButtonActionNames
