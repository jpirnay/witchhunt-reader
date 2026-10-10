#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"

// WARNING asks whether to clear through a ConfirmationActivity pushed above this one, like every
// other question; the answer starts the clearing or leaves. The other states are progress/result
// screens with nothing to answer.
class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  void clearCache();

  void startClearing();
};
