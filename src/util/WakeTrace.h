#pragma once

#include <stdint.h>

// --- Wake-to-page trace -------------------------------------------------------------
// Answers one question: when the device wakes from deep sleep straight back into a book,
// where does the time between the wake gesture and the page appearing actually go?
//
// The existing boot trace (main.cpp, BootPhase) stops at `route` — the moment the reader
// activity is entered. On a reader wake that is precisely where the expensive part begins:
// Epub::load() reads book.bin, EpubReaderActivity::onEnter() reads progress.bin/bookmarks/
// recent-books, and the first render() either hits the section cache or rebuilds it. All of
// that is invisible in the boot trace, which is why "is wake latency dominated by the boot
// path or by the book open?" could not be answered without guessing.
//
// This trace continues past `route` with the same design as logBootTrace(): a fixed array
// of millis() stamps, a reached-mask (0 is a legal stamp), and one summary line emitted when
// the first page is on the panel. Stamps are absolute millis() so they share a time origin
// with the boot trace and the two lines can be read as one timeline.
//
// Cost: 4 bytes per phase of .bss plus one log line per reader open. No heap, no allocation.
// Always compiled in — unlike DEBUG_MEMORY_CONSUMPTION this is a handful of stores on a path
// that runs once per book open, and the whole point is to have the number available from a
// field device (`CMD:BOOTLOG`-style) rather than only from an instrumented build.
namespace WakeTrace {

enum class Phase : uint8_t {
  ReaderEnter,     // ReaderActivity::onEnter — book path known, format dispatch about to run
  BookLoaded,      // Epub/Txt/Xtc load() returned (book.bin read, or first-open index built)
  ActivityEnter,   // EpubReaderActivity::onEnter begins (the Epub is now owned by the reader)
  ProgressLoaded,  // progress.bin read + pending sync/bookmark overlays applied
  StoresLoaded,    // bookmarks, recent-books overrides, reading-stats session
  RenderStart,     // first render() reached the pass dispatcher
  SectionReady,    // section cache loaded (hit) or rebuilt (miss) — see setSectionCacheHit()
  // First page SETTLED on the panel — after the waveform, not merely after the content was
  // handed to the driver. The distinction is not academic on X3: the first refresh after a wake
  // is a mandatory full sync whose waveform passes run after triggerDisplay() returns, so
  // stamping at the trigger understated a measured wake by ~2.1 s. The trace is emitted here.
  PageVisible,
  Count,
};

// Stamp a phase with the current millis(). FIRST WRITE WINS -- a second mark() of the same
// phase is ignored, because several call sites re-run within one open (render() is re-entered
// for the pre-render pass, the section build, and status-bar updates) and a later overwrite
// would retime the phase and invalidate every delta before it. The reader is also re-entered
// on orientation changes and setting edits, so `begin()` resets the whole trace rather than
// letting a second open interleave with the first.
void mark(Phase phase);

// Arm the "this open is a deep-sleep resume" flag. Called from setup() on the one branch that
// routes a wake straight back into the book. It is a ONE-SHOT consumed by the next begin():
// APP_STATE.lastSleepFromReader cannot serve this purpose because nothing clears it during a
// session, so a library open later in the same session would still read as a resume.
void armResume();

// Clear all stamps and start a fresh trace. Called from ReaderActivity::onEnter, which is the
// single funnel for every book open (library, recent books, and the wake resume alike).
void begin();

// Records how the section was obtained, which is the single most important bit for deciding
// whether a wake shortcut is worth building: a cache hit means the time is elsewhere, a miss
// means the section rebuild dominates and no amount of boot-path trimming will help.
void setSectionCacheHit(bool hit);

// Emit the summary line. Called once, when the first page is on the panel. Safe to call
// again (subsequent calls are ignored until the next begin()) so the several render paths
// that can produce the first visible page need no coordination between them.
void logSummary();

}  // namespace WakeTrace
