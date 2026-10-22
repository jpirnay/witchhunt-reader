#include "ReadingStatsJson.h"

#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "ReadingStats.h"

namespace ReadingStatsJson {
namespace {

// Reads in blocks, not bytes. Larger blocks buy little: on the X3 a 108 KB history scans in 241 ms
// with 1 KB blocks and 228 ms with 4 KB ones.
constexpr size_t kReadBlock = 1024;

void emit(Print& out, const char* text, const size_t len) { out.write(reinterpret_cast<const uint8_t*>(text), len); }

void emit(Print& out, const char* text) { emit(out, text, strlen(text)); }

// Emits what snprintf() wrote into `buf`, and nothing when it did not fit: snprintf() returns the
// length it wanted, and emitting that from a smaller buffer reads past it.
void emitBounded(Print& out, const char* buf, const int n, const size_t cap) {
  if (n > 0 && static_cast<size_t>(n) < cap) {
    emit(out, buf, static_cast<size_t>(n));
  } else {
    LOG_ERR("RSF", "formatted field did not fit (%d of %u bytes); left out", n, static_cast<unsigned>(cap));
  }
}

// One `,"key":value` field. 48 bytes hold the longest key and any 64-bit value.
void emitNumber(Print& out, const char* key, const long long value) {
  char field[48];
  const int n = snprintf(field, sizeof(field), ",\"%s\":%lld", key, value);
  emitBounded(out, field, n, sizeof(field));
}

void emitDays(Print& out, const std::vector<DayBucket>& days) {
  emit(out, "[");
  char pair[32];
  for (size_t i = 0; i < days.size(); ++i) {
    const int n = snprintf(pair, sizeof(pair), "%s[%u,%lu]", i == 0 ? "" : ",", days[i].dayIndex,
                           static_cast<unsigned long>(days[i].seconds));
    emitBounded(out, pair, n, sizeof(pair));
  }
  emit(out, "]");
}

void emitString(Print& out, const std::string& text) {
  emit(out, "\"");
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        emit(out, "\\\"");
        break;
      case '\\':
        emit(out, "\\\\");
        break;
      case '\b':
        emit(out, "\\b");
        break;
      case '\f':
        emit(out, "\\f");
        break;
      case '\n':
        emit(out, "\\n");
        break;
      case '\r':
        emit(out, "\\r");
        break;
      case '\t':
        emit(out, "\\t");
        break;
      default:
        if (c < 0x20) {
          char escaped[8];
          const int n = snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          emitBounded(out, escaped, n, sizeof(escaped));
        } else {
          out.write(c);
        }
    }
  }
  emit(out, "\"");
}

uint32_t toCount(const char* text) {
  const long long v = strtoll(text, nullptr, 10);
  if (v < 0) return 0;
  if (v > static_cast<long long>(std::numeric_limits<uint32_t>::max())) return std::numeric_limits<uint32_t>::max();
  return static_cast<uint32_t>(v);
}

time_t toEpoch(const char* text) {
  const long long v = strtoll(text, nullptr, 10);
  return v > 0 ? static_cast<time_t>(v) : 0;
}

// Walks the file with the SAX parser, keeping track of where it is in the stats layout, and hands
// the subclasses what they need: the top-level counters, the global day buckets, and each book.
// Fed a block at a time; nothing needs the offset of a byte.
//
//   { "totalSeconds": n, ..., "globalDays": [[d, s], ...], "books": [ { ..., "days": [[d, s]] } ] }
//   depth 1                   2            3              2          3            4  5
class Scanner {
 public:
  struct Book {
    std::string docId;
    std::string title;
    std::string author;
    uint32_t totalSeconds = 0;
    uint32_t pagesTurned = 0;
    uint32_t sessions = 0;
    time_t firstReadEpoch = 0;
    time_t lastReadEpoch = 0;
    uint8_t progress = 0;
    uint16_t finishedCount = 0;
    bool hasFinishedCount = false;
    bool finishedFlag = false;  // the legacy bool, read only when finishedCount is absent
    time_t lastFinishedEpoch = 0;
    std::vector<DayBucket> days;

    bool finished() const { return hasFinishedCount ? finishedCount > 0 : finishedFlag; }

