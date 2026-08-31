#pragma once

#include <vector>

#include "../Activity.h"
#include "MdReaderActivity.h"
#include "util/ButtonNavigator.h"

class MdReaderTocSelectionActivity final : public Activity {
  std::vector<MdHeading> headings;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  int getPageItems() const;
  int getTotalItems() const;

 public:
  explicit MdReaderTocSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::vector<MdHeading> headings, int currentHeadingIndex)
      : Activity("MdReaderTocSelection", renderer, mappedInput), headings(std::move(headings)), selectorIndex(0) {
    if (currentHeadingIndex >= 0 && currentHeadingIndex < static_cast<int>(headings.size())) {
      selectorIndex = currentHeadingIndex;
    }
  }

  void onEnter() override;
  void onExit() override;
  // Tap on a heading row -> move the selection there; ActivityManager synthesizes Confirm.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;
};
