#include "ReadingStats.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <ctime>

namespace {
constexpr char READING_STATS_FILE[] = "/.crosspoint/reading-stats.json";

// Add `seconds` to the bucket for `dayIndex` in `days`, inserting in sorted
// position if absent. dayIndex == 0 ("unknown day") is silently skipped here —
// the caller decides whether to credit unknown-day reading to a sentinel
// bucket or drop it entirely.
void mergeDay(std::vector<DayBucket>& days, uint16_t dayIndex, uint32_t seconds, const size_t maxDays) {
  if (dayIndex == 0 || seconds == 0) return;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) {
    it->seconds += seconds;
  } else {
    days.insert(it, {dayIndex, seconds});
  }
  // Sorted ascending, so the oldest buckets are at the front.
  if (days.size() > maxDays) days.erase(days.begin(), days.begin() + static_cast<long>(days.size() - maxDays));
}

// Length of the run of consecutive reading days that ends on `day`, from a sorted day map.
uint16_t runEndingAt(const std::vector<DayBucket>& days, const uint16_t day) {
  auto it =
      std::lower_bound(days.begin(), days.end(), day, [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it == days.end() || it->dayIndex != day) return 0;
  uint16_t run = 1;
  while (it != days.begin()) {
    const auto prev = it - 1;
    if (prev->dayIndex + 1 != it->dayIndex) break;
    ++run;
    it = prev;
  }
  return run;
}

uint16_t dayIndexFromLocaltime(const struct tm& t) {
  // Days since 1970-01-01 by Y/M/D in local time. Uses the proleptic
  // Gregorian calendar — close enough for a 65k-day uint16 range (≈179
  // years). We deliberately do NOT call mktime() to avoid DST round-trip
  // surprises near transition midnights.
  const int year = t.tm_year + 1900;
  const int month = t.tm_mon + 1;
  const int day = t.tm_mday;
  // Howard Hinnant's days-from-civil, lightly inlined.
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + static_cast<long>(doe) - 719468L;
  if (days < 1 || days > 65535) return 0;  // outside our uint16_t window
  return static_cast<uint16_t>(days);
}

}  // namespace

uint16_t localDayIndexFromEpoch(time_t epoch) {
  if (epoch == 0) return 0;
  struct tm t{};
  localtime_r(&epoch, &t);
  return dayIndexFromLocaltime(t);
}

uint16_t currentLocalDayIndex() {
  if (!HalClock::isSynced()) return 0;
  return localDayIndexFromEpoch(HalClock::now());
}

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::recordSession(const std::string& docId, const std::string& title, const std::string& author,
                                      uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress,
                                      time_t walltimeEpoch) {
  if (docId.empty()) {
    return;
  }

  auto it = std::find_if(books.begin(), books.end(), [&docId](const BookReadingStats& b) { return b.docId == docId; });
  if (it == books.end()) {
    // A book opened and closed without reading is not history: no entry for a zero-second
    // session (it used to get one, for ever). An existing entry still takes the progress below.
    if (sessionSeconds == 0) return;
    if (books.size() >= kMaxBooks) {
      // Evict the least recently read book. An entry that was never read with the clock set
      // (lastReadEpoch 0) sorts as the oldest.
      auto victim =
          std::min_element(books.begin(), books.end(), [](const BookReadingStats& a, const BookReadingStats& b) {
            if (a.lastReadEpoch != b.lastReadEpoch) return a.lastReadEpoch < b.lastReadEpoch;
            return a.totalSeconds < b.totalSeconds;
          });
      LOG_INF("RST", "Book cap (%u) reached; dropping the least recently read: %s", static_cast<unsigned>(kMaxBooks),
              victim->title.c_str());
      books.erase(victim);
    }
    BookReadingStats fresh;
    fresh.docId = docId;
    fresh.title = title;
    fresh.author = author;
    books.push_back(std::move(fresh));
    it = books.end() - 1;
  } else {
    if (!title.empty()) it->title = title;
    if (!author.empty()) it->author = author;
  }

  it->totalSeconds += sessionSeconds;
  it->pagesTurned += sessionPagesTurned;
  it->progress = progress;
  if (sessionSeconds > 0) {
    it->sessions += 1;
    globalTotalSessions += 1;
  }
  if (walltimeEpoch != 0) {
    if (it->firstReadEpoch == 0) it->firstReadEpoch = walltimeEpoch;
    it->lastReadEpoch = walltimeEpoch;
    const uint16_t day = localDayIndexFromEpoch(walltimeEpoch);
    mergeDay(it->days, day, sessionSeconds, kMaxBookDays);
    mergeDay(globalDays, day, sessionSeconds, kMaxGlobalDays);
    // Fold this day's run into the persisted longest streak while the whole run is still in the
    // window (a run longer than the window is already the record).
    const uint16_t run = runEndingAt(globalDays, day);
    if (run > longestStreak_) longestStreak_ = run;
  }

  globalTotalSeconds += sessionSeconds;
  globalTotalPagesTurned += sessionPagesTurned;
}

