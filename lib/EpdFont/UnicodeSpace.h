#pragma once

#include <cstdint>

// The blanks of Unicode's Zs category that have a width of their own, as a fraction of an em.
//
// A font that has one of these draws its own. One that does not would answer with the box (EpdFont::getGlyph()), which
// is right for a letter, where the reader should see that something is missing, and wrong for a blank, where there is
// nothing to see. CrossInk 20d74368 (Julia Nguyen). U+0020 and U+00A0 are not here: every face has them, and the parser
// turns U+00A0 and U+202F into its own glued space token before a font sees them.
namespace unicode_space {

struct Width {
  uint8_t numerator;    ///< 0: the face's word space
  uint8_t denominator;  ///< 0: not a blank
};

constexpr Width widthOf(const uint32_t cp) {
  switch (cp) {
    case 0x1680:  // OGHAM SPACE MARK
    case 0x2008:  // PUNCTUATION SPACE
      return {0, 1};
    case 0x2000:  // EN QUAD
    case 0x2002:  // EN SPACE
    case 0x2007:  // FIGURE SPACE
      return {1, 2};
    case 0x2001:  // EM QUAD
    case 0x2003:  // EM SPACE
    case 0x3000:  // IDEOGRAPHIC SPACE
      return {1, 1};
    case 0x2004:  // THREE-PER-EM SPACE
      return {1, 3};
    case 0x2005:  // FOUR-PER-EM SPACE
      return {1, 4};
    case 0x2006:  // SIX-PER-EM SPACE
      return {1, 6};
    case 0x2009:  // THIN SPACE
    case 0x202F:  // NARROW NO-BREAK SPACE
      return {1, 5};
    case 0x200A:  // HAIR SPACE
      return {1, 10};
    case 0x205F:  // MEDIUM MATHEMATICAL SPACE
      return {2, 9};
    default:
      return {0, 0};
  }
}

constexpr bool isBlank(const uint32_t cp) { return widthOf(cp).denominator != 0; }

}  // namespace unicode_space
