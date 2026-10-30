#include "VerticalDraw.h"

#include <GfxRenderer.h>

#include <cstring>

#include "VerticalOrientation.h"

namespace {

constexpr uint32_t kIdeographOne = 0x4E00;
constexpr char kIdeographOneUtf8[] = "\xE4\xB8\x80";

int scaled(const int raw, const float scale) { return scale == 1.0f ? raw : static_cast<int>(raw * scale + 0.5f); }

// The advance of 一 unscaled; 0 in a font that has none.
int rawEm(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style) {
  if (!renderer.hasGlyph(fontId, kIdeographOne, style)) return 0;
  return renderer.getTextAdvanceX(fontId, kIdeographOneUtf8, style);
}

// How far a piece runs down the column, unscaled.
int rawColumnAdvance(const GfxRenderer& renderer, const int fontId, const char* piece, const bool upright,
                     const EpdFontFamily::Style style, const int em) {
  if (upright && em > 0) return em;
  return renderer.getTextAdvanceX(fontId, piece, style);
}

int ascenderOf(const GfxRenderer& renderer, const int fontId, const float scale) {
  return scale == 1.0f ? renderer.getFontAscenderSize(fontId) : renderer.getFontAscenderSizeScaled(fontId, scale);
}

void drawIn(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
            const EpdFontFamily::Style style, const float scale) {
  if (scale == 1.0f) {
    renderer.drawText(fontId, x, y, text, true, style);
  } else {
    renderer.drawTextScaled(fontId, x, y, text, true, style, scale);
  }
}

}  // namespace

int columnEm(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const float scale) {
  const int raw = rawEm(renderer, fontId, style);
  return raw > 0 ? scaled(raw, scale) : ascenderOf(renderer, fontId, scale);
}

int verticalTextAdvance(const GfxRenderer& renderer, const int fontId, const char* text,
                        const EpdFontFamily::Style style, const float scale) {
  const int em = rawEm(renderer, fontId, style);
  char piece[kMaxColumnRunBytes + 1];
  int raw = 0;
  bool upright = false;
  while (const size_t length = nextColumnPiece(text, upright)) {
    memcpy(piece, text, length);
    piece[length] = '\0';
    raw += rawColumnAdvance(renderer, fontId, piece, upright, style, em);
    text += length;
  }
  return scaled(raw, scale);
}

void drawTextVertical(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
                      const EpdFontFamily::Style style, const float scale) {
  if (renderer.isFontCacheScanning()) {
    // Record 一 in this style too: the draw below measures with it, and an unprewarmed glyph is a miss on SD/TTF fonts.
    (void)rawEm(renderer, fontId, style);
    drawIn(renderer, fontId, x, y, text, style, scale);
    return;
  }
  const GfxRenderer::Orientation reader = renderer.getHeldOrientation();
  const GfxRenderer::Orientation content = renderer.getOrientation();
  const int readerWidth = renderer.getScreenWidth(reader);
  const int ascender = ascenderOf(renderer, fontId, scale);
  const int rawOne = rawEm(renderer, fontId, style);
  const int em = rawOne > 0 ? scaled(rawOne, scale) : ascender;
  const int axis = columnAxis(y + ascender, em);

  char piece[kMaxColumnRunBytes + 1];
  int rawBefore = 0;
  bool upright = false;
  while (const size_t length = nextColumnPiece(text, upright)) {
    memcpy(piece, text, length);
    piece[length] = '\0';
    text += length;
    const int pen = x + scaled(rawBefore, scale);
    rawBefore += rawColumnAdvance(renderer, fontId, piece, upright, style, rawOne);
    if (!upright) {
      drawIn(renderer, fontId, pen, y, piece, style, scale);
      continue;
    }
    const int adv = scaled(renderer.getTextAdvanceX(fontId, piece, style), scale);
    renderer.setDrawOrientation(reader);
    drawIn(renderer, fontId, readerWidth - 1 - axis - adv / 2, pen + (em * 88 + 50) / 100 - ascender, piece, style,
           scale);
    renderer.setDrawOrientation(content);
  }
}
