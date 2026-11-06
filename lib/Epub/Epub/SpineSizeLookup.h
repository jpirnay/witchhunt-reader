#pragma once

#include <cstddef>

// The first spine item whose cumulative size reaches `target`, or the last one when none does.
//
// Cumulative sizes never fall, an empty chapter's included, so this is a binary search. Each probe is two seeks and a
// record read in book.bin: a linear walk paid that for every chapter before the target, 1,700 of them for a jump to the
// end of a large book, where this pays 11. No table: per-spine data stays on the card.
//
// From CrossInk 52e9fc49 (Julia Nguyen), MIT.
template <typename ReadCumulativeSize>
int findSpineForSize(const int count, const size_t target, ReadCumulativeSize&& readSize) {
  if (count <= 0) return -1;
  int first = 0;
  int last = count;
  while (first < last) {
    const int middle = first + (last - first) / 2;
    if (readSize(middle) < target) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first < count ? first : count - 1;
}
