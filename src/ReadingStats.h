#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Day buckets are keyed by an ordinal day count (days since 1970-01-01 in
// LOCAL time, computed by localDayIndex() below). A "reading day" is the
// calendar day the session ENDED in — phase 2 keeps this simple and doesn't
// model the KOReader "day shift" / hour cutoff setting yet.
struct DayBucket {
  uint16_t dayIndex = 0;
  uint32_t seconds = 0;
};

// Helpers — both return 0 when HalClock is unsynced (caller should skip).
uint16_t localDayIndexFromEpoch(time_t epoch);
uint16_t currentLocalDayIndex();

// Per-book reading statistics. Keyed by KOReader document hash (or filename
// hash fallback) so a renamed/moved file keeps its history.
struct BookReadingStats {
  std::string docId;
  std::string title;
  std::string author;
  uint32_t totalSeconds = 0;  // idle-clamped, sum across all sessions
  uint32_t pagesTurned = 0;   // forward + backward
  uint32_t sessions = 0;      // session-open count
  // 0 if HalClock was never synced when the session ran. Treat as "unknown".
  time_t firstReadEpoch = 0;
  time_t lastReadEpoch = 0;
  uint8_t progress = 0;          // 0-100, snapshot of last known progress
  uint16_t finishedCount = 0;    // number of times the user has marked it finished
  time_t lastFinishedEpoch = 0;  // wallclock of the most recent finish (0 if unknown)
  // Sparse day buckets, sorted ascending by dayIndex. Only days with reading
  // are stored — the typical case is a few dozen entries. Bucket with
  // dayIndex == 0 is reserved for "clock-unknown" sessions and is excluded
  // from sparklines/streaks but kept so totals remain consistent.
  std::vector<DayBucket> days;
};

// Singleton store for per-book + global reading stats.
//
// Persistence model (mirrors RecentBooksStore):
//   /.crosspoint/reading-stats.json   — one file, all books + global counters
//
// One file is fine for phase 1: ESP32 memory + SD seek cost both favour a
// single small JSON over per-book files. We'll split if it ever grows too
// large (likely never — 50 books * ~120 bytes ≈ 6 KB).
class ReadingStatsStore {
  static ReadingStatsStore instance;

  std::vector<BookReadingStats> books;

  // Global aggregates — sum across all books.
  uint32_t globalTotalSeconds = 0;
  uint32_t globalTotalSessions = 0;
  uint32_t globalTotalPagesTurned = 0;
  // Global per-day reading time, sorted ascending. Same shape as per-book.
  // Used to compute streaks and the sparkline on the stats screen.
  std::vector<DayBucket> globalDays;
  // Longest run of consecutive reading days ever seen, persisted. globalDays keeps only the
  // newest kMaxGlobalDays buckets (memory audit 2026-09, R8), so a streak that started before
  // that window would otherwise be forgotten; recordSession() folds each session's run into it.
  uint16_t longestStreak_ = 0;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  // Apply a finished session to the store. Creates a per-book entry on first
  // use. Increments aggregate counters. Updates first/last epoch when
  // walltimeEpoch != 0 (HalClock was synced). When walltimeEpoch != 0, also
  // credits the session into a local-day bucket on both the book and the
  // global map. Caller is responsible for calling saveToFile() — we don't
  // auto-persist on every page turn.
  void recordSession(const std::string& docId, const std::string& title, const std::string& author,
                     uint32_t sessionSeconds, uint32_t sessionPagesTurned, uint8_t progress, time_t walltimeEpoch);

  // Mark the given book as having been finished once more. Bumps the per-
  // book counter and last-finished epoch (when walltime is available).
  // Creates the per-book entry on demand for books that have been opened
  // but never accumulated a session — e.g. a quick "mark as read" from the
  // menu before any reading time was recorded.
  void markFinished(const std::string& docId, const std::string& title, const std::string& author,
                    time_t walltimeEpoch);

  // Lookup by document hash; returns nullptr if unknown.
  const BookReadingStats* findBook(const std::string& docId) const;

  // ---- Reading speed / time-to-finish estimates -----------------------------
  //
  // Average seconds spent reading per 1% of book progress. We compute this
  // from a book's own history when it has covered enough ground to be
  // statistically meaningful, otherwise fall back to the global average over
  // all books. Returns 0 when no usable signal exists yet (caller should
  // render the ETA as "—").
  //
  // The minimum-progress gate prevents wildly optimistic estimates from books
  // the user has barely opened (e.g. 30 seconds of reading at 2% progress
  // shouldn't extrapolate to a 25-minute book).
  static constexpr uint8_t MIN_BOOK_PROGRESS_FOR_PERSONAL_RATE = 3;  // %
  static constexpr uint32_t MIN_GLOBAL_SECONDS_FOR_RATE = 60;        // s
  float avgSecondsPerPercent(const std::string& docId) const;
  float globalAvgSecondsPerPercent() const;

