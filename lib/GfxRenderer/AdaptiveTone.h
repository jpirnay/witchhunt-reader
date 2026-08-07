#pragma once

#include <cstdint>

// --- Adaptive tone mapping ---------------------------------------------------
// Ported from crosspoint-reader PR #2861 by Totofaki (Sichroteph), where the
// algorithm and these constants were developed and hardware-tuned on an X3 panel
// (originally in YACP). The algorithm and its tuning are theirs.
//
// Images whose useful luminance sits in a narrow band lose shadow or highlight
// detail under fixed display-tuned thresholds. This derives black and white points
// from the image's own luminance percentiles and stretches toward them before
// dithering.
//
// The two decoders that use this differ in how they feed it, which is why the
// analysis lives here rather than in either one:
//   - Bitmap (BMP): rewind is a seek, so it can afford a cheap row-subsampled
//     pre-pass over the pixel data.
//   - PngToFramebufferConverter: inflate is sequential with no rewind, so the
//     histogram costs a full extra decode. Subsampling saves only the per-pixel
//     histogram work there, not the decode itself.
//
// Both paths share the constants, the percentile search, and the mapping, so a
// tuning change applies uniformly.
namespace adaptive_tone {

// Black/white points come from the 1st and 99th luminance percentiles, which are
// robust against a handful of stray extreme pixels in a way min/max is not.
inline constexpr uint32_t LOW_PERCENTILE_PERMILLE = 10;
inline constexpr uint32_t HIGH_PERCENTILE_PERMILLE = 990;
// Below this spread the image already uses most of the range, so a stretch would
// amplify noise for no visible gain -- fall back to the untouched renderer.
inline constexpr int MIN_RANGE = 96;
// Correction strength, applied as a blend between the original and fully-stretched
// luminance. 3/4 is upstream's X3-tuned value, confirmed on both X3 and X4.
inline constexpr int BLEND_NUM = 3;
inline constexpr int BLEND_DEN = 4;
// Never darken near-whites: on e-ink, pulling paper-white down to a grey level is
// far more noticeable than the highlight detail it would recover.
inline constexpr int HIGHLIGHT_FLOOR = 242;
// Sample every Nth row where the source allows skipping (see the note above).
inline constexpr int ROW_STEP = 4;

// Black/white points derived from a luminance histogram. `active` is false when the
// analysis declined -- too narrow a range, or the caller never ran it -- in which
// case apply() is the identity and the caller's existing renderer is untouched.
struct Points {
  bool active = false;
  uint8_t blackPoint = 0;
  uint8_t whitePoint = 255;
};

// Derives black/white points from a 256-bin luminance histogram.
// `sampleCount` is the number of pixels accumulated into it.
// Returns an inactive result when the useful range is too narrow to be worth
// stretching, which callers should treat as "render exactly as before".
inline Points derivePoints(const uint32_t* histogram, uint64_t sampleCount) {
  Points points;
  if (!histogram || sampleCount == 0) return points;

  const uint64_t lowTarget = (sampleCount * LOW_PERCENTILE_PERMILLE + 999u) / 1000u;
  const uint64_t highTarget = (sampleCount * HIGH_PERCENTILE_PERMILLE + 999u) / 1000u;
  uint64_t cumulative = 0;
  uint8_t low = 0;
  uint8_t high = 255;
  bool foundLow = false;
  for (int i = 0; i < 256; i++) {
    cumulative += histogram[i];
    if (!foundLow && cumulative >= lowTarget) {
      low = static_cast<uint8_t>(i);
      foundLow = true;
    }
    if (cumulative >= highTarget) {
      high = static_cast<uint8_t>(i);
      break;
    }
  }

  if (static_cast<int>(high) - static_cast<int>(low) < MIN_RANGE) return points;

  points.active = true;
  points.blackPoint = low;
  points.whitePoint = high;
  return points;
}

// Maps one luminance through the correction. Identity when `points` is inactive.
inline uint8_t apply(const Points& points, const uint8_t luminance) {
  if (!points.active || points.whitePoint <= points.blackPoint) return luminance;

  int leveled;
  if (luminance <= points.blackPoint) {
    leveled = 0;
  } else if (luminance >= points.whitePoint) {
    leveled = 255;
  } else {
    leveled = ((static_cast<int>(luminance) - points.blackPoint) * 255) /
              (static_cast<int>(points.whitePoint) - points.blackPoint);
  }

  int adjusted = (static_cast<int>(luminance) * (BLEND_DEN - BLEND_NUM) + leveled * BLEND_NUM) / BLEND_DEN;
  if (luminance > HIGHLIGHT_FLOOR && adjusted < luminance) adjusted = luminance;
  if (adjusted < 0) return 0;
  if (adjusted > 255) return 255;
  return static_cast<uint8_t>(adjusted);
}

}  // namespace adaptive_tone
