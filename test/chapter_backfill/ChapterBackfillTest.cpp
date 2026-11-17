// ChapterBackfill: when Background-B lays out a chapter's earlier files for its page count, and which.
#include <gtest/gtest.h>

#include "activities/reader/ChapterBackfill.h"

using ChapterBackfill::mayLook;
using ChapterBackfill::State;
using ChapterBackfill::takes;

TEST(ChapterBackfill, LooksOnlyWhenTheCountIsShownAndNothingElseIsOwed) {
  State s;
  s.counterShown = true;
  EXPECT_TRUE(mayLook(s));
  // Every combination of the four reasons not to: any one of them is enough.
  for (int mask = 1; mask < 16; ++mask) {
    State t = s;
    t.chapterBuilding = (mask & 1) != 0;
    t.pausedForChapter = (mask & 2) != 0;
    t.doneForSpine = (mask & 4) != 0;
    t.backfilling = (mask & 8) != 0;
    EXPECT_FALSE(mayLook(t)) << "mask " << mask;
  }
  State hidden;
  EXPECT_FALSE(mayLook(hidden)) << "no count on the status bar, nothing to firm up";
}

TEST(ChapterBackfill, TakesAFileOnlyOnce) {
  EXPECT_TRUE(takes(22, -1));
  EXPECT_TRUE(takes(21, 22)) << "the next earlier file, once the nearer one has its count";
  EXPECT_FALSE(takes(22, 22)) << "laid out and still without a count: a file of no pages";
  EXPECT_FALSE(takes(-1, -1)) << "none left";
}

using ChapterBackfill::buildMs;
using ChapterBackfill::mayStart;
using ChapterBackfill::msPerKb;

constexpr uint32_t kQuiet = 1500;

TEST(ChapterBackfill, TimesABuildByTheRateTheLastOneHeldItsBufferFor) {
  // X3, 2026-11-17: a 20,997-byte file held the buffer 1,657 ms.
  const uint32_t rate = msPerKb(1657, 20997, ChapterBackfill::kDefaultMsPerKb);
  EXPECT_EQ(80u, rate);
  EXPECT_EQ(1640u, buildMs(20997, rate));
  EXPECT_EQ(ChapterBackfill::kDefaultMsPerKb, msPerKb(295, 865, ChapterBackfill::kDefaultMsPerKb))
      << "a heading file's build is its setup: no rate from it";
  EXPECT_EQ(0u, buildMs(0, rate)) << "a file of unknown size";
}

TEST(ChapterBackfill, StartsAFileOnlyWhenItEndsBeforeTheNextTurn) {
  // A page every 3.1 s from press to press; B could borrow 1.5 s after the page came up, 2.6 s after the press.
  EXPECT_FALSE(mayStart(1640, 2600, 3100, 1500, 0, kQuiet)) << "X3, a page every 2 s: the backfill met two turns";
  EXPECT_TRUE(mayStart(400, 2600, 3100, 1500, 0, kQuiet)) << "a small file fits between two turns";
  EXPECT_TRUE(mayStart(1640, 1600, 20000, 1500, 0, kQuiet)) << "a reader at 20 s a page";
}

TEST(ChapterBackfill, WithoutAPaceOnlyAPauseStartsAFile) {
  // Before a second turn: the first page of a book just opened, or the page after its first turn.
  EXPECT_FALSE(mayStart(100, 1600, ChapterBackfill::kNoPace, 6499, 0, kQuiet));
  EXPECT_TRUE(mayStart(1640, 1600, ChapterBackfill::kNoPace, 6500, 0, kQuiet)) << "5 s plus the quiet period";
}

TEST(ChapterBackfill, AReaderWhoPausedPastTheirPaceGetsTheFile) {
  EXPECT_FALSE(mayStart(1640, 7400, 3100, 6499, 0, kQuiet)) << "a pause is at least 5 s, however fast the pace";
  EXPECT_TRUE(mayStart(1640, 7400, 3100, 6500, 0, kQuiet));
  EXPECT_FALSE(mayStart(1640, 9500, 8000, 9499, 0, kQuiet)) << "and longer than the pace";
  EXPECT_TRUE(mayStart(1640, 9500, 8000, 9500, 0, kQuiet));
}

TEST(ChapterBackfill, AfterATurnTookOneAwayOnlyALongerPauseStartsAnother) {
  // The turn came 7 s after its page did: the next file waits for a page up 8.5 s, however slow the pace.
  EXPECT_FALSE(mayStart(100, 1600, 60000, 1500, 7000, kQuiet));
  EXPECT_FALSE(mayStart(100, 8500, 3100, 8499, 7000, kQuiet));
  EXPECT_TRUE(mayStart(1640, 8500, 3100, 8500, 7000, kQuiet));
}
