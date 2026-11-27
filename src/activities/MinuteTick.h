#pragma once

#include <atomic>

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

// Whether the next render pass draws the minute tick and nothing else. Such a frame moves only the
// clock, so it does not spend the ghost-clearing count that the menus' HALF follows
// (ActivityManager::armGhostClearingRefresh), as the reader's status-bar tick does not spend its page
// budget (EpubReaderActivity::renderContents). Counted, an idle menu took a full-screen HALF every
// refreshFrequencyPages minutes for a clock digit; seen on the T5S3 2026-11-26, whose menus follow
// that count. Any other request for the same frame makes it an ordinary one.
class ClockOnlyFrame {
 public:
  // The tick asked for a frame. `othersAsked`: something else already had, in this loop() pass.
  void tick(const bool othersAsked) { pending.store(!othersAsked, std::memory_order_relaxed); }
  // Something other than the tick asked for a frame.
  void other() { pending.store(false, std::memory_order_relaxed); }
  // The render task starts a pass: whether it draws only the tick. Each request answers one pass.
  bool takeForPass() { return pending.exchange(false, std::memory_order_relaxed); }

 private:
  std::atomic<bool> pending{false};
};

}  // namespace MinuteTick
