#pragma once

#include <cstddef>
#include <cstdint>

// The Arabic script runs CrossPoint shapes: what the layout, the shaper's callers and the dev commands need to know
// about which code points belong to a run.
//
// A run's members are the code points of U+0600-06FF, 0750-077F and 0870-08FF whose bidi class is AL or NSM, less
// U+061C (ALM, a bidi control). AL punctuation and symbols are members: they are strong right-to-left, so a run
// cannot straddle a level, and joining gives them no joining. Digits (AN, EN), the number signs, ET, the Arabic comma,
// the neutral symbols and the presentation forms are not: bidi orders them, or the code points are already drawn.
// Brackets and every other mirrored code point are outside by construction.
namespace arabic {

constexpr uint32_t kProbe = 0x0628;  // beh: a font that maps it draws Arabic
constexpr uint32_t ZWNJ = 0x200C;
constexpr uint32_t ZWJ = 0x200D;
constexpr uint32_t TATWEEL = 0x0640;

// The three blocks the run rules cover.
constexpr bool inBlocks(const uint32_t cp) {
  return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x0870 && cp <= 0x08FF);
}

// The marks (bidi class NSM) among the members.
constexpr bool isMark(const uint32_t cp) {
  return (cp >= 0x0610 && cp <= 0x061A) || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
         (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) || cp == 0x06E7 || cp == 0x06E8 ||
         (cp >= 0x06EA && cp <= 0x06ED) || (cp >= 0x0897 && cp <= 0x089F) || (cp >= 0x08CA && cp <= 0x08E1) ||
         (cp >= 0x08E3 && cp <= 0x08FF);
}

// True for a code point that can be part of a run (never ZWJ or ZWNJ, which the scan takes inside a run).
constexpr bool isRunMember(const uint32_t cp) {
  if (!inBlocks(cp)) return false;
  if (cp < 0x0700) {
    return cp == 0x0608 || cp == 0x060B || cp == 0x060D || (cp >= 0x0610 && cp <= 0x061B) ||
           (cp >= 0x061D && cp <= 0x065F) || (cp >= 0x066D && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E8) ||
           (cp >= 0x06EA && cp <= 0x06EF) || cp >= 0x06FA;
  }
  if (cp < 0x0800) return true;  // 0750-077F
  return cp <= 0x088F || (cp >= 0x0897 && cp <= 0x08E1) || cp >= 0x08E3;
}

constexpr bool isLetterOrMark(const uint32_t cp) { return isRunMember(cp); }

// The AL punctuation and symbols among the members (the Arabic semicolon, question mark, full stop, percent-like signs
// and the like): strong right-to-left, but joining gives them no joining and they are no letter.
constexpr bool isPunctuationMember(const uint32_t cp) {
  return cp == 0x0608 || cp == 0x060B || cp == 0x060D || cp == 0x061B || cp == 0x061D || cp == 0x061E || cp == 0x061F ||
         cp == 0x066D || cp == 0x06D4 || cp == 0x06FD || cp == 0x06FE || cp == 0x0888;
}

// A letter or a mark of the blocks: a run member that is not AL punctuation or a symbol. These are the code points a
// ZWJ or ZWNJ can follow.
constexpr bool takesJoiner(const uint32_t cp) { return isRunMember(cp) && !isPunctuationMember(cp); }

// True when `cp` starts a new cluster after `prev`: a mark, a joiner, or anything after a ZWJ stays in the cluster.
constexpr bool isClusterBreak(const uint32_t prev, const uint32_t cp) {
  return !isMark(cp) && cp != ZWJ && cp != ZWNJ && prev != ZWJ;
}

// A byte pre-test: true when the UTF-8 holds a code point of the blocks (U+0600-06FF: D8-DB; 0750-077F: DD 90-BF,
// DD 80-8F being Syriac; 0870-08FF: E0 A1 B0-BF, E0 A2, E0 A3). Digit-only text passes: its scan finds no run.
inline bool holdsArabic(const char* utf8, const size_t len) {
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  for (size_t i = 0; i < len; i++) {
    const unsigned char b = p[i];
    if (b >= 0xD8 && b <= 0xDB) return true;
    if (b == 0xDD && i + 1 < len && p[i + 1] >= 0x90 && p[i + 1] <= 0xBF) return true;
    if (b == 0xE0 && i + 1 < len) {
      if (p[i + 1] == 0xA2 || p[i + 1] == 0xA3) return true;
      if (p[i + 1] == 0xA1 && i + 2 < len && p[i + 2] >= 0xB0 && p[i + 2] <= 0xBF) return true;
    }
  }
  return false;
}

// True when the UTF-8 holds a run member (isRunMember): a code point an Arabic run takes. holdsArabic's byte test
// first, so text without a byte of the blocks costs one scan; digits, the number signs and the Arabic comma alone are
// no run.
inline bool holdsRunMember(const char* utf8, const size_t len) {
  if (!holdsArabic(utf8, len)) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  for (size_t i = 0; i < len;) {
    const unsigned char b = p[i];
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
    if (n == 1 || i + n > len) {
      i++;
      continue;
    }
    uint32_t cp = b & (0x7F >> n);
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | (p[i + k] & 0x3F);
    if (isRunMember(cp)) return true;
    i += n;
  }
  return false;
}
// True when the last code point that is not a mark is a run letter, not AL punctuation (tatweel included): the word
// ends joined-ready.
inline bool endsWithLetter(const char* utf8, const size_t len) {
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  size_t end = len;
  while (end > 0) {
    size_t start = end - 1;
    while (start > 0 && (p[start] & 0xC0) == 0x80) start--;
    uint32_t cp = p[start];
    if (cp >= 0xF0) {
      cp &= 0x07;
    } else if (cp >= 0xE0) {
      cp &= 0x0F;
    } else if (cp >= 0xC0) {
      cp &= 0x1F;
    }
    for (size_t i = start + 1; i < end; i++) cp = (cp << 6) | (p[i] & 0x3F);
    if (!isMark(cp)) return takesJoiner(cp);
    end = start;
  }
  return false;
}

}  // namespace arabic
