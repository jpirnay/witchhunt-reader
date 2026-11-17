// LookAheadPause: when Background-B's look-ahead tries a chapter again after page turns took its build away twice.
#include <gtest/gtest.h>

#include <cstdint>

#include "activities/reader/LookAheadPause.h"

namespace {

using LookAheadPause::pauseMs;
using LookAheadPause::readerPaused;

constexpr uint32_t kQuietMs = 1500;  // BG_BUILD_BORROW_QUIET_MS

// The look-ahead borrowed 1.5 s after a page appeared and the next turn came 0.4 s later: a page every 2.5 s on the
// X4, 2026-11-16.
constexpr uint32_t kStayThatBeatIt = 1900;

TEST(LookAheadPause, APauseOutlastsThePagesThatBeatTheBuildByTheQuietPeriod) {
  EXPECT_EQ(pauseMs(kStayThatBeatIt, kQuietMs), 3400u);
}

// The case the stop exists for: the reader keeps the pace that beat the build, and the look-ahead stays out of it.
TEST(LookAheadPause, TheSamePaceIsNoPause) {
  EXPECT_FALSE(readerPaused(kStayThatBeatIt, kStayThatBeatIt, kQuietMs, /*inputQueued=*/false));
}

TEST(LookAheadPause, APaceALittleSlowerIsNoPauseEither) {
  EXPECT_FALSE(readerPaused(kStayThatBeatIt + kQuietMs - 1, kStayThatBeatIt, kQuietMs, /*inputQueued=*/false));
}

TEST(LookAheadPause, APageThatStaysUpPastThePaceByTheQuietPeriodIsAPause) {
  EXPECT_TRUE(readerPaused(kStayThatBeatIt + kQuietMs, kStayThatBeatIt, kQuietMs, /*inputQueued=*/false));
  EXPECT_TRUE(readerPaused(20000, kStayThatBeatIt, kQuietMs, /*inputQueued=*/false));  // reading
}

// A press already queued becomes the turn that takes the build away again.
TEST(LookAheadPause, NoPauseWithAPressQueued) {
  EXPECT_FALSE(readerPaused(20000, kStayThatBeatIt, kQuietMs, /*inputQueued=*/true));
}

// With no stay on record the rule is the quiet period itself, as before the stop.
TEST(LookAheadPause, WithNoStayOnRecordTheQuietPeriodIsThePause) {
  EXPECT_FALSE(readerPaused(kQuietMs - 1, 0, kQuietMs, /*inputQueued=*/false));
  EXPECT_TRUE(readerPaused(kQuietMs, 0, kQuietMs, /*inputQueued=*/false));
}

// Each time it is beaten again the bar is that pace's: a slower reader who still beats the build raises it.
TEST(LookAheadPause, TheBarFollowsTheLongestStayThatBeatTheBuild) {
  constexpr uint32_t kSlowerStay = 2600;
  EXPECT_TRUE(readerPaused(3500, kStayThatBeatIt, kQuietMs, /*inputQueued=*/false));
  EXPECT_FALSE(readerPaused(3500, kSlowerStay, kQuietMs, /*inputQueued=*/false));
}

}  // namespace
