#include "VerticalDraw.h"

#include <GfxRenderer.h>
#include <Utf8.h>

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
            const EpdFontFamily::Style style, const float scale, const bool black) {
  if (scale == 1.0f) {
    renderer.drawText(fontId, x, y, text, black, style);
  } else {
    renderer.drawTextScaled(fontId, x, y, text, black, style, scale);
  }
}

// How far an upright piece's ink moves in the reader's frame: right by dx, down by dy.
struct Shift {
  int dx = 0;
  int dy = 0;
};

// The ink of `cp` at `scale`. False when the font has no glyph of its own (a lookalike or U+FFFD is never placed) or no
// ink for it.
bool scaledInk(const GfxRenderer& renderer, const int fontId, const uint32_t cp, const EpdFontFamily::Style style,
               const float scale, GlyphInk& ink) {
  if (!renderer.hasGlyph(fontId, cp, style) || !renderer.getGlyphInk(fontId, cp, style, ink)) return false;
  ink.left = scaled(ink.left, scale);
  ink.width = scaled(ink.width, scale);
  ink.top = scaled(ink.top, scale);
  ink.height = scaled(ink.height, scale);
  return true;
}

// The shift a placement gives a piece of advance `adv` in an em cell, from its ink.
Shift shiftOf(const UprightPlacement placement, const GlyphInk& ink, const int em, const int adv,
              const bool centredMarks) {
  Shift shift;
  if (placement == UprightPlacement::SmallKana) {
    shift.dx = (em * 10 + 50) / 100;
    shift.dy = -shift.dx;
  } else if (placement == UprightPlacement::Reflect) {
    const int b = (em * 88 + 50) / 100;
    shift.dx = 2 * (adv / 2) - 2 * ink.left - ink.width;
    shift.dy = em - 2 * b + 2 * ink.top - ink.height;
    if (centredMarks) {
      shift.dx /= 2;
      shift.dy /= 2;
    }
  }
  return shift;
}

}  // namespace

bool columnInk(const GfxRenderer& renderer, const int fontId, const uint32_t cp, const EpdFontFamily::Style style,
               const bool centredMarks, int& inkStart, int& inkSize, int& cell) {
  if (!drawsUpright(cp)) return false;
  GlyphInk ink;
  if (!scaledInk(renderer, fontId, cp, style, 1.0f, ink)) return false;
  const int em = columnEm(renderer, fontId, style, 1.0f);
  const Shift shift = shiftOf(uprightPlacementOf(cp), ink, em, ink.advance, centredMarks);
  inkStart = (em * 88 + 50) / 100 - ink.top + shift.dy;
  inkSize = ink.height;
  cell = em;
  return true;
}

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
                      const EpdFontFamily::Style style, const float scale, const bool centredMarks, const bool black) {
  if (renderer.isFontCacheScanning()) {
    // Record 一 in this style too: the draw below measures with it, and an unprewarmed glyph is a miss on SD/TTF fonts.
    (void)rawEm(renderer, fontId, style);
    drawIn(renderer, fontId, x, y, text, style, scale, black);
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
      drawIn(renderer, fontId, pen, y, piece, style, scale, black);
      continue;
    }
    const int adv = scaled(renderer.getTextAdvanceX(fontId, piece, style), scale);
    Shift shift;
    const uint32_t cp = firstCodepointOf(piece);
    const UprightPlacement placement = uprightPlacementOf(cp);
    GlyphInk ink;
    if (placement != UprightPlacement::None && scaledInk(renderer, fontId, cp, style, scale, ink)) {
      shift = shiftOf(placement, ink, em, adv, centredMarks);
    }
    renderer.setDrawOrientation(reader);
    drawIn(renderer, fontId, readerWidth - 1 - axis - adv / 2 + shift.dx,
           pen + (em * 88 + 50) / 100 - ascender + shift.dy, piece, style, scale, black);
    renderer.setDrawOrientation(content);
  }
}

void drawCombinedVertical(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
                          const EpdFontFamily::Style style, const float scale, const bool black) {
  if (renderer.isFontCacheScanning()) {
    (void)rawEm(renderer, fontId, style);
    drawIn(renderer, fontId, x, y, text, style, scale, black);
    return;
  }
  const GfxRenderer::Orientation reader = renderer.getHeldOrientation();
  const GfxRenderer::Orientation content = renderer.getOrientation();
  const int readerWidth = renderer.getScreenWidth(reader);
  const int ascender = ascenderOf(renderer, fontId, scale);
  const int rawOne = rawEm(renderer, fontId, style);
  const int em = rawOne > 0 ? scaled(rawOne, scale) : ascender;
  const int axis = columnAxis(y + ascender, em);

  // A run wider than the em is shrunk to it, on top of the line's own scale.
  const int rawWidth = renderer.getTextAdvanceX(fontId, text, style);
  const int width = scaled(rawWidth, scale);
  const float runScale = width > em && em > 0 ? scale * static_cast<float>(em) / static_cast<float>(width) : scale;
  const int runWidth = scaled(rawWidth, runScale);
  const int runEm = rawOne > 0 ? scaled(rawOne, runScale) : ascenderOf(renderer, fontId, runScale);
  const int runAscender = ascenderOf(renderer, fontId, runScale);

  renderer.setDrawOrientation(reader);
  drawIn(renderer, fontId, readerWidth - 1 - axis - runWidth / 2,
         x + (em - runEm) / 2 + (runEm * 88 + 50) / 100 - runAscender, text, style, runScale, black);
  renderer.setDrawOrientation(content);
}
