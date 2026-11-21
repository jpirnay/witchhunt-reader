// ReadingStatsStore's arithmetic on plain data: the eviction order, a session and a finish applied
// to one book and the global figures, and a book taken back out of them. The streamed file layer
// (ReadingStatsJsonTest) and the store on real files (ReadingStatsStoreTest) build on these.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "HalClock.h"
#include "ReadingStats.h"

// Link stubs. ReadingStats.cpp reaches the clock only for "today"; these tests never do.
namespace HalClock {
time_t now() { return 0; }
bool isSynced() { return false; }
}  // namespace HalClock

namespace {

constexpr time_t kDay = 86400;
// 2026-01-10 12:00 UTC. Noon, so consecutive multiples of kDay land on consecutive local days in
// whatever timezone the host runs in.
constexpr time_t kNoon = 1768046400;

}  // namespace

TEST(ReadingStatsEviction, LeastRecentlyReadGoesFirstThenTheLeastRead) {
  // (lastRead, seconds) pairs: an older last read goes first whatever the time spent...
  EXPECT_TRUE(ReadingStatsStore::evictsBefore(50, 999, 100, 1));
  EXPECT_FALSE(ReadingStatsStore::evictsBefore(100, 1, 50, 999));
  // ...and on the same last read, the book with less time goes first.
  EXPECT_TRUE(ReadingStatsStore::evictsBefore(100, 50, 100, 300));
  EXPECT_FALSE(ReadingStatsStore::evictsBefore(100, 300, 100, 50));
  // A full tie is not "before": the first candidate found stays the victim.
  EXPECT_FALSE(ReadingStatsStore::evictsBefore(100, 50, 100, 50));
}

TEST(ReadingStatsApply, SessionOnANewBookStartsItsHistory) {
  BookReadingStats book;
  book.docId = "a";
  ReadingTotals totals;

  ReadingStatsStore::applySession(book, totals, "Book A", "X", 600, 10, 20, kNoon);

  EXPECT_EQ(book.title, "Book A");
  EXPECT_EQ(book.author, "X");
  EXPECT_EQ(book.totalSeconds, 600u);
  EXPECT_EQ(book.pagesTurned, 10u);
  EXPECT_EQ(book.sessions, 1u);
  EXPECT_EQ(book.progress, 20);
  EXPECT_EQ(book.firstReadEpoch, kNoon);
  EXPECT_EQ(book.lastReadEpoch, kNoon);
  ASSERT_EQ(book.days.size(), 1u);
  EXPECT_EQ(book.days[0].seconds, 600u);
  EXPECT_EQ(totals.totalSeconds, 600u);
  EXPECT_EQ(totals.totalSessions, 1u);
  EXPECT_EQ(totals.totalPagesTurned, 10u);
  ASSERT_EQ(totals.globalDays.size(), 1u);
  EXPECT_EQ(totals.longestStreak, 1u);
}

TEST(ReadingStatsApply, ZeroSecondSessionOnlyMovesProgress) {
  BookReadingStats book;
  book.docId = "a";
  book.totalSeconds = 600;
  book.sessions = 1;
  book.progress = 20;
  ReadingTotals totals;
  totals.totalSeconds = 600;
  totals.totalSessions = 1;

  ReadingStatsStore::applySession(book, totals, "Book A", "", 0, 0, 25, kNoon + kDay);

  EXPECT_EQ(book.progress, 25);
  EXPECT_EQ(book.sessions, 1u);
  EXPECT_EQ(book.totalSeconds, 600u);
  EXPECT_EQ(totals.totalSessions, 1u);
  EXPECT_TRUE(totals.globalDays.empty());
}

TEST(ReadingStatsApply, SessionWithoutAClockLeavesDaysAndDatesAlone) {
  BookReadingStats book;
  book.docId = "a";
  ReadingTotals totals;

  ReadingStatsStore::applySession(book, totals, "Book A", "", 300, 4, 10, /*walltimeEpoch=*/0);

  EXPECT_EQ(book.totalSeconds, 300u);
  EXPECT_EQ(book.sessions, 1u);
  EXPECT_EQ(book.firstReadEpoch, 0);
  EXPECT_EQ(book.lastReadEpoch, 0);
  EXPECT_TRUE(book.days.empty());
  EXPECT_EQ(totals.totalSeconds, 300u);
  EXPECT_TRUE(totals.globalDays.empty());
  EXPECT_EQ(totals.longestStreak, 0u);
}

