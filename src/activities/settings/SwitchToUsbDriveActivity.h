#pragma once

#include "activities/Activity.h"

// Confirms with the user, then switches the boot partition to the MSC firmware
// in app1 and restarts. On return from the confirmation (cancelled or app1 not
// present), finishes normally so the settings screen reappears.
class SwitchToUsbDriveActivity final : public Activity {
 public:
  explicit SwitchToUsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SwitchToUsbDrive", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override {}
  void render(RenderLock&&) override {}
};
