#include "LibraryOrder.h"

#include <FsHelpers.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t SERIES_BYTES = 24;
constexpr size_t TITLE_BYTES = 28;

// Byte-comparable prefixes of the natural sort keys (FsHelpers::naturalSortKey), so the sort itself
// reads no strings and allocates nothing.
struct Key {
  uint8_t series[SERIES_BYTES];
  uint8_t title[TITLE_BYTES];
  float index;
  uint16_t record;
  bool inSeries;
};

void fill(const std::string& text, uint8_t* out, const size_t cap) {
  std::memset(out, 0, cap);
  FsHelpers::naturalSortKey(text.c_str(), out, cap);
}

// strtof reads "nan" as NaN, and a NaN makes every comparison false, which breaks the sort.
float seriesIndex(const std::string& text) {
  const float value = std::strtof(text.c_str(), nullptr);
  return std::isfinite(value) ? value : 0.0f;
}

bool before(const Key& a, const Key& b) {
  if (a.inSeries != b.inSeries) return a.inSeries;
  if (const int bySeries = std::memcmp(a.series, b.series, SERIES_BYTES); bySeries != 0) return bySeries < 0;
  if (a.index != b.index) return a.index < b.index;
  if (const int byTitle = std::memcmp(a.title, b.title, TITLE_BYTES); byTitle != 0) return byTitle < 0;
  return a.record < b.record;  // a total order, so equal keys come out the same way every time
}

}  // namespace

namespace LibraryOrder {

bool sortBySeries(uint16_t* records, const size_t count, BuildArena& arena, const KeyFn keyOf, void* user) {
  if (count < 2) return true;
  arena.reset();
  Key* keys = arena.allocArray<Key>(count);
  if (keys == nullptr) return false;
  for (size_t i = 0; i < count; ++i) {
    BookKey book;
    if (keyOf != nullptr) keyOf(user, records[i], book);
    Key& key = keys[i];
    fill(book.series, key.series, SERIES_BYTES);
    fill(book.title, key.title, TITLE_BYTES);
    key.index = seriesIndex(book.seriesIndex);
    key.record = records[i];
    key.inSeries = !book.series.empty();
  }
  std::sort(keys, keys + count, before);
  for (size_t i = 0; i < count; ++i) records[i] = keys[i].record;
  return true;
}

}  // namespace LibraryOrder
