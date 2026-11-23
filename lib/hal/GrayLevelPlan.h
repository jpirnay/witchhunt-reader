#pragma once

#include <cstdint>

// How a panel without an 8-bit canvas of its own (HalDisplay::borrowGray8Canvas) takes a picture with more than
// four levels: an ordinary B/W base first, then passes that each move the glass by what one or two 1-bit planes
// select. The shape is that of Free-Ink #220 (zuyan9), the X3's calibrated two-pass 16-gray on the UC8253: a B/W
// base, a coarse pass and a fine pass, two planes each. The X4 Pro (UC8179) and the X4 (SSD1677) are to take the
// same shape with waveforms measured on their own panels.
//
// A plan is data, so the host needs no knowledge of the waveforms: it draws every plane from the picture
// (GrayLevelQuantizer gives each pixel its ink), uploads it, and lets the panel run that step. Ink 0 is white and
// ink levels-1 the darkest, as in #220 and in #223's 16-level image.
//
// No panel answers with a plan yet: HalDisplay::grayLevelPlan() is nullptr until the SDK carries the waveforms.
namespace gray_levels {

inline constexpr uint8_t kMaxLevels = 16;
inline constexpr uint8_t kMaxPasses = 2;
inline constexpr uint8_t kPlanesPerPass = 2;

struct Plane {
  // Bit i: the plane's bit for a pixel of ink i, before `invert`.
  uint16_t mask = 0;
  // #220 states its B/W base as the inks it clears (kMaskBwClear); such a mask goes out inverted.
  bool invert = false;

  // The bit as the framebuffer holds it and the controller receives it (1 is the framebuffer's white).
  constexpr bool bitFor(const uint8_t ink) const { return (((mask >> ink) & 1u) != 0) != invert; }
};

struct Pass {
  // planes[0] goes up as the LSB plane, planes[1] as the MSB plane.
  Plane planes[kPlanesPerPass];
  uint8_t planeCount = 0;
};

struct Plan {
  uint8_t levels = 0;
  // L* each ink lands at on this panel, ink 0 first, strictly falling: what the host dithers against.
  uint8_t lightness[kMaxLevels] = {};
  // Drawn by the B/W base refresh (HalDisplay::displayGrayLevelBase) from the framebuffer.
  Plane base;
  Pass passes[kMaxPasses];
  uint8_t passCount = 0;
};

// More than four levels (four is what the plane path already shows), a strictly falling lightness table, and
// one or two passes of one or two planes each.
inline bool isUsable(const Plan& plan) {
  if (plan.levels <= 4 || plan.levels > kMaxLevels) return false;
  for (uint8_t i = 1; i < plan.levels; ++i) {
    if (plan.lightness[i] >= plan.lightness[i - 1]) return false;
  }
  if (plan.passCount == 0 || plan.passCount > kMaxPasses) return false;
  for (uint8_t p = 0; p < plan.passCount; ++p) {
    const uint8_t count = plan.passes[p].planeCount;
    if (count == 0 || count > kPlanesPerPass) return false;
  }
  return true;
}

}  // namespace gray_levels
