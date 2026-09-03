#include "WakeTrace.h"

#include <Arduino.h>
#include <Logging.h>
#include <stdio.h>

namespace WakeTrace {
namespace {

// uint32, not uint16. These are absolute millis() stamps, so they overflow 16 bits after
// 65.5 s of uptime -- and the old saturating cast turned that into silent nonsense rather
// than a visible wrap: every phase past the 65 s mark pinned to 65535, so the deltas between
// them all collapsed to 0 and `open` (the one number this trace exists to produce) read 0.
// Observed on a T5S3 opening a book at 82.9 s uptime: "rdr+65535 ... page=65535 | open=0".
//
// It hid because the case the trace was BUILT for is the one case that works: a deep-sleep
// resume restarts millis() near zero, so resume traces land well inside 16 bits. Only plain
// opens later in a session -- the common case -- saturate.
uint32_t phaseMs[static_cast<uint8_t>(Phase::Count)] = {};
// A stamp of 0 is a legal millis() value, so "was this phase reached" cannot be inferred
// from the stamp itself — same reasoning as bootPhaseReached in main.cpp.
uint16_t phaseReached = 0;
bool traceFromWake = false;
bool resumeArmed = false;
bool sectionCacheHit = false;
bool sectionCacheHitKnown = false;
bool summaryEmitted = true;  // seeded true so a stray logSummary() before begin() is a no-op

// Short labels so the whole trace fits one 256-byte ring-buffer entry (Logging.cpp).
constexpr const char* PHASE_NAMES[] = {"rdr", "book", "act", "prog", "store", "rend", "sect", "page"};
static_assert(sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0]) == static_cast<size_t>(Phase::Count),
              "PHASE_NAMES must stay in sync with Phase");

}  // namespace

void armResume() { resumeArmed = true; }

void begin() {
  for (uint8_t i = 0; i < static_cast<uint8_t>(Phase::Count); i++) {
    phaseMs[i] = 0;
  }
  phaseReached = 0;
  // Consume the one-shot: only the first book open after a wake is the resume. Any later open
  // in the same session (user backed out to the library and picked another book) is a plain open.
  traceFromWake = resumeArmed;
  resumeArmed = false;
  sectionCacheHit = false;
  sectionCacheHitKnown = false;
  summaryEmitted = false;
  mark(Phase::ReaderEnter);
}

void mark(Phase phase) {
  const uint8_t index = static_cast<uint8_t>(phase);
  const uint16_t bit = static_cast<uint16_t>(1u << index);
  // First write wins. Every phase here is a once-per-open milestone, but several of the call
  // sites run repeatedly within one open (render() is re-entered for the pre-render pass, the
  // section-building pass, and periodic status-bar updates). Letting a later call overwrite the
  // stamp would silently retime the phase to whenever the LAST such render happened and make
  // every preceding delta meaningless. begin() clears the mask, so the next open re-stamps.
  if (phaseReached & bit) {
    return;
  }
  phaseMs[index] = static_cast<uint32_t>(millis());
  phaseReached |= bit;
}

void setSectionCacheHit(bool hit) {
  sectionCacheHit = hit;
  sectionCacheHitKnown = true;
}

void logSummary() {
  if (summaryEmitted) {
    return;
  }
  summaryEmitted = true;

  // Deltas rather than absolutes: the cost of a phase is the actionable figure, and the
  // absolutes are just their running sum. The first delta is measured against the reader
  // entry, so `rdr` itself prints as an absolute — it is the handoff point from the boot
  // trace, and lining the two up is the whole reason this trace exists.
  char line[160];
  size_t used = 0;
  uint32_t previous = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(Phase::Count); i++) {
    if ((phaseReached & (1u << i)) == 0) continue;
    const int written = snprintf(line + used, sizeof(line) - used, "%s%s+%u", used ? " " : "", PHASE_NAMES[i],
                                 static_cast<unsigned>(phaseMs[i] - previous));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - used) break;
    used += static_cast<size_t>(written);
    previous = phaseMs[i];
  }

  const uint8_t readerIndex = static_cast<uint8_t>(Phase::ReaderEnter);
  const uint8_t pageIndex = static_cast<uint8_t>(Phase::PageVisible);
  const bool hasPage = (phaseReached & (1u << pageIndex)) != 0;
  // open = reader entry to first visible page (the cost this trace exists to attribute).
  // page = absolute millis() at first paint, so it can be read against the boot trace's
  // `logo=`/`setup=` numbers without adding anything up by hand.
  const unsigned open = hasPage ? static_cast<unsigned>(phaseMs[pageIndex] - phaseMs[readerIndex]) : 0;

  // `section=` distinguishes the three outcomes that need different fixes:
  //   cached    the section LUT was reusable — wake cost is elsewhere (boot path, stores, render)
  //   rebuilt   a full section build sat between the wake and the page
  //   building  the build was still running when the first page was drawn from its partial LUT,
  //             i.e. Background-C already hides the rebuild and `open` understates the real cost
  const char* sectionState = "n/a";
  if (sectionCacheHitKnown) {
    const bool sectionFinished = (phaseReached & (1u << static_cast<uint8_t>(Phase::SectionReady))) != 0;
    sectionState = sectionCacheHit ? "cached" : (sectionFinished ? "rebuilt" : "building");
  }
  LOG_INF("WAKE", "%s phase cost ms: %s | open=%u page=%u section=%s", traceFromWake ? "resume" : "open", line, open,
          hasPage ? static_cast<unsigned>(phaseMs[pageIndex]) : 0u, sectionState);
}

}  // namespace WakeTrace