uint32_t ReadingStatsStore::getSecondsForDay(uint16_t dayIndex) const {
  if (dayIndex == 0) return 0;
  auto it = std::lower_bound(globalDays.begin(), globalDays.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != globalDays.end() && it->dayIndex == dayIndex) return it->seconds;
  return 0;
}

uint16_t ReadingStatsStore::computeCurrentStreak(uint16_t today) const {
  if (today == 0 || globalDays.empty()) return 0;
  // 1-day grace: if there's no reading today, the streak may still end at
  // yesterday. After that the chain is broken.
  uint16_t anchor = today;
  if (getSecondsForDay(anchor) == 0) {
    anchor -= 1;
    if (getSecondsForDay(anchor) == 0) return 0;
  }
  uint16_t streak = 0;
  while (anchor > 0 && getSecondsForDay(anchor) > 0) {
    streak += 1;
    if (anchor == 1) break;
    anchor -= 1;
  }
  return streak;
}

uint16_t ReadingStatsStore::computeLongestStreak() const {
  if (globalDays.empty()) return longestStreak_;
  uint16_t longest = std::max<uint16_t>(1, longestStreak_);
  uint16_t run = 1;
  for (size_t i = 1; i < globalDays.size(); ++i) {
    if (globalDays[i].dayIndex == globalDays[i - 1].dayIndex + 1) {
      run += 1;
      if (run > longest) longest = run;
    } else {
      run = 1;
    }
  }
  return longest;
}

void ReadingStatsStore::markFinished(const std::string& docId, const std::string& title, const std::string& author,
                                     time_t walltimeEpoch) {
  if (docId.empty()) return;
  auto it = std::find_if(books.begin(), books.end(), [&docId](const BookReadingStats& b) { return b.docId == docId; });
  if (it == books.end()) {
    BookReadingStats fresh;
    fresh.docId = docId;
    fresh.title = title;
    fresh.author = author;
    books.push_back(std::move(fresh));
    it = books.end() - 1;
  } else {
    if (!title.empty()) it->title = title;
    if (!author.empty()) it->author = author;
  }
  it->finishedCount += 1;
  it->progress = 100;
  if (walltimeEpoch != 0) {
    it->lastFinishedEpoch = walltimeEpoch;
    if (it->lastReadEpoch < walltimeEpoch) it->lastReadEpoch = walltimeEpoch;
  }
}

size_t ReadingStatsStore::getFinishedBookCount() const {
  return static_cast<size_t>(
      std::count_if(books.begin(), books.end(), [](const BookReadingStats& b) { return b.finishedCount > 0; }));
}

const BookReadingStats* ReadingStatsStore::findBook(const std::string& docId) const {
  auto it = std::find_if(books.begin(), books.end(), [&docId](const BookReadingStats& b) { return b.docId == docId; });
  return it == books.end() ? nullptr : &*it;
}

float ReadingStatsStore::globalAvgSecondsPerPercent() const {
  if (globalTotalSeconds < MIN_GLOBAL_SECONDS_FOR_RATE) return 0.0f;
  // Average over books that have actual progress recorded. A book at 0%
  // contributes time but no progress denominator and would skew the rate
  // toward infinity. Books with progress >= MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE
  // are considered "real readings" for the purpose of the global average.
  uint32_t totalProgressPercents = 0;
  uint32_t totalSecondsFromCountedBooks = 0;
  for (const auto& b : books) {
    if (b.progress < MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE) continue;
    totalProgressPercents += b.progress;
    totalSecondsFromCountedBooks += b.totalSeconds;
  }
  if (totalProgressPercents == 0 || totalSecondsFromCountedBooks == 0) return 0.0f;
  return static_cast<float>(totalSecondsFromCountedBooks) / static_cast<float>(totalProgressPercents);
}

float ReadingStatsStore::avgSecondsPerPercent(const std::string& docId) const {
  const BookReadingStats* b = findBook(docId);
  if (b && b->progress >= MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE && b->totalSeconds > 0) {
    return static_cast<float>(b->totalSeconds) / static_cast<float>(b->progress);
  }
  return globalAvgSecondsPerPercent();
}

uint32_t ReadingStatsStore::estimateRemainingSeconds(const std::string& docId, float remainingPercent) const {
  if (remainingPercent <= 0.0f) return 0;
  if (remainingPercent > 100.0f) remainingPercent = 100.0f;
  const float rate = avgSecondsPerPercent(docId);
  if (rate <= 0.0f) return 0;
  return static_cast<uint32_t>(remainingPercent * rate + 0.5f);
}

