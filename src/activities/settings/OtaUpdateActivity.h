#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "network/OtaUpdater.h"

// The WAITING_CONFIRMATION state asks a yes/no question, so it draws the shared ConfirmDialog
// (see components/ConfirmDialog.h) rather than hand-placed text plus a hint strip. The other seven
// states are progress/result screens with nothing to answer and are unchanged.
class OtaUpdateActivity : public Activity, private UiAppHost {
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

  // Version lines for the confirmation dialog. Members, not locals: OptionDialogProps stores a
  // pointer and the draw happens after the screen fn returns.
  std::string updateDialogBody;

  static void confirmScreen(UiScreen& screen, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onUpdateEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildConfirmScreen(UiScreen& screen);
  // Everything the Confirm answer sets in motion, so the key press and the touch target cannot
  // drift apart.
  void startUpdate();

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), UiAppHost(renderer), updater() {}
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
