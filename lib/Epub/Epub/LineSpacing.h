#pragma once

#include <cstdint>

// The reader's line-spacing steps, in the order the setting stores them (CrossPointSettings::LINE_COMPRESSION asserts
// it matches). FromBook is Book's style: each block's CSS line-height, never tighter than Tight.
enum class LineSpacingStep : uint8_t { Tight = 0, Normal, Wide, ExtraWide, Widest, FromBook };

// What a step means for a book.
//  - Font: a factor on the body font's own line height.
//  - BuiltinNotoSans: the same, with lower factors: the built-in Noto Sans's own line height is taller.
//  - CjkPitch: Chinese, Japanese and Korean books, a line pitch in ems of the body text. JLREQ and CLREQ set the gap
//    between lines at half to one character (a pitch of 1.5 to 2 em); a CJK face's own line height is about 1.45 em,
//    below that. Widest goes past the range, for readers who want more air.
enum class LineSpacingScale : uint8_t { Font, BuiltinNotoSans, CjkPitch };

struct LineSpacing {
  // Line pitch over the body font's line height, for every line the book's line-height does not set.
  float compression = 1.0f;
  // Book's style: no block's compression below this. 0 when the book's line-height is not read.
  float bookFloor = 0.0f;
  // The body text's em in pixels, which a book's line-height multiplies.
  uint16_t emPx = 0;
};

// Every font is converted or rasterised at 150 dpi (fontconvert.py, ttf::ppemFromPoints), so a point is 150/72 px.
constexpr uint16_t lineSpacingEmPx(const uint8_t pointSize) {
  return static_cast<uint16_t>((pointSize * 150 + 36) / 72);
}

namespace line_spacing_detail {
constexpr uint8_t kSteps = static_cast<uint8_t>(LineSpacingStep::FromBook);
constexpr float kFontFactor[kSteps] = {0.95f, 1.0f, 1.1f, 1.2f, 1.3f};
constexpr float kNotoSansFactor[kSteps] = {0.90f, 0.95f, 1.0f, 1.1f, 1.2f};
constexpr float kCjkPitchEm[kSteps] = {1.5f, 1.7f, 1.85f, 2.0f, 2.2f};

constexpr float compressionAt(const uint8_t step, const LineSpacingScale scale, const uint16_t emPx,
                              const int fontLineHeightPx) {
  switch (scale) {
    case LineSpacingScale::Font:
      return kFontFactor[step];
    case LineSpacingScale::BuiltinNotoSans:
      return kNotoSansFactor[step];
    case LineSpacingScale::CjkPitch:
      return fontLineHeightPx > 0 ? kCjkPitchEm[step] * emPx / static_cast<float>(fontLineHeightPx) : 1.0f;
  }
  return 1.0f;
}
}  // namespace line_spacing_detail

// The spacing for a stored step. An unknown step reads as Normal.
constexpr LineSpacing lineSpacingFor(const uint8_t step, const LineSpacingScale scale, const uint8_t pointSize,
                                     const int fontLineHeightPx) {
  using namespace line_spacing_detail;
  constexpr auto normal = static_cast<uint8_t>(LineSpacingStep::Normal);
  constexpr auto tight = static_cast<uint8_t>(LineSpacingStep::Tight);
  LineSpacing ls;
  ls.emPx = lineSpacingEmPx(pointSize);
  if (step == static_cast<uint8_t>(LineSpacingStep::FromBook)) {
    ls.compression = compressionAt(normal, scale, ls.emPx, fontLineHeightPx);
    ls.bookFloor = compressionAt(tight, scale, ls.emPx, fontLineHeightPx);
  } else {
    ls.compression = compressionAt(step < kSteps ? step : normal, scale, ls.emPx, fontLineHeightPx);
  }
  return ls;
}
