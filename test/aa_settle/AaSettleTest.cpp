// AaSettle: how long a freshly drawn page waits for a press before its inline gray pass commits.
#include <gtest/gtest.h>

#include <cstdint>

#include "activities/reader/AaSettle.h"

namespace {

using AaSettle::flipping;
using AaSettle::kMaxWaitMs;
using AaSettle::kNoTurn;
using AaSettle::waitMs;

constexpr uint32_t kSettledMs = 1500;  // BG_BUILD_BORROW_QUIET_MS
// How long the pass before ran past its last abort point (gray refresh + restore), from the 2026-11-15/16 logs.
constexpr uint32_t kX4ProTailMs = 417;
constexpr uint32_t kX4TailMs = 160;

// The case the change exists for: the reader is reading, and the page's gray comes at once.
TEST(AaSettle, AReaderWhoStaysOnThePageDoesNotWait) {
  EXPECT_EQ(waitMs(/*turnGapMs=*/20000, kSettledMs, kX4ProTailMs), 0u);
  EXPECT_EQ(waitMs(5000, kSettledMs, kX4ProTailMs), 0u);
  EXPECT_EQ(waitMs(2500, kSettledMs, kX4ProTailMs), 0u);
  EXPECT_EQ(waitMs(kSettledMs, kSettledMs, kX4ProTailMs), 0u);  // at the threshold: settled
}

// A page turn every 1.2 s or 0.6 s: as before, a press has its reaction time to take the pass away.
TEST(AaSettle, AReaderWhoIsFlippingIsWaitedFor) {
  EXPECT_TRUE(flipping(1200, kSettledMs));
  EXPECT_TRUE(flipping(600, kSettledMs));
  EXPECT_EQ(waitMs(1200, kSettledMs, kX4ProTailMs), kMaxWaitMs);
  EXPECT_EQ(waitMs(600, kSettledMs, kX4ProTailMs), kMaxWaitMs);
  EXPECT_EQ(waitMs(kSettledMs - 1, kSettledMs, kX4ProTailMs), kMaxWaitMs);
}

// A book open, a jump, a redraw of the page on screen: no page turn asked for the pass.
TEST(AaSettle, APageNotReachedByAPageTurnIsNoFlip) {
  EXPECT_FALSE(flipping(kNoTurn, kSettledMs));
  EXPECT_EQ(waitMs(kNoTurn, kSettledMs, kX4ProTailMs), 0u);
  EXPECT_FALSE(flipping(kNoTurn, kNoTurn));  // whatever the threshold
}

// A press later after the page than the pass can still not be stopped would not have met it: no longer wait than that.
TEST(AaSettle, TheWaitIsNoLongerThanThePassCanNotBeStopped) {
  EXPECT_EQ(waitMs(600, kSettledMs, kX4TailMs), kX4TailMs);
  EXPECT_EQ(waitMs(600, kSettledMs, 0), 0u);
}

TEST(AaSettle, TheWaitIsNeverLongerThanTheReactionTime) {
  EXPECT_EQ(waitMs(600, kSettledMs, 2000), kMaxWaitMs);
  EXPECT_EQ(waitMs(0, kSettledMs, kNoTurn), kMaxWaitMs);
}

// The first turn of a book: no turn before it, the gap counts from the reader's start.
TEST(AaSettle, TheFirstTurnAfterALongStayIsNoFlip) { EXPECT_FALSE(flipping(90000, kSettledMs)); }

}  // namespace