    // As a load would hold it.
    BookReadingStats toStats() const {
      BookReadingStats s;
      s.docId = docId;
      s.title = title;
      s.author = author;
      s.totalSeconds = totalSeconds;
      s.pagesTurned = pagesTurned;
      s.sessions = sessions;
      s.firstReadEpoch = firstReadEpoch;
      s.lastReadEpoch = lastReadEpoch;
      s.progress = progress;
      s.finishedCount = hasFinishedCount ? finishedCount : (finishedFlag ? 1 : 0);
      s.lastFinishedEpoch = lastFinishedEpoch;
      s.days = days;
      ReadingStatsStore::trimBookDays(s.days);
      return s;
    }
  };

  Scanner() = default;
  virtual ~Scanner() = default;

  ScanResult run(HalFile& in) {
    auto parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks());
    auto block = makeUniqueNoThrow<char[]>(kReadBlock);
    if (!parser || !block) {
      LOG_ERR("RSF", "OOM: stats file parser");
      return ScanResult::NoMemory;
    }
    if (!in.seekSet(0)) return ScanResult::IoError;
    for (;;) {
      const int n = in.read(block.get(), kReadBlock);
      if (n < 0) return ScanResult::IoError;
      if (n == 0) break;
      parser->feed(block.get(), static_cast<size_t>(n));
    }
    const bool complete = rootClosed_ && depth_ == 0;
    return !parser->hasError() && complete ? ScanResult::Ok : ScanResult::Malformed;
  }

 protected:
  enum class Top : uint8_t { Other, TotalSeconds, TotalSessions, TotalPagesTurned, LongestStreak, GlobalDays, Books };

  virtual void onCounter(Top, uint32_t) {}
  virtual void onGlobalDay(uint16_t, uint32_t) {}
  // While the parser takes in the book's closing brace.
  virtual void onBookEnd(const Book&) {}

 private:
  enum class Field : uint8_t {
    Other,
    DocId,
    Title,
    Author,
    TotalSeconds,
    PagesTurned,
    Sessions,
    FirstRead,
    LastRead,
    Progress,
    FinishedCount,
    Finished,
    LastFinished,
    Days
  };

  static Top topFor(const char* key) {
    if (strcmp(key, "totalSeconds") == 0) return Top::TotalSeconds;
    if (strcmp(key, "totalSessions") == 0) return Top::TotalSessions;
    if (strcmp(key, "totalPagesTurned") == 0) return Top::TotalPagesTurned;
    if (strcmp(key, "longestStreak") == 0) return Top::LongestStreak;
    if (strcmp(key, "globalDays") == 0) return Top::GlobalDays;
    if (strcmp(key, "books") == 0) return Top::Books;
    return Top::Other;
  }

  static Field fieldFor(const char* key) {
    if (strcmp(key, "docId") == 0) return Field::DocId;
    if (strcmp(key, "title") == 0) return Field::Title;
    if (strcmp(key, "author") == 0) return Field::Author;
    if (strcmp(key, "totalSeconds") == 0) return Field::TotalSeconds;
    if (strcmp(key, "pagesTurned") == 0) return Field::PagesTurned;
    if (strcmp(key, "sessions") == 0) return Field::Sessions;
    if (strcmp(key, "firstReadEpoch") == 0) return Field::FirstRead;
    if (strcmp(key, "lastReadEpoch") == 0) return Field::LastRead;
    if (strcmp(key, "progress") == 0) return Field::Progress;
    if (strcmp(key, "finishedCount") == 0) return Field::FinishedCount;
    if (strcmp(key, "finished") == 0) return Field::Finished;
    if (strcmp(key, "lastFinishedEpoch") == 0) return Field::LastFinished;
    if (strcmp(key, "days") == 0) return Field::Days;
    return Field::Other;
  }

  static constexpr int kBookDepth = 3;
  bool atBook() const { return top_ == Top::Books && depth_ == kBookDepth; }
  bool atBookDayPair() const { return top_ == Top::Books && field_ == Field::Days && depth_ == kBookDepth + 2; }
  bool atGlobalDayPair() const { return top_ == Top::GlobalDays && depth_ == 3; }

  void objectStart() {
    ++depth_;
    if (depth_ == 1) rootSeen_ = true;
    if (atBook()) {
      book_ = Book{};
      field_ = Field::Other;
    }
  }

  void objectEnd() {
    if (atBook()) onBookEnd(book_);
    if (depth_ == 1 && rootSeen_) rootClosed_ = true;
    if (depth_ > 0) --depth_;
  }

  void arrayStart() {
    ++depth_;
    if (atGlobalDayPair() || atBookDayPair()) {
      pairIndex_ = 0;
      pairDay_ = 0;
      pairSeconds_ = 0;
    }
  }

  void arrayEnd() {
    if ((atGlobalDayPair() || atBookDayPair()) && pairIndex_ >= 2 && pairDay_ != 0 && pairSeconds_ != 0) {
      // Same filter as the loader: a zero day or zero seconds is not a bucket.
      if (atGlobalDayPair()) {
        onGlobalDay(pairDay_, pairSeconds_);
      } else {
        book_.days.push_back({pairDay_, pairSeconds_});
      }
    }
    if (depth_ > 0) --depth_;
  }

  void key(const char* text) {
    if (depth_ == 1) {
      top_ = topFor(text);
    } else if (atBook()) {
      field_ = fieldFor(text);
    }
  }

  void number(const char* text) {
    if (depth_ == 1) {
      onCounter(top_, toCount(text));
    } else if (atGlobalDayPair() || atBookDayPair()) {
      if (pairIndex_ == 0) pairDay_ = static_cast<uint16_t>(std::min<uint32_t>(toCount(text), 0xFFFF));
      if (pairIndex_ == 1) pairSeconds_ = toCount(text);
      ++pairIndex_;
    } else if (atBook()) {
      switch (field_) {
        case Field::TotalSeconds:
          book_.totalSeconds = toCount(text);
          break;
        case Field::PagesTurned:
          book_.pagesTurned = toCount(text);
          break;
        case Field::Sessions:
          book_.sessions = toCount(text);
          break;
        case Field::FirstRead:
          book_.firstReadEpoch = toEpoch(text);
          break;
        case Field::LastRead:
          book_.lastReadEpoch = toEpoch(text);
          break;
        case Field::Progress:
          book_.progress = static_cast<uint8_t>(std::min<uint32_t>(toCount(text), 0xFF));
          break;
        case Field::FinishedCount:
          book_.finishedCount = static_cast<uint16_t>(std::min<uint32_t>(toCount(text), 0xFFFF));
          book_.hasFinishedCount = true;
          break;
        case Field::LastFinished:
          book_.lastFinishedEpoch = toEpoch(text);
          break;
        default:
          break;
      }
    }
  }

  void string(const char* text) {
    if (!atBook()) return;
    if (field_ == Field::DocId) book_.docId = text;
    if (field_ == Field::Title) book_.title = text;
    if (field_ == Field::Author) book_.author = text;
  }

  void boolean(const bool value) {
    if (atBook() && field_ == Field::Finished) book_.finishedFlag = value;
  }

  JsonCallbacks callbacks() {
    JsonCallbacks cb{};
    cb.ctx = this;
    cb.onKey = [](void* ctx, const char* v, size_t) { static_cast<Scanner*>(ctx)->key(v); };
    cb.onString = [](void* ctx, const char* v, size_t) { static_cast<Scanner*>(ctx)->string(v); };
    cb.onNumber = [](void* ctx, const char* v, size_t) { static_cast<Scanner*>(ctx)->number(v); };
    cb.onBool = [](void* ctx, bool v) { static_cast<Scanner*>(ctx)->boolean(v); };
    cb.onObjectStart = [](void* ctx) { static_cast<Scanner*>(ctx)->objectStart(); };
    cb.onObjectEnd = [](void* ctx) { static_cast<Scanner*>(ctx)->objectEnd(); };
    cb.onArrayStart = [](void* ctx) { static_cast<Scanner*>(ctx)->arrayStart(); };
    cb.onArrayEnd = [](void* ctx) { static_cast<Scanner*>(ctx)->arrayEnd(); };
    return cb;
  }

  int depth_ = 0;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  Top top_ = Top::Other;
  Field field_ = Field::Other;
  uint8_t pairIndex_ = 0;
  uint16_t pairDay_ = 0;
  uint32_t pairSeconds_ = 0;
  Book book_;
};

