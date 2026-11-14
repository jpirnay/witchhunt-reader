#pragma once

#include <string>

#include "activities/Activity.h"

/// The emergency-mode notice (FlashLayoutNotice): what is wrong with the board's partition table, what that costs, and
/// the web flasher's address with its QR code. Shown once, on the way to Home; Back or Confirm marks it read and goes
/// Home.
class FlashLayoutNoticeActivity final : public Activity {
 public:
  FlashLayoutNoticeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FlashLayoutNotice", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string whatIsWrong_;  // built in onEnter(): render() only draws
};
