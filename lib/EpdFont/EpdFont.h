#pragma once
#include "EpdFontData.h"

class EpdFont {
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY,
                     bool smallCaps = false) const;
  /// Interval table plus the SD font's on-demand loader, with no fallbacks --
  /// so getGlyph()'s fallbacks can never recurse into one another.
  const EpdGlyph* findGlyph(uint32_t cp) const;

 public:
  const EpdFontData* data;
  explicit EpdFont(const EpdFontData* data) : data(data) {}
  ~EpdFont() = default;
  void getTextDimensions(const char* string, int* w, int* h, bool smallCaps = false) const;

  /// The glyph for `cp`. When the font has no glyph for it, falls back first to
  /// a close visual relative (see GlyphFallback.h) and then to U+FFFD, so an
  /// uncovered codepoint degrades to something readable rather than a box.
  const EpdGlyph* getGlyph(uint32_t cp) const;

  /// Returns the kerning adjustment (4.4 fixed-point in pixels) between two codepoints.
  /// Returns 0 if no kerning data exists for the pair.
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp) const;

  /// Returns the ligature codepoint for a pair, or 0 if no ligature exists.
  uint32_t getLigature(uint32_t leftCp, uint32_t rightCp) const;

  /// Greedily applies ligature substitutions starting from cp, consuming
  /// as many following codepoints from text as possible. Returns the
  /// (possibly substituted) codepoint; advances text past consumed chars.
  uint32_t applyLigatures(uint32_t cp, const char*& text) const;
};
