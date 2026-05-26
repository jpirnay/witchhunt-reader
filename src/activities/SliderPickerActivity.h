#pragma once

#include <I18nKeys.h>

#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Generic slider picker (0–N range, configurable label, suffix, zero label).
// Returns a PercentResult containing the selected value.
class SliderPickerActivity : public Activity {
 public:
  struct Config {
    StrId titleId;  // Heading shown at top
    StrId hintId;   // Step hint shown below slider
    int minValue = 0;
    int maxValue = 100;
    int initialValue = 0;
    // Suffix appended after the numeric display (e.g. " min", " pages").
    std::string suffix;
    // Label used instead of the numeric value when value == minValue.
    // Empty string = show numeric value even at min.
    std::string zeroLabel;
  };

  explicit SliderPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Config config)
      : Activity("SliderPicker", renderer, mappedInput),
        value(std::max(config.minValue, std::min(config.maxValue, config.initialValue))),
        cfg(std::move(config)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int value;
  Config cfg;
  ButtonNavigator buttonNavigator;

  void adjustValue(int delta);
};
