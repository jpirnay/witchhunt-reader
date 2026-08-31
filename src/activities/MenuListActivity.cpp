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
  handleNavigation();
}

bool MenuListActivity::selectListRow(const int index) {
  // Range-checked against the CURRENT items, not the ones the render saw: the band is recorded
  // on the render task and menuItems can be rebuilt between that render and this tap.
  if (index < 0 || index >= static_cast<int>(menuItems.size())) return false;
  // Separators are already excluded by the band (recorded non-selectable, the same exclusion
  // initMenuList()'s predicate makes for button navigation); declining here as well is belt and
  // braces for a list rebuilt between the render and the tap.
  if (menuItems[index].isSeparator) return false;
  selectedIndex = index;
  return true;
}
