#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Reader status bar configuration activity
class StatusBarSettingsActivity final : public Activity {
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("StatusBarSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  // Tap on a list row -> move the selection there; ActivityManager then synthesizes Confirm.
  bool selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

  void handleSelection();
};