class SummaryScan final : public Scanner {
 public:
  SummaryScan(Summary& summary, const ScanRequest& request) : summary_(summary), request_(request) {}

 private:
  void onCounter(const Top top, const uint32_t value) override {
    switch (top) {
      case Top::TotalSeconds:
        summary_.totalSeconds = value;
        break;
      case Top::TotalSessions:
        summary_.totalSessions = value;
        break;
      case Top::TotalPagesTurned:
        summary_.totalPagesTurned = value;
        break;
      case Top::LongestStreak:
        summary_.longestStreak = static_cast<uint16_t>(std::min<uint32_t>(value, 0xFFFF));
        break;
      default:
        break;
    }
  }

  void onGlobalDay(const uint16_t day, const uint32_t seconds) override {
    summary_.globalDays.push_back({day, seconds});
  }

  void onBookEnd(const Book& book) override {
    // The loader skips an entry without a docId; so does the request.
    if (book.docId.empty()) return;
    if (request_.onBook) request_.onBook(book.toStats());
  }

  Summary& summary_;
  const ScanRequest& request_;
};

}  // namespace

ScanResult scan(HalFile& in, Summary& summary, const ScanRequest& request) {
  summary = Summary{};
  SummaryScan pass(summary, request);
  const ScanResult result = pass.run(in);
  if (result != ScanResult::Ok) return result;
  // What a load would hold: the newest kMaxGlobalDays buckets, the record folded in first.
  ReadingStatsStore::trimGlobalDays(summary.globalDays, summary.longestStreak);
  return ScanResult::Ok;
}

