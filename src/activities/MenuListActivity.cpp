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

// Touch equivalent of "move the selection, then confirm", against the rows drawMenuList()
// actually painted. One place, so all eight subclasses gain it without being touched.
//
// Two-step on purpose, matching the buttons rather than firing on first contact: resting a
// finger on a row moves the highlight (Down), lifting it activates (Tap). On e-paper a
// mis-activation costs a screen the reader then has to navigate back out of, so the cheap
// half happens under the finger and the expensive half only on release.
//
// Separator rows never appear here -- the band records them as non-selectable, the same
// exclusion initMenuList()'s predicate makes for button navigation -- so a tap on a section
// heading falls through as a miss rather than selecting it.
bool MenuListActivity::handleListTouch() {
  int index = -1;
  switch (mappedInput.listTouch(index)) {
    case MappedInputManager::RowTouch::Down:
      // Range-check even though the band was recorded from this same list: the record happens
      // on the render task and menuItems can be rebuilt between that render and this tap.
      if (index < 0 || index >= static_cast<int>(menuItems.size())) return true;
      if (index != selectedIndex) {
        selectedIndex = index;
        requestUpdate();
      }
      return true;
    case MappedInputManager::RowTouch::Tap:
      if (index < 0 || index >= static_cast<int>(menuItems.size())) return true;
      selectedIndex = index;
      // Repaint even when the item did not move the selection: toggleCurrentItem() requests one
      // for a value change, but an ACTION item that opens nothing leaves the highlight where a
      // preceding Down put it, and the row under the finger must end up looking selected.
      requestUpdate();
      toggleCurrentItem();
      return true;
    case MappedInputManager::RowTouch::None:
      return false;
  }
  return false;
}
