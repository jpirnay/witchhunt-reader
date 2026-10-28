#pragma once

#include <cstdint>

#include "CjkPunctuation.h"

// CSS `text-emphasis-style` marks (emphasis dots, 傍点). A mark is one byte: a Shape, or-ed with kOpen for the
// open variant. 0 is no mark.
namespace TextEmphasis {

enum Shape : uint8_t { None = 0, Dot = 1, Circle = 2, DoubleCircle = 3, Triangle = 4, Sesame = 5 };

constexpr uint8_t kOpen = 0x08;

// The glyph CSS Text Decoration 3 names for the mark: • ◦ ● ○ ◉ ◎ ▲ △ ﹅ ﹆; 0 for none.
constexpr uint32_t markCodepoint(const uint8_t mark) {
  const bool open = (mark & kOpen) != 0;
  switch (mark & ~kOpen) {
    case Dot:
      return open ? 0x25E6 : 0x2022;
    case Circle:
      return open ? 0x25CB : 0x25CF;
    case DoubleCircle:
      return open ? 0x25CE : 0x25C9;
    case Triangle:
      return open ? 0x25B3 : 0x25B2;
    case Sesame:
      return open ? 0xFE46 : 0xFE45;
    default:
      return 0;
  }
}

// Whether a character gets a mark: not a space, a control character or punctuation (ASCII and Latin-1 punctuation,
// U+2010-205E, the supplemental, vertical, small-form and halfwidth punctuation, a CjkPunctuation class or the CJK
// marks outside one). CSS Text Decoration 3 and JLREQ skip punctuation.
inline bool takesMark(const uint32_t cp) {
  if (cp <= 0x20 || (cp >= 0x7F && cp <= 0xA0)) return false;  // controls, space, no-break space
  // Latin-1 punctuation: ¡ § « ¶ · » ¿
  if (cp == 0xA1 || cp == 0xA7 || cp == 0xAB || cp == 0xB6 || cp == 0xB7 || cp == 0xBB || cp == 0xBF) return false;
  if (cp == 0xAD) return false;  // soft hyphen: TextBlock strips it before drawing
  if (cp < 0x7F) {
    const bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    return alnum;
  }
  if (cp >= 0x2000 && cp <= 0x200F) return false;  // spaces and format characters
  if (cp >= 0x2010 && cp <= 0x205E) return false;  // general punctuation
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;  // supplemental punctuation: ⸺ ⸻
  if (cp == 0x3000) return false;                  // ideographic space
  if (cp >= 0xFE10 && cp <= 0xFE19) return false;  // vertical forms
  if (cp >= 0xFE50 && cp <= 0xFE6B) return false;  // small forms
  if (cp >= 0xFF61 && cp <= 0xFF65) return false;  // halfwidth ｡｢｣､･
  // The CJK marks no CjkPunctuation class holds: 〃 〜 〰 〽 ゠ －
  if (cp == 0x3003 || cp == 0x301C || cp == 0x3030 || cp == 0x303D || cp == 0x30A0 || cp == 0xFF0D) return false;
  return CjkPunctuation::classOf(cp) == CjkPunctuation::Class::None;
}

}  // namespace TextEmphasis
