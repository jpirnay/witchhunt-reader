#include "TabbedUiListActivity.h"

#include "CrossPointSettings.h"
#include "MappedInputManager.h"

namespace fui = freeink::ui;

void TabbedUiListActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(ACTION_USER, &TabbedUiListActivity::tabActionTrampoline, this);
  // Open on the bar, not on a row: the first thing a reader does here is pick a tab, and
  // starting with row 0 highlighted invites a Confirm that toggles a setting they never chose.
  activeNav().selected = -1;
}

void TabbedUiListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<TabbedUiListActivity*>(user);
  self->app.clearTapFlash();
  self->selectTab(event.value);
}

void TabbedUiListActivity::selectTab(const int slot) {
  if (slot < 0 || slot >= tabCount()) return;
  selectedTabSlot = slot;
  onTabSelected(slot);
  focusTabs();
}

void TabbedUiListActivity::focusTabs() {
  activeNav().selected = -1;
  listTapActivation.reset();
  requestUpdate();
}

void TabbedUiListActivity::buildTabBar(UiScreen& screen) {
  const int count = std::min(tabCount(), MAX_TABS);
  fui::TabItem tabs[MAX_TABS]{};
  for (int slot = 0; slot < count; ++slot) {
    tabs[slot].label = tabLabel(slot);
    tabs[slot].value = static_cast<int16_t>(slot);
    tabs[slot].selected = slot == selectedTabSlot;
  }

  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = static_cast<uint8_t>(count);
  props.action = ACTION_USER;
  props.inputMask = fui::InputTouch;
  props.text = screen.theme().bodyText;
  props.tabInset = fui::Insets{};
  props.contentInset = fui::Insets{};

  // The selected tab reads as filled while the bar holds focus and as a dithered pill once the
  // focus has moved into the list -- so at a glance the bar says whether the next Confirm
  // changes tabs or acts on a row. This is the one piece of styling that is not decoration, so
  // it is set here rather than left to customizeTabBar().
  //
  // Adapted from CrossInk's FreeInkUI tab composition at commit cd4b122e (MIT):
  // https://github.com/uxjulia/crossink
  const bool onTabs = tabsFocused();
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  tabStyles.normal.border = fui::Paint::solid(fui::Color::Black);
  tabStyles.normal.borderWidth = 1;
  tabStyles.selected.background =
      onTabs ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::LightGray);
  tabStyles.selected.foreground = fui::Paint::solid(onTabs ? fui::Color::White : fui::Color::Black);
  tabStyles.selected.border = fui::Paint::solid(fui::Color::Black);
  tabStyles.selected.borderWidth = 1;
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  props.tabStyles = tabStyles;

  customizeTabBar(screen, props);
  fui::tabBar(screen.frame(), screen.takeTop(tabBarHeight()), props);
}

bool TabbedUiListActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (tabsFocused()) {
      onBackFromTabs();
    } else {
      focusTabs();
    }
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      selectTab(ButtonNavigator::nextIndex(selectedTabSlot, tabCount()));
    } else {
      activateIndex(activeNav().selected);
    }
    return true;
  }
  return false;
}

// Up/Down walk the bar and the rows as one range: position 0 is the bar, position i+1 is row i.
// Stepping in that shifted frame is what makes walking up off row 0 land on the bar instead of
// wrapping to the bottom of the list.
void TabbedUiListActivity::stepSelection(const bool forward) {
  const int count = listCount();
  const auto selectable = [this](const int shifted) { return shifted == 0 || isRowSelectable(shifted - 1); };
  const int current = activeNav().selected + 1;
  const int next = forward ? ButtonNavigator::nextIndex(current, count + 1, selectable)
                           : ButtonNavigator::previousIndex(current, count + 1, selectable);
  activeNav().selected = next - 1;
  if (activeNav().selected >= 0) activeNav().follow(count);
  listTapActivation.reset();
  requestUpdate();
}

void TabbedUiListActivity::navigateButtons() {
  buttonNavigator.onNextRelease([this] { stepSelection(true); });
  buttonNavigator.onPreviousRelease([this] { stepSelection(false); });
  buttonNavigator.onNextContinuous([this] { selectTab(ButtonNavigator::nextIndex(selectedTabSlot, tabCount())); });
  buttonNavigator.onPreviousContinuous(
      [this] { selectTab(ButtonNavigator::previousIndex(selectedTabSlot, tabCount())); });
}

ListRowTap::Result TabbedUiListActivity::selectListRow(const int index) {
  if (index >= 0 && index < listCount() && !isRowSelectable(index)) return ListRowTap::Result::Rejected;
  return ListRowTap::apply(index, listCount(), activeNav().selected);
}
