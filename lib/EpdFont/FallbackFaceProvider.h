#pragma once

#include <cstdint>

#include "EpdFontData.h"
#include "GlyphProvider.h"

class FontDecompressor;

/// Serves a font's missing glyphs out of fallback faces: built-in UI fallback faces (such as CJK)
/// and, when an SD card or reader font is open, that font's matching UI size (src/CjkUiFonts.cpp).
/// The fallback mechanism is script-agnostic: any non-ASCII character missing from the primary
/// UI face (CJK, Greek, Hebrew, Cyrillic, symbols, etc.) can be borrowed from the reader font.
///
/// Installed as EpdFontData::provider, so EpdFont asks it only after the font's own interval table
/// has missed, and the renderer gets the bitmap back through ownsBitmap(). Kerning stays the font's
/// own: a codepoint the font has no glyph for is in none of its kern classes.
///
/// A built-in face may be flat (its bitmap a plain pointer into flash, mapped or not) or compressed
/// in DEFLATE groups (compress_ui_face.py), whose glyphs ownsBitmap() decodes through the
/// FontDecompressor it was given -- into that decompressor's per-glyph fallback cache, so the
/// pointer it hands back lives until the next decode, which is the lifetime the renderer already
/// honours for every compressed glyph. A chained 2-bit face (TtfFont) is packed to a 1-bit ink mask
/// in ownsBitmap() into a shared 256-byte static buffer.
class FallbackFaceProvider final : public GlyphProvider {
 public:
  static constexpr uint8_t MAX_FACES = 4;
  static constexpr uint16_t PACKED_BUF_BYTES = 256;

  /// Where compressed faces are decoded. setFaces() refuses one until this is set.
  void setDecompressor(FontDecompressor* decompressor) { decompressor_ = decompressor; }

  /// The built-in faces to search, first match wins. False, and no faces, when there are too many
  /// or one of them is compressed and there is no decompressor.
  bool setFaces(const EpdFontData* const* faces, uint8_t count);

  /// Optional primary UI face (e.g. inter_ui_10_regular) whose interval table is searched before
  /// this provider; used by prefetch() to skip characters the primary face already covers.
  void setPrimaryFace(const EpdFontData* primary) { primary_ = primary; }

  /// Optional reader face (e.g. TtfFont at 10 or 12 pt) chained into the lookup order. When
  /// `afterFirstFace` is true (active UI language is CJK) and preferChained_ is false, `faces_[0]`
  /// is searched first and `face` second, ahead of `faces_[1..]`. When `afterFirstFace` is false
  /// or preferChained_ is true, `face` is searched before `faces_[0..]`.
  void setChainedFace(const EpdFontData* face, bool afterFirstFace = true);
  void setAfterFirstFace(bool afterFirstFace) { afterFirstFace_ = afterFirstFace; }
  bool hasChainedFace() const { return chained_ != nullptr; }

  /// Invalidates all ring slots so none can reference stale records after a wipe or font close.
  void invalidateRing();

  /// Sets hook called on-demand with the missing codepoint when an unsupported character needs borrowing.
  void setOnDemandHook(void (*hook)(uint32_t codepoint)) { onDemandAttach_ = hook; }
  void resetOnDemand() {}

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
  void prefetch(const char* utf8Text) override;

  /// Batch-prefetch missing glyphs across multiple strings without per-string MissScope churn.
  void prefetch(const char* (*getter)(void* ctx, int index), void* ctx, int count);

 private:
  struct Served {
    EpdGlyph record;
    const EpdFontData* face;
    const EpdGlyph* sourceRecord;
    uint16_t index;
  };
  static constexpr uint8_t RING_SIZE = 8;
  static uint8_t packedBuf_[PACKED_BUF_BYTES];

  Served ring_[RING_SIZE] = {};
  uint8_t next_ = 0;
  const EpdFontData* faces_[MAX_FACES] = {};
  uint8_t faceCount_ = 0;
  const EpdFontData* primary_ = nullptr;
  const EpdFontData* chained_ = nullptr;
  bool afterFirstFace_ = true;
  bool loggedOversize_ = false;
  void (*onDemandAttach_)(uint32_t codepoint) = nullptr;
  mutable bool inOnDemand_ = false;
  bool preferChained_ = false;

  static bool faceCovers(const EpdFontData* face, uint32_t cp);
  void ensureChainedAttached(uint32_t codepoint = 0) const;
  uint32_t lastPrefetchHash_ = 0;
  const EpdFontData* lastPrefetchChained_ = nullptr;
  uint32_t standIn_ = 0;
  bool (*claims_)(uint32_t) = nullptr;
  FontDecompressor* decompressor_ = nullptr;

  const EpdGlyph* serve(uint32_t codepoint);
  const EpdGlyph* serveStandIn();
  const EpdGlyph* serveFace(const EpdFontData* face, uint32_t codepoint);
};
