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
