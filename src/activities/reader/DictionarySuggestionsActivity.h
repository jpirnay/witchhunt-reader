#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "util/Dictionary.h"

// "Did you mean": the headwords the dictionary has near a word it does not (Dictionary::suggest), under the word that
// missed. Confirm hands the chosen one back as a DictionarySuggestionResult for DictionaryWordSelectActivity to look up
// and show; Back returns to the page.
class DictionarySuggestionsActivity final : public UiListActivity {
 public:
  DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string missedWord,
                                const DictSuggestions& suggestions)
      : UiListActivity("DictionarySuggestions", renderer, mappedInput),
        missedWord(std::move(missedWord)),
        suggestions(suggestions) {}

  void onEnter() override;
  // The word and the headwords are the book's language: a script the UI faces lack draws in the book's font.
  bool drawsBookText() const override { return true; }

 private:
  int listCount() const override { return suggestions.count; }
  const char* headerTitle() const override;
  void drawChrome() override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;

  const std::string missedWord;  // not `word`: Arduino.h defines word() as a macro
  const DictSuggestions suggestions;
  std::vector<freeink::ui::ListItem> rowItems;
};
