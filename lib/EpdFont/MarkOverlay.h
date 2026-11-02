#pragma once

#include <BidiUtils.h>
#include <Utf8.h>

#include <cstdint>

/// True for a mark drawn over the letter before it, with no advance of its own: the Latin and CJK combining
/// blocks, and the Hebrew and Arabic marks (niqqud, harakat) the bidi pass treats as transparent. The range test
/// keeps Latin and CJK text off BidiUtils. Measurement (EpdFont::getTextBounds, GfxRenderer) and drawing share it.
inline bool overlaysItsBase(const uint32_t cp) {
  return utf8IsCombiningMark(cp) || (cp >= 0x0591 && cp <= 0x08FF ? BidiUtils::isTransparentMark(cp) : cp == 0xFB1E);
}
