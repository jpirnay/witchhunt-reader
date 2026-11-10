#pragma once

#include "EpdFont.h"
#include "EpdFontData.h"
#include "GlyphProvider.h"

/// A glyph of an SD (.cpfont) face that a UI face's provider serves as its own fallback: a script pack's
/// (UiFaceProvider, FallbackFaceProvider). An SD face hands out its own records, from the table its prewarm built or
/// from its miss ring, so the provider returns that record and later finds the bitmap where GfxRenderer finds an SD
/// glyph's. A record lives until the face's next prewarm, and the renderer draws a glyph before it resolves another.
namespace sd_face {

/// `face`'s record for `codepoint`, nullptr when it has none or is not of the 1-bit depth of the UI faces.
inline const EpdGlyph* glyph(const EpdFontData* face, const uint32_t codepoint) {
  if (!face || face->is2Bit) return nullptr;
  const EpdGlyphRef ref = EpdFont(face).findGlyph(codepoint);
  return ref ? ref.sdRecord : nullptr;
}

/// Whether `glyph` is a record `face` handed out, and then its bitmap: the miss ring's, or the table's.
inline bool owns(const EpdFontData* face, const EpdGlyph* glyph, const uint8_t*& bitmap) {
  if (!face) return false;
  if (face->provider && face->provider->ownsBitmap(glyph, bitmap)) return true;
  if (!face->glyph || !face->intervals || face->intervalCount == 0) return false;
  const EpdUnicodeInterval& last = face->intervals[face->intervalCount - 1];
  const uint32_t count = last.offset + (last.last - last.first) + 1;
  if (glyph < face->glyph || glyph >= face->glyph + count) return false;
  bitmap = glyph->dataLength == 0 ? nullptr : face->bitmap + glyph->dataOffset;
  return true;
}

}  // namespace sd_face
