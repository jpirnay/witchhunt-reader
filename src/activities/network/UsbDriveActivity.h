#pragma once

#include <HalStorage.h>

#include "activities/Activity.h"

// USB Drive: presents the SD card to a computer as an ordinary removable disk,
// so books go on and come off with the host's file manager instead of a
// transfer protocol. Ported from crosspoint-reader PR #3203 (Julia,
// julia@uxj.io); the UI is rewritten for this fork's GUI/theme layer.
//
// Unlike every other activity, this one gives the raw card away: HalStorage
// unmounts the filesystem and TinyUSB serves sectors straight to the host. So
// while it runs:
//  - requiresExclusiveStorageLoop() makes main.cpp and ActivityManager stand
//    down (no sleep, no screenshots, no shortcuts, no navigation), because any
//    filesystem user would be reading a volume that is not mounted.
//  - it never transitions out. Every exit path is restartToHomeAfterStorageHandoff(),
//    a reboot — the host may have rewritten anything, so every cache the
//    firmware holds is suspect and only a clean boot is honest.
//
// The screen is deliberately static: the render task and the TinyUSB task are
// the only things running, and a paint here is a 1-2 s blocking e-ink refresh,
// so it repaints on state changes only.
class UsbDriveActivity final : public Activity {
 public:
  UsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbDrive", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool requiresExclusiveStorageLoop() const override { return true; }
  // Sleeping while a host holds the card would cut the SD rail underneath it.
  // The IoError case counts too: the forced-disconnect handshake is in flight.
  bool preventAutoSleep() override { return state == UsbDriveState::Connected || (!startFailed && isIoError()); }
  // Static screen — the minute-tick repaint would hold the render mutex for a
  // full refresh and has nothing new to show.
  bool shouldSkipPeriodicUpdate() const override { return true; }

 private:
  // No host has enumerated us in five minutes: the user most likely walked away
  // without plugging anything in. Reboot rather than sit with the card detached.
  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  // MSC never came up; show why, then leave on its own if nobody presses Back.
  static constexpr unsigned long START_FAILURE_TIMEOUT_MS = 30UL * 1000UL;
  // How long to wait for tud_disconnect() to actually drop the host after an
  // I/O error before giving up and rebooting out from under it.
  static constexpr unsigned long FORCED_DISCONNECT_TIMEOUT_MS = 1000UL;

  bool isIoError() const { return state == UsbDriveState::IoError; }
  void restartToHome();

  UsbDriveState state = UsbDriveState::Unsupported;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool forcedDisconnectRequested = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long forcedDisconnectRequestedAt = 0;
};
