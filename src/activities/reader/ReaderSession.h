#pragma once

#include <cstdint>

// The active-reading summary behind the plugin event "reader.session" (src/util/PluginEvents.h). The reader reports
// each page turn before it renders and each finished page after, and the summary counts the time a page stayed on
// screen before an ordinary forward turn left it: at least 2 s, at most 30 min a page. Backward turns, jumps and
// failed turns count nothing and restart the timer. A summary worth sending has at least 10 s and a trusted start
// time. Fixed scalars: no allocation, no I/O.
//
// Ported from crosspoint-reader PR #3114 ("feat: SD card based plugin system", Justin Mitchell / @itsthisjustin).
class ReaderSession {
 public:
  void noteTurn(bool forward, bool succeeded);
  // `trustedEpoch`: wall-clock seconds, 0 when the clock is not trusted; `progressBp`: whole-book progress, 0..10000.
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBp);
  void reset();

  bool isEmitWorthy() const { return startEpoch > 0 && duration >= 10 && endTime() > startEpoch; }
  int64_t startTime() const { return startEpoch; }
  // The start plus the counted time: idle gaps are left out, so it is not the wall-clock end.
  int64_t endTime() const { return startEpoch > 0 ? startEpoch + duration : 0; }
  uint32_t durationSeconds() const { return duration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }

 private:
  static uint16_t clampProgress(int progressBp);

  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  bool hasAnchor = false;
  bool pendingForward = false;
};
