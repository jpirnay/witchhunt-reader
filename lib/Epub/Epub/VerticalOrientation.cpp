#include "VerticalOrientation.h"

#include <Utf8.h>

#include "VerticalOrientationTable.h"

VerticalOrientation verticalOrientationOf(const uint32_t cp) {
  // The last run starting at or before cp; the first run starts at 0, so one always exists.
  size_t lo = 0;
  size_t hi = kVerticalOrientationRunCount;
  while (hi - lo > 1) {
    const size_t mid = lo + (hi - lo) / 2;
    if ((kVerticalOrientationRuns[mid] >> 2) <= cp) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return static_cast<VerticalOrientation>(kVerticalOrientationRuns[lo] & 3);
}

bool drawsUpright(const uint32_t cp) {
  const VerticalOrientation orientation = verticalOrientationOf(cp);
  return orientation == VerticalOrientation::U || orientation == VerticalOrientation::Tu;
}

UprightPlacement uprightPlacementOf(const uint32_t cp) {
  switch (cp) {
    case 0x3001:  // ideographic comma and full stop
    case 0x3002:
    case 0xFF0C:  // fullwidth comma and full stop
    case 0xFF0E:
      return UprightPlacement::Reflect;
    case 0x3041:  // small hiragana: a i u e o, tu, ya yu yo, wa, ka ke
    case 0x3043:
    case 0x3045:
    case 0x3047:
    case 0x3049:
    case 0x3063:
    case 0x3083:
    case 0x3085:
    case 0x3087:
    case 0x308E:
    case 0x3095:
    case 0x3096:
    case 0x30A1:  // small katakana, the same set
    case 0x30A3:
    case 0x30A5:
    case 0x30A7:
    case 0x30A9:
    case 0x30C3:
    case 0x30E3:
    case 0x30E5:
    case 0x30E7:
    case 0x30EE:
    case 0x30F5:
    case 0x30F6:
      return UprightPlacement::SmallKana;
    default:
      return (cp >= 0x31F0 && cp <= 0x31FF) ? UprightPlacement::SmallKana : UprightPlacement::None;  // Ainu extensions
  }
}

namespace {

bool followsItsCharacter(const uint32_t cp) {
  return utf8IsCombiningMark(cp) || cp == 0x3099 || cp == 0x309A || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         (cp >= 0xE0100 && cp <= 0xE01EF);
}

// Bytes of the character at `text` and the marks after it, at most kMaxColumnRunBytes: a cluster longer than that is
// cut at the last codepoint that fits and the remaining marks start the next piece. 0 at the end.
size_t clusterLength(const char* text) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  if (utf8NextCodepoint(&p) == 0) return 0;
  for (;;) {
    const unsigned char* next = p;
    if (!followsItsCharacter(utf8NextCodepoint(&next))) break;
    if (static_cast<size_t>(next - reinterpret_cast<const unsigned char*>(text)) > kMaxColumnRunBytes) break;
    p = next;
  }
  return static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(text));
}

}  // namespace

uint32_t firstCodepointOf(const char* utf8) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
  return utf8NextCodepoint(&p);
}

size_t nextColumnPiece(const char* text, bool& upright) {
  const size_t first = clusterLength(text);
  if (first == 0) return 0;
  upright = drawsUpright(firstCodepointOf(text));
  if (upright) return first;

  size_t length = first;
  for (;;) {
    const char* at = text + length;
    if (drawsUpright(firstCodepointOf(at))) break;
    const size_t next = clusterLength(at);
    if (next == 0 || length + next > kMaxColumnRunBytes) break;
    length += next;
  }
  return length;
}
