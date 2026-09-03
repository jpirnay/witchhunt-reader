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
// Below this spread the percentiles sit so close together that stretching them apart
// would need a gain of 2.7x or more, which amplifies sensor noise and JPEG blocking
// far more than it reveals detail -- fall back to the untouched renderer.
// Note this is a floor, not a ceiling: it does NOT catch the opposite case, an image
// whose p1..p99 already span most of the range (gain ~1.0) so that the stretch is
// nearly a no-op. Those are left alone by arithmetic rather than by a gate -- see
// the note on Mode::Stretch below for why that case still looks flat, and what
// Mode::Equalize does about it.
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

// --- Equalize mode --------------------------------------------------------------
// Mode::Stretch anchors on the 1st/99th percentiles, which only helps when the image's
// mass actually spans them. It cannot help a bimodal image: a dark scene with a small
// bright element (title text, a white dress) puts p99 up at ~253 on the strength of a
// few percent of pixels, leaving a gain near 1.0 while 90% of the picture stays
// compressed into a narrow dark band. Measured across seven covers the implied gain
// was 1.03-1.13 on five of them -- effectively a no-op.
//
// Mode::Equalize instead maps luminance through the histogram's own CDF, so the range
// each tone gets is proportional to how many pixels actually carry it. That is exactly
// the "where is the mass" question the endpoint search cannot ask. It costs nothing
// extra: the histogram is already built, and the result is the same 256-entry curve.
// The trade is that flattening a smooth gradient hands it real contrast and therefore
// visible dither texture, which is a matter of taste -- hence a user-selectable mode
// rather than a replacement.
enum class Mode : uint8_t { Stretch, Equalize };

// --- Why the CDF is clipped ------------------------------------------------------
// Proportional-to-mass is the whole point of equalization and also its failure mode: it
// is proportional with no upper bound, so a single dominant tone claims output range in
// proportion to how dominant it is. Book covers are the worst case for that -- a dark
// night scene puts 40% of its pixels in a narrow near-black band, the CDF hands that band
// most of the range, and the black background comes back as mid-grey. Measured on the
// sample covers, unclipped equalization lifted luminance 32 to 82 and 64 to 129 on one
// and 64 to 91, 96 to 151 on another: shadows gone, the image washed out. That is the
// textbook global-histogram-equalization result, not a bug in the CDF.
//
// The textbook answer is the contrast limit from CLAHE: cap what any one bin may
// contribute before integrating, and hand the clipped-off excess back to every bin
// evenly. A spike can then buy at most EQ_CLIP_MULTIPLE times the flat share of the
// output range, and the more an image is dominated by one tone the closer its curve
// falls back to the identity -- which is the right way for this to fail. It stays two
// passes over 256 bins with no extra buffer.
inline constexpr uint64_t EQ_CLIP_MULTIPLE = 2;

// Equalization strength, as a blend against the original luminance. Deliberately gentler
// than Stretch's 3/4: a full CDF map is a much stronger transform, and above 1/4 the
// smooth background gradients on photographic covers break into visible dither banding
// and dark covers still read as lifted even with the clip above holding the curve back.
inline constexpr int EQ_BLEND_NUM = 1;
inline constexpr int EQ_BLEND_DEN = 4;

// ...but "visible dither banding" is a statement about a FOUR-level panel, where
// every tone the curve separates has to be faked out of two rails and two greys.
// A panel that resolves eleven has far more room before a flattened gradient
// breaks up, and correspondingly more to gain: the whole point of equalization is
// to spend output range where the pixels are, and there is only range to spend if
// the panel has levels to spend it on.
//
// Measured on two covers through the T5S3's clean-bank response (11 levels, gamma
// 2.2), raising the blend improves brightness AND local contrast monotonically --
// 1/4 -> 3/4 takes a bimodal cover from 54.5%/0.653 to 58.6%/0.726, where the
// no-filter baseline is 52.4%/0.611. At 1/4 the same cover was reported on
// hardware as "barely different" from no filter at all.
//
// This is a property of the RENDER PATH, not of the device: a deep panel still
// draws in-book images and covers through the dual-plane path, and those want the
// conservative value. So it is a parameter, passed by whoever knows which way the
// image is going out, and not a global set once from the panel.
inline constexpr int EQ_BLEND_NUM_DEEP = 3;

// --- Where the curve lives -------------------------------------------------------
// Both modes bake into a 256-entry lookup table rather than being recomputed per pixel:
// apply() sits in the dither inner loop, so a table read replaces a multiply and a
// divide there. Equalization genuinely needs the full resolution -- a CDF has steps
// wherever the histogram has a spike, and approximating it with an interpolated knot
// table was measured at up to 50 levels of error, which no number of knots fixes.
//
// That table cannot live inside Points. Points is held by RenderConfig, which is a stack
// local in three places (SleepActivity, BmpViewerActivity, ImageBlock), and the image
// decode path runs on the render task -- whose stack abuts the heap top, so an overflow
// spills into the heap and surfaces as a corruption assert elsewhere (see the xTaskCreate
// note in ActivityManager.cpp). The project's stack budget is 256 bytes per frame. Nor
// should it be a heap allocation: a transient few-hundred-byte block taken and returned
// around every image decode is exactly the churn that breaks up the contiguous region a
// later framebuffer realloc needs (docs/memory-allocation-strategy.md rule 4).
//
// So the table is a single static, and Points is a small handle onto it. Every caller
// analyses one image, renders it, and moves on, so only one curve is ever live -- but
// rather than leave that as an unwritten invariant, each analysis takes a new generation
// and apply() checks it. A Points left over from a superseded analysis therefore reads as
// the identity and renders the image untoned, which is how every other failure in this
// feature degrades, instead of silently levelling one image with another's curve.
namespace detail {
inline uint8_t g_curve[256] = {};
inline uint16_t g_generation = 0;
}  // namespace detail

