#pragma once

#include <algorithm>
#include <cstdint>

// The chapter page count's backfill. The count of a chapter split over several spine items is exact only once each of
// its files has been laid out under the current settings (ChapterPageSpan, SpinePageIndex), and Background-B lays out
// only the files after the one on screen. A reader who came into the chapter past its first file (a saved position
// after a font change, a bookmark, a jump, a chapter whose heading is a file of its own) kept the "~" to the chapter's
// last page. So once the look-ahead has nothing left to do, B lays out the chapter's earlier files too, the nearest
// first. Class 3 work (docs/future_work/reader-priority-scheme.md): under B's gates, and it never asks for a refresh
// (rule R6): the next draw that happens anyway shows the exact count.
//
// And it never costs a page turn. A turn takes the buffer back from a build in progress and throws the build away; on
// the C3 that turn waits for the slice, the return and the discard (X3, 2026-11-17, a page every 2 s: +224 and +256 ms
// on the two turns that met one). So a file starts only when its build is expected to end before the next turn at the
// reader's pace, or once the reader has paused: no press for some seconds, longer than their pace and than a page a
// turn took a build away from (LookAheadPause's rule). If the reader turns faster than a file builds, the count stays
// "~".
namespace ChapterBackfill {

struct State {
  bool counterShown = false;      // the status bar shows the chapter's count (as text or a chapter progress bar)
  bool chapterBuilding = false;   // the chapter on screen is still being laid out (Background-C)
  bool pausedForChapter = false;  // B paused its look-ahead until the next chapter (memory ran short)
  bool doneForSpine = false;      // the chapter has no such file left, or its backfill gave up
  bool backfilling = false;       // B works on such a file now
};

// Whether B may look for an earlier file of the chapter to lay out.
constexpr bool mayLook(const State& s) {
  return s.counterShown && !s.chapterBuilding && !s.pausedForChapter && !s.doneForSpine && !s.backfilling;
}

// Whether B takes `target` (-1: the page index found none). A target laid out already for this spine and still
// without a count (a file of no pages, which records none) ends the backfill rather than be laid out again and again.
constexpr bool takes(const int target, const int lastTarget) { return target >= 0 && target != lastTarget; }

// Before a build has been timed: an X3's rate for running text (1.66 s for 21.0 KB, 2026-11-17), rounded up.
inline constexpr uint32_t kDefaultMsPerKb = 100;
// A smaller file's build is mostly its setup and says little about the rate.
inline constexpr uint32_t kMinTimedBytes = 4096;

// How long laying out `bytes` of XHTML takes at `msPerKb`.
constexpr uint32_t buildMs(const uint32_t bytes, const uint32_t msPerKb) {
  return static_cast<uint32_t>(static_cast<uint64_t>(bytes) * msPerKb / 1024);
}

// The rate a finished build of `bytes` that held its buffer for `elapsedMs` gives, or `previous` for a small file.
constexpr uint32_t msPerKb(const uint32_t elapsedMs, const uint32_t bytes, const uint32_t previous) {
  return bytes < kMinTimedBytes ? previous : static_cast<uint32_t>(static_cast<uint64_t>(elapsedMs) * 1024 / bytes);
}

// The reader's pace is not known until a second page turn: the first one after a book opens says nothing about it.
inline constexpr uint32_t kNoPace = UINT32_MAX;
// The shortest pause: a page up this long, plus the quiet period, starts a file whatever the pace.
inline constexpr uint32_t kMinPauseMs = 5000;

// Whether a file whose build takes `buildMs` may start. `sinceTurnMs`: since the last page turn was asked for;
// `paceMs`: from the turn before it to that one; `pageUpMs`: how long the page on screen has been up;
// `preemptedStayMs`: 0, or how long the page was up when a turn last took a build of this chapter's away; `quietMs`:
// the look-ahead's settled-reader period (BG_BUILD_BORROW_QUIET_MS).
constexpr bool mayStart(const uint32_t buildMs, const uint32_t sinceTurnMs, const uint32_t paceMs,
                        const uint32_t pageUpMs, const uint32_t preemptedStayMs, const uint32_t quietMs) {
  const uint32_t longest = std::max({paceMs == kNoPace ? 0 : paceMs, preemptedStayMs, kMinPauseMs});
  if (uint64_t{pageUpMs} >= uint64_t{longest} + quietMs) return true;  // the reader paused
  return preemptedStayMs == 0 && paceMs != kNoPace && uint64_t{sinceTurnMs} + buildMs <= paceMs;
}

// Rule R3: on a board whose builds take a display buffer (the C3; the S3 boards build in PSRAM), the buffer lent is
// the copy of the page on screen that the next turn's composed one-pass gray is made from
// (FreeInkDisplay::beginOnePassMask), and that turn goes out in two passes instead. No file is laid out then.
constexpr bool costsOnePass(const bool buildTakesDisplayBuffer, const bool onePassComposed,
                            const bool bufferHoldsFrameOnScreen) {
  return buildTakesDisplayBuffer && onePassComposed && bufferHoldsFrameOnScreen;
}

}  // namespace ChapterBackfill
