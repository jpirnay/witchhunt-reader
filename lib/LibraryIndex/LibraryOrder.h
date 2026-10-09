#pragma once

#include <BuildArena.h>

#include <cstddef>
#include <cstdint>
#include <string>

// An author's books in the order the Library lists them: by series, then series index, then title,
// with the books in no series after the series.
namespace LibraryOrder {

// What a book sorts by, as its details give it.
struct BookKey {
  std::string series;
  std::string seriesIndex;  // as the book writes it: "3", "2.5"; not a number sorts as 0
  std::string title;
};

// Fills `key` for the book with index record `record`; false when its details cannot be read, which
// sorts it as a book with no series and no title.
using KeyFn = bool (*)(void* user, uint16_t record, BookKey& key);

// Sorts `records` in place. The keys go into `arena`, which is reset first -- it is scratch: the
// lent framebuffer, never the heap -- as fixed-length prefixes: two titles that agree on their first
// 28 bytes keep their order. False, with `records` untouched, when the arena cannot hold the keys.
bool sortBySeries(uint16_t* records, size_t count, BuildArena& arena, KeyFn keyOf, void* user);

}  // namespace LibraryOrder
