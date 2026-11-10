#pragma once

#include <string>

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

// WAITING_CONFIRMATION asks whether to update through a ConfirmationActivity pushed above this one,
// like every other question; the answer starts the download or goes on to the packs. The other states
// are progress/result screens with nothing to answer.
//
// Once the firmware needs nothing (it is up to date, or its update was declined), the same screen checks
// the language and hyphenation packs on the card against the Font Manager's manifest, and says so in a
// line under the versions. Updates are offered the same way; Update hands the screen to the Font
// Manager, which updates the packs alone (FontDownloadActivity::Wanted::packUpdates).
class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,  // nothing to install: no newer release, or the reader declined it; the packs' line below
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };
  // The packs' line on the NO_UPDATE screen.
  enum class PackCheck : uint8_t { NotRun, Checking, UpToDate, Updates, Failed };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  uint32_t lastOtaDrawMs = 0;  // throttles progress redraws during the streaming install
  OtaUpdater updater;
  OtaUpdater::OtaUpdaterError failureReason = OtaUpdater::OK;
  PackCheck packCheck = PackCheck::NotRun;
  uint16_t packUpdates = 0;
  bool firmwareDeclined = false;  // NO_UPDATE after Cancel on the update question
  // Leaving for the Font Manager, which takes the radio over and restarts when it is left.
  bool handingOver = false;

  void onWifiSelectionComplete(bool success);
  void askToUpdate();
  void startUpdate();
  // The packs' check, from NO_UPDATE: the count of their updates, and the question when there are any.
  void checkPacks();
  void askToUpdatePacks();
  void updatePacks();
  // The packs' line for packCheck, formatted into `buf` when it holds the count; nullptr before the check.
  const char* packLine(char* buf, size_t size) const;

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS || packCheck == PackCheck::Checking;
  }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
