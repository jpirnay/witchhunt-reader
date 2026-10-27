#pragma once

#include <cstdint>

#include "EpdFontData.h"
#include "GlyphProvider.h"

class FontDecompressor;

/// Serves a font's missing glyphs out of other BUILT-IN faces: the CJK UI faces behind the Inter UI
/// faces (src/CjkUiFonts.cpp).
///
/// Installed as EpdFontData::provider, so EpdFont asks it only after the font's own interval table
/// has missed, and the renderer gets the bitmap back through ownsBitmap(). Kerning stays the font's
/// own: a codepoint the font has no glyph for is in none of its kern classes.
///
/// A face may be flat (its bitmap a plain pointer into flash, mapped or not) or compressed in
/// DEFLATE groups (compress_ui_face.py), whose glyphs ownsBitmap() decodes through the
/// FontDecompressor it was given -- into that decompressor's per-glyph fallback cache, so the
/// pointer it hands back lives until the next decode, which is the lifetime the renderer already
/// honours for every compressed glyph. Faces must be of the bit depth of the font asking, because
/// the renderer draws at the depth of the font it was given; the depth is the caller's to match.
///
/// One more face may come from the reader's font (setReaderFace): a 2-bit TrueType face at this size, whose glyphs
/// are drawn as the ink of their black-and-white pass (packInkMask), so a book title in a script the built-in faces
/// lack draws in the font the book is read in.
class FallbackFaceProvider final : public GlyphProvider {
 public:
  static constexpr uint8_t MAX_FACES = 4;

  /// Where compressed faces are decoded. setFaces() refuses one until this is set.
  void setDecompressor(FontDecompressor* decompressor) { decompressor_ = decompressor; }

  /// The faces to search, first match wins. False, and no faces, when there are too many or one of
  /// them is compressed and there is no decompressor.
  bool setFaces(const EpdFontData* const* faces, uint8_t count);

  /// The reader font's face at this size, from `face(ctx)`: asked only when a glyph is missing from the faces before
  /// it, so the caller makes it when UI text first needs it, and nullptr while there is none or it may not be read
  /// now. It is searched before the faces, or after the first one when `afterFirst` (the active UI language's own).
  void setReaderFace(const EpdFontData* (*face)(void* ctx), void* ctx) {
    readerFace_ = face;
    readerCtx_ = ctx;
  }
  void setReaderFaceAfterFirst(const bool afterFirst) { readerAfterFirst_ = afterFirst; }

  /// A codepoint `claims` returns true for that no face has is served as `standIn` instead, from
  /// the first face that has that. Subset faces carry only part of their script, and a character
  /// of it they lack should show as missing rather than vanish from the middle of a word. Codepoints
  /// outside the claim stay missing, as for any font.
  void setStandIn(uint32_t standIn, bool (*claims)(uint32_t codepoint)) {
    standIn_ = standIn;
    claims_ = claims;
  }

  const EpdGlyph* missGlyph(uint32_t codepoint) override;
  /// Always true for a served record, even when a compressed glyph could not be decoded (no memory
  /// for the ring): `bitmap` is then nullptr and the glyph draws nothing, which FontDecompressor
  /// logs. Answering false would send the renderer to the asking font's own bitmap at an offset
  /// that belongs to another face.
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override;

 private:
  // A served glyph as GlyphProvider hands it out: a full EpdGlyph record (the faces store the 6-byte
  // packed form, which has no field to say whose glyph it is) and which glyph of which face it is,
  // which is all ownsBitmap() needs to find its bitmap, flat or compressed. `record` comes first so
  // ownsBitmap() can get from the record pointer back to the slot. The face is a pointer rather than
  // a position in faces_ so a record stays right across setFaces(): 24 B a slot.
  struct Served {
    EpdGlyph record;
    const EpdFontData* face;
    const EpdGlyph* source;  // the reader face's own record, whose 2-bit bitmap ownsBitmap() packs; null otherwise
    uint16_t index;
  };
  // A reader glyph's 1-bit ink: a 12 pt CJK glyph is about 25 x 26 px, 82 B. Larger glyphs are left to the next face.
  static constexpr uint16_t kInkBytes = 128;
  static uint8_t ink_[kInkBytes];
  // EpdFont copies the metrics out at once and the renderer asks for the bitmap before it resolves
  // the next glyph, so one slot would do for that caller. The few more cost 24 B each and keep a
  // caller that resolves a glyph or two ahead correct, the same lifetime SdCardFont's miss ring
  // gives (GlyphProvider::missGlyph).
  static constexpr uint8_t RING_SIZE = 8;
  Served ring_[RING_SIZE] = {};
  uint8_t next_ = 0;
  const EpdFontData* faces_[MAX_FACES] = {};
  uint8_t faceCount_ = 0;
  uint32_t standIn_ = 0;
  bool (*claims_)(uint32_t) = nullptr;
  FontDecompressor* decompressor_ = nullptr;

  const EpdFontData* (*readerFace_)(void* ctx) = nullptr;
  void* readerCtx_ = nullptr;
  bool readerAfterFirst_ = false;

  const EpdGlyph* serve(uint32_t codepoint);
  const EpdGlyph* serveFrom(const EpdFontData* face, uint32_t codepoint, bool reader);
};
