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
  static constexpr uint32_t kNoOffset = UINT32_MAX;

  // Section::layoutFingerprint() of the layout pageNumber was counted in, and that layout's page count for the
  // chapter. 0: not known (a TXT page, a star from older firmware); such a star keeps its page.
  uint32_t layout = 0;
  uint16_t pageCount = 0;
  // ChapterXPathIndexer's name for the text the page starts with, the one a KOReader push sends. Empty: none yet, or
  // none to be had.
  std::string xpath;

  // This session only, never saved. Where the page's text starts in the chapter (Section::getVisibleTextOffsetForPage,
  // VisibleText.h): from the page when the star was made, or from resolving the XPath. Both mappings turn into it, so
  // once it is known neither parses the chapter again before the book closes.
  uint32_t offset = kNoOffset;
  // A mapping failed this session (no text at the offset, a parse the heap refused, no offset table): nothing more is
  // tried for this star until the book opens again, so idle time does not parse the chapter on every tick.
  bool mappingFailed = false;
};
