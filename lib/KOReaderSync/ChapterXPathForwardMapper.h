#pragma once

#include <Epub.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

std::string findXPathForProgressInternal(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

// The XPath of the text at `visibleOffset` (VisibleText.h's count from the chapter's start).
// Text that is a direct child of a block element is named to the codepoint, /text()[N].M; text
// inside an inline element is named by the element. Empty when the offset is past the chapter's
// text or the parse fails.
std::string findXPathForVisibleOffsetInternal(const std::shared_ptr<Epub>& epub, int spineIndex,
                                              uint32_t visibleOffset);

}  // namespace ChapterXPathIndexerInternal
