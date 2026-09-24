#pragma once

#include "ReadingStats.h"
#include "activities/Activity.h"

// Phase-1 reading-stats screen. Read-only summary of the per-book and global
// reading counters collected by ReadingSessionTracker. Mirrors the layout of
// SystemInformationActivity so it slots into the existing settings flow with
// no new theme work. Future phases will add per-book drill-in, day-by-day
// sparklines, and a web-frontend dashboard.
class ReadingStatsActivity final : public Activity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // The history is not resident while reading (see ReadingStatsStore::ScopedLoad); this screen
  // holds it for as long as it is on screen.
  //
  // Taken in onEnter(), not at construction. replaceActivity() builds the new activity while the
  // old one is still alive, so a screen reached from one that already holds the store -- the home
  // menu does -- would construct its guard as a NON-owner, and then the outgoing screen, which
  // was the owner, releases the store on its way out. This screen would render against an empty
  // store and report no history over a full file. onEnter() runs after that teardown, so the
  // guard is taken when the answer is final.
  std::optional<ReadingStatsStore::ScopedLoad> statsLoad_;

  // Last millis() at which we refreshed the live "this session" block.
  // We only redraw once per second to avoid hammering the e-ink panel.
  uint32_t lastLiveRefreshMs = 0;
};
