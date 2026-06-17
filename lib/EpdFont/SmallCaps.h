#pragma once
#include <cstdint>

// Shared helpers for font-variant: small-caps rendering.
//
// Small-caps folds lowercase letters to their uppercase glyph and draws them at a
// reduced scale; characters that are not lowercase letters (already-uppercase,
// digits, punctuation) render at full size.  The fold and the scale must be applied
// identically in every measurement and rendering path so layout and drawing agree.
namespace smallCaps {

// Scale factor applied to folded (was-lowercase) glyphs.  ~0.75 keeps small caps
// visibly smaller than full capitals while staying legible on e-ink.
inline constexpr float SCALE = 0.75f;

// Fold a codepoint to its uppercase form for small-caps rendering.
// Returns true and rewrites `cp` to the uppercase codepoint when the input was a
// lowercase letter (and therefore should be drawn scaled); returns false and leaves
// `cp` unchanged otherwise.
//
// Coverage: ASCII a-z, Latin-1 Supplement lowercase (à-þ, excluding ÷ and ß which
// has no single-codepoint uppercase), and Latin Extended-A even/odd lowercase pairs.
inline bool fold(uint32_t& cp) {
  // ASCII a-z -> A-Z
  if (cp >= 0x61 && cp <= 0x7A) {
    cp -= 0x20;
    return true;
  }
  // Latin-1 Supplement lowercase 0x00E0-0x00FE -> uppercase -0x20.
  // 0x00F7 (÷) is a math symbol, not a letter; 0x00DF (ß) is below this range and
  // has no single-codepoint uppercase, so both are left full-size.
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) {
    cp -= 0x20;
    return true;
  }
  // Latin Extended-A 0x0100-0x017F: most letters come in (even=upper, odd=lower)
  // pairs, e.g. 0x0100 Ā / 0x0101 ā.  An odd codepoint folds to the preceding even.
  // The handful of irregular entries in this block (0x0130/0x0131, 0x0138, 0x0149,
  // 0x017F) are rare and intentionally left unfolded rather than mis-mapped.
  if (cp >= 0x0100 && cp <= 0x017F && (cp & 1) == 1) {
    switch (cp) {
      case 0x0131:  // ı dotless i — no regular pairing here
      case 0x0149:  // ŉ — deprecated, no single uppercase
      case 0x017F:  // ſ long s
        return false;
      default:
        cp -= 1;
        return true;
    }
  }
  return false;
}

}  // namespace smallCaps
