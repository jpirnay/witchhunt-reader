#pragma once

// Ported from crosspoint-reader (PR #2836 by Uri Tauber).

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
//
// The fallback for a dictionary definition whose HTML the layout path could not
// use -- too damaged to parse, too large to keep resident as laid-out Pages, or
// laid out on a heap that could not spare the room. See DictHtmlPages.h.
std::string htmlToPlainText(const std::string& html);
