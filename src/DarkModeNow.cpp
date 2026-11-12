#include "DarkModeNow.h"

#include <HalClock.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DarkMode.h"

namespace DarkModeNow {

namespace {

// The time of day as the status bar's clock tells it (HalClock::formatTime): local, and unknown until the clock has
// been set. Read at each render pass's start, so a change of the night hours lands on the next frame, between frames.
DarkMode::ClockReading readClock() {
  if (!HalClock::isSynced()) return {false, 0, 0};
  const time_t now = HalClock::now();
  tm local{};
  localtime_r(&now, &local);
  return {true, static_cast<uint32_t>(now), local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec};
}

DarkMode::NightFlip savedFlip() { return {APP_STATE.darkModeFlipUntil, APP_STATE.darkModeFlipDark}; }

}  // namespace

bool isDark() {
  return DarkMode::isDark(SETTINGS.darkMode, SETTINGS.darkModeFrom, SETTINGS.darkModeUntil, readClock(), savedFlip());
}

// The settings row's own field and save when that is all there is to it (Off, On); under Scheduled the flip the night
// hours carry until they next change (DarkMode::flipped). The drawer is a second way to the same setting.
void flip() {
  const auto result =
      DarkMode::flipped(SETTINGS.darkMode, SETTINGS.darkModeFrom, SETTINGS.darkModeUntil, readClock(), savedFlip());
  if (result.setting != SETTINGS.darkMode) {
    SETTINGS.darkMode = result.setting;
    SETTINGS.saveToFile();
  }
  if (result.flip.until != APP_STATE.darkModeFlipUntil || result.flip.dark != APP_STATE.darkModeFlipDark) {
    APP_STATE.darkModeFlipUntil = result.flip.until;
    APP_STATE.darkModeFlipDark = result.flip.dark;
    APP_STATE.saveToFile();
  }
}

}  // namespace DarkModeNow
