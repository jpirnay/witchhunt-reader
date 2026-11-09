#include "UiFaceProvider.h"

#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "SdFaceGlyph.h"

const EpdGlyph* UiFaceProvider::missGlyph(const uint32_t codepoint) {
  // In the reader the book's face goes first for what it maps (FallbackFaceProvider::readerMaps): no pack is opened.
  if (pack_.covers && pack_.face && pack_.covers(codepoint) && !(shared_ && shared_->readerMaps(codepoint))) {
    const EpdFontData* face = pack_.face(pack_.ctx);
    if (const EpdGlyph* glyph = sd_face::glyph(face, codepoint)) {
      served_ = face;
      return glyph;
    }
  }
  return shared_ ? shared_->missGlyph(codepoint) : nullptr;
}

bool UiFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  if (served_ && sd_face::owns(served_, glyph, bitmap)) return true;
  return shared_ && shared_->ownsBitmap(glyph, bitmap);
}

bool UiFaceProvider::packCovers(void* ctx, const uint32_t codepoint) {
  const auto* self = static_cast<const UiFaceProvider*>(ctx);
  const Pack& pack = self->pack_;
  return pack.covers && pack.covers(codepoint) && pack.installed && pack.installed(pack.ctx) &&
         !(self->shared_ && self->shared_->readerMaps(codepoint));
}

void UiFaceProvider::beginText(const char* utf8Text) {
  served_ = nullptr;
  // In the reader the book's face draws what it maps; a character it lacks comes from the pack one by one.
  if (pack_.prepare && !(shared_ && shared_->inReader())) pack_.prepare(pack_.ctx, utf8Text);
  if (shared_) shared_->beginText(utf8Text, &UiFaceProvider::packCovers, this);
}

bool UiFaceProvider::formsArabic() const {
  return (pack_.installed && pack_.installed(pack_.ctx)) || (shared_ && shared_->formsArabic());
}
