#pragma once

#include <cmath>
#include <cstdint>

// Gives a picture's 8-bit gray (sRGB, 0 black .. 255 white) one of a panel's measured inks, with an ordered dither.
//
// For panels that take a picture with more than four levels as a sequence of 1-bit planes (GrayLevelPlan.h). The
// picture is drawn once per plane, so every draw must give each pixel the same ink. Error diffusion cannot promise
// that: a scaled picture writes some screen pixels more than once, and an error carried from pixel to pixel then
// depends on the order of those writes. A threshold that depends only on where the pixel sits gives the same ink
// every time. With 16 levels the steps between neighbouring inks are small, so the pattern of an ordered dither
// stays faint.
//
// The picture's lightness range is stretched onto the panel's, so the panel's own black and white stay reachable;
// in between, a pixel lands between the two inks that bracket its target lightness, in proportion.
class GrayLevelQuantizer {
 public:
  static constexpr uint8_t kMaxLevels = 16;

  // `lightness`: L* of each ink, ink 0 (white) first, strictly falling. False, leaving the quantizer unusable,
  // for fewer than two levels, more than kMaxLevels, or a table that does not fall.
  bool init(const uint8_t* lightness, const uint8_t levels) {
    levels_ = 0;
    if (!lightness || levels < 2 || levels > kMaxLevels) return false;
    for (uint8_t i = 1; i < levels; ++i) {
      if (lightness[i] >= lightness[i - 1]) return false;
    }
    const float white = lightness[0];
    const float black = lightness[levels - 1];
    for (int gray = 0; gray < 256; ++gray) {
      const float target = black + (white - black) * lightnessOf(gray) / 100.0f;
      uint8_t ink = 0;
      while (ink + 1 < levels && static_cast<float>(lightness[ink + 1]) >= target) ++ink;
      lighter_[gray] = ink;
      if (ink + 1 >= levels) {
        step_[gray] = 0;  // at or below the panel's black
        continue;
      }
      // lightness[ink] >= target > lightness[ink + 1]: the share of the way to the darker ink, below 255.
      const float span = static_cast<float>(lightness[ink] - lightness[ink + 1]);
      const float share = (static_cast<float>(lightness[ink]) - target) / span;
      const int step = static_cast<int>(share * 256.0f);
      step_[gray] = static_cast<uint8_t>(step < 0 ? 0 : (step > 255 ? 255 : step));
    }
    levels_ = levels;
    return true;
  }

  uint8_t levels() const { return levels_; }
  uint8_t darkest() const { return levels_ - 1; }

  // The ink for `gray` at physical pixel (x, y). The darker ink of the pair wins where the pixel's share of the way
  // there exceeds its threshold in an 8x8 Bayer matrix (2..254).
  uint8_t ink(const uint8_t gray, const unsigned x, const unsigned y) const {
    const uint8_t threshold = static_cast<uint8_t>(kBayer8[y & 7u][x & 7u] * 4 + 2);
    return static_cast<uint8_t>(lighter_[gray] + (step_[gray] > threshold ? 1 : 0));
  }

  // CIE L* (0..100) of an sRGB gray value.
  static float lightnessOf(const int gray) {
    const float c = static_cast<float>(gray) / 255.0f;
    const float y = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    return y > 0.008856f ? 116.0f * std::cbrt(y) - 16.0f : 903.3f * y;
  }

 private:
  static constexpr uint8_t kBayer8[8][8] = {
      {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
      {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
      {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
  };

  uint8_t levels_ = 0;
  // Per input gray: the lighter ink of the bracketing pair, and how far toward the darker one (0..255).
  uint8_t lighter_[256] = {};
  uint8_t step_[256] = {};
};
