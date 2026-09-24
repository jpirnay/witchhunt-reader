#include "HomeMoreActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "GlobalBookmarkIndex.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"

HomeMoreActivity::HomeMoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : MenuListActivity("HomeMore", renderer, mappedInput) {
  const HomeMenuAvailability availability{.hasBookmarks = !GLOBAL_BOOKMARKS.isEmpty(),
                                          .hasOpdsServers = OPDS_STORE.hasServers()};
  std::vector<HomeMenuEntry> entries;
  entries.reserve(7);
  collectHomeMenuEntries(HomeMenuPlacement::More, availability, entries);

  menuItems.reserve(entries.size());
  actions.reserve(entries.size());
  for (const HomeMenuEntry& entry : entries) {
    menuItems.push_back(SettingInfo::Action(entry.label, SettingAction::None));
    actions.push_back(entry.action);
  }
}

void HomeMoreActivity::onActionSelected(const int index) {
  if (index < 0 || index >= static_cast<int>(actions.size())) return;
  activityManager.goToHomeMenuAction(actions[index]);
}

void HomeMoreActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_MORE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing * 2;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
