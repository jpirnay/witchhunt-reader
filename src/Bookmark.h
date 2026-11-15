#pragma once

#include <cstdint>
#include <string>

struct Bookmark {
  uint16_t spineIndex;
  uint16_t pageNumber;
  std::string name;  // optional user-provided label (empty = use default)
};

// What finds a starred page's text again once the chapter is laid out anew (another font, size, margin, spacing):
// the page number belongs to one layout, the KOReader XPath of the page's first text to none, and neither does the
// text's offset in the chapter, while the section format it was counted under stands. Kept by one book's
// BookmarkStore beside its Bookmarks, not in them: the global index holds every book's bookmarks in RAM and needs
// none of this, so it stays out of global_bookmarks.bin as well.
struct StarAnchor {
  static constexpr uint32_t kNoOffset = UINT32_MAX;

  // Section::layoutFingerprint() of the layout pageNumber was counted in, and that layout's page count for the
  // chapter. 0: not known: a TXT page, which keeps its page, or a bookmark from older firmware, whose page is taken as
  // counted in the layout its chapter is first shown complete in (StarAnchoring::settleKnown).
  uint32_t layout = 0;
  uint16_t pageCount = 0;
  // ChapterXPathIndexer's name for the text the page starts with, the one a KOReader push sends: what the text is,
  // whatever counts it. Empty: none yet, or none to be had.
  std::string xpath;
  // Where that text starts in the chapter (Section::getVisibleTextOffsetForPage, VisibleText.h), and the section
  // format it was counted under (Section::formatVersion()): from the page when the star was made, or from resolving
  // the XPath. The count is about the book's source, not its layout, so a relayout needs nothing else; under another
  // format it is not to be trusted, and the XPath is resolved again. kNoOffset and 0: not known.
  uint32_t offset = kNoOffset;
  uint8_t offsetFormat = 0;

  // This session only, never saved. A bookmark made or anchored this session, whose text is still to be named: tried
  // once, whatever comes of it. A chapter with no text (a cover) has nothing to name, and its star goes by the offset
  // alone; asking again at every open would parse it every time.
  bool nameOwed = false;
  // A mapping failed (no text at the offset, a parse the heap refused, no offset
  // table): nothing more is tried for this star until the book opens again, so idle time does not parse the chapter on
  // every tick.
  bool mappingFailed = false;
};
