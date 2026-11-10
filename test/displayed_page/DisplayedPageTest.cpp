// DisplayedPage: what a reader render pass records once its page is on the panel, when the loop task may have moved
// the position on while the pass had the lock given up for the waveform.
#include <gtest/gtest.h>

#include "activities/reader/DisplayedPage.h"

namespace {

using DisplayedPage::Identity;
using DisplayedPage::LivePosition;
using DisplayedPage::settle;

constexpr int kSpines = 58;

void expectRecords(const DisplayedPage::Settlement& s, const Identity& page) {
  ASSERT_TRUE(s.recordProgress);
  EXPECT_EQ(page.spine, s.progress.spine);
  EXPECT_EQ(page.page, s.progress.page);
  EXPECT_EQ(page.pageCount, s.progress.pageCount);
}

TEST(DisplayedPage, AnUndisturbedPassRecordsItsPage) {
  const Identity drawn{9, 4, 21};
  const auto s = settle(drawn, {true, 9, 4}, kSpines);
  EXPECT_FALSE(s.superseded);
  expectRecords(s, drawn);
}

TEST(DisplayedPage, ATurnDuringTheWaveformLeavesTheRecordOnTheDrawnPage) {
  // The next page is not on the panel yet; the pass the turn asked for records it once it is.
  const Identity drawn{9, 4, 21};
  const auto s = settle(drawn, {true, 9, 5}, kSpines);
  EXPECT_TRUE(s.superseded);
  expectRecords(s, drawn);
  // A burst: two turns taken in one waveform.
  expectRecords(settle(drawn, {true, 9, 6}, kSpines), drawn);
  expectRecords(settle(drawn, {true, 9, 3}, kSpines), drawn);
}

TEST(DisplayedPage, ATurnAcrossASpineBoundaryStillRecordsTheChaptersLastPage) {
  // The turn reset the section: the pass relocks to no section at all. The page it drew is the chapter's last, which
  // nothing else will record.
  const Identity drawn{3, 2, 3};
  const auto forward = settle(drawn, {false, 4, 0}, kSpines);
  EXPECT_TRUE(forward.superseded);
  expectRecords(forward, drawn);

  const Identity first{9, 0, 21};
  const auto back = settle(first, {false, 8, 0}, kSpines);
  EXPECT_TRUE(back.superseded);
  expectRecords(back, first);
}

TEST(DisplayedPage, AJumpThatReloadsTheSameSpineIsSupersededToo) {
  // A link into the same chapter resets the section without changing the spine.
  const Identity drawn{9, 4, 21};
  const auto s = settle(drawn, {false, 9, 0}, kSpines);
  EXPECT_TRUE(s.superseded);
  expectRecords(s, drawn);
}

TEST(DisplayedPage, ATurnPastTheLastPageLeavesTheRecordToTheFinishedBookFlow) {
  // The finished-book pass writes its 100% record directly; a page record published here would land after it.
  const Identity last{kSpines - 1, 11, 12};
  const auto s = settle(last, {false, kSpines, 0}, kSpines);
  EXPECT_TRUE(s.superseded);
  EXPECT_FALSE(s.recordProgress);
  // The book's last page itself, undisturbed, is recorded as usual.
  expectRecords(settle(last, {true, kSpines - 1, 11}, kSpines), last);
}

using DisplayedPage::redrawsStatusBarPage;
using DisplayedPage::StatusBarPage;

TEST(DisplayedPage, AClockTickRedrawsThePageTheStatusBarShows) {
  EXPECT_TRUE(redrawsStatusBarPage(StatusBarPage{9, 5}, 9, 5));
}

TEST(DisplayedPage, ATurnToTheSamePageNumberOfTheNextSpineIsATurn) {
  // A one-page chapter, then the next one's first page: both are page 1. Counted as a tick, the turn refreshed FAST
  // and spent nothing from the refresh budget.
  EXPECT_FALSE(redrawsStatusBarPage(StatusBarPage{9, 1}, 10, 1));
  EXPECT_FALSE(redrawsStatusBarPage(StatusBarPage{10, 1}, 9, 1));
}

TEST(DisplayedPage, AnyOtherPageIsATurn) {
  EXPECT_FALSE(redrawsStatusBarPage(StatusBarPage{9, 5}, 9, 6));
  EXPECT_FALSE(redrawsStatusBarPage(StatusBarPage{9, 5}, 9, 4));
}

TEST(DisplayedPage, NothingIsATickBeforeTheFirstStatusBar) {
  EXPECT_FALSE(redrawsStatusBarPage(StatusBarPage{}, -1, -1));
  EXPECT_FALSE(redrawsStatusBarPage(StatusBarPage{}, 0, 1));
}

TEST(DisplayedPage, ARunOfOnePageSpinesSpendsTheBudgetEveryTurn) {
  // Twenty one-page chapters in a row, read through: every render is a turn and spends a page of the refresh budget,
  // so the periodic HALF comes inside the run. It never came when each counted as a tick.
  StatusBarPage shown;
  int turns = 0;
  for (int spine = 0; spine < 20; ++spine) {
    if (!redrawsStatusBarPage(shown, spine, 1)) ++turns;
    shown = {spine, 1};
  }
  EXPECT_EQ(20, turns);
}

}  // namespace
