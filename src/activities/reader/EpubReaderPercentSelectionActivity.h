#pragma once

#include "MappedInputManager.h"
#include "activities/SliderPickerActivity.h"

// Thin wrapper that launches the generic slider for book-percent navigation.
class EpubReaderPercentSelectionActivity final : public SliderPickerActivity {
 public:
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent)
      : SliderPickerActivity(renderer, mappedInput,
                             SliderPickerActivity::Config{
                                 .titleId = StrId::STR_GO_TO_PERCENT,
                                 .hintId = StrId::STR_PERCENT_STEP_HINT,
                                 .minValue = 0,
                                 .maxValue = 100,
                                 .initialValue = initialPercent,
                                 .suffix = "%",
                             }) {}
};
