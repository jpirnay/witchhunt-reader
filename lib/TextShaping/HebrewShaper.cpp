#include "HebrewShaper.h"

#include "BidiUtils.h"

namespace HebrewShaper {
namespace {

constexpr uint32_t YOD = 0x05D9;
constexpr uint32_t HE = 0x05D4;
constexpr uint32_t VAV = 0x05D5;
constexpr uint32_t GERESH = 0x05F3;

// Decodes the code point at text[pos], never reading past len, with the same rule as utf8NextCodepoint (a bad or
// missing continuation byte consumes the bytes read so far; an overlong, surrogate or out-of-range value, or a
// lead byte that starts nothing, consumes one) so the units match what the rest of the pipeline decodes.
uint32_t decodeAt(const char* text, size_t len, size_t& pos) {
  const auto b0 = static_cast<uint8_t>(text[pos]);
  if (b0 < 0x80) {
    pos++;
    return b0;
  }
  size_t n = 0;
  uint32_t cp = 0;
  if ((b0 >> 5) == 0x6) {
    n = 2;
    cp = b0 & 0x1F;
  } else if ((b0 >> 4) == 0xE) {
    n = 3;
    cp = b0 & 0x0F;
  } else if ((b0 >> 3) == 0x1E) {
    n = 4;
    cp = b0 & 0x07;
  } else {
    pos++;
    return 0xFFFD;
  }
  for (size_t i = 1; i < n; i++) {
    if (pos + i >= len || (static_cast<uint8_t>(text[pos + i]) & 0xC0) != 0x80) {
      pos += i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (static_cast<uint8_t>(text[pos + i]) & 0x3F);
  }
  const bool overlong = (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
  if (overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    pos++;
    return 0xFFFD;
  }
  pos += n;
  return cp;
}

// Skips transparent marks from pos; returns the offset of the first code point that is not one.
size_t skipMarks(const char* text, size_t len, size_t pos) {
  while (pos < len) {
    size_t next = pos;
    if (!BidiUtils::isTransparentMark(decodeAt(text, len, next))) break;
    pos = next;
  }
  return pos;
}

// If the divine name starts at `start` (a yod), returns the offset just past it and its trailing marks, else 0.
size_t matchNameAt(const char* text, size_t len, size_t start) {
  static constexpr uint32_t kRest[] = {HE, VAV, HE};
  size_t pos = start;
  decodeAt(text, len, pos);  // the yod
  for (const uint32_t want : kRest) {
    pos = skipMarks(text, len, pos);
    if (pos >= len) return 0;
    if (decodeAt(text, len, pos) != want) return 0;
  }
  return skipMarks(text, len, pos);
}

// Walks the token once. Counts the units the result needs and whether a name was found; when `out` is not null
// it also writes them (the caller has checked the count against the capacity).
void walk(const char* text, size_t len, TextShaping::Unit* out, size_t& units, bool& found) {
  units = 0;
  found = false;
  size_t pos = 0;
  while (pos < len) {
    const size_t at = pos;
    size_t next = pos;
    const uint32_t cp = decodeAt(text, len, next);
    const size_t matchEnd = (cp == YOD) ? matchNameAt(text, len, at) : 0;
    const auto cluster = static_cast<uint16_t>(at);
    if (matchEnd != 0) {
      if (out != nullptr) {
        out[units] = {HE, cluster};
        out[units + 1] = {GERESH, cluster};
      }
      units += 2;
      found = true;
      pos = matchEnd;
    } else {
      if (out != nullptr) out[units] = {cp, cluster};
      units += 1;
      pos = next;
    }
  }
}

}  // namespace

bool endsWithName(const char* text, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    const size_t at = pos;
    if (decodeAt(text, len, pos) == YOD && matchNameAt(text, len, at) == len) return true;
  }
  return false;
}

bool shape(const char* text, size_t len, TextShaping::Unit* units, size_t capacity, size_t& count) {
  if (len > UINT16_MAX) return false;  // clusters are 16-bit; no token is this long
  size_t needed = 0;
  bool found = false;
  walk(text, len, nullptr, needed, found);  // decide first: a false result must leave the buffer untouched
  if (!found || needed > capacity) return false;
  walk(text, len, units, needed, found);
  count = needed;
  return true;
}

}  // namespace HebrewShaper
