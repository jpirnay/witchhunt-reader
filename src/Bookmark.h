#pragma once

#include <cstdint>
#include <string>

struct Bookmark {
  uint16_t spineIndex;
  uint16_t pageNumber;
  std::string name;  // optional user-provided label (empty = use default)
};

// What finds a starred page's text again once the chapter is laid out anew (another font, size, margin, spacing):
// the page number belongs to one layout, the KOReader XPath of the page's first text to none. Kept by one book's
// BookmarkStore beside its Bookmarks, not in them: the global index holds every book's bookmarks in RAM and needs
// none of this, so it stays out of global_bookmarks.bin as well.
struct StarAnchor {
  // Section::layoutFingerprint() of the layout pageNumber was counted in, and that layout's page count for the
  // chapter. 0: not known (a TXT page, a star from older firmware); such a star keeps its page.
  uint32_t layout = 0;
  uint16_t pageCount = 0;
  // ChapterXPathIndexer's name for the text the page starts with, the one a KOReader push sends. Empty: none yet, or
  // none to be had.
  std::string xpath;
};
