#pragma once

#include <cstdint>

// What Background-B's look-ahead does once page turns have taken a chapter's build away as often as it allows
// (BG_BUILD_MAX_PREEMPTIONS): the reader turns faster than that chapter builds.
//
// It used to move on to the chapter after it, with a fresh allowance, and so on down the book. Every other turn then
// met a build that had just borrowed the framebuffer, lost its one-pass anti-aliasing to it, and threw the build away.
// X4, 2026-11-16, a page every 2.5 s through half a chapter: 20 chapters abandoned, 45 builds thrown away, 43 of 71
// turns in two passes (1.1-1.3 s against 0.70 s).
//
// Now it stops there. The chapter it gave up stays its target, and it starts no build until the reader pauses or
// enters another chapter. The next chapter still gets its build on the chapter's last pages, where turns do not take
// it away (BG_BUILD_THROUGH_PAGES).
//
// A pause is the quiet rule the look-ahead always waited for, raised past the pace that beat it: a page that stays up
// longer, by the quiet period, than the longest-standing page whose turn took a build of that chapter away. The
// chapter then has its allowance again.
//
// The decision alone, apart from the reader, so the host can test it.
namespace LookAheadPause {

// How long a page has to stay up before the look-ahead tries the chapter again.
constexpr uint32_t pauseMs(const uint32_t longestPreemptedStayMs, const uint32_t quietMs) {
  return longestPreemptedStayMs + quietMs;
}

// The reader has paused on the page on screen. Not with a press queued: the turn it becomes would take the build away.
constexpr bool readerPaused(const uint32_t pageUpForMs, const uint32_t longestPreemptedStayMs, const uint32_t quietMs,
                            const bool inputQueued) {
  return !inputQueued && pageUpForMs >= pauseMs(longestPreemptedStayMs, quietMs);
}

}  // namespace LookAheadPause
