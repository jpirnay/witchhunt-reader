#pragma once

#include <algorithm>
#include <cstdint>

// How long a freshly drawn black-and-white page stays up before its inline gray pass commits (the X4 and the X4 Pro
// in two passes, EpubReaderActivity::renderContents()).
//
// The wait is there for a reader who is flipping. Past its end the pass cannot be stopped, and a press that came
// during it was held for the rest of it: X4 Pro, 2026-11-06, one page per 1.27 s however fast the buttons went, the
// page itself up after 0.65 s. A press answering a page needs a reaction time after the page appears, so the pass
// waits and asks again. Taken on every page, though, the wait also held back the gray of every page the reader stays
// on: a book's first page, a chapter's, a page back, a redraw, each 0.4 s late.
//
// So the pass waits only while the reader is flipping: the page was reached by a page turn that was asked for sooner
// after the turn before it than the look-ahead's own measure of a settled reader (BG_BUILD_BORROW_QUIET_MS, 1.5 s).
// On the X4 Pro that is also how long after a turn a press can still meet the pass at all: page up after 0.80 s,
// pass over 0.65 s later (2026-11-17). A book open, a jump and a redraw are not page turns.
//
// And it never waits longer than the part of the pass that cannot be stopped, its gray refresh and the restore,
// measured on the pass before: a press later than that after the page would not have met a pass started at once.
// X4 Pro 0.42 s, X4 0.16 s (2026-11-15/16 logs).
//
// The decision alone, apart from the reader, so the host can test it.
namespace AaSettle {

// The longest wait: the reaction time a press answering a page is given.
inline constexpr uint32_t kMaxWaitMs = 400;
// For the gap since the turn before: the page was not reached by a page turn.
inline constexpr uint32_t kNoTurn = UINT32_MAX;

// The reader is flipping: a page turn, asked for `turnGapMs` after the turn before it, sooner than a settled reader's.
constexpr bool flipping(const uint32_t turnGapMs, const uint32_t settledMs) {
  return turnGapMs != kNoTurn && turnGapMs < settledMs;
}

// How long the pass waits for a press before it commits. `unstoppableMs`: how long the pass before ran past its
// last abort point.
constexpr uint32_t waitMs(const uint32_t turnGapMs, const uint32_t settledMs, const uint32_t unstoppableMs) {
  return flipping(turnGapMs, settledMs) ? std::min(kMaxWaitMs, unstoppableMs) : 0;
}

}  // namespace AaSettle
