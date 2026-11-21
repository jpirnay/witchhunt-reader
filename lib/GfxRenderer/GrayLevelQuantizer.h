#pragma once

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
//
// Integer only. The plane path serves the C3 boards, which have no FPU: computing L* there took soft-float and
// libm's pow and cbrt, about 7 KB of flash. sRGB -> L* comes from a table instead, and the setup works in hundredths
// of L*.
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
    const int32_t white = lightness[0];
    const int32_t black = lightness[levels - 1];
    for (int gray = 0; gray < 256; ++gray) {
      // Hundredths of L*: the gray's L* stretched onto the panel's range, rounded. At most 100 * 10000.
      const int32_t target = black * 100 + ((white - black) * kLightnessCenti[gray] + 50) / 100;
      uint8_t ink = 0;
      while (ink + 1 < levels && lightness[ink + 1] * 100 >= target) ++ink;
      lighter_[gray] = ink;
      if (ink + 1 >= levels) {
        step_[gray] = 0;  // at the panel's black
        continue;
      }
      // lightness[ink] >= target > lightness[ink + 1]: the share of the way to the darker ink, in 256ths, rounded.
      const int32_t span = (lightness[ink] - lightness[ink + 1]) * 100;
      const int32_t step = ((lightness[ink] * 100 - target) * 256 + span / 2) / span;
      step_[gray] = static_cast<uint8_t>(step > 255 ? 255 : step);
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

  // CIE L* of an sRGB gray value, in hundredths (0 .. 10000).
  static uint16_t lightnessCenti(const uint8_t gray) { return kLightnessCenti[gray]; }

 private:
  static constexpr uint8_t kBayer8[8][8] = {
      {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
      {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
      {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
  };

  // CIE L* of each sRGB gray, in hundredths, rounded. GrayLevelPlanTest recomputes every entry from the sRGB and
  // CIE formulas, so a wrong entry fails the host tests.
  static constexpr uint16_t kLightnessCenti[256] = {
      0,    27,   55,   82,   110,  137,  165,  192,  219,   247,  274,  302,  332,  364,  397,  432,  468,  506,  546,
      588,  632,  677,  725,  774,  825,  876,  926,  977,   1027, 1077, 1126, 1176, 1225, 1274, 1323, 1371, 1420, 1468,
      1516, 1564, 1611, 1659, 1706, 1753, 1800, 1847, 1894,  1940, 1987, 2033, 2079, 2125, 2170, 2216, 2262, 2307, 2352,
      2397, 2442, 2487, 2532, 2576, 2621, 2665, 2709, 2753,  2797, 2841, 2885, 2929, 2972, 3016, 3059, 3103, 3146, 3189,
      3232, 3275, 3318, 3360, 3403, 3445, 3488, 3530, 3572,  3615, 3657, 3699, 3741, 3782, 3824, 3866, 3907, 3949, 3990,
      4032, 4073, 4114, 4155, 4196, 4237, 4278, 4319, 4360,  4401, 4441, 4482, 4522, 4563, 4603, 4644, 4684, 4724, 4764,
      4804, 4844, 4884, 4924, 4964, 5003, 5043, 5083, 5122,  5162, 5201, 5241, 5280, 5319, 5359, 5398, 5437, 5476, 5515,
      5554, 5593, 5632, 5670, 5709, 5748, 5786, 5825, 5864,  5902, 5940, 5979, 6017, 6056, 6094, 6132, 6170, 6208, 6246,
      6284, 6322, 6360, 6398, 6436, 6474, 6511, 6549, 6587,  6624, 6662, 6699, 6737, 6774, 6812, 6849, 6887, 6924, 6961,
      6998, 7035, 7072, 7110, 7147, 7184, 7221, 7257, 7294,  7331, 7368, 7405, 7442, 7478, 7515, 7551, 7588, 7625, 7661,
      7698, 7734, 7770, 7807, 7843, 7879, 7916, 7952, 7988,  8024, 8060, 8097, 8133, 8169, 8205, 8241, 8276, 8312, 8348,
      8384, 8420, 8456, 8491, 8527, 8563, 8598, 8634, 8670,  8705, 8741, 8776, 8812, 8847, 8882, 8918, 8953, 8988, 9024,
      9059, 9094, 9129, 9164, 9200, 9235, 9270, 9305, 9340,  9375, 9410, 9445, 9480, 9515, 9549, 9584, 9619, 9654, 9688,
      9723, 9758, 9793, 9827, 9862, 9896, 9931, 9965, 10000,
  };

  uint8_t levels_ = 0;
  // Per input gray: the lighter ink of the bracketing pair, and how far toward the darker one (0..255).
  uint8_t lighter_[256] = {};
  uint8_t step_[256] = {};
};
