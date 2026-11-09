#include "UiFaceProvider.h"

#include "EpdFont.h"
#include "FallbackFaceProvider.h"

namespace {

// The glyphs `face` resolves by its own table: its intervals' span.
uint32_t glyphCount(const EpdFontData* face) {
  if (!face->intervals || face->intervalCount == 0) return 0;
  const EpdUnicodeInterval& last = face->intervals[face->intervalCount - 1];
  return last.offset + (last.last - last.first) + 1;
}

}  // namespace

const EpdGlyph* UiFaceProvider::missGlyph(const uint32_t codepoint) {
  if (pack_.covers && pack_.face && pack_.covers(codepoint)) {
    if (const EpdFontData* face = pack_.face(pack_.ctx)) {
      // A pack face is an SD font of the UI faces' depth: every glyph comes with its record (sdRecord), from the
      // table the prewarm built or from the font's miss ring.
      const EpdGlyphRef glyph = EpdFont(face).findGlyph(codepoint);
      if (glyph && glyph.sdRecord && !face->is2Bit) {
        served_ = face;
        return glyph.sdRecord;
      }
    }
  }
  return shared_ ? shared_->missGlyph(codepoint) : nullptr;
}

bool UiFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  if (served_) {
    // The font's miss ring first, then its table: the same two places GfxRenderer finds an SD glyph's bitmap.
    if (served_->provider && served_->provider->ownsBitmap(glyph, bitmap)) return true;
    if (served_->glyph && glyph >= served_->glyph && glyph < served_->glyph + glyphCount(served_)) {
      bitmap = glyph->dataLength == 0 ? nullptr : served_->bitmap + glyph->dataOffset;
      return true;
    }
  }
  return shared_ && shared_->ownsBitmap(glyph, bitmap);
}

bool UiFaceProvider::packCovers(void* ctx, const uint32_t codepoint) {
  const auto* self = static_cast<const UiFaceProvider*>(ctx);
  const Pack& pack = self->pack_;
  return pack.covers && pack.covers(codepoint) && pack.installed && pack.installed(pack.ctx);
}

void UiFaceProvider::beginText(const char* utf8Text) {
  served_ = nullptr;
  if (pack_.prepare) pack_.prepare(pack_.ctx, utf8Text);
  if (shared_) shared_->beginText(utf8Text, &UiFaceProvider::packCovers, this);
}

bool UiFaceProvider::formsArabic() const {
  return (pack_.installed && pack_.installed(pack_.ctx)) || (shared_ && shared_->formsArabic());
}
