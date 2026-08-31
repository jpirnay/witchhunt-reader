#pragma once
#include <I18n.h>

#include <vector>

#include "SettingInfo.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SettingsActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedCategoryIndex = 0;  // Currently selected category
  int selectedSettingIndex = 0;
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  std::vector<SettingInfo::SubmenuData> submenuData;
  bool needsHalfRefresh = false;

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();
  [[nodiscard]] bool isListItemSelectable(int settingIdx) const;

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Settings", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  // Tap on a settings row. NOTE the +1: index 0 of selectedSettingIndex is the category
  // tab, and render() hands drawList `selectedSettingIndex - 1`, so band row i is row i+1
  // here. Tapping cannot reach the tab row -- that is what the tab bar is for.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;
};