// A derived tone correction. `active` is false when the analysis declined -- line art,
// too narrow a range, or the caller never ran it -- in which case apply() is the identity
// and the caller's existing renderer is untouched.
// blackPoint/whitePoint are the derived percentiles, kept for logging and tests; only
// Mode::Stretch is gated on them.
struct Points {
  bool active = false;
  uint8_t blackPoint = 0;
  uint8_t whitePoint = 255;
  uint16_t generation = 0;
};

// Builds the tone curve from a 256-bin luminance histogram.
// `sampleCount` is the number of pixels accumulated into it.
// Returns an inactive result when the image is line art, or (Stretch only) when the
// useful range is too narrow to be worth stretching. Callers should treat an inactive
// result as "render exactly as before".
// `eqBlendNum` is the equalization strength out of EQ_BLEND_DEN, for Mode::Equalize
// only: EQ_BLEND_NUM for a dual-plane target, EQ_BLEND_NUM_DEEP where the output
// resolves more levels than that. Ignored by Mode::Stretch.
inline Points derivePoints(const uint32_t* histogram, uint64_t sampleCount, Mode mode = Mode::Stretch,
                           int eqBlendNum = EQ_BLEND_NUM) {
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

  // Reported for both modes; only Stretch is gated on them.
  points.blackPoint = low;
  points.whitePoint = high;

  if (mode == Mode::Stretch) {
    if (static_cast<int>(high) - static_cast<int>(low) < MIN_RANGE) return points;

    points.generation = ++detail::g_generation;
    const int span = static_cast<int>(high) - static_cast<int>(low);
    for (int i = 0; i < 256; i++) {
      int leveled;
      if (i <= static_cast<int>(low)) {
        leveled = 0;
      } else if (i >= static_cast<int>(high)) {
        leveled = 255;
      } else {
        leveled = ((i - static_cast<int>(low)) * 255) / span;
      }
      int adjusted = (i * (BLEND_DEN - BLEND_NUM) + leveled * BLEND_NUM) / BLEND_DEN;
      if (i > HIGHLIGHT_FLOOR && adjusted < i) adjusted = i;
      if (adjusted < 0) adjusted = 0;
      if (adjusted > 255) adjusted = 255;
      detail::g_curve[i] = static_cast<uint8_t>(adjusted);
    }
    points.active = true;
    return points;
  }

  // Mode::Equalize. The CDF is evaluated at the TOP of each bin, so a level holding a
  // large share of the pixels is pushed up by its own weight rather than by the weight
  // of everything below it -- the usual off-by-one that leaves equalized images dark.
  // No MIN_RANGE gate: a narrow band is precisely what equalization is for, and the
  // line-art rejection above has already turned away the images that must not be touched.
  //
  // Bin counts are clipped to EQ_CLIP_MULTIPLE of the flat share and the excess shared
  // out evenly before integrating, so no single dominant tone can claim the range -- see
  // the note on EQ_CLIP_MULTIPLE for what that failure looks like on a dark cover.
  const uint64_t clipLimit = (sampleCount * EQ_CLIP_MULTIPLE) / 256u;
  uint64_t excess = 0;
  for (int i = 0; i < 256; i++) {
    if (histogram[i] > clipLimit) excess += histogram[i] - clipLimit;
  }
  const uint64_t shared = excess / 256u;
  uint64_t clippedTotal = 0;
  for (int i = 0; i < 256; i++) {
    clippedTotal += (histogram[i] < clipLimit ? histogram[i] : clipLimit) + shared;
  }
  // Only reachable on a degenerate histogram (fewer samples than bins); leave the caller
  // untoned rather than divide by it.
  if (clippedTotal == 0) return points;

  points.generation = ++detail::g_generation;
  uint64_t running = 0;
  for (int i = 0; i < 256; i++) {
    running += (histogram[i] < clipLimit ? histogram[i] : clipLimit) + shared;
    const int equalized = static_cast<int>((running * 255u) / clippedTotal);
    int adjusted = (i * (EQ_BLEND_DEN - eqBlendNum) + equalized * eqBlendNum) / EQ_BLEND_DEN;
    if (i > HIGHLIGHT_FLOOR && adjusted < i) adjusted = i;
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;
    detail::g_curve[i] = static_cast<uint8_t>(adjusted);
  }
  points.active = true;
  return points;
}

// Maps one luminance through the correction. Identity when `points` is inactive, or when
// a later analysis has taken over the shared curve (see the note on detail::g_curve).
// Called once per pixel from the dither loops, hence the plain table read.
inline uint8_t apply(const Points& points, const uint8_t luminance) {
  if (!points.active || points.generation != detail::g_generation) return luminance;
  return detail::g_curve[luminance];
}

}  // namespace adaptive_tone