  // ETA in seconds for finishing `remainingPercent` (0..100) of a book.
  // Uses the per-book rate when available, else the global average. Returns
  // 0 when no rate is available or when remainingPercent <= 0.
  uint32_t estimateRemainingSeconds(const std::string& docId, float remainingPercent) const;

  const std::vector<BookReadingStats>& getBooks() const { return books; }
  // Bounds (memory audit 2026-09, R8). The store used to grow without limit -- an entry per
  // book ever opened, a day bucket per reading day per book and globally, for ever -- and its
  // load (file + JSON document + vectors) ran inside the reader at session end with ~35-45 KB
  // free: ~1.5 KB per book. Past these caps the least recently read book goes, and the oldest
  // day buckets go; the sparkline needs 30 days and the streak walk needs the current run, both
  // well inside the global window, and the longest streak is kept as a number.
  static constexpr size_t kMaxBooks = 100;
  static constexpr size_t kMaxBookDays = 60;
  static constexpr size_t kMaxGlobalDays = 400;
  uint16_t getLongestStreakSeen() const { return longestStreak_; }
  // Loader entry point (JsonSettingsIO::loadReadingStats): replaces the whole in-memory history.
  void replaceLoaded(std::vector<BookReadingStats>&& loadedBooks, std::vector<DayBucket>&& loadedGlobalDays,
                     uint32_t totalSeconds, uint32_t totalSessions, uint32_t totalPagesTurned, uint16_t longestStreak);
  uint32_t getGlobalTotalSeconds() const { return globalTotalSeconds; }
  uint32_t getGlobalTotalSessions() const { return globalTotalSessions; }
  uint32_t getGlobalTotalPagesTurned() const { return globalTotalPagesTurned; }
  size_t getBookCount() const { return books.size(); }
  // Count of distinct books that have been finished at least once. Derived
  // on read so we don't need a separate aggregate counter to keep in sync.
  size_t getFinishedBookCount() const;

  // Read-only view of the global day map.
  const std::vector<DayBucket>& getGlobalDays() const { return globalDays; }

  // Seconds read on a specific local-day index. 0 if unknown.
  uint32_t getSecondsForDay(uint16_t dayIndex) const;

  // Current streak in days, ending at `today` (or `today-1` for a 1-day grace
  // so a session that ended just past midnight still extends yesterday's
  // streak when you check this morning). Returns 0 if globalDays is empty or
  // if HalClock is unsynced (so `today == 0`).
  uint16_t computeCurrentStreak(uint16_t today) const;

  // Longest run of consecutive days with any reading.
  uint16_t computeLongestStreak() const;

  // Atomic: serialised straight into a temporary file that replaces the store's file only when
  // complete, so a power loss mid-write leaves the previous history intact. Refused while the
  // store is not loaded (see loadFromFile).
  bool saveToFile() const;
  // Streams the file into the store. The store counts as loaded only when the parse succeeded or
  // the file is genuinely absent or empty: a parse that failed for want of memory leaves it NOT
  // loaded (so saveToFile() refuses, and the next ScopedLoad simply tries again with whatever
  // heap it has), and a file that is corrupt is set aside as reading-stats.corrupt.json and the
  // store starts empty. It used to mark itself loaded before parsing, so a failed parse left an
  // empty "loaded" store that the next session end saved as the whole history.
  bool loadFromFile();

  // Lazy lifecycle. The store used to be loaded at boot and kept resident for the whole session;
  // measured on X4, 36 books cost ~15 KB standing and dropped the largest free block from 65524
  // to 26612 before the reader had opened anything — which is the difference between a large
  // chapter's section build finishing and aborting. Nothing needs the history while reading: the
  // session tracker accumulates entirely in its own members and only touches the store at
  // markFinished()/end(). So callers now load it for as long as they need it and release it after.
  //
  // Consumers: the stats screens, BookInfo, the Home themes' ETA badge, and the session tracker
  // on book exit. Read accessors on a released store return empty/zero, which renders as "no
  // history" rather than misreporting — but that is a caller bug, so load before you read.
  bool ensureLoaded();
  void release();
  bool isLoaded() const { return loaded_; }

  // RAII wrapper for the load-use-release cycle. Releases only if it did the loading, so a nested
  // scope (Home's ETA badge inside an already-loaded stats screen) does not pull the store out
  // from under its owner.
  class ScopedLoad {
   public:
    ScopedLoad() : ownedLoad_(!ReadingStatsStore::getInstance().isLoaded()) {
      ReadingStatsStore::getInstance().ensureLoaded();
    }
    ~ScopedLoad() {
      if (ownedLoad_) ReadingStatsStore::getInstance().release();
    }
    ScopedLoad(const ScopedLoad&) = delete;
    ScopedLoad& operator=(const ScopedLoad&) = delete;

   private:
    bool ownedLoad_;
  };

 private:
  bool loaded_ = false;
};

#define READING_STATS ReadingStatsStore::getInstance()
