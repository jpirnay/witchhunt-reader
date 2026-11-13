#pragma once

#include <Epub/BookSearch.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// A search of the open book: the query, its options, and the hits found so far, in reading order from the page the
// search began on, round to the page before it. The reader keeps it while its hits are wanted -- the results list,
// a hit shown on its page, the next and the previous -- and lets it go when the search is closed.
//
// The hits are a fixed array of kMaxHits, taken once (about 4.6 KB): a search that finds more says so and keeps the
// first ones.
struct BookSearchSession {
  static constexpr size_t kMaxHits = 50;

  std::string query;
  bool wholeWords = false;
  std::unique_ptr<BookSearch::Hit[]> hits;
  size_t count = 0;
  // Where the search began: the page's chapter and the visible-text offset at its start.
  uint16_t originSpine = 0;
  uint32_t originOffset = 0;
  // The search read the whole book (not cancelled, the hits not full).
  bool complete = false;
  // The hit the reader shows, or -1.
  int current = -1;
  // The reader saved where it was before its first jump to a hit (the link-back stack), so Back returns there.
  bool originSaved = false;

  // The hit array, taken the first time. False when there is no memory for it.
  bool ensureHits() {
    if (!hits) hits = makeUniqueNoThrow<BookSearch::Hit[]>(kMaxHits);
    return hits != nullptr;
  }
  void clearResults() {
    count = 0;
    complete = false;
    current = -1;
  }
  bool full() const { return count >= kMaxHits; }
};
