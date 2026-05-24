#pragma once

#include <GfxRenderer.h>

#include "../Activity.h"
#include "SettingInfo.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/// Full-screen list of all available actions for a single button press
/// (short / double / long). Replaces in-place enum cycling for entries
/// marked with `.withSelectorActivity(SettingSelectorKind::ButtonAction)`.
class ButtonActionSelectionActivity final : public Activity {
 public:
  ButtonActionSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SettingInfo& setting)
      : Activity("BtnActSelect", renderer, mappedInput), setting(setting) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  uint8_t getActiveIndex() const;
  uint8_t getOptionCount() const;
  std::string getOptionLabel(uint8_t index) const;

  ButtonNavigator buttonNavigator;
  const SettingInfo& setting;
  int selectedIndex = 0;
  uint8_t optionCount = 0;
};
