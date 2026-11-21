// The pieces a gray-level plan's sleep picture is drawn with (GrayLevelPlan.h, GrayLevelQuantizer.h): which bit a
// plane gives each ink, which plans the sleep screen will run, and how the quantizer spreads a gray over a panel's
// measured inks. The picture is drawn once per plane, so the quantizer must give a pixel the same ink every time
// and must place the inks so that their average lands where the gray asks.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "GrayLevelPlan.h"
#include "GrayLevelQuantizer.h"

namespace {

// Free-Ink #223's calibrated L* per level on the Picco, ink 0 (white) first.
constexpr uint8_t kPiccoLightness[16] = {100, 89, 86, 82, 77, 72, 69, 66, 62, 55, 52, 49, 45, 40, 34, 29};

gray_levels::Plan sixteenLevelPlan() {
  gray_levels::Plan plan;
  plan.levels = 16;
  std::copy(std::begin(kPiccoLightness), std::end(kPiccoLightness), plan.lightness);
  plan.base = {0xFF00u, true};  // #220's kMaskBwClear: inks 8..15 black
  plan.passes[0] = {{{0x24EEu, false}, {0x3756u, false}}, 2};
  plan.passes[1] = {{{0x71DAu, false}, {0x6D0Au, false}}, 2};
  plan.passCount = 2;
  return plan;
}

// Mean L* the inks of one 8x8 dither tile land at.
double tileLightness(const GrayLevelQuantizer& q, const uint8_t* lightness, const uint8_t gray) {
  int sum = 0;
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) sum += lightness[q.ink(gray, x, y)];
  }
  return sum / 64.0;
}

// CIE L* (0..100) of an sRGB gray, in double on the host: what the quantizer's integer table must hold.
double referenceLightness(const int gray) {
  const double c = gray / 255.0;
  const double y = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  return y > 216.0 / 24389.0 ? 116.0 * std::cbrt(y) - 16.0 : 24389.0 / 27.0 * y;
}

TEST(GrayLevelPlane, MaskBitPerInkAndInversion) {
  const gray_levels::Plane bleach{0x0005u, false};
  EXPECT_TRUE(bleach.bitFor(0));
  EXPECT_FALSE(bleach.bitFor(1));
  EXPECT_TRUE(bleach.bitFor(2));
  EXPECT_FALSE(bleach.bitFor(15));

  // #220 writes its base as cleared inks: kMaskBwClear leaves 0..7 white (bit 1) and makes 8..15 black (bit 0).
  const gray_levels::Plane base{0xFF00u, true};
  for (uint8_t ink = 0; ink < 8; ++ink) EXPECT_TRUE(base.bitFor(ink)) << int(ink);
  for (uint8_t ink = 8; ink < 16; ++ink) EXPECT_FALSE(base.bitFor(ink)) << int(ink);
}

TEST(GrayLevelPlan, SixteenLevelsInTwoPassesIsUsable) { EXPECT_TRUE(gray_levels::isUsable(sixteenLevelPlan())); }

TEST(GrayLevelPlan, RejectsWhatTheSleepScreenCannotRun) {
  gray_levels::Plan plan = sixteenLevelPlan();
  plan.levels = 4;  // the plane path already shows four
  EXPECT_FALSE(gray_levels::isUsable(plan));

  plan = sixteenLevelPlan();
  plan.lightness[9] = plan.lightness[8];  // not strictly falling
  EXPECT_FALSE(gray_levels::isUsable(plan));

  plan = sixteenLevelPlan();
  plan.passCount = 0;
  EXPECT_FALSE(gray_levels::isUsable(plan));

  plan = sixteenLevelPlan();
  plan.passCount = gray_levels::kMaxPasses + 1;
  EXPECT_FALSE(gray_levels::isUsable(plan));

  plan = sixteenLevelPlan();
  plan.passes[1].planeCount = 0;
  EXPECT_FALSE(gray_levels::isUsable(plan));

  plan = sixteenLevelPlan();
  plan.passes[0].planeCount = gray_levels::kPlanesPerPass + 1;
  EXPECT_FALSE(gray_levels::isUsable(plan));
}

TEST(GrayLevelQuantizer, RejectsTablesThatDoNotFall) {
  GrayLevelQuantizer q;
  EXPECT_FALSE(q.init(nullptr, 16));
  EXPECT_FALSE(q.init(kPiccoLightness, 1));
  EXPECT_FALSE(q.init(kPiccoLightness, 17));
  const uint8_t rising[3] = {20, 50, 90};
  EXPECT_FALSE(q.init(rising, 3));
  const uint8_t flat[3] = {90, 50, 50};
  EXPECT_FALSE(q.init(flat, 3));
  EXPECT_EQ(q.levels(), 0);
  ASSERT_TRUE(q.init(kPiccoLightness, 16));
  EXPECT_EQ(q.levels(), 16);
  EXPECT_EQ(q.darkest(), 15);
}

