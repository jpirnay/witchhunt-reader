#include "EnumSelectionActivity.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

uint8_t EnumSelectionActivity::optionCount() const {
  return overrideCount > 0 ? overrideCount : setting.getEnumOptionCount();
}

std::string EnumSelectionActivity::optionLabel(uint8_t index) const {
  if (labelOverride) {
    std::string label = labelOverride(index);
    if (!label.empty()) return label;
  }
  return setting.getEnumOptionLabel(index);
}

void EnumSelectionActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = static_cast<int>(setting.getEnumSelectedIndex());
  const int count = static_cast<int>(optionCount());
  if (selectedIndex >= count) selectedIndex = 0;  // clamp stale/out-of-range persisted value
  requestUpdate();
}

void EnumSelectionActivity::onExit() { Activity::onExit(); }

void EnumSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int count = static_cast<int>(optionCount());
  buttonNavigator.onNextList(selectedIndex, count, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, count, [this] { requestUpdate(); });
}

ListRowTap::Result EnumSelectionActivity::selectListRow(const int index) {
  // Point-then-confirm: the first tap moves the highlight, a tap on the row
  // already highlighted lets ActivityManager synthesize Confirm, which runs
  // loop()'s own handleSelection() rather than a parallel copy of it.
  return ListRowTap::apply(index, static_cast<int>(optionCount()), selectedIndex);
}

void EnumSelectionActivity::handleSelection() {
  setting.setEnumSelectedIndex(static_cast<uint8_t>(selectedIndex));
  finish();
}

void EnumSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(setting.nameId));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  const int count = static_cast<int>(optionCount());
  const uint8_t activeIndex = setting.getEnumSelectedIndex();
  GUI.drawList(
      renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, count, selectedIndex,
      [this](int index) { return optionLabel(static_cast<uint8_t>(index)); }, nullptr, nullptr,
      [activeIndex](int index) -> std::string {
        return index == static_cast<int>(activeIndex) ? tr(STR_SELECTED) : "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
