#pragma once

#include <EpdFontFamily.h>
#include <GlyphRun.h>

#include <cstddef>
#include <cstdint>

/// Coalesces per-piece glyph ID warmups across a paragraph into deduplicated batches of up to
/// `kCapacity` unique glyph IDs per `(fontId, style)` (~144 B on the stack, zero heap allocation),
/// issuing one `renderer.prefetchGlyphs()` / MissScope commit per batch rather than per word token.
class GlyphPrefetchBatch {
 public:
  static constexpr size_t kCapacity = 64;

  template <typename Renderer>
  void addRun(const Renderer& renderer, const int fontId, const GlyphRun& run, const EpdFontFamily::Style style) {
    forEachGlyph(run, 0, false, [&](const GlyphRecord& record, int) { add(renderer, fontId, style, record.glyph); });
  }

  template <typename Renderer>
  void add(const Renderer& renderer, const int fontId, const EpdFontFamily::Style style, const uint16_t glyph) {
    if (count_ > 0 && (fontId_ != fontId || style_ != style)) {
      flush(renderer);
    }
    fontId_ = fontId;
    style_ = style;
    for (size_t i = 0; i < count_; ++i) {
      if (glyphs_[i] == glyph) return;
    }
    glyphs_[count_++] = glyph;
    if (count_ == kCapacity) flush(renderer);
  }

  template <typename Renderer>
  void flush(const Renderer& renderer) {
    if (count_ == 0) return;
    renderer.prefetchGlyphs(fontId_, glyphs_, count_, style_);
    count_ = 0;
  }

 private:
  uint16_t glyphs_[kCapacity] = {};
  size_t count_ = 0;
  int fontId_ = 0;
  EpdFontFamily::Style style_ = EpdFontFamily::REGULAR;
};
