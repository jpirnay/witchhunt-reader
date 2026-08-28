// Host tests for the sleep-screen / inline-image tone curves.
//
// derivePoints() had no coverage despite its constants being documented as tuned
// against synthetic distributions. These rebuild those distributions so a future
// tuning change has to state what it is breaking.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <numeric>

#include "AdaptiveTone.h"

namespace {

using adaptive_tone::Mode;
using adaptive_tone::Points;

using Histogram = std::array<uint32_t, 256>;

uint64_t total(const Histogram& h) { return std::accumulate(h.begin(), h.end(), uint64_t{0}); }

Points derive(const Histogram& h, const Mode mode = Mode::Stretch) {
  return adaptive_tone::derivePoints(h.data(), total(h), mode);
}

// Flat mass across [lo, hi], nothing outside it.
Histogram band(const int lo, const int hi, const uint32_t perLevel = 100) {
  Histogram h{};
  for (int i = lo; i <= hi; i++) h[static_cast<size_t>(i)] = perLevel;
  return h;
}

// The original, pre-LUT formulation of the stretch, kept verbatim as the oracle for
// the baked curve. If someone changes the curve maths, this test should be updated
// deliberately -- not silently satisfied.
uint8_t referenceStretch(const Points& points, const uint8_t luminance) {
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
  int adjusted = (static_cast<int>(luminance) * (adaptive_tone::BLEND_DEN - adaptive_tone::BLEND_NUM) +
                  leveled * adaptive_tone::BLEND_NUM) /
                 adaptive_tone::BLEND_DEN;
  if (luminance > adaptive_tone::HIGHLIGHT_FLOOR && adjusted < luminance) adjusted = luminance;
  if (adjusted < 0) return 0;
  if (adjusted > 255) return 255;
  return static_cast<uint8_t>(adjusted);
}

// --- shared guards ----------------------------------------------------------

TEST(AdaptiveTone, DeclinesWithoutAHistogram) {
  EXPECT_FALSE(adaptive_tone::derivePoints(nullptr, 1000).active);
  const Histogram h = band(0, 255);
  EXPECT_FALSE(adaptive_tone::derivePoints(h.data(), 0).active);
}

TEST(AdaptiveTone, SupersededPointsFallBackToTheIdentity) {
  // The curve lives in one shared static, so a Points from an earlier analysis must not
  // read a later image's curve. It degrades to untoned -- the same way every other
  // failure in this feature degrades -- rather than levelling one image with another's
  // numbers. Only one curve is ever live in the firmware; this is the guard on that.
  // Both bands must actually move 128, or the test could not tell a superseded curve
  // from a live one. A band flat across the whole range would not -- see
  // AdaptiveToneEqualize.LeavesAnAlreadyUniformHistogramAlone.
  const Points first = derive(band(60, 190), Mode::Equalize);
  ASSERT_TRUE(first.active);
  const uint8_t firstAt128 = adaptive_tone::apply(first, 128);
  ASSERT_NE(firstAt128, 128) << "fixture must produce a curve that moves 128";

  const Points second = derive(band(40, 200), Mode::Equalize);
  ASSERT_TRUE(second.active);
  ASSERT_NE(adaptive_tone::apply(second, 128), 128);

  EXPECT_EQ(adaptive_tone::apply(first, 128), 128) << "superseded curve must read as identity";
}

TEST(AdaptiveTone, DeclinedAnalysisLeavesTheLiveCurveAlone) {
  // A declined analysis must not consume a generation, or it would silently disable a
  // perfectly good curve derived just before it.
  const Points good = derive(band(0, 255), Mode::Stretch);
  ASSERT_TRUE(good.active);
  const uint8_t before = adaptive_tone::apply(good, 100);

  ASSERT_FALSE(derive(band(100, 150), Mode::Stretch).active);  // below MIN_RANGE
  EXPECT_EQ(adaptive_tone::apply(good, 100), before);
}

TEST(AdaptiveTone, InactivePointsAreTheIdentity) {
  const Points inactive;
  for (int i = 0; i < 256; i++) {
    EXPECT_EQ(adaptive_tone::apply(inactive, static_cast<uint8_t>(i)), i);
  }
}

TEST(AdaptiveTone, RejectsLineArtInBothModes) {
  // Pure ink and paper, plus the JPEG ringing that smears them across the end zones.
  // No tone to stretch or equalize, so both modes must decline and leave the strokes be.
  Histogram h{};
  for (int i = 0; i <= 8; i++) h[static_cast<size_t>(i)] = 4000;
  for (int i = 247; i <= 255; i++) h[static_cast<size_t>(i)] = 40000;
  for (int i = 60; i <= 200; i++) h[static_cast<size_t>(i)] = 5;  // well under MIN_MIDTONE_PERMILLE

  EXPECT_FALSE(derive(h, Mode::Stretch).active);
  EXPECT_FALSE(derive(h, Mode::Equalize).active);
}

TEST(AdaptiveTone, AcceptsTonalArtWithFullRange) {
  // A photograph reaching both extremes: same 0..255 span as line art, but the mass
  // is in the middle, which is the distinction the midtone share exists to draw.
  const Histogram h = band(0, 255);
  EXPECT_TRUE(derive(h, Mode::Stretch).active);
  EXPECT_TRUE(derive(h, Mode::Equalize).active);
}

// --- Mode::Stretch ----------------------------------------------------------

TEST(AdaptiveToneStretch, FindsTailPercentiles) {
  const Points points = derive(band(40, 200));
  ASSERT_TRUE(points.active);
  // 1st/99th of a flat band sit just inside its edges, not at min/max.
  EXPECT_GE(points.blackPoint, 40);
  EXPECT_LE(points.blackPoint, 43);
  EXPECT_GE(points.whitePoint, 197);
  EXPECT_LE(points.whitePoint, 200);
}

TEST(AdaptiveToneStretch, DeclinesBelowMinRange) {
  // A band narrower than MIN_RANGE would need a >2.7x gain; noise wins over detail.
  EXPECT_FALSE(derive(band(100, 150)).active);
  // Just over the threshold is accepted.
  EXPECT_TRUE(derive(band(60, 190)).active);
}

TEST(AdaptiveToneStretch, BakedCurveMatchesTheOriginalArithmetic) {
  // The curve was moved into a LUT to get the multiply/divide out of the dither inner
  // loop. That must be a pure refactor: every input maps exactly as it did before.
  for (const auto& h : {band(40, 200), band(0, 255), band(10, 240), band(80, 250)}) {
    const Points points = derive(h);
    if (!points.active) continue;
    for (int i = 0; i < 256; i++) {
      const auto lum = static_cast<uint8_t>(i);
      EXPECT_EQ(adaptive_tone::apply(points, lum), referenceStretch(points, lum))
          << "black=" << +points.blackPoint << " white=" << +points.whitePoint << " lum=" << i;
    }
  }
}

TEST(AdaptiveToneStretch, NeverDarkensNearWhites) {
  // Paper-white pulled down to a grey level is far more visible on e-ink than the
  // highlight detail it would buy back.
  const Points points = derive(band(0, 255));
  ASSERT_TRUE(points.active);
  for (int i = adaptive_tone::HIGHLIGHT_FLOOR + 1; i < 256; i++) {
    EXPECT_GE(adaptive_tone::apply(points, static_cast<uint8_t>(i)), i) << "lum=" << i;
  }
}

TEST(AdaptiveToneStretch, IsNearlyANoOpOnAnImageAlreadySpanningTheRange) {
  // The case that made adaptive sleep covers look flat: a dark scene with a small bright
  // element pins p99 near 255, so the gain is ~1.0 and the curve mostly just subtracts
  // the black point. Documented here as known behaviour -- it is what Equalize addresses.
  Histogram h{};
  for (int i = 27; i <= 90; i++) h[static_cast<size_t>(i)] = 1000;  // ~90% of the mass
  for (int i = 248; i <= 253; i++) h[static_cast<size_t>(i)] = 1200;

  const Points points = derive(h);
  ASSERT_TRUE(points.active);
  const int span = points.whitePoint - points.blackPoint;
  EXPECT_GT(span, 200) << "endpoints should already be far apart";

  // The dominant band is pushed down, not spread out.
  EXPECT_LT(adaptive_tone::apply(points, 68), 68);
  const int spreadBefore = 90 - 27;
  const int spreadAfter = adaptive_tone::apply(points, 90) - adaptive_tone::apply(points, 27);
  EXPECT_LT(spreadAfter, spreadBefore * 5 / 4) << "stretch buys almost no extra separation here";
}

// --- Mode::Equalize ---------------------------------------------------------

TEST(AdaptiveToneEqualize, SpreadsTheDominantBand) {
  // Same bimodal histogram as above. Equalization allocates output range by pixel mass,
  // so the band carrying 90% of the image gets most of the range.
  Histogram h{};
  for (int i = 27; i <= 90; i++) h[static_cast<size_t>(i)] = 1000;
  for (int i = 248; i <= 253; i++) h[static_cast<size_t>(i)] = 1200;

  const Points points = derive(h, Mode::Equalize);
  ASSERT_TRUE(points.active);

  const int spreadBefore = 90 - 27;
  const int spreadAfter = adaptive_tone::apply(points, 90) - adaptive_tone::apply(points, 27);
  EXPECT_GT(spreadAfter, spreadBefore) << "the band carrying the image must gain separation";
}

TEST(AdaptiveToneEqualize, HasNoMinRangeGate) {
  // A narrow band is exactly what equalization is for, so unlike Stretch it must not
  // decline one. (Line art is already turned away before the mode split.)
  EXPECT_FALSE(derive(band(100, 150), Mode::Stretch).active);
  EXPECT_TRUE(derive(band(100, 150), Mode::Equalize).active);
}

TEST(AdaptiveToneEqualize, IsMonotonic) {
  // A tone curve that ever reverses would invert local contrast -- edges would flip.
  Histogram h{};
  for (int i = 0; i < 256; i++) h[static_cast<size_t>(i)] = static_cast<uint32_t>(i % 17) + 1;
  const Points points = derive(h, Mode::Equalize);
  ASSERT_TRUE(points.active);
  for (int i = 1; i < 256; i++) {
    EXPECT_GE(adaptive_tone::apply(points, static_cast<uint8_t>(i)),
              adaptive_tone::apply(points, static_cast<uint8_t>(i - 1)))
        << "lum=" << i;
  }
}

TEST(AdaptiveToneEqualize, NeverDarkensNearWhites) {
  // The highlight floor is a panel property, so it binds in this mode too.
  Histogram h{};
  for (int i = 20; i <= 120; i++) h[static_cast<size_t>(i)] = 900;
  for (int i = 121; i <= 255; i++) h[static_cast<size_t>(i)] = 30;
  const Points points = derive(h, Mode::Equalize);
  ASSERT_TRUE(points.active);
  for (int i = adaptive_tone::HIGHLIGHT_FLOOR + 1; i < 256; i++) {
    EXPECT_GE(adaptive_tone::apply(points, static_cast<uint8_t>(i)), i) << "lum=" << i;
  }
}

TEST(AdaptiveToneEqualize, LeavesAnAlreadyUniformHistogramAlone) {
  // A flat histogram is what equalization aims at, so its CDF is already the ramp and the
  // curve must come out as the identity. If this ever drifts, every image is being pushed
  // around by a rounding bias rather than by its own tonal distribution.
  const Points points = derive(band(0, 255), Mode::Equalize);
  ASSERT_TRUE(points.active);
  for (int i = 0; i < 256; i++) {
    EXPECT_EQ(adaptive_tone::apply(points, static_cast<uint8_t>(i)), i) << "lum=" << i;
  }
}

TEST(AdaptiveToneEqualize, ReachesFullWhiteAtTheTop) {
  // The CDF is evaluated at the top of each bin, so the brightest occupied level maps to
  // 255 before blending. Sampling the CDF below the bin instead is the classic off-by-one
  // that leaves equalized images a level too dark everywhere.
  const Histogram h = band(0, 255);
  const Points points = derive(h, Mode::Equalize);
  ASSERT_TRUE(points.active);
  EXPECT_EQ(adaptive_tone::apply(points, 255), 255);
}

}  // namespace
