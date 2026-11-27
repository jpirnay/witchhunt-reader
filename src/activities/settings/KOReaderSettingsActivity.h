#pragma once

#include "KOReaderAuthActivity.h"
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
  // Authenticate or Register: over this screen where the check ends without a restart, in place of it where it does.
  void openAccountCheck(KOReaderAuthActivity::Mode mode);

  // MenuListActivity overrides
  std::string getItemValueString(int index) const override;
  void onActionSelected(int index) override;
};
