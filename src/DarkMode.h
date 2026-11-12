#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Dark Mode (CrossPointSettings::darkMode): the panel shows white on black. What it decides, apart from the code that
// acts on it so the host can test it (test/dark_mode).
//
// The frame buffers keep their colours and nothing draws differently: the display inverts each frame on its way to
// the panel (HalDisplay::setInverted), and pictures draw inverted so that they show as they are
// (GfxRenderer::keepsImagePolarity). Design after crosspoint-reader PR #2705 (Justin Mitchell).
namespace DarkMode {

// What a screen asks of the panel's polarity (Activity::outputPolarity).
enum class ScreenPolarity : uint8_t {
  FollowDarkMode,  // every ordinary screen
  Normal,          // a picture of its own: the sleep screens, the image viewer
  AsOnScreen,      // the frame already on screen, in the polarity it went out in: Quick Resume's page
};

// Whether the screen on top goes out inverted; `invertedNow` is how the frame on screen went out.
constexpr bool invertsOutput(const bool darkMode, const ScreenPolarity screen, const bool invertedNow) {
  switch (screen) {
    case ScreenPolarity::Normal:
      return false;
    case ScreenPolarity::AsOnScreen:
      return invertedNow;
    case ScreenPolarity::FollowDarkMode:
      break;
  }
  return darkMode;
}

// Whether text anti-aliasing the reader asks for reaches the panel. Inverted, the gray planes do not survive
// (HalDisplay::setInverted), so the gray passes would cost their render and show nothing; only a panel whose single
// push mirrors its greys keeps them (HalDisplay::invertsGrayFrame: the LilyGo T5S3).
constexpr bool showsTextAntiAliasing(const bool wanted, const bool outputInverted, const bool panelInvertsGrayFrame) {
  return wanted && (!outputInverted || panelInvertsGrayFrame);
}

// Quick Resume adds its moon icon to the page on screen. A HALF drives every pixel through its inverse, which on a
// dark page is a white flash across the whole screen; only the moon changes, so a FAST adds it. Light mode keeps its
// HALF. After crosspoint-reader 5bfaf7bdb (Justin Mitchell).
constexpr bool moonGoesOutFast(const bool outputInverted) { return outputInverted; }

// Quick Resume's saved frame is kept as it was on the glass: inverted on its way to the card when it went out
// inverted, and inverted again on its way back when the wake's output is. The wake's polarity cannot be known at sleep
// (the night hours may begin or end in between), so each end converts by its own: frame buffer and glass differ
// exactly when the output is inverted.
constexpr bool glassDiffersFromBuffer(const bool outputInverted) { return outputInverted; }

// The ghosting budget, in pages or frames between ghost-clearing refreshes (Refresh Frequency). White text leaves more
// behind on black than black text on white, so Dark Mode clears twice as often: 15 becomes 8. Never (0) stays never.
constexpr int refreshFrequency(const int pages, const bool darkMode) {
  return darkMode && pages > 1 ? (pages + 1) / 2 : pages;
}

// --- The setting, and its night hours ---

// CrossPointSettings::darkMode. Off and On are the 0 and 1 the setting stored as a toggle; Scheduled (the night hours)
// is offered on the boards with a reading light and a clock that keeps time through sleep
// (HalCapabilities::hasFrontlightAndRtc).
enum Setting : uint8_t { SETTING_OFF = 0, SETTING_ON = 1, SETTING_SCHEDULED = 2, SETTING_COUNT = 3 };

// The night hours run from one half-hour of the day to another: slot 44 is 22:00, slot 14 is 07:00.
constexpr int SLOT_MINUTES = 30;
constexpr int SLOTS_PER_DAY = 24 * 60 / SLOT_MINUTES;
constexpr int MINUTES_PER_DAY = 24 * 60;

// Whether a minute of the day (0..1439) falls in the night hours [from, until), which may run past midnight. From and
// until on the same slot: no night hours.
constexpr bool inNightHours(const int minuteOfDay, const uint8_t fromSlot, const uint8_t untilSlot) {
  const int from = fromSlot * SLOT_MINUTES;
  const int until = untilSlot * SLOT_MINUTES;
  if (from == until) return false;
  return from < until ? minuteOfDay >= from && minuteOfDay < until : minuteOfDay >= from || minuteOfDay < until;
}

// Minutes from a minute of the day to a time of day, 1 to 1440: a boundary at this very minute is a day away.
constexpr int minutesAhead(const int minuteOfDay, const int boundary) {
  const int ahead = boundary - minuteOfDay;
  return ahead <= 0 ? ahead + MINUTES_PER_DAY : ahead;
}

// Minutes from a minute of the day to the next start or end of the night hours.
constexpr int minutesToNextChange(const int minuteOfDay, const uint8_t fromSlot, const uint8_t untilSlot) {
  const int toStart = minutesAhead(minuteOfDay, fromSlot * SLOT_MINUTES);
  const int toEnd = minutesAhead(minuteOfDay, untilSlot * SLOT_MINUTES);
  return toStart < toEnd ? toStart : toEnd;
}

// The wall clock as the night hours read it. Not `known` while the clock has never been set: the hours then count as
// day, and the screens stay light.
struct ClockReading {
  bool known;
  uint32_t epoch;   // seconds; when the drawer's flip runs out
  int secondOfDay;  // local time, 0..86399
};

// The light drawer's switch while the night hours run Dark Mode: the other polarity until the hours next change.
// `until` is epoch seconds, 0 for none. Kept in CrossPointState, so a sleep does not undo it.
struct NightFlip {
  uint32_t until;
  bool dark;
};

// Whether the screens are dark: the setting, and in night hours the clock, or the drawer's flip while it lasts.
constexpr bool isDark(const uint8_t setting, const uint8_t fromSlot, const uint8_t untilSlot, const ClockReading clock,
                      const NightFlip flip) {
  if (setting == SETTING_ON) return true;
  if (setting != SETTING_SCHEDULED || !clock.known) return false;
  if (flip.until != 0 && clock.epoch < flip.until) return flip.dark;
  return inNightHours(clock.secondOfDay / 60, fromSlot, untilSlot);
}

// What the drawer's switch leaves: Off becomes On and On Off. In night hours the setting stays and the flip holds the
// other polarity until the hours next change; flipped back before then, the flip goes. Without a clock there is no
// "until", so night hours give way to On or Off like the others.
struct Flipped {
  uint8_t setting;
  NightFlip flip;
};
constexpr Flipped flipped(const uint8_t setting, const uint8_t fromSlot, const uint8_t untilSlot,
                          const ClockReading clock, const NightFlip flip) {
  const bool darkNow = isDark(setting, fromSlot, untilSlot, clock, flip);
  if (setting != SETTING_SCHEDULED || !clock.known) {
    return {darkNow ? SETTING_OFF : SETTING_ON, {0, false}};
  }
  if (flip.until != 0 && clock.epoch < flip.until) return {setting, {0, false}};
  const int minuteOfDay = clock.secondOfDay / 60;
  const int secondsToChange = minutesToNextChange(minuteOfDay, fromSlot, untilSlot) * 60 - clock.secondOfDay % 60;
  return {setting, {clock.epoch + static_cast<uint32_t>(secondsToChange), !darkNow}};
}

// A slot as the settings list shows it, in the clock's format (HalClock::formatTime): "22:00", or "10:00PM".
inline void formatSlot(char* buf, const size_t size, const uint8_t slot, const bool use24h) {
  const int minutes = (slot % SLOTS_PER_DAY) * SLOT_MINUTES;
  const int hour = minutes / 60;
  const int minute = minutes % 60;
  if (use24h) {
    snprintf(buf, size, "%02d:%02d", hour, minute);
  } else {
    snprintf(buf, size, "%d:%02d%s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
  }
}

}  // namespace DarkMode
