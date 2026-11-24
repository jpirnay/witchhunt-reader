#pragma once

// Whether the clock's minute tick redraws the screen on top (ActivityManager::loop). Each redraw is a
// panel refresh, so the tick serves only a screen that shows the time:
//  - `useClock` (Use Clock) asks for it on every screen, for the header and status-bar clocks;
//  - `screenShowsTime` is the screen's own clock or day (Activity::onClockMinute, Home's Clock,
//    Calendar and This week widgets), which asks whatever Use Clock says.
// `skips` is the screen's shouldSkipPeriodicUpdate(), its pace (HomeClockCadence) or its reason to
// wait. It is asked only when the tick would redraw: the reader's answer records a refresh it then
// owes, which a screen with Use Clock off and no clock of its own must not start owing.
namespace MinuteTick {

template <typename Skips>
bool redraws(const bool useClock, const bool screenShowsTime, Skips&& skips) {
  return (useClock || screenShowsTime) && !skips();
}

}  // namespace MinuteTick
