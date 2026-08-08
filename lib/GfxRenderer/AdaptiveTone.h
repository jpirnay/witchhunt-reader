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

// --- Line-art rejection ------------------------------------------------------
// The MIN_RANGE guard above is a percentile spread, so it does NOT catch line art:
// a pure black-on-white drawing puts the 1st percentile at 0 and the 99th at 255,
// a spread of 255, and sails through. But such an image has no tones to stretch --
// every pixel is already ink or paper -- so the correction only moves the dither
// threshold around and can thicken or break up strokes.
//
// The distinguishing property is not how many levels are occupied but WHERE the mass
// sits: line art puts nearly every pixel at the two extremes and leaves the middle
// empty. Measuring that directly is robust to two things a distinct-level count gets
// wrong -- JPEG ringing around hard edges (which smears ink and paper across dozens of
// near-black/near-white bins, so bilevel art can touch 50+ levels), and smooth tonal
// artwork like pencil shading (where a real ramp spreads so thinly that no single
// level looks individually significant).
//
// Bins within this distance of 0/255 count as ink/paper rather than tone.
inline constexpr int EXTREME_MARGIN = 24;
// Minimum share of pixels that must fall between those end zones for the image to be
// treated as tonal. Measured against synthetic distributions: bilevel and screentone
// art land at 0-42 permille, while diagrams with flat fills, engravings, pencil
// shading and photographs all sit at 173 permille or above.
inline constexpr uint32_t MIN_MIDTONE_PERMILLE = 60;

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

  // Line-art / bilevel rejection -- see the constants above. Runs before the
  // percentile search because that search cannot tell an ink-and-paper drawing
  // (0 and 255, spread 255) from a photograph using the full range.
  uint64_t midtoneCount = 0;
  for (int i = EXTREME_MARGIN; i <= 255 - EXTREME_MARGIN; i++) {
    midtoneCount += histogram[i];
  }
  if ((midtoneCount * 1000u) / sampleCount < MIN_MIDTONE_PERMILLE) return points;

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
