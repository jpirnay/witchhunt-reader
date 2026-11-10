#include "FallbackFaceProvider.h"

#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "EpdFont.h"
#include "FontDecompressor.h"
#include "GlyphKey.h"
#include "SdFaceGlyph.h"

uint8_t FallbackFaceProvider::ink_[FallbackFaceProvider::kInkBytes] = {};

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
  // A glyph key names a glyph of the reader's face by its ID (shapingSource()): that face alone is asked for it, and
  // never for glyph 0, its .notdef, which is no real character either.
  if (glyph_key::is(codepoint)) {
    if (!readerFace_ || codepoint == glyph_key::of(0)) return nullptr;
    return serveFrom(readerFace_(readerCtx_), codepoint, true);
  }
  if (const EpdGlyph* glyph = serve(codepoint)) return glyph;
  if (claims_ && codepoint != standIn_ && claims_(codepoint)) return serve(standIn_);
  return nullptr;
}

uint8_t FallbackFaceProvider::facesBeforeReader() const {
  return readerAfterFirst_ && !bookText_ && faceCount_ > 0 ? 1 : 0;
}

const EpdGlyph* FallbackFaceProvider::serve(const uint32_t codepoint) {
  // The built-in faces before the reader's: none, or the active UI language's own, but for book text.
  const uint8_t before = facesBeforeReader();
  for (uint8_t i = 0; i < before; ++i) {
    if (const EpdGlyph* glyph = serveFrom(faces_[i], codepoint, false)) return glyph;
  }
  // The packs on the card, before the reader's face: a title draws alike whatever book was read last. In the reader,
  // where the book's own face is open, that face goes first for what it maps, so a book never opens a pack.
  const bool readerFirst = readerMaps(codepoint);
  if (!readerFirst) {
    if (const EpdGlyph* glyph = servePack(codepoint)) return glyph;
  }
  // The reader's face lends real characters only. Its replacement glyph (asked for once a character is in no face) or
  // its own copy of the stand-in would draw over what the UI faces leave blank or draw as their stand-in, and a book's
  // font is opened for any script (openUiBookFonts), so it often has nothing else for a title.
  if (readerFace_ && codepoint != REPLACEMENT_GLYPH && codepoint != standIn_) {
    if (const EpdGlyph* glyph = serveFrom(readerFace_(readerCtx_), codepoint, true)) return glyph;
  }
  if (readerFirst) {
    if (const EpdGlyph* glyph = servePack(codepoint)) return glyph;
  }
  for (uint8_t i = before; i < faceCount_; ++i) {
    if (const EpdGlyph* glyph = serveFrom(faces_[i], codepoint, false)) return glyph;
  }
  return nullptr;
}

const EpdFontData* FallbackFaceProvider::shapingSource(const uint32_t probe) {
  // serve()'s order up to the reader's face, asked of the tables alone: the UI face itself (beginText()'s test), the
  // active UI language's own face but for book text, and the packs but for what the book's face maps in the reader. A
  // face before the reader's that holds the probe draws the script as its characters; so does the reader's face where
  // serve() would not lend it.
  if (!readerFace_ || probe == REPLACEMENT_GLYPH || probe == standIn_ || inTable(primary_, probe)) return nullptr;
  for (uint8_t i = 0; i < facesBeforeReader(); ++i) {
    if (inTable(faces_[i], probe)) return nullptr;
  }
  if (!readerMaps(probe) && packs_.covers && packs_.covers(packs_.ctx, probe)) return nullptr;
  const EpdFontData* const face = readerFace_(readerCtx_);
  // The faces serveFrom() lends a glyph from, drawn as their ink: 2-bit, behind a provider; here one that draws keys.
  if (!face || !face->is2Bit || !face->provider || !face->provider->drawsGlyphKeys()) return nullptr;
  return face;
}

const EpdGlyph* FallbackFaceProvider::servePack(const uint32_t codepoint) {
  if (!packs_.face) return nullptr;
  const EpdFontData* pack = packs_.face(packs_.ctx, codepoint);
  const EpdGlyph* glyph = sd_face::glyph(pack, codepoint);
  if (glyph) servedPack_ = pack;
  return glyph;
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

void FallbackFaceProvider::beginText(const char* utf8Text) {
  bookText_ = false;
  servedPack_ = nullptr;
  if (!utf8Text || (!readerFace_ && !packs_.prepare)) return;
  // The characters the UI face lacks, up to a batch each, as UTF-8: those a pack holds, which the packs read for the
  // string, and the rest, which the reader's face is asked for. Both only for book text: a string is when one of them
  // is not the active language's either (for any other UI language, any of them), and a translation draws from its
  // language's own face.
  char lacking[96];
  size_t used = 0;
  char packed[96];
  size_t packedUsed = 0;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8Text);
  while (*p != 0) {
    const uint8_t* const start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp < 0x80 || inTable(primary_, cp)) continue;
    if (!readerAfterFirst_ || faceCount_ == 0 || !inTable(faces_[0], cp)) bookText_ = true;
    const size_t bytes = static_cast<size_t>(p - start);
    // A pack draws it before the reader's face would: the reader's font is not opened or prepared for it. In the
    // reader, what the book's face maps is that face's, and the packs are not asked for it.
    const bool inPack = packs_.covers && packs_.covers(packs_.ctx, cp) && !readerMaps(cp);
    char* const batch = inPack ? packed : lacking;
    size_t& batchUsed = inPack ? packedUsed : used;
    if (batchUsed + bytes < sizeof(lacking)) {
      std::memcpy(batch + batchUsed, start, bytes);
      batchUsed += bytes;
    }
  }
  if (!bookText_) return;
  if (packedUsed > 0 && packs_.prepare) {
    packed[packedUsed] = '\0';
    packs_.prepare(packs_.ctx, packed);
  }
  if (used == 0 || !readerFace_) return;
  const EpdFontData* const reader = readerFace_(readerCtx_);
  if (!reader || !reader->provider) return;
  lacking[used] = '\0';
  reader->provider->prefetch(lacking);
}

bool FallbackFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  if (servedPack_ && sd_face::owns(servedPack_, glyph, bitmap)) return true;
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
