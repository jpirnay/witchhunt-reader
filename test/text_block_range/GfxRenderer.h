#pragma once
// Host-test shim for GfxRenderer — resolves ahead of lib/GfxRenderer/GfxRenderer.h
// via include-path order (this directory is listed before lib/GfxRenderer, which is
// deliberately NOT on the include path of the WordSizeLayoutTest target).
//
// Implements exactly the measurement/draw surface that ParsedText.cpp and
// TextBlock.cpp use, with deterministic fixed-width metrics:
//   - every codepoint advances GLYPH_W pixels (scaled by the per-word scale)
//   - space advance is SPACE_W, kerning is always 0
//   - ascender is ASCENDER, scaled variants multiply and round
// drawText/drawTextScaled calls are recorded so tests can assert baseline
// alignment and per-word scales.

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  void setOrientation(const Orientation o) {
    held = o;
    live = o;
  }
  void setDrawOrientation(const Orientation o) const { live = o; }
  Orientation getOrientation() const { return live; }
  Orientation getHeldOrientation() const { return held; }
  int getScreenWidth(const Orientation o) const { return o == Portrait || o == PortraitInverted ? 480 : 800; }

  static constexpr int GLYPH_W = 10;
  static constexpr int SPACE_W = 5;
  static constexpr int ASCENDER = 16;
  static constexpr int LINE_HEIGHT = 20;

  struct DrawCall {
    std::string text;
    int fontId;
    int x;
    int y;
    float scale;
    EpdFontFamily::Style style;
    Orientation frame;  // the live orientation at the call
  };
  mutable std::vector<DrawCall> drawCalls;

  struct RectCall {
    int x;
    int y;
    int w;
    int h;
    bool state;
  };
  mutable std::vector<RectCall> fillRectCalls;

  static int countCodepoints(const char* text) {
    int count = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
      if ((*p & 0xC0) != 0x80) ++count;  // count UTF-8 lead bytes
    }
    return count;
  }

  // --- Measurement surface used by ParsedText ---
  int getSpaceWidth(int /*fontId*/, EpdFontFamily::Style /*style*/ = EpdFontFamily::REGULAR) const { return SPACE_W; }
  int getTextAdvanceX(int /*fontId*/, const char* text, EpdFontFamily::Style /*style*/) const {
    return countCodepoints(text) * GLYPH_W;
  }
  int getSpaceAdvance(int /*fontId*/, uint32_t /*leftCp*/, uint32_t /*rightCp*/, EpdFontFamily::Style /*style*/) const {
    return SPACE_W;
  }
  int getKerning(int /*fontId*/, uint32_t /*leftCp*/, uint32_t /*rightCp*/, EpdFontFamily::Style /*style*/) const {
    return 0;
  }
  int getFontAscenderSize(int /*fontId*/) const { return ASCENDER; }
  int getFontAscenderSizeScaled(int /*fontId*/, const float scale) const {
    return static_cast<int>(ASCENDER * scale + 0.5f);
  }
  int getLineHeight(int /*fontId*/) const { return LINE_HEIGHT; }
  int getLineHeightScaled(int /*fontId*/, const float scale) const {
    return static_cast<int>(LINE_HEIGHT * scale + 0.5f);
  }
  void ensureFontReady(int /*fontId*/, const char* /*utf8Text*/) const {}
  bool hasGlyph(int /*fontId*/, uint32_t /*cp*/, EpdFontFamily::Style /*style*/) const { return true; }

  // --- Render surface used by TextBlock ---
  bool isFontCacheScanning() const { return false; }
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return getTextAdvanceX(fontId, text, style);
  }
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, const float scale) const {
    return static_cast<int>(getTextAdvanceX(fontId, text, style) * scale + 0.5f);
  }
  void drawText(int fontId, int x, int y, const char* text, bool /*black*/ = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    drawCalls.push_back({text, fontId, x, y, 1.0f, style, live});
  }
  void drawTextScaled(int fontId, int x, int y, const char* text, bool /*black*/, EpdFontFamily::Style style,
                      const float scale) const {
    drawCalls.push_back({text, fontId, x, y, scale, style, live});
  }
  void drawLine(int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/, int /*lineWidth*/, bool /*state*/) const {}
  void fillRect(int x, int y, int width, int height, bool state = true) const {
    fillRectCalls.push_back({x, y, width, height, state});
  }

 private:
  Orientation held = Portrait;
  mutable Orientation live = Portrait;
};
