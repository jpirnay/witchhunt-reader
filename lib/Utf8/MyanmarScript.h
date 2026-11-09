#pragma once

#include <cstddef>
#include <cstdint>

// The Myanmar script CrossPoint shapes (Burmese, Mon, Shan, the Karen languages and the rest): which code points a run
// holds, the letter a face that draws the script maps, and where a Myanmar word may be cut: between syllables, the
// rule of the Myanmar design's section 2 (docs/superpowers/specs/2026-11-09-myanmar-design.md). A header of its own, as
// Arabic has ArabicScripts.h: IndicScripts.h indexes U+0900-0DFF by block, and Thai, Lao and Tibetan lie between those
// blocks and U+1000.
//
// The letters and digits are those of the shaper's Myanmar categories (lib/OtShaper/OtUnicodeData.h, HarfBuzz
// 14.5.0's): the consonants are its C and Ra, the independent vowels its IV, in the three blocks. The break rule's
// exceptions (the stacker, asat, the dot below, digits) follow Somnius/CrossInk-Indic's myanmarBreakBefore (commit
// 6c545b38 by Lef, MIT).
namespace myanmar {

constexpr uint32_t kProbe = 0x1000;  // ka: a font that maps it draws Myanmar
constexpr uint32_t ZWNJ = 0x200C;
constexpr uint32_t ZWJ = 0x200D;
constexpr uint32_t DOTTED_CIRCLE = 0x25CC;
constexpr uint32_t DOT_BELOW = 0x1037;
constexpr uint32_t STACKER = 0x1039;  // the invisible virama: the next consonant is stacked below
constexpr uint32_t ASAT = 0x103A;     // the visible killer: the consonant before it ends a syllable

// Myanmar (U+1000-109F), Myanmar Extended-B (U+A9E0-A9FF) and Myanmar Extended-A (U+AA60-AA7F): the blocks the
// shaper's Myanmar tables cover. Extended-C (U+116D0-116FF) is left out: no Noto font maps it.
constexpr bool isRunMember(const uint32_t cp) {
  return (cp >= 0x1000 && cp <= 0x109F) || (cp >= 0xA9E0 && cp <= 0xA9FF) || (cp >= 0xAA60 && cp <= 0xAA7F);
}

// The code points a run takes beside its members, as the shaper's tables do: the joiners and the dotted circle.
constexpr bool isShared(const uint32_t cp) { return cp == ZWNJ || cp == ZWJ || cp == DOTTED_CIRCLE; }

// The consonants: the shaper's C and Ra (U+104E, the symbol aforementioned, is one).
constexpr bool isConsonant(const uint32_t cp) {
  if (cp < 0xA9E0) {
    return (cp >= 0x1000 && cp <= 0x1020) || cp == 0x103F || cp == 0x104E || cp == 0x1050 || cp == 0x1051 ||
           (cp >= 0x105A && cp <= 0x105D) || cp == 0x1061 || cp == 0x1065 || cp == 0x1066 ||
           (cp >= 0x106E && cp <= 0x1070) || (cp >= 0x1075 && cp <= 0x1081) || cp == 0x108E;
  }
  return (cp >= 0xA9E0 && cp <= 0xA9E4) || (cp >= 0xA9E7 && cp <= 0xA9EF) || (cp >= 0xA9FA && cp <= 0xA9FE) ||
         (cp >= 0xAA60 && cp <= 0xAA6F) || (cp >= 0xAA71 && cp <= 0xAA73) || cp == 0xAA7A || cp == 0xAA7E ||
         cp == 0xAA7F;
}

// The independent vowels: the shaper's IV (U+1021-102A, 1052-1055).
constexpr bool isIndependentVowel(const uint32_t cp) {
  return (cp >= 0x1021 && cp <= 0x102A) || (cp >= 0x1052 && cp <= 0x1055);
}

// The digits: Myanmar's, Shan's and Tai Laing's.
constexpr bool isDigit(const uint32_t cp) {
  return (cp >= 0x1040 && cp <= 0x1049) || (cp >= 0x1090 && cp <= 0x1099) || (cp >= 0xA9F0 && cp <= 0xA9F9);
}

// What a syllable starts with: a consonant, an independent vowel, a digit, or one of the signs U+104C-104F. Never the
// section marks U+104A and 104B, which end a phrase.
constexpr bool startsSyllable(const uint32_t cp) {
  return isConsonant(cp) || isIndependentVowel(cp) || isDigit(cp) || (cp >= 0x104C && cp <= 0x104F);
}

// Whether a Myanmar word may be cut between `prev` and `cur`: before a syllable's start, but never after the stacker or
// a ZWJ, never before a consonant that asat or the stacker follows (a killed final consonant, and kinzi, stay with the
// syllable before them), and never between two digits. `next` is the first code point after `cur` other than the dot
// below (which precedes asat in canonical order, as in U+1004 1037 103A), 0 at the text's end.
constexpr bool breakAllowed(const uint32_t prev, const uint32_t cur, const uint32_t next) {
  if (!startsSyllable(cur) || prev == STACKER || prev == ZWJ) return false;
  if (isConsonant(cur) && (next == ASAT || next == STACKER)) return false;
  return !(isDigit(prev) && isDigit(cur));
}

// The Myanmar part of a cut between `prev` and `cur` in any text: breakAllowed where `cur` is a run member, never
// before a joiner or the dotted circle after a member; anywhere else Myanmar does not object.
constexpr bool cutAllowed(const uint32_t prev, const uint32_t cur, const uint32_t next) {
  if (isRunMember(cur)) return breakAllowed(prev, cur, next);
  return !(isShared(cur) && isRunMember(prev));
}

// Whether the UTF-8 at `p` (3 readable bytes) starts a run member: U+1000-109F encode as E1 80 80 .. E1 82 9F, U+A9E0-
// A9FF as EA A7 A0 .. EA A7 BF, U+AA60-AA7F as EA A9 A0 .. EA A9 BF.
inline bool startsMember(const unsigned char* p) {
  if (p[0] == 0xE1) return p[1] == 0x80 || p[1] == 0x81 || (p[1] == 0x82 && p[2] >= 0x80 && p[2] <= 0x9F);
  if (p[0] != 0xEA) return false;
  return (p[1] == 0xA7 && p[2] >= 0xA0 && p[2] <= 0xBF) || (p[1] == 0xA9 && p[2] >= 0xA0 && p[2] <= 0xBF);
}

// Whether `len` bytes of UTF-8 hold a run member: a byte scan, bounded by `len`.
inline bool holdsMyanmar(const char* utf8, const size_t len) {
  if (utf8 == nullptr) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  for (size_t i = 0; i + 3 <= len; i++) {
    if (startsMember(p + i)) return true;
  }
  return false;
}

}  // namespace myanmar