void writeDashboardHead(Print& out, const ReadingTotals& totals, const uint32_t bookCount,
                        const uint32_t finishedBookCount, const uint16_t today) {
  char head[192];
  int n = snprintf(head, sizeof(head),
                   "{\"totalSeconds\":%lu,\"totalSessions\":%lu,\"totalPagesTurned\":%lu,\"bookCount\":%lu,"
                   "\"finishedBookCount\":%lu,\"todayDayIndex\":%u",
                   static_cast<unsigned long>(totals.totalSeconds), static_cast<unsigned long>(totals.totalSessions),
                   static_cast<unsigned long>(totals.totalPagesTurned), static_cast<unsigned long>(bookCount),
                   static_cast<unsigned long>(finishedBookCount), today);
  emitBounded(out, head, n, sizeof(head));
  if (today != 0 && !totals.globalDays.empty()) {
    n = snprintf(head, sizeof(head), ",\"currentStreak\":%u,\"longestStreak\":%u",
                 ReadingStatsStore::currentStreakIn(totals.globalDays, today),
                 ReadingStatsStore::longestStreakIn(totals.globalDays, totals.longestStreak));
    emitBounded(out, head, n, sizeof(head));
  }
  emit(out, ",\"globalDays\":");
  emitDays(out, totals.globalDays);
  emit(out, ",\"books\":[");
}

void writeFileHead(Print& out, const ReadingTotals& totals) {
  char head[160];
  const int n =
      snprintf(head, sizeof(head),
               "{\"totalSeconds\":%lu,\"totalSessions\":%lu,\"totalPagesTurned\":%lu,\"longestStreak\":%u,"
               "\"globalDays\":",
               static_cast<unsigned long>(totals.totalSeconds), static_cast<unsigned long>(totals.totalSessions),
               static_cast<unsigned long>(totals.totalPagesTurned), totals.longestStreak);
  emitBounded(out, head, n, sizeof(head));
  emitDays(out, totals.globalDays);
  emit(out, ",\"books\":[");
}

void writeBookSeparator(Print& out) { emit(out, ","); }

void writeTail(Print& out) { emit(out, "]}"); }

void writeBook(Print& out, const BookReadingStats& book, const long long etaSeconds) {
  emit(out, "{\"docId\":");
  emitString(out, book.docId);
  emit(out, ",\"title\":");
  emitString(out, book.title);
  emit(out, ",\"author\":");
  emitString(out, book.author);
  // One field at a time: a finished book on a synced clock with real reading overflowed the single
  // 192-byte line this used to be, and every write of it then failed its read-back.
  emitNumber(out, "totalSeconds", book.totalSeconds);
  emitNumber(out, "pagesTurned", book.pagesTurned);
  emitNumber(out, "sessions", book.sessions);
  emitNumber(out, "firstReadEpoch", static_cast<long long>(book.firstReadEpoch));
  emitNumber(out, "lastReadEpoch", static_cast<long long>(book.lastReadEpoch));
  emitNumber(out, "progress", book.progress);
  emitNumber(out, "finishedCount", book.finishedCount);
  emitNumber(out, "lastFinishedEpoch", static_cast<long long>(book.lastFinishedEpoch));
  emit(out, book.finishedCount > 0 ? ",\"finished\":true,\"days\":" : ",\"finished\":false,\"days\":");
  emitDays(out, book.days);
  if (etaSeconds >= 0) emitNumber(out, "etaSeconds", etaSeconds);
  emit(out, "}");
}

}  // namespace ReadingStatsJson
