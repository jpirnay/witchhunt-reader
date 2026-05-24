#include "ButtonActionSelectionActivity.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

void ButtonActionSelectionActivity::onEnter() {
  Activity::onEnter();
  optionCount = getOptionCount();
  selectedIndex = static_cast<int>(getActiveIndex());
  if (selectedIndex >= optionCount) selectedIndex = 0;
  requestUpdate();
}

void ButtonActionSelectionActivity::onExit() { Activity::onExit(); }

void ButtonActionSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNextList(selectedIndex, optionCount, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, optionCount, [this] { requestUpdate(); });
}

void ButtonActionSelectionActivity::handleSelection() {
  if (setting.valuePtr) {
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(selectedIndex);
  } else if (setting.valueSetter) {
    setting.callValueSetter(static_cast<uint8_t>(selectedIndex));
  }
  finish();
}

uint8_t ButtonActionSelectionActivity::getActiveIndex() const {
  if (setting.valuePtr) return SETTINGS.*(setting.valuePtr);
  if (setting.valueGetter) return setting.callValueGetter();
  return 0;
}

uint8_t ButtonActionSelectionActivity::getOptionCount() const {
  if (!setting.enumLabels.empty()) return static_cast<uint8_t>(setting.enumLabels.size());
  return static_cast<uint8_t>(setting.enumValues.size());
}

std::string ButtonActionSelectionActivity::getOptionLabel(uint8_t index) const {
  if (!setting.enumLabels.empty()) {
    if (index < setting.enumLabels.size()) return setting.enumLabels[index];
    return {};
  }
  if (index < setting.enumValues.size()) return std::string(I18N.get(setting.enumValues[index]));
  return {};
}

void ButtonActionSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(setting.nameId));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  const uint8_t activeIndex = getActiveIndex();
  GUI.drawList(
      renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, optionCount, selectedIndex,
      [this](int index) { return getOptionLabel(static_cast<uint8_t>(index)); }, nullptr, nullptr,
      [activeIndex](int index) -> std::string { return index == activeIndex ? tr(STR_SELECTED) : ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
