#include "DictionarySuggestionsActivity.h"

#include <I18n.h>

#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void DictionarySuggestionsActivity::onEnter() {
  rowItems.clear();
  rowItems.reserve(suggestions.count);
  for (uint8_t i = 0; i < suggestions.count; ++i) {
    fui::ListItem item;
    item.label = suggestions.words[i];
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
  UiListActivity::onEnter();
}

const char* DictionarySuggestionsActivity::headerTitle() const { return tr(STR_DICT_DID_YOU_MEAN); }

void DictionarySuggestionsActivity::drawChrome() {
  GUI.drawHeader(renderer, listHeaderRect(), headerTitle(), missedWord.c_str());
}

void DictionarySuggestionsActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen, 0, 0, /*spacer=*/true, missedWord.c_str());
  auto props = listProps(screen);
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  addList(screen, props);
}

void DictionarySuggestionsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index < 0 || index >= suggestions.count) return;
  setResult(DictionarySuggestionResult{std::string(suggestions.words[index])});
  finish();
}
