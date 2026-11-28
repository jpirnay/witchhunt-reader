#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "CrossPointSettings.h"

// What a button or gesture action row offers, in the order its picker lists it: every BUTTON_ACTION but BTN_DEFAULT
// (each row puts its own default first), with its label and what the board needs for it. The one place that says which
// action a board can perform: SettingsList.h builds the rows from it, CrossPointSettings::dropUnsupportedActions()
// clears bindings by it.
//
// A row stores the action's value, not the option's position (SettingInfo::withOptionValues), so an action that only
// some boards offer can sit anywhere here and in the enum without moving any other option off its value.
namespace ButtonActionOptions {

using S = CrossPointSettings;

enum class Needs : uint8_t {
  Nothing,
  Light,      // a reading light (HalCapabilities::hasFrontlight)
  WarmLight,  // a second, warm light channel (HalCapabilities::hasColorTemperature)
};

struct Option {
  uint8_t action;  // S::BUTTON_ACTION
  StrId label;
  Needs needs;
};

inline constexpr Option ALL[] = {
    {S::BTN_PAGE_FORWARD, StrId::STR_BTN_ACT_PAGE_FORWARD, Needs::Nothing},
    {S::BTN_PAGE_BACK, StrId::STR_BTN_ACT_PAGE_BACK, Needs::Nothing},
    {S::BTN_PAGE_FORWARD_10, StrId::STR_BTN_ACT_PAGE_FORWARD_10, Needs::Nothing},
    {S::BTN_PAGE_BACK_10, StrId::STR_BTN_ACT_PAGE_BACK_10, Needs::Nothing},
    {S::BTN_GO_HOME, StrId::STR_BTN_ACT_GO_HOME, Needs::Nothing},
    {S::BTN_SLEEP, StrId::STR_BTN_ACT_SLEEP, Needs::Nothing},
    {S::BTN_FORCE_REFRESH, StrId::STR_BTN_ACT_FORCE_REFRESH, Needs::Nothing},
    {S::BTN_FORCE_FAST_REFRESH, StrId::STR_BTN_ACT_FORCE_FAST_REFRESH, Needs::Nothing},
    {S::BTN_OPEN_TOC, StrId::STR_BTN_ACT_OPEN_TOC, Needs::Nothing},
    {S::BTN_OPEN_BOOKMARKS, StrId::STR_BTN_ACT_OPEN_BOOKMARKS, Needs::Nothing},
    {S::BTN_STAR_PAGE, StrId::STR_BTN_ACT_STAR_PAGE, Needs::Nothing},
    {S::BTN_FOOTNOTES, StrId::STR_BTN_ACT_FOOTNOTES, Needs::Nothing},
    {S::BTN_NEXT_SECTION, StrId::STR_BTN_ACT_NEXT_SECTION, Needs::Nothing},
    {S::BTN_PREV_SECTION, StrId::STR_BTN_ACT_PREV_SECTION, Needs::Nothing},
    {S::BTN_EXIT_READER, StrId::STR_BTN_ACT_EXIT_READER, Needs::Nothing},
    {S::BTN_READER_MENU, StrId::STR_BTN_ACT_READER_MENU, Needs::Nothing},
    {S::BTN_TOGGLE_BIONIC_READING, StrId::STR_BTN_ACT_TOGGLE_BIONIC_READING, Needs::Nothing},
    {S::BTN_KOREADER_SYNC, StrId::STR_BTN_ACT_KOREADER_SYNC, Needs::Nothing},
    {S::BTN_CYCLE_FONT_SIZE, StrId::STR_BTN_ACT_CYCLE_FONT_SIZE, Needs::Nothing},
    {S::BTN_CYCLE_ORIENTATION, StrId::STR_BTN_ACT_CYCLE_ORIENTATION, Needs::Nothing},
    {S::BTN_QUICK_OVERRIDES, StrId::STR_BTN_ACT_QUICK_OVERRIDES, Needs::Nothing},
    {S::BTN_IGNORE, StrId::STR_BTN_ACT_IGNORE, Needs::Nothing},
    {S::BTN_DICTIONARY, StrId::STR_DICTIONARY, Needs::Nothing},
    {S::BTN_FONT_SIZE_SMALLER, StrId::STR_BTN_ACT_FONT_SIZE_SMALLER, Needs::Nothing},
    {S::BTN_FONT_SIZE_LARGER, StrId::STR_BTN_ACT_FONT_SIZE_LARGER, Needs::Nothing},
    {S::BTN_TOGGLE_TOUCH_UI, StrId::STR_BTN_ACT_TOGGLE_TOUCH_UI, Needs::Nothing},
    {S::BTN_CYCLE_ORIENTATION_BACK, StrId::STR_BTN_ACT_CYCLE_ORIENTATION_BACK, Needs::Nothing},
    {S::BTN_BACK, StrId::STR_BACK, Needs::Nothing},
    {S::BTN_READER_BARS, StrId::STR_BTN_ACT_READER_BARS, Needs::Nothing},
    {S::BTN_CONFIRM, StrId::STR_CONFIRM, Needs::Nothing},
    {S::BTN_TOGGLE_DARK_MODE, StrId::STR_BTN_ACT_TOGGLE_DARK_MODE, Needs::Nothing},
    {S::BTN_AUTO_PAGE_TURN, StrId::STR_BTN_ACT_AUTO_PAGE_TURN, Needs::Nothing},
    {S::BTN_SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON, Needs::Nothing},
    {S::BTN_TOGGLE_TOUCH_LOCK, StrId::STR_BTN_ACT_TOGGLE_TOUCH_LOCK, Needs::Nothing},
    {S::BTN_LIGHT_TOGGLE, StrId::STR_BTN_ACT_LIGHT_TOGGLE, Needs::Light},
    {S::BTN_LIGHT_BRIGHTER, StrId::STR_BTN_ACT_LIGHT_BRIGHTER, Needs::Light},
    {S::BTN_LIGHT_DIMMER, StrId::STR_BTN_ACT_LIGHT_DIMMER, Needs::Light},
    {S::BTN_LIGHT_PANEL, StrId::STR_BTN_ACT_LIGHT_PANEL, Needs::Light},
    {S::BTN_LIGHT_WARMER, StrId::STR_BTN_ACT_LIGHT_WARMER, Needs::WarmLight},
    {S::BTN_LIGHT_COOLER, StrId::STR_BTN_ACT_LIGHT_COOLER, Needs::WarmLight},
};

// Every action but BTN_DEFAULT exactly once: a row that missed one could neither show nor keep a binding to it.
static_assert(std::size(ALL) == static_cast<size_t>(S::BUTTON_ACTION_COUNT) - 1,
              "ButtonActionOptions::ALL must list every BUTTON_ACTION but BTN_DEFAULT");
constexpr bool listsEveryActionOnce() {
  for (uint8_t action = 1; action < S::BUTTON_ACTION_COUNT; ++action) {
    unsigned seen = 0;
    for (const Option& option : ALL) seen += option.action == action ? 1u : 0u;
    if (seen != 1) return false;
  }
  return true;
}
static_assert(listsEveryActionOnce(), "ButtonActionOptions::ALL must list every BUTTON_ACTION but BTN_DEFAULT once");

// What this board has (ButtonActionCaps.h reads it from the board profile).
struct Caps {
  bool light = false;
  bool warmLight = false;
};

constexpr bool offered(const Needs needs, const Caps caps) {
  switch (needs) {
    case Needs::Nothing:
      return true;
    case Needs::Light:
      return caps.light;
    case Needs::WarmLight:
      return caps.warmLight;
  }
  return false;
}

// BTN_DEFAULT is always offered: it is every row's first option.
constexpr bool offered(const uint8_t action, const Caps caps) {
  if (action == S::BTN_DEFAULT) return true;
  for (const Option& option : ALL) {
    if (option.action == action) return offered(option.needs, caps);
  }
  return false;
}

// A row's options: BTN_DEFAULT first, then what the board offers, in ALL's order. Walked on every call rather than
// kept in a table: the capabilities are read at runtime (one binary runs the X3 and the X4), and a table filled then
// would be written while the web server's task reads the rows.
constexpr uint8_t optionCount(const Caps caps) {
  uint8_t count = 1;
  for (const Option& option : ALL) count += offered(option.needs, caps) ? 1 : 0;
  return count;
}

// The action option `index` stores; BTN_DEFAULT for an index past the last option.
constexpr uint8_t actionAt(const uint8_t index, const Caps caps) {
  uint8_t at = 1;
  for (const Option& option : ALL) {
    if (!offered(option.needs, caps)) continue;
    if (at == index) return option.action;
    ++at;
  }
  return S::BTN_DEFAULT;
}

// The option that stores `action`, -1 when the board does not offer it.
constexpr int optionOf(const uint8_t action, const Caps caps) {
  if (action == S::BTN_DEFAULT) return 0;
  int at = 1;
  for (const Option& option : ALL) {
    if (!offered(option.needs, caps)) continue;
    if (option.action == action) return at;
    ++at;
  }
  return -1;
}

}  // namespace ButtonActionOptions
