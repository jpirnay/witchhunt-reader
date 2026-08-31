#include "DictionarySelectionActivity.h"

#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

void DictionarySelectionActivity::onEnter() {
  Activity::onEnter();
  DictionaryRegistry::discover(dictionaries);

  // Point at what is currently selected. A configured dictionary that is no
  // longer on the card falls back to "None" in the list without clearing the
  // setting -- the card may simply not be readable this boot.
  activeIndex = 0;
  for (size_t i = 0; i < dictionaries.size(); i++) {
    if (dictionaries[i].name == SETTINGS.dictionaryName) {
      activeIndex = static_cast<int>(i) + 1;
      break;
    }
  }
  selectedIndex = activeIndex;
  requestUpdate();
}

std::string DictionarySelectionActivity::optionLabel(const int index) const {
  if (index <= 0) return std::string(tr(STR_NONE_OPT));
  return dictionaries[static_cast<size_t>(index) - 1].name;
}

void DictionarySelectionActivity::handleSelection() {
  if (selectedIndex <= 0) {
    SETTINGS.dictionaryName[0] = '\0';
  } else {
    const std::string& name = dictionaries[static_cast<size_t>(selectedIndex) - 1].name;
    strncpy(SETTINGS.dictionaryName, name.c_str(), sizeof(SETTINGS.dictionaryName) - 1);
    SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  }
  // Persist here rather than leaning on the settings screen: the reader menu
  // can send the user straight into this picker when no dictionary is set, and
  // that path never passes through SettingsActivity.
  SETTINGS.saveToFile();
  finish();
}

void DictionarySelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  int count = static_cast<int>(optionCount());
  buttonNavigator.onNextList(selectedIndex, count, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, count, [this] { requestUpdate(); });
}

void DictionarySelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(StrId::STR_DICTIONARY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  const int active = activeIndex;
  GUI.drawList(
      renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, static_cast<int>(optionCount()),
      selectedIndex, [this](int index) { return optionLabel(index); }, nullptr, nullptr,
      [active](int index) -> std::string { return index == active ? tr(STR_SELECTED) : ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
