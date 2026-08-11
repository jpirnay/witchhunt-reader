#include "FootnoteShape.h"

#include <cstdint>
#include <cstring>

namespace {

bool isAsciiSpaceChar(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// Decodes one UTF-8 codepoint at `i`. On a truncated or malformed sequence returns the raw
// lead byte and advances 1 — that byte is never an allowed marker codepoint, so a broken
// sequence rejects rather than resynchronising into a false positive.
uint32_t decodeCodepoint(const char* text, const size_t len, const size_t i, size_t& advance) {
  uint32_t cp = static_cast<unsigned char>(text[i]);
  advance = 1;
  if (cp >= 0xF0 && i + 3 < len) {
    cp = ((cp & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
    advance = 4;
  } else if (cp >= 0xE0 && i + 2 < len) {
    cp = ((cp & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
    advance = 3;
  } else if (cp >= 0xC0 && i + 1 < len) {
    cp = ((cp & 0x1F) << 6) | (text[i + 1] & 0x3F);
    advance = 2;
  }
  return cp;
}

// Space codepoints trimmed from the ends of link text. Beyond ASCII because French
// typography puts an espace insécable before a marker ("texte ¹") and CJK markup uses
// the ideographic space — neither is an allowed marker codepoint, so without trimming they
// would reject the marker they decorate.
bool isMarkerSpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x00A0 /* NBSP */ ||
         (cp >= 0x2000 && cp <= 0x200B) /* en quad … zero width space */ || cp == 0x202F /* narrow NBSP */ ||
         cp == 0x205F /* medium mathematical space */ || cp == 0x3000 /* ideographic space */ ||
         cp == 0xFEFF /* zero-width NBSP / BOM */;
}

// Start of each 10-codepoint Unicode decimal-digit block a book might number notes with.
// Latin digits are not the only ones publishers use: Persian and Urdu books number with
// U+06F0.., Hindi with U+0966.., Thai with U+0E50.., and Japanese typesetting with the
// fullwidth forms at U+FF10.. Each block is `start .. start + 9`, by Unicode's own rule
// that decimal-digit blocks are contiguous and in ascending value order.
constexpr uint32_t DIGIT_BLOCK_STARTS[] = {
    0x0660,  // Arabic-Indic
    0x06F0,  // Extended Arabic-Indic (Persian, Urdu)
    0x07C0,  // NKo
    0x0966,  // Devanagari
    0x09E6,  // Bengali
    0x0A66,  // Gurmukhi
    0x0AE6,  // Gujarati
    0x0B66,  // Oriya
    0x0BE6,  // Tamil
    0x0C66,  // Telugu
    0x0CE6,  // Kannada
    0x0D66,  // Malayalam
    0x0DE6,  // Sinhala
    0x0E50,  // Thai
    0x0ED0,  // Lao
    0x0F20,  // Tibetan
    0x1040,  // Myanmar
    0x17E0,  // Khmer
    0x1810,  // Mongolian
    0xFF10,  // Fullwidth
};

// Han numerals one … ten. Not contiguous, so they need a table rather than a range.
constexpr uint32_t HAN_NUMERALS[] = {
    0x4E00,  // 一
    0x4E8C,  // 二
    0x4E09,  // 三
    0x56DB,  // 四
    0x4E94,  // 五
    0x516D,  // 六
    0x4E03,  // 七
    0x516B,  // 八
    0x4E5D,  // 九
    0x5341,  // 十
};

bool isMarkerDigit(const uint32_t cp) {
  if (cp >= '0' && cp <= '9') return true;
  // Superscripts, the commonest marker of all. ¹ ² ³ predate Unicode's superscript block
  // and are stranded in Latin-1; ⁰ and ⁴-⁹ live together at U+2070.
  if (cp == 0x00B9 || cp == 0x00B2 || cp == 0x00B3) return true;
  if (cp == 0x2070 || (cp >= 0x2074 && cp <= 0x2079)) return true;
  if (cp >= 0x2080 && cp <= 0x2089) return true;  // subscript digits
  for (const uint32_t start : DIGIT_BLOCK_STARTS) {
    if (cp >= start && cp <= start + 9) return true;
  }
  for (const uint32_t han : HAN_NUMERALS) {
    if (cp == han) return true;
  }
  return false;
}

// Pre-composed numbers that stand alone as a marker: ① ⑴ ⒈ ⓪ ❶ ➀ ➊.
bool isEnclosedNumber(const uint32_t cp) {
  return (cp >= 0x2460 && cp <= 0x249B) || cp == 0x24EA || (cp >= 0x2776 && cp <= 0x2793);
}

// Punctuation a marker may be wrapped or terminated with, in Latin and CJK forms.
bool isMarkerPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case '[':
    case ']':
    case '(':
    case ')':
    case 0x207D:  // ⁽ superscript left parenthesis
    case 0x207E:  // ⁾ superscript right parenthesis
    case 0x208D:  // ₍ subscript left parenthesis
    case 0x208E:  // ₎ subscript right parenthesis
    case 0x3008:  // 〈
    case 0x3009:  // 〉
    case 0x3010:  // 【
    case 0x3011:  // 】
    case 0x3014:  // 〔
    case 0x3015:  // 〕
    case 0xFF08:  // （
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF3B:  // ［
    case 0xFF3D:  // ］
      return true;
    default:
      return false;
  }
}

// Reference symbols used instead of a number.
bool isMarkerSymbol(const uint32_t cp) {
  switch (cp) {
    case '*':
    case 0x00A7:  // § section sign
    case 0x00B6:  // ¶ pilcrow
    case 0x2020:  // † dagger
    case 0x2021:  // ‡ double dagger
    case 0x2042:  // ⁂ asterism
    case 0x204E:  // ⁎ low asterisk
    case 0x2051:  // ⁑ two asterisks aligned vertically
    case 0x203B:  // ※ reference mark (Japanese, Korean)
    case 0xFE61:  // ﹡ small asterisk
    case 0xFF0A:  // ＊ fullwidth asterisk
      return true;
    default:
      return false;
  }
}

bool isMarkerCodepoint(const uint32_t cp) {
  return isMarkerDigit(cp) || isEnclosedNumber(cp) || isMarkerPunctuation(cp) || isMarkerSymbol(cp);
}

}  // namespace

namespace FootnoteShape {

bool hasAttributeToken(const char* value, const char* token) {
  if (!value) return false;
  const size_t tokenLen = strlen(token);
  const char* cursor = value;
  while (*cursor != '\0') {
    while (*cursor != '\0' && isAsciiSpaceChar(*cursor)) ++cursor;
    const char* start = cursor;
    while (*cursor != '\0' && !isAsciiSpaceChar(*cursor)) ++cursor;
    if (static_cast<size_t>(cursor - start) == tokenLen && strncmp(start, token, tokenLen) == 0) return true;
  }
  return false;
}

bool isNoterefTagged(const char* epubType, const char* role) {
  return hasAttributeToken(epubType, "noteref") || hasAttributeToken(role, "doc-noteref");
}

bool isMarkerText(const char* text, const size_t len) {
  // Trim leading space codepoints.
  size_t start = 0;
  while (start < len) {
    size_t advance;
    if (!isMarkerSpace(decodeCodepoint(text, len, start, advance))) break;
    start += advance;
  }

  // Find the end of the last non-space codepoint. Interior spaces need no special handling:
  // a space is not an allowed marker codepoint, so "1 2" rejects in the classify pass below.
  size_t end = start;
  for (size_t i = start; i < len;) {
    size_t advance;
    if (!isMarkerSpace(decodeCodepoint(text, len, i, advance))) end = i + advance;
    i += advance;
  }

  int codepoints = 0;
  for (size_t i = start; i < end;) {
    size_t advance;
    if (!isMarkerCodepoint(decodeCodepoint(text, len, i, advance))) return false;
    if (++codepoints > 4) return false;
    i += advance;
  }
  return codepoints >= 1;
}

}  // namespace FootnoteShape
