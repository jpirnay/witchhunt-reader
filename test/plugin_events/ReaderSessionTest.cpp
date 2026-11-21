// The active-reading summary behind "reader.session" (src/activities/reader/ReaderSession.h).
#include <gtest/gtest.h>

#include "activities/reader/ReaderSession.h"

namespace {
constexpr int64_t kEpoch = 1795000000;

// A page shown at `ms`, reached by a turn of the given kind.
void page(ReaderSession& s, const uint32_t ms, const int bp, const bool forward = true, const bool ok = true,
          const int64_t epoch = kEpoch) {
  s.noteTurn(forward, ok);
  s.onRenderComplete(ms, epoch + ms / 1000, bp);
}
}  // namespace

TEST(ReaderSession, CountsTheTimeBeforeForwardTurns) {
  ReaderSession s;
  s.onRenderComplete(0, kEpoch, 1000);  // the first page, opened
  page(s, 30000, 1010);                 // 30 s on it, then forward
  page(s, 75000, 1020);                 // 45 s
  EXPECT_EQ(s.durationSeconds(), 75u);
  EXPECT_EQ(s.startTime(), kEpoch);
  EXPECT_EQ(s.endTime(), kEpoch + 75);
  EXPECT_EQ(s.startProgressBp(), 1000);
  EXPECT_EQ(s.endProgressBp(), 1020);
  EXPECT_TRUE(s.isEmitWorthy());
}

TEST(ReaderSession, SkimmingBackwardAndFailedTurnsCountNothing) {
  ReaderSession s;
  s.onRenderComplete(0, kEpoch, 1000);
  page(s, 1500, 1010);                            // under 2 s: skimmed
  page(s, 40000, 1000, /*forward=*/false);        // backward
  page(s, 80000, 1000, /*forward=*/true, false);  // a turn that failed (end of book)
  EXPECT_EQ(s.durationSeconds(), 0u);
  EXPECT_FALSE(s.isEmitWorthy());
  page(s, 110000, 1010);  // 30 s since the last anchor, forward: counts
  EXPECT_EQ(s.durationSeconds(), 30u);
}

TEST(ReaderSession, CapsAPageAndNeedsTenSecondsAndATrustedClock) {
  ReaderSession s;
  s.onRenderComplete(0, kEpoch, 0);
  page(s, 3 * 3600 * 1000, 10);  // three hours on one page: 30 min count
  EXPECT_EQ(s.durationSeconds(), 1800u);

  ReaderSession shortRead;
  shortRead.onRenderComplete(0, kEpoch, 0);
  page(shortRead, 5000, 10);
  EXPECT_EQ(shortRead.durationSeconds(), 5u);
  EXPECT_FALSE(shortRead.isEmitWorthy());

  ReaderSession noClock;
  noClock.onRenderComplete(0, 0, 0);
  page(noClock, 60000, 10, true, true, /*epoch=*/0);
  EXPECT_FALSE(noClock.isEmitWorthy());
}

TEST(ReaderSession, EndProgressNeverFallsAndResetEmpties) {
  ReaderSession s;
  s.onRenderComplete(0, kEpoch, 5000);
  page(s, 20000, 5100);
  page(s, 40000, 4900);  // a forward turn reporting less (a relaid chapter): the end stays
  EXPECT_EQ(s.endProgressBp(), 5100);
  page(s, 60000, 12000);  // clamped to the scale
  EXPECT_EQ(s.endProgressBp(), 10000);
  s.reset();
  EXPECT_EQ(s.durationSeconds(), 0u);
  EXPECT_EQ(s.startTime(), 0);
  EXPECT_FALSE(s.isEmitWorthy());
}
