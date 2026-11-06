#include "ReadingSessionTracker.h"

#include <Arduino.h>  // millis()
#include <HalClock.h>
#include <Logging.h>

#include "ReadingStats.h"

ReadingSessionTracker& globalReadingSessionTracker() {
  static ReadingSessionTracker instance;
  return instance;
}

void ReadingSessionTracker::flushIdleSinceLastActivity() {
  if (!active) return;
  const uint32_t now = millis();
  const uint32_t delta = now - lastActivityMs;  // unsigned wrap is fine
  // Cap idle gaps. A user idle for an hour shouldn't get credited for it.
  const uint32_t capped = delta > MAX_IDLE_MS ? MAX_IDLE_MS : delta;
  accumulatedMs += capped;
  lastActivityMs = now;
}

void ReadingSessionTracker::begin(const std::string& docId_, const std::string& title_, const std::string& author_) {
  if (active) {
    // Don't lose data from a previous session that wasn't explicitly closed.
    end();
  }
  active = true;
  docId = docId_;
  title = title_;
  author = author_;
  walltimeStartEpoch = HalClock::isSynced() ? static_cast<int64_t>(HalClock::now()) : 0;
  lastActivityMs = millis();
  accumulatedMs = 0;
  pagesTurnedThisSession = 0;
  lastKnownProgress = 0;
  carriedSeconds = 0;
  carriedPages = 0;
  LOG_DBG("RST", "Session begin doc=%s sync=%d", docId.c_str(), HalClock::isSynced() ? 1 : 0);
}

void ReadingSessionTracker::onPageTurn() {
  if (!active) return;
  flushIdleSinceLastActivity();
  pagesTurnedThisSession += 1;
  cutAtMidnight();
}

// The store files a session under the day it began. The first page turn of a new day ends the session there and starts
// another, so each day keeps what was read on it, and a streak the day it was read on. One write, once a night. Idea
// from CrossInk e7760b58 (Julia Nguyen), which keeps dated arrays for this; a session is already a dated record here.
void ReadingSessionTracker::cutAtMidnight() {
  if (walltimeStartEpoch == 0 || !HalClock::isSynced()) return;
  const auto now = static_cast<time_t>(HalClock::now());
  if (localDayIndexFromEpoch(now) <= localDayIndexFromEpoch(static_cast<time_t>(walltimeStartEpoch))) return;

  const std::string book = docId;
  const std::string bookTitle = title;
  const std::string bookAuthor = author;
  const uint8_t progress = lastKnownProgress;
  const uint32_t seconds = getLiveSeconds();
  const uint32_t pages = getLivePages();
  end();
  begin(book, bookTitle, bookAuthor);
  lastKnownProgress = progress;
  carriedSeconds = seconds;
  carriedPages = pages;
}

void ReadingSessionTracker::updateProgress(uint8_t progress) {
  if (!active) return;
  lastKnownProgress = progress;
}

void ReadingSessionTracker::markFinished() {
  if (!active) return;
  const int64_t walltime = HalClock::isSynced() ? static_cast<int64_t>(HalClock::now()) : 0;
  const auto result = READING_STATS.markFinished(docId, title, author, static_cast<time_t>(walltime));
  if (result != ReadingStatsStore::WriteResult::Done) {
    LOG_ERR("RST", "markFinished not recorded (%u) doc=%s wall=%lld", static_cast<unsigned>(result), docId.c_str(),
            (long long)walltime);
  }
  LOG_DBG("RST", "Marked finished doc=%s wall=%lld", docId.c_str(), (long long)walltime);
}

void ReadingSessionTracker::end() {
  if (!active) return;
  // Final idle flush so we credit the time between the last page turn and now,
  // capped at MAX_IDLE_MS just like all the other gaps.
  flushIdleSinceLastActivity();

  const uint32_t seconds = static_cast<uint32_t>(accumulatedMs / 1000);
  // Prefer the walltime captured at begin(); if HalClock has only just become
  // synced, fall back to "now" as the lastReadEpoch.
  int64_t walltime = walltimeStartEpoch;
  if (walltime == 0 && HalClock::isSynced()) {
    walltime = static_cast<int64_t>(HalClock::now());
  }

  LOG_DBG("RST", "Session end doc=%s secs=%u pages=%u prog=%u wall=%lld", docId.c_str(), seconds,
          pagesTurnedThisSession, lastKnownProgress, (long long)walltime);

  if (!docId.empty()) {
    // One book and the global figures, written in place copy-on-write: ~10 KB of heap whatever the history holds.
    const auto result = READING_STATS.recordSession(docId, title, author, seconds, pagesTurnedThisSession,
                                                    lastKnownProgress, static_cast<time_t>(walltime));
    if (result != ReadingStatsStore::WriteResult::Done) {
      LOG_ERR("RST", "Session not recorded (%u) doc=%s secs=%u pages=%u wall=%lld", static_cast<unsigned>(result),
              docId.c_str(), seconds, pagesTurnedThisSession, (long long)walltime);
    }
  }

  active = false;
  docId.clear();
  title.clear();
  author.clear();
  walltimeStartEpoch = 0;
  lastActivityMs = 0;
  accumulatedMs = 0;
  pagesTurnedThisSession = 0;
  lastKnownProgress = 0;
}

uint32_t ReadingSessionTracker::getLiveSeconds() const {
  if (!active) return 0;
  // Best-effort live readout — does not mutate state, so it does not include
  // the in-flight idle gap. Good enough for "you've been reading for X".
  return carriedSeconds + static_cast<uint32_t>(accumulatedMs / 1000);
}
