#include "ListTabBar.h"

namespace fui = freeink::ui;

namespace ListTabBar {

fui::TabBarProps props(UiAppHost::UiScreen& screen, const fui::TabItem* tabs, const uint8_t count,
                       const fui::ActionId action, const bool barFocused) {
  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = count;
  props.action = action;
  props.inputMask = fui::InputTouch;
  props.text = screen.theme().bodyText;
  props.tabInset = fui::Insets{};
  props.contentInset = fui::Insets{};

  // The focus-dependent selected styling is the one piece that is not decoration, so it is set here
  // rather than left to a screen's own touches.
  //
  // Adapted from CrossInk (MIT): https://github.com/uxjulia/crossink -- the FreeInkUI tab
  // composition and touch tab routing at commit cd4b122e, which the settings screen and the
  // reader menu had each taken a copy of. The reader menu's icon-tab treatment (commit 60cc4da5)
  // stays with it, in its customizeTabBar().
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  tabStyles.normal.border = fui::Paint::solid(fui::Color::Black);
  tabStyles.normal.borderWidth = 1;
  tabStyles.selected.background =
      barFocused ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::LightGray);
  tabStyles.selected.foreground = fui::Paint::solid(barFocused ? fui::Color::White : fui::Color::Black);
  tabStyles.selected.border = fui::Paint::solid(fui::Color::Black);
  tabStyles.selected.borderWidth = 1;
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  props.tabStyles = tabStyles;
  return props;
}

}  // namespace ListTabBar
