#pragma once

#include "components/UiAppHost.h"

// The text tab bar the tabbed screens share: Settings, the reader menu and the Library.
namespace ListTabBar {

constexpr int16_t HEIGHT = 54;

// Props for `count` tabs (the caller's array, alive until the bar is drawn); a tap on one fires
// `action` with that tab's value. The selected tab is filled while the bar holds focus and a dithered
// pill while the focus is in the list, so the bar says whether the next Confirm changes tabs or acts
// on a row.
freeink::ui::TabBarProps props(UiAppHost::UiScreen& screen, const freeink::ui::TabItem* tabs, uint8_t count,
                               freeink::ui::ActionId action, bool barFocused);

}  // namespace ListTabBar
