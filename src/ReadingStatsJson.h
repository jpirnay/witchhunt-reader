#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ReadingStatsTypes.h"

// The reading history as JSON: the reading-stats.json of older firmware, read once to import it
// (ReadingStatsStore::prepare()), and the same format written for the web -- the /api/stats payload
// and the export, which older firmware reads back. Reading is streamed: one parser, a small read
// buffer and the global day buckets, however many books the file has.
namespace ReadingStatsJson {

constexpr char kPath[] = "/.crosspoint/reading-stats.json";

// How a pass over the file ended. NoMemory and IoError are transient — the file may be fine and a
// later pass may succeed; Malformed is the file's own fault.
enum class ScanResult : uint8_t { Ok, NoMemory, IoError, Malformed };

// What a pass collects beyond the global figures.
struct ScanRequest {
  // Each book with a docId, as the pass finishes it: in file order, every field decoded, days
  // trimmed the way a load would.
  std::function<void(const BookReadingStats&)> onBook;
};

// One pass over the file: the global figures and whatever the request asked for. Days are
// filtered and trimmed the way a load would.
struct Summary : ReadingTotals {};

ScanResult scan(HalFile& in, Summary& summary, const ScanRequest& request);

// One book as a JSON object, in the field order the device has always written. Strings are
// escaped the way ArduinoJson does: quote, backslash and control characters; UTF-8 passes through.
// With `etaSeconds` not negative, the dashboard's `"etaSeconds"` field goes last, before the
// closing brace, where the web page has always found it.
void writeBook(Print& out, const BookReadingStats& book, long long etaSeconds = -1);

// The history file's text up to its books, `{"totalSeconds":…,"globalDays":[…],"books":[`. The
// books follow through writeBook() and writeBookSeparator(), then writeTail().
void writeFileHead(Print& out, const ReadingTotals& totals);

// The /api/stats payload up to its books: the figures, the counts, today's day index, the streaks
// when `today` is known and there are days, and the global days.
void writeDashboardHead(Print& out, const ReadingTotals& totals, uint32_t bookCount, uint32_t finishedBookCount,
                        uint16_t today);

void writeBookSeparator(Print& out);  // between two books
void writeTail(Print& out);           // after the last book: closes the books and the object

}  // namespace ReadingStatsJson
