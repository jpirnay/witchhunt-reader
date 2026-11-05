#include "TextShaping.h"

#include <cstring>

#include "BidiUtils.h"
#include "HebrewShaper.h"
#include "IndicScripts.h"
#include "Utf8.h"

namespace TextShaping {

uint32_t decodeAt(std::string_view text, size_t& pos) {
  // utf8NextCodepoint reads up to four bytes and stops at a NUL, so it decodes a NUL-padded copy of what the view
  // holds.
  unsigned char padded[5] = {};
  const size_t n = text.size() - pos < 4 ? text.size() - pos : 4;
  memcpy(padded, text.data() + pos, n);
  const unsigned char* cursor = padded;
  const uint32_t cp = utf8NextCodepoint(&cursor);
  const size_t used = static_cast<size_t>(cursor - padded);
  pos += used == 0 ? 1 : used;
  return cp;
}

namespace {

// The last code point of a non-empty view: the lead byte is at most three continuation bytes back.
uint32_t lastCodepoint(std::string_view text) {
  size_t start = text.size() - 1;
  while (start > 0 && text.size() - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) start--;
  size_t pos = start;
  const uint32_t cp = decodeAt(text, pos);
  return pos == text.size() ? cp : 0xFFFD;
}

bool holdsMarksOnly(std::string_view text) {
  size_t pos = 0;
  while (pos < text.size()) {
    if (!BidiUtils::isTransparentMark(decodeAt(text, pos))) return false;
  }
  return !text.empty();
}

}  // namespace

bool mergesWithPrevious(const GroupToken& previous, const GroupToken& token, bool indicGroup) {
  if (previous.text.empty() || token.text.empty()) return false;
  if (indicGroup) {
    size_t pos = 0;
    if (previous.styleKey == token.styleKey ||
        !indic::syllableBreakAllowed(lastCodepoint(previous.text), decodeAt(token.text, pos))) {
      return true;
    }
  }
  return holdsMarksOnly(token.text) && HebrewShaper::endsWithName(previous.text.data(), previous.text.size());
}

bool shape(const char* text, size_t len, Unit* units, size_t capacity, size_t& count) {
  // Hebrew letters and points are U+0590-05FF; the lead byte 0xD7 covers U+05C0-05FF, which holds every letter.
  if (text == nullptr || memchr(text, 0xD7, len) == nullptr) return false;
  return HebrewShaper::shape(text, len, units, capacity, count);
}

}  // namespace TextShaping
