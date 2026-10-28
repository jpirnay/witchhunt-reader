#pragma once

#include <cstdint>

#include "TypographyProfile.h"

// The widths of full-width CJK punctuation (spec section 5.2). A full-width mark is ink on one half of its em
// and blank on the other; set next to other punctuation or at a line edge, part of that blank goes, as the
// layout requirements of each language describe (W3C JLREQ, CLREQ). The rules are written from those documents;
// KOReader's crengine tables (textlang.cpp) were the cross-check, not the source.
namespace CjkPunctuation {

// The JLREQ classes of the marks a table adjusts.
enum class Class : uint8_t {
  None,              // text: Han, kana, Latin, and marks no table adjusts
  Opening,           // 「『（【〔〈《〘〖｟〝［｛, and the curly “‘ where full-width
  Closing,           // 」』）】〕〉》〙〗｠〞〟］｝, and the curly ”’ where full-width
  Dividing,          // ！？‼⁇⁈⁉
  MiddleDot,         // ・：；
  FullStop,          // 。．
  Comma,             // 、，
  IdeographicSpace,  // U+3000, which keeps its full width
  AmbiguousQuote,    // ＂＇, which may open or close
};

Class classOf(uint32_t cp);

// The curly quotes are CJK punctuation only where the font draws them full-width (a CJK font in CJK text); in a
// Latin font they are narrow and are left alone. The caller checks the width.
bool isCurlyQuote(uint32_t cp);

// A width in eighths of the mark's full advance (8: unchanged). `forced` always applies; `minimum` is how far
// the line may shrink the mark when it needs room (push-in, spec 5.3).
struct Width {
  uint8_t forced;
  uint8_t minimum;
};

// The width of `mark` beside its neighbour. An opening mark looks at the character BEFORE it, every other mark
// at the character AFTER it, as JLREQ's spacing does. `neighbour` is None for text or a space there.
// `atLineEdge`: a line starts at the opening mark, or ends after the other mark, so the neighbour is the edge.
Width widthOf(TypographyProfile::Punctuation table, Class mark, Class neighbour, bool atLineEdge);

// The gap between Han or kana and a Latin letter or digit, in eighths of the CJK side's em.
constexpr uint8_t kCjkLatinGap = 2;

// Ideographs, 々〆〇, and the kana letters with ー; not ・, not half-width katakana, not Hangul.
bool isHanOrKana(uint32_t cp);

// ASCII letters and digits, and the accented Latin letters; not full-width forms, not Greek or Cyrillic.
bool isLatinLetterOrDigit(uint32_t cp);

// Whether a gap sits between two adjacent characters: one is Han or kana, the other a Latin letter or digit.
bool takesCjkLatinGap(uint32_t leftCp, uint32_t rightCp);

}  // namespace CjkPunctuation