TEST(GrayLevelQuantizer, WhiteAndBlackAreSolid) {
  GrayLevelQuantizer q;
  ASSERT_TRUE(q.init(kPiccoLightness, 16));
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) {
      EXPECT_EQ(q.ink(255, x, y), 0) << x << "," << y;
      EXPECT_EQ(q.ink(0, x, y), 15) << x << "," << y;
    }
  }
}

// Only position decides between the two inks, so the same pixel gets the same ink on every draw, and the pattern
// repeats every 8 pixels.
TEST(GrayLevelQuantizer, InkDependsOnlyOnGrayAndPosition) {
  GrayLevelQuantizer q;
  ASSERT_TRUE(q.init(kPiccoLightness, 16));
  for (int gray = 0; gray < 256; gray += 5) {
    for (unsigned y = 0; y < 8; ++y) {
      for (unsigned x = 0; x < 8; ++x) {
        const uint8_t ink = q.ink(static_cast<uint8_t>(gray), x, y);
        EXPECT_EQ(q.ink(static_cast<uint8_t>(gray), x + 8, y + 16), ink);
        EXPECT_EQ(q.ink(static_cast<uint8_t>(gray), x + 792, y + 528), ink);
      }
    }
  }
}

// Each pixel takes one of the two inks that bracket its gray, never one further away.
TEST(GrayLevelQuantizer, UsesOnlyNeighbouringInks) {
  GrayLevelQuantizer q;
  ASSERT_TRUE(q.init(kPiccoLightness, 16));
  for (int gray = 0; gray < 256; ++gray) {
    uint8_t lo = 255, hi = 0;
    for (unsigned y = 0; y < 8; ++y) {
      for (unsigned x = 0; x < 8; ++x) {
        const uint8_t ink = q.ink(static_cast<uint8_t>(gray), x, y);
        lo = std::min(lo, ink);
        hi = std::max(hi, ink);
      }
    }
    EXPECT_LE(hi - lo, 1) << "gray " << gray;
  }
}

// A darker gray never comes out lighter, and the tile's average lands where the gray's exact L*, stretched onto the
// panel's range, asks: within one of the tile's 64 thresholds of the widest gap between neighbouring inks. The
// integer setup rounds twice (target and share), which this bound has room for even with inks one L* apart.
TEST(GrayLevelQuantizer, TileAverageFollowsTheTarget) {
  constexpr uint8_t kWide[4] = {95, 70, 40, 12};
  constexpr uint8_t kCrowded[16] = {100, 99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, 87, 86, 85};
  struct Inks {
    const uint8_t* lightness;
    uint8_t levels;
  };
  for (const Inks& inks : {Inks{kPiccoLightness, 16}, Inks{kWide, 4}, Inks{kCrowded, 16}}) {
    GrayLevelQuantizer q;
    ASSERT_TRUE(q.init(inks.lightness, inks.levels));
    const double white = inks.lightness[0];
    const double black = inks.lightness[inks.levels - 1];
    int widest = 0;
    for (uint8_t i = 1; i < inks.levels; ++i) widest = std::max(widest, inks.lightness[i - 1] - inks.lightness[i]);
    const double tolerance = widest / 64.0 + 0.02;
    double previous = 1000.0;
    for (int gray = 255; gray >= 0; --gray) {
      const double mean = tileLightness(q, inks.lightness, static_cast<uint8_t>(gray));
      EXPECT_LE(mean, previous + 1e-9) << int(inks.levels) << " levels, gray " << gray;
      previous = mean;
      const double target = black + (white - black) * referenceLightness(gray) / 100.0;
      EXPECT_NEAR(mean, target, tolerance) << int(inks.levels) << " levels, gray " << gray;
    }
  }
}

TEST(GrayLevelQuantizer, LightnessTableMatchesTheFormula) {
  for (int gray = 0; gray < 256; ++gray) {
    EXPECT_EQ(GrayLevelQuantizer::lightnessCenti(static_cast<uint8_t>(gray)),
              std::lround(referenceLightness(gray) * 100.0))
        << "gray " << gray;
  }
  EXPECT_EQ(GrayLevelQuantizer::lightnessCenti(0), 0);
  EXPECT_EQ(GrayLevelQuantizer::lightnessCenti(255), 10000);
  // sRGB 119 is the classic middle gray, L* ~50.
  EXPECT_NEAR(GrayLevelQuantizer::lightnessCenti(119), 5000, 50);
}

}  // namespace
