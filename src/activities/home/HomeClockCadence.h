#pragma once

// How often Home redraws its clock. Each redraw is a panel refresh, and a Home screen nobody is
// using does not need one a minute: after kIdleAfterMs without user activity the clock moves on
// the minutes the clock shows as a multiple of kIdleStepMinutes only (12:00, 12:10, ...). Any
// input brings the minute back.
namespace HomeClockCadence {

constexpr unsigned long kIdleAfterMs = 10UL * 60UL * 1000UL;
constexpr int kIdleStepMinutes = 10;

// Whether the minute tick redraws Home, `sinceInputMs` after the last user activity. `shownMinute`
// is the minute of the hour the clock shows, in local time (tm_min), not one counted in UTC: a
// zone 45 minutes off the hour would otherwise move it at :05, :15 and so on.
inline bool redraws(const unsigned long sinceInputMs, const int shownMinute) {
  if (sinceInputMs < kIdleAfterMs) return true;
  return shownMinute % kIdleStepMinutes == 0;
}

}  // namespace HomeClockCadence
