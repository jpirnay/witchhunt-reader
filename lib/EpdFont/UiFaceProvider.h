#pragma once

#include <cstdint>

#include "EpdFontData.h"
#include "GlyphProvider.h"

class FallbackFaceProvider;

/// The provider of one built-in UI face (CjkUiFonts::attach): a script pack's glyph for this face's own size and
/// weight first, then whatever the FallbackFaceProvider its size shares serves (the CJK UI faces, the last book's
/// font).
/// In the reader, the book's face goes before the pack for what it maps (FallbackFaceProvider::readerMaps).
///
/// A script pack is a 1-bit .cpfont family on the card holding what the UI faces leave out for flash, rendered as they
/// are (lib/EpdFont/scripts/build_ui_script_pack.py): the Arabic presentation forms today (src/UiScriptPack.cpp). Its
/// faces are SdCardFont faces, so a glyph's record and bitmap are the font's own: in the page arrays its prewarm built
/// for the string, or in its miss ring. Either lives until the next string is prepared, and the renderer draws a glyph
/// before it resolves another, as for any SD font.
class UiFaceProvider final : public GlyphProvider {
 public:
  /// What the pack gives this face. Every hook takes `ctx`; any may be null.
  struct Pack {
    /// The pack's face at this face's size and weight, opened on first ask; nullptr while there is none.
    const EpdFontData* (*face)(void* ctx) = nullptr;
    /// Whether the pack may hold `codepoint`, asked before face(): text it has no part in opens nothing.
    bool (*covers)(uint32_t codepoint) = nullptr;
    /// Whether the pack is on the card for this face, without opening it.
    bool (*installed)(void* ctx) = nullptr;
    /// `utf8Text` is measured or drawn next: the pack reads its characters of it in one pass.
    void (*prepare)(void* ctx, const char* utf8Text) = nullptr;
    void* ctx = nullptr;
  };

  void bind(FallbackFaceProvider* shared, const Pack& pack) {
    shared_ = shared;
    pack_ = pack;
  }

  const EpdGlyph* missGlyph(uint32_t codepoint) override;
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override;
  /// The pack prepares the string; the shared provider then takes it with the pack's characters as covered, so a
  /// title the pack draws is no book text and never opens the last book's font.
  void beginText(const char* utf8Text) override;
  /// With the pack installed the face draws the forms; without it, when the last book's font does (the shared
  /// provider).
  bool formsArabic() const override;
  /// What the shared provider shapes a script over, unless the pack draws `probe` first (missGlyph()'s order).
  const EpdFontData* shapingSource(uint32_t probe) override;

  /// Whether the pack covers `codepoint` for the face behind `ctx` (a UiFaceProvider): FallbackFaceProvider's
  /// beginText() hook.
  static bool packCovers(void* ctx, uint32_t codepoint);

 private:
  FallbackFaceProvider* shared_ = nullptr;
  Pack pack_{};
  const EpdFontData* served_ = nullptr;  // the pack face the last glyph missGlyph() served came from
};
