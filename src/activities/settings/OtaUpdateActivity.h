#pragma once

#include <string>

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

// WAITING_CONFIRMATION asks whether to update through a ConfirmationActivity pushed above this one,
// like every other question; the answer starts the download or leaves. The other states are
// progress/result screens with nothing to answer.
class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  uint32_t lastOtaDrawMs = 0;  // throttles progress redraws during the streaming install
  OtaUpdater updater;
  OtaUpdater::OtaUpdaterError failureReason = OtaUpdater::OK;

  void onWifiSelectionComplete(bool success);
  void askToUpdate();
  void startUpdate();

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
