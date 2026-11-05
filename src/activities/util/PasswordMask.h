#pragma once

#include <Utf8.h>

#include <cstddef>
#include <string>

// What a password field shows: a star per byte, except for the character that ends at `revealEnd`
// (std::string::npos for none). Per byte, so the result keeps the text's length and every index means
// the same thing in both; a non-ASCII character therefore shows as two or three stars. The fix for a
// lone revealed byte comes from crosspoint-reader #4106 (Leopoldo Pla Sempere).
inline std::string maskPassword(const std::string& text, const size_t revealEnd) {
  std::string masked(text.size(), '*');
  if (revealEnd == std::string::npos || revealEnd == 0 || revealEnd > text.size()) return masked;
  // The whole character: a lone byte of one is no text, and draws as a garbled glyph.
  const size_t start = utf8PrevCharStart(text, revealEnd);
  masked.replace(start, revealEnd - start, text, start, revealEnd - start);
  return masked;
}
