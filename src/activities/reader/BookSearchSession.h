#pragma once

#include <Epub/BookSearch.h>
#include <Epub/BookSearchHitFile.h>

#include <cstddef>
#include <cstdint>
#include <string>

// A search of the open book: the query, its options, and how many hits it found, in reading order from the page the
// search began on, round to the page before it. The hits themselves are on the card (BookSearch::HitFile, in the
// book's cache folder): the results list reads a window of them, the reader one by its index. The reader keeps the
// session while its hits are wanted -- the results list, a hit shown on its page, the next and the previous -- and
// lets it go, and the file, when the search is closed.
struct BookSearchSession {
  // A query of one letter finds a book's worth: the search stops here, and says so. About 89 KB on the card.
  static constexpr size_t kMaxHits = 1000;

  std::string query;
  bool wholeWords = false;
  // The hits' file.
  std::string hitsPath;
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

  void clearResults() {
    count = 0;
    complete = false;
    current = -1;
  }
  bool full() const { return count >= kMaxHits; }
  // Hit `index`, read from the card. False when there is none or it cannot be read.
  bool hit(const size_t index, BookSearch::Hit& out) const {
    return index < count && BookSearch::HitFile::read(hitsPath, index, &out, 1) == 1;
  }
};
