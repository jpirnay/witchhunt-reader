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
float tileLightness(const GrayLevelQuantizer& q, const uint8_t* lightness, const uint8_t gray) {
  int sum = 0;
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) sum += lightness[q.ink(gray, x, y)];
  }
  return static_cast<float>(sum) / 64.0f;
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

// A darker gray never comes out lighter, and the tile's average lands where the gray's L*, stretched onto the
// panel's range, asks: within the step a 64-threshold tile can resolve.
TEST(GrayLevelQuantizer, TileAverageFollowsTheTarget) {
  GrayLevelQuantizer q;
  ASSERT_TRUE(q.init(kPiccoLightness, 16));
  const float white = kPiccoLightness[0];
  const float black = kPiccoLightness[15];
  float previous = 1000.0f;
  for (int gray = 255; gray >= 0; --gray) {
    const float mean = tileLightness(q, kPiccoLightness, static_cast<uint8_t>(gray));
    EXPECT_LE(mean, previous + 1e-3f) << "gray " << gray;
    previous = mean;
    const float target = black + (white - black) * GrayLevelQuantizer::lightnessOf(gray) / 100.0f;
    // Widest gap between neighbouring inks is 11 L* (100 -> 89): one threshold of 64 is ~0.2 L*.
    EXPECT_NEAR(mean, target, 0.5f) << "gray " << gray;
  }
}

TEST(GrayLevelQuantizer, LightnessOfSrgbGray) {
  EXPECT_NEAR(GrayLevelQuantizer::lightnessOf(0), 0.0f, 1e-4f);
  EXPECT_NEAR(GrayLevelQuantizer::lightnessOf(255), 100.0f, 1e-3f);
  // sRGB 119 is the classic middle gray, L* ~50.
  EXPECT_NEAR(GrayLevelQuantizer::lightnessOf(119), 50.0f, 0.5f);
}

}  // namespace
