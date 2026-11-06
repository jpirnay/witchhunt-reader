// A session past midnight: each day keeps the reading done on it.
//
// The store files a session under the day it began, so an hour read from 23:30 went to the day before, and the streak
// lost the day it was read on. CrossInk e7760b58 (Julia Nguyen) keeps dated arrays for this; here the session is cut in
// two at the first page turn of the new day.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Arduino.h"
#include "HalClock.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"

namespace {

struct Session {
  std::string docId;
  uint32_t seconds;
  uint32_t pages;
  uint8_t progress;
  time_t walltime;
};
std::vector<Session> g_sessions;

time_t g_now = 0;
bool g_synced = true;

constexpr time_t kDay = 86400;
constexpr time_t kDayStart = 20463 * kDay;  // any day: the stub store's days are UTC
constexpr time_t kBeforeMidnight = kDayStart + kDay - 120;

}  // namespace

namespace HalClock {
time_t now() { return g_now; }
bool isSynced() { return g_synced; }
}  // namespace HalClock

// The store as a recorder: what the tracker hands it is what the test reads. A day is 86400 s of the epoch.
ReadingStatsStore ReadingStatsStore::instance;
uint16_t localDayIndexFromEpoch(const time_t epoch) { return static_cast<uint16_t>(epoch / kDay); }
ReadingStatsStore::WriteResult ReadingStatsStore::recordSession(const std::string& docId, const std::string&,
                                                                const std::string&, const uint32_t seconds,
                                                                const uint32_t pages, const uint8_t progress,
                                                                const time_t walltime) {
  g_sessions.push_back({docId, seconds, pages, progress, walltime});
  return WriteResult::Done;
}
ReadingStatsStore::WriteResult ReadingStatsStore::markFinished(const std::string&, const std::string&,
                                                               const std::string&, time_t) {
  return WriteResult::Done;
}

namespace {

class Tracker : public ::testing::Test {
 protected:
  void SetUp() override {
    g_sessions.clear();
    host_clock::valueRef() = 1000;
    g_now = kBeforeMidnight;
    g_synced = true;
  }

  // Reading on for `seconds`, then a page turn.
  void readFor(const uint32_t seconds) {
    host_clock::valueRef() += seconds * 1000UL;
    g_now += seconds;
    tracker.onPageTurn();
  }

  ReadingSessionTracker tracker;
};

uint16_t dayOf(const Session& session) { return localDayIndexFromEpoch(session.walltime); }

}  // namespace

TEST_F(Tracker, ASessionWithinOneDayIsOneSession) {
  tracker.begin("doc", "T", "A");
  readFor(30);
  readFor(30);
  tracker.end();
  ASSERT_EQ(g_sessions.size(), 1u);
  EXPECT_EQ(g_sessions[0].seconds, 60u);
  EXPECT_EQ(g_sessions[0].pages, 2u);
}

TEST_F(Tracker, ReadingPastMidnightIsFiledUnderBothDays) {
  tracker.begin("doc", "T", "A");
  readFor(60);  // 23:59:00
  readFor(80);  // 00:00:20: the first turn of the new day
  readFor(40);
  readFor(40);
  tracker.end();
  ASSERT_EQ(g_sessions.size(), 2u);
  EXPECT_EQ(dayOf(g_sessions[0]), localDayIndexFromEpoch(kBeforeMidnight));
  EXPECT_EQ(dayOf(g_sessions[1]), localDayIndexFromEpoch(kBeforeMidnight) + 1);
  EXPECT_EQ(g_sessions[0].seconds, 140u);
  EXPECT_EQ(g_sessions[1].seconds, 80u);
}

TEST_F(Tracker, NoSecondAndNoPageIsLostAtTheCut) {
  tracker.begin("doc", "T", "A");
  readFor(60);
  readFor(80);
  readFor(40);
  tracker.end();
  ASSERT_EQ(g_sessions.size(), 2u);
  EXPECT_EQ(g_sessions[0].seconds + g_sessions[1].seconds, 180u);
  EXPECT_EQ(g_sessions[0].pages + g_sessions[1].pages, 3u);
}

TEST_F(Tracker, TheCutKeepsTheBookAndItsProgress) {
  tracker.begin("doc", "T", "A");
  tracker.updateProgress(42);
  readFor(150);
  EXPECT_TRUE(tracker.isActive());
  EXPECT_EQ(tracker.getDocId(), "doc");
  tracker.end();
  ASSERT_EQ(g_sessions.size(), 2u);
  EXPECT_EQ(g_sessions[0].progress, 42);
  EXPECT_EQ(g_sessions[1].progress, 42);
}

// The reader's "current session" card is one sitting, whichever days it is filed under.
TEST_F(Tracker, TheLiveFiguresRunOnAcrossTheCut) {
  tracker.begin("doc", "T", "A");
  readFor(60);
  readFor(80);
  readFor(40);
  EXPECT_EQ(tracker.getLiveSeconds(), 180u);
  EXPECT_EQ(tracker.getLivePages(), 3u);
}

TEST_F(Tracker, WithoutAClockThereIsNoDayToCutAt) {
  g_synced = false;
  tracker.begin("doc", "T", "A");
  readFor(60);
  readFor(80);
  tracker.end();
  EXPECT_EQ(g_sessions.size(), 1u);
}

// A clock corrected backwards is not a new day of reading.
TEST_F(Tracker, AClockSetBackIsNoCut) {
  tracker.begin("doc", "T", "A");
  g_now -= 2 * kDay;
  readFor(60);
  tracker.end();
  EXPECT_EQ(g_sessions.size(), 1u);
}
