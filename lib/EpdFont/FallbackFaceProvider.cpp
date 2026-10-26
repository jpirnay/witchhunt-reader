#include "FallbackFaceProvider.h"

#include <cstring>

#include "EpdFont.h"
#include "FontDecompressor.h"
#include "Logging.h"
#include "Utf8.h"

uint8_t FallbackFaceProvider::packedBuf_[FallbackFaceProvider::PACKED_BUF_BYTES] = {};

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

void FallbackFaceProvider::invalidateRing() {
  for (Served& slot : ring_) {
    slot.face = nullptr;
    slot.sourceRecord = nullptr;
  }
}

void FallbackFaceProvider::setChainedFace(const EpdFontData* face, const bool afterFirstFace) {
  invalidateRing();
  chained_ = face;
  afterFirstFace_ = afterFirstFace;
  preferChained_ = false;
}

void FallbackFaceProvider::ensureChainedAttached(const uint32_t codepoint) const {
  if (onDemandAttach_ && !inOnDemand_ && (!chained_ || (codepoint != 0 && !EpdFont(chained_).findGlyph(codepoint)))) {
    inOnDemand_ = true;
    onDemandAttach_(codepoint);
    inOnDemand_ = false;
  }
}

const EpdGlyph* FallbackFaceProvider::missGlyph(const uint32_t codepoint) {
  if (const EpdGlyph* glyph = serve(codepoint)) return glyph;
  if (claims_ && codepoint != standIn_ && claims_(codepoint)) return serveStandIn();
  return nullptr;
}

const EpdGlyph* FallbackFaceProvider::serve(const uint32_t codepoint) {
  const bool chainedFirst = preferChained_ || !afterFirstFace_ || faceCount_ == 0;
  if (chainedFirst) {
    ensureChainedAttached(codepoint);
    if (const EpdGlyph* g = serveFace(chained_, codepoint)) return g;
    for (uint8_t i = 0; i < faceCount_; ++i) {
      if (const EpdGlyph* g = serveFace(faces_[i], codepoint)) return g;
    }
    return nullptr;
  }
  if (const EpdGlyph* g = serveFace(faces_[0], codepoint)) return g;
  ensureChainedAttached(codepoint);
  if (const EpdGlyph* g = serveFace(chained_, codepoint)) return g;
  for (uint8_t i = 1; i < faceCount_; ++i) {
    if (const EpdGlyph* g = serveFace(faces_[i], codepoint)) return g;
  }
  return nullptr;
}

const EpdGlyph* FallbackFaceProvider::serveStandIn() {
  for (uint8_t i = 0; i < faceCount_; ++i) {
    if (const EpdGlyph* g = serveFace(faces_[i], standIn_)) return g;
  }
  return serveFace(chained_, standIn_);
}

const EpdGlyph* FallbackFaceProvider::serveFace(const EpdFontData* face, const uint32_t codepoint) {
  if (!face) return nullptr;
  const EpdGlyphRef glyph = EpdFont(face).findGlyph(codepoint);
  if (!glyph) return nullptr;

  const uint16_t len = glyphDataBytes(glyph.width, glyph.height, false);
  if (face->is2Bit && len > PACKED_BUF_BYTES) {
    if (!loggedOversize_) {
      loggedOversize_ = true;
      LOG_DBG("UI_FONT", "Chained glyph U+%04lX (%ux%u, %u B) exceeds %u B pack buffer",
              static_cast<unsigned long>(codepoint), glyph.width, glyph.height, len, PACKED_BUF_BYTES);
    }
    return nullptr;
  }

  Served& slot = ring_[next_];
  next_ = static_cast<uint8_t>((next_ + 1) % RING_SIZE);
  slot.record = EpdGlyph{glyph.width,
                         glyph.height,
                         glyph.advanceX,
                         glyph.left,
                         glyph.top,
                         len,
                         face->groups ? 0 : epdGlyphBitmapOffset(face, glyph)};
  slot.face = face;
  slot.sourceRecord = glyph.sdRecord;
  slot.index = glyph.index;
  return &slot.record;
}

