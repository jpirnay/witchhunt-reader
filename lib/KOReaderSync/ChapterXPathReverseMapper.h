#pragma once

#include <Epub.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

// outListItemIndex (when non-null) receives the running <li> count at the matched
// element's position whenever the target XPath's deepest element is /li[N]. Set to
// 0 if the target wasn't <li>-anchored or no match was found.
// outVisibleOffset (when non-null) receives the matched position as a visible-text offset in the
// chapter (VisibleText.h's rule). Set to 0 when no match was found. Only an exact match makes it
// a position; for an inexact one it is the stand-in's offset.
bool findProgressForXPathInternal(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                  float& outIntraSpineProgress, bool& outExactMatch,
                                  uint16_t* outListItemIndex = nullptr, uint32_t* outVisibleOffset = nullptr);

}  // namespace ChapterXPathIndexerInternal