TEST(ReadingStatsApply, EmptyTitleOrAuthorKeepsTheOldOne) {
  BookReadingStats book;
  book.docId = "a";
  book.title = "Old title";
  book.author = "Old author";
  ReadingTotals totals;

  ReadingStatsStore::applySession(book, totals, "", "", 60, 1, 1, kNoon);

  EXPECT_EQ(book.title, "Old title");
  EXPECT_EQ(book.author, "Old author");
}

TEST(ReadingStatsApply, FinishCountsAndMovesTheDates) {
  BookReadingStats book;
  book.docId = "a";
  book.progress = 97;
  book.lastReadEpoch = kNoon;

  ReadingStatsStore::applyFinish(book, "Book A", "X", kNoon + kDay);

  EXPECT_EQ(book.finishedCount, 1);
  EXPECT_EQ(book.progress, 100);
  EXPECT_EQ(book.lastFinishedEpoch, kNoon + kDay);
  EXPECT_EQ(book.lastReadEpoch, kNoon + kDay);
  EXPECT_EQ(book.title, "Book A");
}

TEST(ReadingStatsTakeOut, NeverWrapsBelowZero) {
  BookReadingStats book;
  book.totalSeconds = 500;
  book.sessions = 3;
  book.pagesTurned = 40;
  book.days = {{localDayIndexFromEpoch(kNoon), 500}};
  ReadingTotals totals;
  totals.totalSeconds = 200;
  totals.totalSessions = 1;
  totals.totalPagesTurned = 10;
  totals.globalDays = {{localDayIndexFromEpoch(kNoon), 200}};

  ReadingStatsStore::takeOut(book, totals);

  EXPECT_EQ(totals.totalSeconds, 0u);
  EXPECT_EQ(totals.totalSessions, 0u);
  EXPECT_EQ(totals.totalPagesTurned, 0u);
  EXPECT_TRUE(totals.globalDays.empty());
}

// --- Pace and time to finish, in integers ----------------------------------------------------------
//
// These were floats: seconds per percent as a float quotient, the estimate as `remaining * pace + 0.5f`, pages a
// minute as printf's "%.1f" of `pages * 60.0f / seconds`. The integer forms keep the ratio and round once; the
// sweeps below hold them to the float forms over the ranges a reader produces.

namespace {

using Pace = ReadingStatsStore::SecondsPerPercent;

uint32_t floatEta(const uint32_t seconds, const uint32_t percents, const int remaining) {
  const float pace = static_cast<float>(seconds) / static_cast<float>(percents);
  return static_cast<uint32_t>(static_cast<float>(remaining) * pace + 0.5f);
}

std::string floatPagesPerMinute(const uint32_t pages, const uint32_t seconds) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", (pages * 60.0f) / seconds);
  return buf;
}

std::string intPagesPerMinute(const uint32_t pages, const uint32_t seconds) {
  const uint32_t tenths = ReadingStatsStore::pagesPerMinuteTenths(pages, seconds);
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u", tenths / 10, tenths % 10);
  return buf;
}

}  // namespace

TEST(ReadingStatsPace, OwnPaceNeedsEnoughGround) {
  EXPECT_FALSE(ReadingStatsStore::ownSecondsPerPercent(600, 2).known());  // below MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE
  EXPECT_FALSE(ReadingStatsStore::ownSecondsPerPercent(0, 40).known());
  const Pace own = ReadingStatsStore::ownSecondsPerPercent(600, 3);
  EXPECT_TRUE(own.known());
  EXPECT_EQ(ReadingStatsStore::etaSeconds(own, 97), 19400u);  // 200 s a percent
}

TEST(ReadingStatsPace, PooledPaceNeedsAMinuteOverall) {
  EXPECT_FALSE(ReadingStatsStore::pooledSecondsPerPercent(59, 59, 10).known());
  EXPECT_FALSE(ReadingStatsStore::pooledSecondsPerPercent(600, 0, 10).known());
  EXPECT_FALSE(ReadingStatsStore::pooledSecondsPerPercent(600, 600, 0).known());
  EXPECT_TRUE(ReadingStatsStore::pooledSecondsPerPercent(60, 60, 10).known());
}

