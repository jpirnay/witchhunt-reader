#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity showing the list of configured OPDS servers.
 * Allows adding new servers and editing/deleting existing ones.
 * When pickerMode is true, selecting a server navigates to the OPDS browser
 * instead of opening the editor (used from the home screen).
 */
class OpdsServerListActivity final : public Activity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false,
                                  std::string initialQuery = {})
      : Activity("OpdsServerList", renderer, mappedInput),
        pickerMode(pickerMode),
        initialQuery_(std::move(initialQuery)) {}

  void onEnter() override;
  void onExit() override;
  // Tap on a list row -> move the selection there; ActivityManager then synthesizes Confirm.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool pickerMode = false;
  std::string initialQuery_;

  int getItemCount() const;
  void handleSelection();
};
