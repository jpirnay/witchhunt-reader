#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"

class ButtonRemapActivity final : public Activity {
 public:
  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ButtonRemap", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  // Deliberately does NOT implement selectListRow(): this screen is a wizard that assigns
  // each role from the physical button the reader presses, so its list is a progress
  // display, not a chooser. Accepting a tap would move currentStep AND have
  // ActivityManager synthesize a Confirm press -- which is exactly the input this screen
  // exists to capture, so a tap would record itself as the user's chosen button.

  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Rendering task state.

  // Index of the logical role currently awaiting input.
  uint8_t currentStep = 0;
  // Temporary mapping from logical role -> hardware button index.
  uint8_t tempMapping[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  // Error banner timing (used when reassigning duplicate buttons).
  unsigned long errorUntil = 0;
  std::string errorMessage;

  // Commit temporary mapping to settings.
  void applyTempMapping();
  // Returns false if a hardware button is already assigned to a different role.
  bool validateUnassigned(uint8_t pressedButton);
  // Labels for UI display.
  const char* getRoleName(uint8_t roleIndex) const;
  const char* getHardwareName(uint8_t buttonIndex) const;
};
