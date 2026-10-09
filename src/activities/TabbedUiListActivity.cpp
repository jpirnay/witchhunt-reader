#include "TabbedUiListActivity.h"

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"

namespace fui = freeink::ui;

namespace {
// A tabbed list switches tabs on a long Up/Down instead of jumping to its ends (spec R4).
ListDeclaration tabbedDeclaration() {
  ListDeclaration declaration;
  declaration.tabbed = true;
  return declaration;
}
}  // namespace

TabbedUiListActivity::TabbedUiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity(name, renderer, mappedInput, tabbedDeclaration()) {}

void TabbedUiListActivity::onEnter() {
  // One ListNav per tab, so each remembers its own row and scroll position; all open on the bar.
  // Sized before the base onEnter(), which resets the active one.
  tabNavs.assign(static_cast<size_t>(std::max(1, tabCount())), fui::ListNav{});
  for (auto& tab : tabNavs) tab.reset(-1);
  UiListActivity::onEnter();
  app.on(ACTION_USER, &TabbedUiListActivity::tabActionTrampoline, this);
  // Open on the bar, not on a row: the first thing a reader does here is pick a tab, and
  // starting with row 0 highlighted invites a Confirm that toggles a setting they never chose.
  activeNav().reset(-1);
}

fui::ListNav& TabbedUiListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // before onEnter() sized them
  const auto slot = static_cast<size_t>(selectedTabSlot);
  return tabNavs[slot < tabNavs.size() ? slot : 0];
}

void TabbedUiListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<TabbedUiListActivity*>(user);
  self->app.clearTapFlash();
  self->selectTab(event.value);
}

void TabbedUiListActivity::selectTab(const int slot) { enterTab(slot, true); }

void TabbedUiListActivity::enterTab(const int slot, const bool barFocus) {
  if (slot < 0 || slot >= tabCount()) return;
  selectedTabSlot = slot;
  onTabSelected(slot);
  resetPublishedWindow();
  if (barFocus) {
    focusTabs();
    return;
  }
  listTapActivation.reset();
  requestUpdate();
}

void TabbedUiListActivity::focusTabs() {
  activeNav().requestSelection(-1);
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

  // Filled while the bar holds focus, a dithered pill once the focus has moved into the list.
  auto props = ListTabBar::props(screen, tabs, static_cast<uint8_t>(count), ACTION_USER, tabsFocused());
  customizeTabBar(screen, props);
  fui::tabBar(screen.frame(), screen.takeTop(tabBarHeight()), props);
}

int TabbedUiListActivity::selectedPosition() const {
  // activeNav() hands out a mutable reference; reading through it changes nothing.
  return const_cast<TabbedUiListActivity*>(this)->activeNav().selected + 1;
}

void TabbedUiListActivity::selectPosition(const int position) {
  // requestSelection() rather than writing the viewport here: `top` belongs to the render task,
  // and the deferred pull resolves it in syncToProps -- including the bar (selection -1), which
  // snaps the viewport to the top.
  activeNav().requestSelection(position - 1);
  if (position > 0) onSelectionChanged(position - 1);
  listTapActivation.reset();
  requestUpdate();
}

void TabbedUiListActivity::showPositionPage(const int position, const int topPosition) {
  if (position <= 0) {
    selectPosition(0);
    return;
  }
  // The inverse of positionWindow(): a screen starting at the bar starts at row 0.
  showRowPage(position - 1, topPosition > 0 ? topPosition - 1 : 0);
  listTapActivation.reset();
}

ListWindow TabbedUiListActivity::positionWindow() const {
  const ListWindow rows = publishedWindow();
  ListWindow window;
  // The bar sits in front of row 0, so the first screen shows it as well.
  window.top = rows.top == 0 ? 0 : rows.top + 1;
  window.drawn = rows.top == 0 ? rows.drawn + 1 : rows.drawn;
  return window;
}

bool TabbedUiListActivity::isPositionSelectable(const int position) const {
  return position == 0 || isRowSelectable(position - 1);
}

void TabbedUiListActivity::activatePosition(const int position, bool /*longPress*/) {
  if (position == 0) {
    // Confirm on the bar moves to the next tab.
    selectTab((selectedTabSlot + 1) % std::max(1, tabCount()));
    return;
  }
  if (position - 1 < listCount()) activateIndex(position - 1);
}

void TabbedUiListActivity::backFromPosition(const int position) {
  if (position == 0) {
    onBackFromTabs();
  } else {
    focusTabs();
  }
}

void TabbedUiListActivity::switchTab(const int direction) {
  const int count = tabCount();
  if (count <= 0) return;
  enterTab(((selectedTabSlot + direction) % count + count) % count, /*barFocus=*/false);
}

ListRowTap::Result TabbedUiListActivity::selectListRow(const int index) {
  if (index >= 0 && index < listCount() && !isRowSelectable(index)) return ListRowTap::Result::Rejected;
  return ListRowTap::apply(index, listCount(), activeNav().selected);
}
