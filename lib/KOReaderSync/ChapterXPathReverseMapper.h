#pragma once

#include <Epub.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

// outListItemIndex (when non-null) receives the running <li> count at the matched
// element's position whenever the target XPath's deepest element is /li[N]. Set to
// 0 if the target wasn't <li>-anchored or no match was found.
// outVisibleOffset (when non-null) receives the match's anchor as a visible-text offset in the
// chapter (VisibleText.h's rule). Set to 0 when no match was found.
// outIsTextPoint (when non-null) is true only when the match is a /text()[N].M point resolved to
// the codepoint: the one case where outVisibleOffset is a position. An element match, exact or
// not, is anchored at its first direct text or, lacking any, at its END tag.
bool findProgressForXPathInternal(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                  float& outIntraSpineProgress, bool& outExactMatch,
                                  uint16_t* outListItemIndex = nullptr, uint32_t* outVisibleOffset = nullptr,
                                  bool* outIsTextPoint = nullptr);

}  // namespace ChapterXPathIndexerInternal
