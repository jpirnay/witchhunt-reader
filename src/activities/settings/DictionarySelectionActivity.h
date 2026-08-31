#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryRegistry.h"

class MappedInputManager;

/// Full-screen list of the StarDict dictionaries found on the SD card, plus a
/// "None" entry that clears the selection.
///
/// An activity rather than an enum picker because the options are discovered by
/// scanning /dictionaries and /.dictionaries when the list opens, so they cannot
/// be a fixed list in SettingsList.h.
class DictionarySelectionActivity final : public Activity {
 public:
  explicit DictionarySelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionarySelect", renderer, mappedInput) {}

  void onEnter() override;
  // Tap on a list row -> move the selection there; ActivityManager then synthesizes Confirm.
  bool selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Index 0 is always "None"; entry i>0 is dictionaries[i-1].
  size_t optionCount() const { return dictionaries.size() + 1; }
  std::string optionLabel(int index) const;
  void handleSelection();

  ButtonNavigator buttonNavigator;
  std::vector<DictionaryEntry> dictionaries;
  int selectedIndex = 0;
  int activeIndex = 0;
};
