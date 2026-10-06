#pragma once

#include <cstdint>

/**
 * A reading position in CrossPoint's terms: a spine item and a page within it, plus whatever
 * anchors the section cache's LUTs can snap to a page. Produced by ProgressMapper from a KOReader
 * record; consumed by the reader (to land) and by ProgressComparison (to decide which side is
 * further).
 */
struct CrossPointPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item (estimated if no paragraph LUT)
  int totalPages;                  // Total pages in the current spine item
  uint16_t paragraphIndex = 0;     // 1-based <p> index (0 if unavailable)
  bool hasParagraphIndex = false;  // True when paragraphIndex is valid
  uint16_t listItemIndex = 0;      // 1-based running <li> count when target XPath ends in /li[N]
  bool hasListItemIndex = false;   // True when listItemIndex is valid
  // The page's start as a visible-text offset in the chapter (Section::getVisibleTextOffsetForPage;
  // VisibleText.h's rule). Exact: a push names the text at this offset to the character, a pull
  // resolves to the page this offset falls on. Absent when the chapter is not laid out, or when the
  // pull did not resolve a /text()[N].M point to the codepoint (an element or inexact match).
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
  // True when spineIndex came from the record's own DocFragment (or from local state), not from
  // the percentage. A percentage-derived spine is an estimate that must not outrank one.
  bool hasResolvedSpineIndex = false;
};