TEST(ReadingStatsPace, EtaRoundsHalfUpAndStaysInRange) {
  EXPECT_EQ(ReadingStatsStore::etaSeconds({3, 2}, 1), 2u);  // 1.5 s: half up
  EXPECT_EQ(ReadingStatsStore::etaSeconds({1, 6}, 3), 1u);  // 0.5 s exactly
  EXPECT_EQ(ReadingStatsStore::etaSeconds({1, 3}, 1), 0u);
  EXPECT_EQ(ReadingStatsStore::etaSeconds({100, 10}, 0), 0u);
  EXPECT_EQ(ReadingStatsStore::etaSeconds({100, 10}, -5), 0u);
  EXPECT_EQ(ReadingStatsStore::etaSeconds({100, 10}, 250), 1000u);  // at most 100 % remains
  EXPECT_EQ(ReadingStatsStore::etaSeconds({}, 50), 0u);
  EXPECT_EQ(ReadingStatsStore::etaSeconds({UINT32_MAX, 1}, 100), UINT32_MAX);
}

// The float estimate over paces and remainders a history produces. They agree but where the exact estimate lies
// within the float's error of a half second: a float holds 24 bits, so an estimate of days was a quarter second
// coarse, and which way it rounded there was that error's doing.
TEST(ReadingStatsPace, EtaMatchesTheFloatFormula) {
  int compared = 0;
  int differing = 0;
  for (uint32_t percents = 3; percents <= 400; percents += 7) {
    for (uint32_t seconds = 60; seconds <= 200000; seconds = seconds * 3 / 2 + 13) {
      for (int remaining = 1; remaining <= 100; ++remaining) {
        ++compared;
        const uint32_t ours = ReadingStatsStore::etaSeconds({seconds, percents}, remaining);
        const uint32_t theirs = floatEta(seconds, percents, remaining);
        if (ours == theirs) continue;
        ++differing;
        const double exact = static_cast<double>(remaining) * seconds / percents;
        EXPECT_LE(std::fabs(exact - std::floor(exact) - 0.5), exact * 3e-7)
            << seconds << " s over " << percents << " %, " << remaining << " % left: " << ours << " vs " << theirs;
        EXPECT_EQ(ours > theirs ? ours - theirs : theirs - ours, 1u);
      }
    }
  }
  EXPECT_GT(compared, 100000);
  EXPECT_LT(differing, compared / 1000);
}

// Pages a minute as the screens showed it. A value exactly halfway between two tenths goes to the even one, as printf
// sends a float that lands on the half; one that float rounding had nudged off the half (0.05, 0.35, 0.85, 0.95
// pages a minute exactly) could show the other neighbour before. The sweep counts those and nothing else.
TEST(ReadingStatsPace, PagesPerMinuteMatchesTheFloatFormula) {
  EXPECT_EQ(intPagesPerMinute(1, 240), "0.2");  // 0.25 exactly: to the even tenth, as "%.1f" printed it
  EXPECT_EQ(intPagesPerMinute(1, 80), "0.8");   // 0.75 exactly
  EXPECT_EQ(intPagesPerMinute(84, 3600), "1.4");
  EXPECT_EQ(intPagesPerMinute(1000, 60), "1000.0");
  int differing = 0;
  int ties = 0;
  for (uint32_t seconds = 60; seconds <= 4000; ++seconds) {
    for (uint32_t pages = 1; pages <= 400; ++pages) {
      const bool tie = (pages * 1200) % seconds == 0 && ((pages * 1200) / seconds) % 2 == 1;
      ties += tie ? 1 : 0;
      if (intPagesPerMinute(pages, seconds) != floatPagesPerMinute(pages, seconds)) {
        ++differing;
        EXPECT_TRUE(tie) << pages << " pages in " << seconds << " s: " << intPagesPerMinute(pages, seconds) << " vs "
                         << floatPagesPerMinute(pages, seconds);
      }
    }
  }
  EXPECT_LT(differing, ties);
}
