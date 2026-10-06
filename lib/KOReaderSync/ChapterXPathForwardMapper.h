#pragma once

#include <Epub.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

std::string findXPathForProgressInternal(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

// The XPath of the text at `visibleOffset` (VisibleText.h's count from the chapter's start), named
// to the codepoint as /text()[N].M in a block element or inside inline elements
// (/p[K]/span[1]/text()[N].M); stray text in table rows by its element. An offset before an image
// names the first text after it; an offset equal to the chapter's total names the end of its last
// text node. Empty when the offset is past the chapter's text or the parse fails.
std::string findXPathForVisibleOffsetInternal(const std::shared_ptr<Epub>& epub, int spineIndex,
                                              uint32_t visibleOffset);

}  // namespace ChapterXPathIndexerInternal