bool ReadingStatsStore::saveToFile() const {
  if (!loaded_) {
    LOG_ERR("RST", "saveToFile refused: store not loaded (would erase history)");
    return false;
  }
  Storage.mkdir("/.crosspoint");
  // Straight into a temporary file (no in-RAM copy of the JSON), then rename over the store's
  // file: a power loss mid-write leaves the previous history intact.
  const std::string tmpPath = std::string(READING_STATS_FILE) + ".tmp";
  {
    FsFile out;
    if (!Storage.openFileForWrite("RST", tmpPath.c_str(), out)) return false;
    const bool ok = JsonSettingsIO::saveReadingStats(*this, out);
    out.close();
    if (!ok) {
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }
  Storage.remove(READING_STATS_FILE);
  if (!Storage.rename(tmpPath.c_str(), READING_STATS_FILE)) {
    LOG_ERR("RST", "saveToFile: could not rename %s into place", tmpPath.c_str());
    return false;
  }
  return true;
}

bool ReadingStatsStore::loadFromFile() {
  loaded_ = false;
  if (!Storage.exists(READING_STATS_FILE)) {
    loaded_ = true;  // an absent file is a legitimately empty history, not a failure to load
    return true;
  }
  FsFile in;
  if (!Storage.openFileForRead("RST", READING_STATS_FILE, in)) {
    LOG_ERR("RST", "History file could not be opened; the store stays unloaded (no save will overwrite it)");
    return false;
  }
  if (in.size() == 0) {
    in.close();
    loaded_ = true;
    return true;
  }
  const JsonSettingsIO::ReadingStatsLoad result = JsonSettingsIO::loadReadingStats(*this, in);
  in.close();
  switch (result) {
    case JsonSettingsIO::ReadingStatsLoad::Ok:
      loaded_ = true;
      return true;
    case JsonSettingsIO::ReadingStatsLoad::NoMemory:
      // Transient: the heap could not hold the document. Not loaded, so nothing saves over the
      // file; the next ScopedLoad tries again.
      LOG_ERR("RST", "History not loaded (out of memory parsing it); it is kept as is until it loads");
      return false;
    case JsonSettingsIO::ReadingStatsLoad::Corrupt:
    default: {
      // Permanent: set the file aside for forensics rather than lose it or stall on it for ever,
      // and start an empty history.
      const std::string asidePath = "/.crosspoint/reading-stats.corrupt.json";
      Storage.remove(asidePath.c_str());
      const bool moved = Storage.rename(READING_STATS_FILE, asidePath.c_str());
      LOG_ERR("RST", "History file unreadable; %s and starting an empty history",
              moved ? "set aside as reading-stats.corrupt.json" : "could not even be set aside");
      loaded_ = moved;  // if it cannot be moved, refuse to save over it
      return moved;
    }
  }
}

bool ReadingStatsStore::ensureLoaded() {
  if (loaded_) {
    return true;
  }
  loadFromFile();
  LOG_DBG("RST", "Store loaded on demand (%zu books, free=%lu contig=%lu)", books.size(),
          static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  return loaded_;
}

void ReadingStatsStore::replaceLoaded(std::vector<BookReadingStats>&& loadedBooks,
                                      std::vector<DayBucket>&& loadedGlobalDays, const uint32_t totalSeconds,
                                      const uint32_t totalSessions, const uint32_t totalPagesTurned,
                                      const uint16_t longestStreak) {
  books = std::move(loadedBooks);
  globalDays = std::move(loadedGlobalDays);
  globalTotalSeconds = totalSeconds;
  globalTotalSessions = totalSessions;
  globalTotalPagesTurned = totalPagesTurned;
  longestStreak_ = longestStreak;
  // Files written before the caps existed: trim once here, the next save persists it.
  for (auto& book : books) {
    if (book.days.size() > kMaxBookDays)
      book.days.erase(book.days.begin(), book.days.begin() + static_cast<long>(book.days.size() - kMaxBookDays));
  }
  if (globalDays.size() > kMaxGlobalDays) {
    // The record streak may live in the buckets about to go: measure before trimming.
    const uint16_t scanned = computeLongestStreak();
    if (scanned > longestStreak_) longestStreak_ = scanned;
    globalDays.erase(globalDays.begin(), globalDays.begin() + static_cast<long>(globalDays.size() - kMaxGlobalDays));
  }
}

void ReadingStatsStore::release() {
  if (!loaded_) {
    return;
  }
  // clear() keeps capacity, which is the whole cost here — 36 books measured at ~15 KB standing
  // plus a per-book days vector each. Swap with an empty temporary so the buffers actually go
  // back to the heap.
  std::vector<BookReadingStats>().swap(books);
  std::vector<DayBucket>().swap(globalDays);
  globalTotalSeconds = 0;
  globalTotalSessions = 0;
  globalTotalPagesTurned = 0;
  longestStreak_ = 0;
  loaded_ = false;
  LOG_DBG("RST", "Store released (free=%lu contig=%lu)", static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
}