bool FallbackFaceProvider::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  for (const Served& slot : ring_) {
    if (&slot.record != glyph) continue;
    const EpdGlyph& r = slot.record;
    bitmap = nullptr;
    if (r.width == 0 || r.height == 0 || !slot.face) return true;
    if (slot.face->is2Bit) {
      if (r.dataLength > PACKED_BUF_BYTES) return true;
      const uint8_t* src = nullptr;
      if (slot.face->provider && slot.sourceRecord) {
        if (!slot.face->provider->ownsBitmap(slot.sourceRecord, src) && slot.face->bitmap) {
          src = slot.face->bitmap + slot.sourceRecord->dataOffset;
        }
      } else if (slot.face->bitmap) {
        src = slot.face->bitmap + r.dataOffset;
      }
      if (!src) return true;
      packInkMask(src, glyphDataBytes(r.width, r.height, true), packedBuf_);
      bitmap = packedBuf_;
      return true;
    }
    if (!slot.face->groups) {
      if (slot.face->provider && slot.sourceRecord && slot.face->provider->ownsBitmap(slot.sourceRecord, bitmap)) {
        return true;
      }
      bitmap = slot.face->bitmap ? slot.face->bitmap + r.dataOffset : nullptr;
      return true;
    }
    const EpdGlyphRef ref{
        nullptr, r.advanceX, slot.index, r.width, r.height, static_cast<int8_t>(r.left), static_cast<int8_t>(r.top),
        true};
    if (decompressor_) bitmap = decompressor_->getBitmap(slot.face, ref, slot.index);
    return true;
  }
  return false;
}

void FallbackFaceProvider::prefetch(const char* (*getter)(void* ctx, int index), void* ctx, const int count) {
  if (!getter || count <= 0) return;

  bool hasNonAscii = false;
  for (int i = 0; i < count; ++i) {
    const char* p = getter(ctx, i);
    if (!p) continue;
    while (*p) {
      if (static_cast<unsigned char>(*p) >= 0x80) {
        hasNonAscii = true;
        break;
      }
      ++p;
    }
    if (hasNonAscii) break;
  }
  if (!hasNonAscii) return;

  uint32_t firstMissingCp = 0;
  const bool checkFirstFace = afterFirstFace_ && faceCount_ > 0 && faces_[0] != nullptr;
  for (int i = 0; i < count && firstMissingCp == 0; ++i) {
    const char* p = getter(ctx, i);
    if (!p) continue;
    while (*p) {
      const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p));
      if (cp == 0) break;
      if (cp < 0x80) continue;
      if (primary_ && EpdFont(primary_).findGlyph(cp)) continue;
      if (checkFirstFace && EpdFont(faces_[0]).findGlyph(cp)) continue;
      firstMissingCp = cp;
      break;
    }
  }

  if (firstMissingCp == 0 && checkFirstFace) {
    preferChained_ = false;
    return;
  }

  ensureChainedAttached(firstMissingCp);
  if (!chained_) {
    preferChained_ = false;
    return;
  }

  preferChained_ = true;
  if (!chained_->provider) return;

  char buf[196];
  size_t bufLen = 0;
  for (int i = 0; i < count; ++i) {
    const char* p = getter(ctx, i);
    if (!p) continue;
    while (*p) {
      const char* spanStart = p;
      const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p));
      const size_t spanLen = static_cast<size_t>(p - spanStart);
      if (cp == 0 || spanLen == 0) break;
      if (cp < 0x80) continue;
      if (primary_ && EpdFont(primary_).findGlyph(cp)) continue;
      if (bufLen + spanLen >= sizeof(buf)) {
        buf[bufLen] = 0;
        chained_->provider->prefetch(buf);
        bufLen = 0;
      }
      if (spanLen < sizeof(buf)) {
        std::memcpy(buf + bufLen, spanStart, spanLen);
        bufLen += spanLen;
      }
    }
  }
  if (bufLen > 0) {
    buf[bufLen] = 0;
    chained_->provider->prefetch(buf);
  }
}

void FallbackFaceProvider::prefetch(const char* utf8Text) {
  if (!utf8Text || !*utf8Text) return;
  auto getSingle = [](void* ctx, int /*index*/) -> const char* {
    return static_cast<const char*>(ctx);
  };
  prefetch(getSingle, const_cast<char*>(utf8Text), 1);
}
