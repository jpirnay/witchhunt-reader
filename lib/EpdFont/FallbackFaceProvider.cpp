#include "FallbackFaceProvider.h"

#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "EpdFont.h"
#include "FontDecompressor.h"

uint8_t FallbackFaceProvider::ink_[FallbackFaceProvider::kInkBytes] = {};

bool FallbackFaceProvider::setFaces(const EpdFontData* const* faces, const uint8_t count) {
  faceCount_ = 0;
  if (count > MAX_FACES) return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (faces[i]->groups && !decompressor_) return false;
  }
  for (uint8_t i = 0; i < count; ++i) faces_[i] = faces[i];
  faceCount_ = count;
  return true;
}

const EpdGlyph* FallbackFaceProvider::missGlyph(const uint32_t codepoint) {
  if (const EpdGlyph* glyph = serve(codepoint)) return glyph;
  if (claims_ && codepoint != standIn_ && claims_(codepoint)) return serve(standIn_);
  return nullptr;
}

const EpdGlyph* FallbackFaceProvider::serve(const uint32_t codepoint) {
  // The built-in faces before the reader's: none, or the active UI language's own, but for book text.
  const uint8_t before = readerAfterFirst_ && !bookText_ && faceCount_ > 0 ? 1 : 0;
  for (uint8_t i = 0; i < before; ++i) {
    if (const EpdGlyph* glyph = serveFrom(faces_[i], codepoint, false)) return glyph;
  }
  if (readerFace_) {
    if (const EpdGlyph* glyph = serveFrom(readerFace_(readerCtx_), codepoint, true)) return glyph;
  }
  for (uint8_t i = before; i < faceCount_; ++i) {
    if (const EpdGlyph* glyph = serveFrom(faces_[i], codepoint, false)) return glyph;
  }
  return nullptr;
}

const EpdGlyph* FallbackFaceProvider::serveFrom(const EpdFontData* face, const uint32_t codepoint, const bool reader) {
  // A reader glyph is a provider's 2-bit one, drawn as its 1-bit ink, which has to fit the buffer ownsBitmap() packs
  // it into.
  if (!face || (reader && (!face->is2Bit || !face->provider))) return nullptr;
  const EpdGlyphRef glyph = EpdFont(face).findGlyph(codepoint);
  if (!glyph || (reader && !glyph.sdRecord)) return nullptr;
  const uint16_t bytes = glyphDataBytes(glyph.width, glyph.height, false);
  if (reader && bytes > kInkBytes) return nullptr;

  Served& slot = ring_[next_];
  next_ = static_cast<uint8_t>((next_ + 1) % RING_SIZE);
  // No bitmap is touched here: measuring text resolves glyphs by the hundred and never draws them,
  // so a compressed face pays its decode only in ownsBitmap(), for a glyph actually drawn.
  // dataOffset is the face's own for a flat face and 0 for a compressed one, which has no flat
  // array; nothing reads it for a provider record, whose bitmap comes from ownsBitmap().
  slot.record = EpdGlyph{glyph.width,
                         glyph.height,
                         glyph.advanceX,
                         glyph.left,
                         glyph.top,
                         reader ? bytes : glyphDataBytes(glyph.width, glyph.height, face->is2Bit),
                         face->groups || reader ? 0 : epdGlyphBitmapOffset(face, glyph)};
  slot.face = face;
  slot.source = reader ? glyph.sdRecord : nullptr;
  slot.index = glyph.index;
  return &slot.record;
}

namespace {

// Whether `face`'s own interval table holds `cp`: no provider, no fallbacks.
bool inTable(const EpdFontData* face, const uint32_t cp) {
  if (!face || !face->intervals || face->intervalCount == 0) return false;
  const EpdUnicodeInterval* const end = face->intervals + face->intervalCount;
  const EpdUnicodeInterval* const after =
      std::upper_bound(face->intervals, end, cp,
                       [](const uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });
  return after != face->intervals && cp <= (after - 1)->last;
}

}  // namespace

void FallbackFaceProvider::beginText(const char* utf8Text) {
  bookText_ = false;
  if (!utf8Text || !readerFace_) return;
  // The characters the UI face lacks, up to a batch, as UTF-8: what the reader's face is asked for when the string is
  // book text. It is when one of them is not the active language's either (for any other UI language, any of them).
  char lacking[96];
  size_t used = 0;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8Text);
  while (*p != 0) {
    const uint8_t* const start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp < 0x80 || inTable(primary_, cp)) continue;
    if (!readerAfterFirst_ || faceCount_ == 0 || !inTable(faces_[0], cp)) bookText_ = true;
    const size_t bytes = static_cast<size_t>(p - start);
    if (used + bytes < sizeof(lacking)) {
      std::memcpy(lacking + used, start, bytes);
      used += bytes;
    }
  }
  if (!bookText_ || used == 0) return;
  const EpdFontData* const reader = readerFace_(readerCtx_);
  if (!reader || !reader->provider) return;
  lacking[used] = '\0';
  reader->provider->prefetch(lacking);
}

bool FallbackFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  for (const Served& slot : ring_) {
    if (&slot.record != glyph) continue;
    const EpdGlyph& r = slot.record;
    bitmap = nullptr;
    if (r.width == 0 || r.height == 0 || !slot.face) return true;
    if (slot.source) {
      // The reader face's bitmap, as the renderer finds a provider glyph's (GfxRenderer::getGlyphBitmap), packed to
      // the ink its black-and-white pass draws. Valid until the next reader glyph's: the renderer draws it first.
      const uint8_t* source = nullptr;
      if (!slot.face->provider->ownsBitmap(slot.source, source)) {
        source = slot.face->bitmap + slot.source->dataOffset;
      }
      if (source) {
        packInkMask(source, glyphDataBytes(r.width, r.height, true), ink_);
        bitmap = ink_;
      }
      return true;
    }
    if (!slot.face->groups) {
      bitmap = slot.face->bitmap + r.dataOffset;
      return true;
    }
    // setFaces() admits a compressed face only with a decompressor. The pointer is the
    // decompressor's fallback cache slot: valid until its next decode, and the renderer draws it
    // before resolving another glyph.
    const EpdGlyphRef ref{
        nullptr, r.advanceX, slot.index, r.width, r.height, static_cast<int8_t>(r.left), static_cast<int8_t>(r.top),
        true};
    if (decompressor_) bitmap = decompressor_->getBitmap(slot.face, ref, slot.index);
    return true;
  }
  return false;
}
