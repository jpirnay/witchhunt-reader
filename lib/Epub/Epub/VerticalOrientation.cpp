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

uint32_t firstCodepoint(const char* text) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  return utf8NextCodepoint(&p);
}

}  // namespace

size_t nextColumnPiece(const char* text, bool& upright) {
  const size_t first = clusterLength(text);
  if (first == 0) return 0;
  upright = drawsUpright(firstCodepoint(text));
  if (upright) return first;

  size_t length = first;
  for (;;) {
    const char* at = text + length;
    if (drawsUpright(firstCodepoint(at))) break;
    const size_t next = clusterLength(at);
    if (next == 0 || length + next > kMaxColumnRunBytes) break;
    length += next;
  }
  return length;
}
