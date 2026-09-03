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

  // --- Cable-gone detection ------------------------------------------------
  // The ESP32-S3 cannot report an unplug through TinyUSB (Arduino inits the PHY
  // with otg_io_conf = NULL, so the OTG core is told VBUS is permanently valid
  // and never sees session end). tud_mounted() therefore stays true forever and
  // the session would sit on the "Connected" screen after the cable is pulled —
  // device-observed on a LilyGo T5 S3. So the activity watches for it itself.
  //
  // Preferred signal: the board's physical VBUS reading, when it has one (the
  // LilyGo's BQ25896). Unambiguous — it cannot be confused with a host
  // suspending — so it only needs debouncing, not a long hold. How long it must
  // read absent: one sample is a ~1 ms I2C read that a bus glitch can spoil,
  // and the penalty for believing it wrongly is rebooting under a mounted
  // volume, so require a few consecutive polls rather than a single one.
  static constexpr unsigned long POWER_GONE_CONFIRM_MS = 750UL;
  // Poll cadence for that reading. It is I2C traffic, so it does not belong on
  // every 10 ms loop tick.
  static constexpr unsigned long POWER_POLL_INTERVAL_MS = 250UL;

  // Fallback for boards with no VBUS reading (the X4 Pro's CW2017 gauge has no
  // charger IC on the bus): bus suspend. The core detects it from bus idle, so
  // it survives the forced B-valid — but a host suspending an idle bus looks
  // exactly the same, and acting on that would reboot out from under a mounted
  // volume and lose whatever the host had cached. Hence a deliberately long
  // hold: an unplug is permanent, so the only cost of waiting is latency, while
  // the cost of being wrong is a corrupt card.
  static constexpr unsigned long SUSPEND_GONE_CONFIRM_MS = 8000UL;

  bool isIoError() const { return state == UsbDriveState::IoError; }
  // Returns true once the cable is believed gone. Owns the debouncing for both
  // signals above; call once per loop tick while a session is live.
  bool cableLooksGone();
  void restartToHome();

  UsbDriveState state = UsbDriveState::Unsupported;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool forcedDisconnectRequested = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long forcedDisconnectRequestedAt = 0;

  // Debounce state for cableLooksGone(). "…Since = 0" means "not currently
  // absent"; millis() can return 0 only in the first tick after boot, which
  // this activity cannot be running in.
  unsigned long lastPowerPollAt = 0;
  unsigned long powerAbsentSince = 0;
  unsigned long suspendedSince = 0;
};
