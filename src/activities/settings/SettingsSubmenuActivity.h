#pragma once
#include <I18n.h>

#include <functional>
#include <vector>

#include "SettingInfo.h"
#include "activities/MenuListActivity.h"

// Displays a flat list of SettingInfo items launched from a SettingsActivity submenu entry.
// Supports subcategory separators (withSubcategory) exactly as the parent settings tabs do.
class SettingsSubmenuActivity final : public MenuListActivity {
  StrId titleId;
  std::function<std::string(const SettingInfo&)> itemValueStringOverride;
  bool needsHalfRefresh = false;
  bool persistSettingsOnChange = true;

  // MenuListActivity overrides
  void onEnter() override;
  void toggleCurrentItem() override;
  void onActionSelected(int index) override;
  void onSettingToggled(int index) override;
  std::string getItemValueString(int index) const override;
  // A row changed: the settings that depend on it follow, and the file is saved (when this submenu saves at all).
  void persistChange();

 public:
  explicit SettingsSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                                   std::vector<SettingInfo> items,
                                   std::function<std::string(const SettingInfo&)> itemValueStringOverride = {},
                                   bool persistSettingsOnChange = true)
      : MenuListActivity("SettingsSubmenu", renderer, mappedInput),
        titleId(titleId),
        itemValueStringOverride(std::move(itemValueStringOverride)),
        persistSettingsOnChange(persistSettingsOnChange) {
    menuItems = std::move(items);
  }

  void render(RenderLock&&) override;
  bool releaseBuriedState() override {
    std::vector<SettingInfo>().swap(menuItems);  // a copy of the submenu's rows
    return true;
  }
};
