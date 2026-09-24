#pragma once

#include <vector>

#include "../MenuListActivity.h"
#include "HomeMenu.h"

// The home screen's "More" entry: the destinations the user moved off the home screen in
// Settings > Display > Home screen. Replaces Home rather than stacking on it, like every other
// home entry, so Home's cover work is torn down first; Back returns through the ReturnHint Home
// set, which re-focuses the More row.
class HomeMoreActivity final : public MenuListActivity {
 public:
  HomeMoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void render(RenderLock&&) override;

 private:
  // Parallel to menuItems: the destination each row opens.
  std::vector<HomeMenuAction> actions;

  void onActionSelected(int index) override;
};
