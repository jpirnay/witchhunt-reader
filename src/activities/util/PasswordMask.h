#pragma once

#include <Utf8.h>

#include <cstddef>
#include <string>

// What a password field shows: a star per character, except for the character that ends at `revealEnd`
// (std::string::npos for none), shown whole. A star per character, so a Hebrew, Greek or Cyrillic letter is one star
// like a Latin one, rather than one per byte of its UTF-8. The fix for a lone revealed byte comes from
// crosspoint-reader #4106 (Leopoldo Pla Sempere).
//
// The mask is shorter than the text wherever a character takes more than one byte, so a position in one is carried
// to the other with maskedOffset() and unmaskedOffset().

namespace password_mask_detail {

// Where the character starting at `i` ends.
inline size_t nextCharStart(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  ++i;
  while (i < s.size() && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}

// The first byte of the character shown in the clear, or npos when none is.
inline size_t revealStart(const std::string& text, const size_t revealEnd) {
  if (revealEnd == std::string::npos || revealEnd == 0 || revealEnd > text.size()) return std::string::npos;
  return utf8PrevCharStart(text, revealEnd);
}

}  // namespace password_mask_detail

inline std::string maskPassword(const std::string& text, const size_t revealEnd) {
  // The whole character: a lone byte of one is no text, and draws as a garbled glyph.
  const size_t shown = password_mask_detail::revealStart(text, revealEnd);
  std::string masked;
  masked.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const size_t next = password_mask_detail::nextCharStart(text, i);
    if (i == shown) {
      masked.append(text, i, next - i);
    } else {
      masked += '*';
    }
    i = next;
  }
  return masked;
}

// Where byte `pos` of `text`, a character boundary, falls in maskPassword(text, revealEnd).
inline size_t maskedOffset(const std::string& text, const size_t revealEnd, const size_t pos) {
  const size_t shown = password_mask_detail::revealStart(text, revealEnd);
  size_t masked = 0;
  for (size_t i = 0; i < pos && i < text.size();) {
    const size_t next = password_mask_detail::nextCharStart(text, i);
    masked += i == shown ? next - i : 1;
    i = next;
  }
  return masked;
}

// The reverse: the byte of `text` where the character at `maskedPos` of the mask starts, or the first boundary after
// it when `maskedPos` falls inside a character shown in the clear.
inline size_t unmaskedOffset(const std::string& text, const size_t revealEnd, const size_t maskedPos) {
  const size_t shown = password_mask_detail::revealStart(text, revealEnd);
  size_t masked = 0;
  size_t i = 0;
  while (i < text.size() && masked < maskedPos) {
    const size_t next = password_mask_detail::nextCharStart(text, i);
    masked += i == shown ? next - i : 1;
    i = next;
  }
  return i;
}
