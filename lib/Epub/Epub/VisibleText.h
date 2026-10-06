#pragma once

#include <strings.h>

#include <cctype>
#include <cstddef>

// The one definition of "visible text" for KOReader sync.
//
// A page's content offset (Section's paragraph LUT) is the number of visible bytes of the
// chapter's source text before the page's first element. The layout parser counts them while it
// lays the chapter out; the XPath mappers count them while they resolve a position. The two have
// to agree down to the byte, so the rule lives here and nowhere else:
//   every non-whitespace byte of SAX-delivered character data inside <body> and outside head,
//   script and style; entity references as their expansion; text the layout hides or drops all
//   the same, because the rule is about the source, not the layout.
namespace VisibleText {

inline bool isSpace(const unsigned char c) { return std::isspace(c) != 0; }

inline size_t visibleBytes(const char* text, const int len) {
  size_t count = 0;
  for (int i = 0; i < len; i++) {
    if (!isSpace(static_cast<unsigned char>(text[i]))) {
      count++;
    }
  }
  return count;
}

// Tags whose text is never visible. Case-insensitive: the mappers lowercase tag names, the
// layout parser does not.
inline bool isNonVisibleTag(const char* tag) {
  return strcasecmp(tag, "head") == 0 || strcasecmp(tag, "script") == 0 || strcasecmp(tag, "style") == 0;
}

}  // namespace VisibleText
