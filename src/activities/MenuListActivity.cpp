#include "MenuListActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "settings/SettingsSubmenuActivity.h"

void MenuListActivity::initMenuList() {
  const int count = static_cast<int>(menuItems.size());
  const auto pred = UITheme::makeSelectablePredicate(count, [this](int i) { return menuItems[i].getTitle(); });
  buttonNavigator.setSelectablePredicate(pred, count);
  if (count > 0 && !pred(selectedIndex)) {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
  }
}

void MenuListActivity::onEnter() {
  Activity::onEnter();
  if (!submenusPrepared) {
    prepareSubmenus();
    SettingInfo::insertSubcategorySeparators(menuItems);
    submenusPrepared = true;
  }
  initMenuList();
  requestUpdate();
}

void MenuListActivity::handleNavigation() {
  const int count = static_cast<int>(menuItems.size());
  // Up/Down step, Left/Right page by whatever the last render fit on screen. A menu shorter than
  // one page pages by a single item, so short menus are unchanged.
  buttonNavigator.onNextList(selectedIndex, count, [this] { requestUpdate(); }, listView.visibleRows);
  buttonNavigator.onPreviousList(selectedIndex, count, [this] { requestUpdate(); }, listView.visibleRows);
}

void MenuListActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const auto& item = menuItems[selectedIndex];
  if (item.isSeparator) return;

  if (item.type == SettingType::ACTION) {
    if (item.action == SettingAction::Submenu) {
      openSubmenu(item);
      return;
    }
    onActionSelected(selectedIndex);
    return;
  }

  menuItems[selectedIndex].toggleValue();
  onSettingToggled(selectedIndex);
  requestUpdate();
}

std::string MenuListActivity::getItemValueString(int index) const { return menuItems[index].getDisplayValue(); }

void MenuListActivity::drawMenuList(const Rect& rect) {
  const int count = static_cast<int>(menuItems.size());
  GUI.drawList(
      renderer, rect, count, selectedIndex, [this](int index) { return menuItems[index].getTitle(); }, nullptr, nullptr,
      [this](int index) { return getItemValueString(index); }, true, &listView);
}

void MenuListActivity::prepareSubmenus() { SettingInfo::prepareSubmenus(menuItems, submenuData); }

void MenuListActivity::openSubmenu(const SettingInfo& submenuEntry) {
  auto it = std::find_if(submenuData.begin(), submenuData.end(),
                         [&submenuEntry](const SettingInfo::SubmenuData& d) { return d.id == submenuEntry.nameId; });
  if (it == submenuData.end()) return;

  startActivityForResult(
      std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, submenuEntry.nameId, it->items),
      [this](const ActivityResult&) { requestUpdate(); });
}

void MenuListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackPressed();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentItem();
    return;
  }
  if (handleListTouch()) return;
  handleNavigation();
}

// Activate the row under the finger, against the rows drawMenuList() actually painted. One
// place, so all eight subclasses gain it without being touched.
//
// Tap only, and NOT the two-step "Down moves the highlight, Tap activates" that the buttons
// imply. Down fires once a contact has been held TOUCH_DOWN_SELECT_DELAY_MS (90 ms), which an
// ordinary finger tap comfortably exceeds -- so acting on it would move the selection, repaint,
// and then activate and repaint again: two full e-paper refreshes, roughly a second, for one
// tap. The first of those is meant to be feedback, but at that latency it lands after the
// finger has already gone, so it is pure cost.
//
// Down is still claimed (returns true) rather than ignored, so no other consumer treats the
// same contact as its own. The real answer to tap feedback is a cheap localised invert --
// upstream's setFlash/clearTapFlash -- which this fork does not have yet; it is P2 in
// docs/touch-input-migration-2026-08-14.md. Until it exists, one refresh per tap is the right
// trade.
//
// Separator rows never appear here -- the band records them as non-selectable, the same
// exclusion initMenuList()'s predicate makes for button navigation -- so a tap on a section
// heading falls through as a miss rather than selecting it.
bool MenuListActivity::handleListTouch() {
  int index = -1;
  switch (mappedInput.listTouch(index)) {
    case MappedInputManager::RowTouch::Down:
      return true;  // claimed, deliberately without a repaint -- see above
    case MappedInputManager::RowTouch::Tap:
      // Range-check even though the band was recorded from this same list: the record happens
      // on the render task and menuItems can be rebuilt between that render and this tap.
      if (index < 0 || index >= static_cast<int>(menuItems.size())) return true;
      selectedIndex = index;
      // Repaint even when toggleCurrentItem() does not: an ACTION item that opens nothing still
      // has to leave the highlight on the row the finger landed on.
      requestUpdate();
      toggleCurrentItem();
      return true;
    case MappedInputManager::RowTouch::None:
      return false;
  }
  return false;
}
