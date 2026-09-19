#pragma once

#include "activities/MenuListActivity.h"
#include "activities/reader/KOReaderAutoSync.h"

/**
 * Submenu for KOReader Sync settings.
 * Shows username, password, server URL, document matching, authenticate, and register.
 */
class KOReaderSettingsActivity final : public MenuListActivity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void render(RenderLock&&) override;

 private:
  void buildMenuItems();
  void buildAutoSyncItems();

  // MenuListActivity overrides
  std::string getItemValueString(int index) const override;
  void onActionSelected(int index) override;
};
