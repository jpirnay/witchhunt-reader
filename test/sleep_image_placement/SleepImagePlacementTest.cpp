// Where a custom sleep BMP lands (SleepImagePlacement.h), checked by what GfxRenderer::drawBitmap()
// makes of the placement: it cuts the crop in whole pixels, scales down to the screen but never up,
// and clips at the screen's edges.
//
// The bug this pins (reported against CrossInk as its #925): in Crop mode an image larger than the
// screen one way and smaller the other was cut to the screen's shape on the assumption that it would
// then be scaled up to fill it. drawBitmap does not scale up, so it came out unscaled in the top-left
// corner -- an X4-sized 480x800 image on the X3's 528x792 as 480x720 at (0, 0).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "activities/boot_sleep/SleepImagePlacement.h"

namespace {

using SleepImagePlacement::place;
using SleepImagePlacement::Placement;

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool scaled = false;
};

bool operator==(const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

std::ostream& operator<<(std::ostream& out, const Rect& r) {
  return out << r.w << "x" << r.h << " at (" << r.x << ", " << r.y << ")" << (r.scaled ? " scaled" : "");
}

// The screen pixels drawBitmap(bitmap, p.x, p.y, pageWidth, pageHeight, p.cropX, p.cropY) covers.
Rect drawn(const int w, const int h, const int pageWidth, const int pageHeight, const Placement& p) {
  const int cropPixX = static_cast<int>(std::floor(static_cast<float>(w) * p.cropX / 2.0f));
  const int cropPixY = static_cast<int>(std::floor(static_cast<float>(h) * p.cropY / 2.0f));
  const float croppedWidth = (1.0f - p.cropX) * static_cast<float>(w);
  const float croppedHeight = (1.0f - p.cropY) * static_cast<float>(h);
  const float fitScale =
      std::min(static_cast<float>(pageWidth) / croppedWidth, static_cast<float>(pageHeight) / croppedHeight);
  Rect r;
  r.scaled = fitScale < 1.0f;
  const int renderWidth = w - 2 * cropPixX;
  const int renderHeight = h - 2 * cropPixY;
  // Each source pixel's offset is scaled and floored: the last lands at floor((n - 1) * scale).
  const auto extent = [&](const int n) {
    return r.scaled ? static_cast<int>(std::floor(static_cast<float>(n - 1) * fitScale)) + 1 : n;
  };
  const int x1 = std::min(p.x + extent(renderWidth), pageWidth);
  const int y1 = std::min(p.y + extent(renderHeight), pageHeight);
  r.x = std::max(p.x, 0);
  r.y = std::max(p.y, 0);
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  return r;
}

Rect drawnFor(const int w, const int h, const int pageWidth, const int pageHeight, const bool crop) {
  return drawn(w, h, pageWidth, pageHeight, place(w, h, pageWidth, pageHeight, crop));
}

constexpr int kX3W = 528;
constexpr int kX3H = 792;
constexpr int kX4W = 480;
constexpr int kX4H = 800;

TEST(SleepImagePlacement, CropCutsOnlyTheOverflowOfATallImageNarrowerThanTheScreen) {
  // The X4's own size on the X3: 8 rows too many, 48 columns too few. Nothing to scale; centred.
  EXPECT_EQ(drawnFor(480, 800, kX3W, kX3H, true), (Rect{24, 0, 480, kX3H}));
}

TEST(SleepImagePlacement, CropCutsOnlyTheOverflowOfAWideImageShorterThanTheScreen) {
  EXPECT_EQ(drawnFor(800, 480, kX3W, kX3H, true), (Rect{0, 156, kX3W, 480}));
}

TEST(SleepImagePlacement, CropOfAMixedImageIsNeverScaled) {
  for (const int extra : {1, 2, 3, 7, 8, 333}) {
    const Rect tall = drawnFor(480, kX3H + extra, kX3W, kX3H, true);
    EXPECT_FALSE(tall.scaled) << "+" << extra;
    EXPECT_EQ(tall, (Rect{24, 0, 480, kX3H})) << "+" << extra;
    const Rect wide = drawnFor(kX3W + extra, 600, kX3W, kX3H, true);
    EXPECT_FALSE(wide.scaled) << "+" << extra;
    EXPECT_EQ(wide, (Rect{0, 96, kX3W, 600})) << "+" << extra;
  }
}

TEST(SleepImagePlacement, CropFillsTheScreenWithAnImageLargerBothWays) {
  EXPECT_EQ(drawnFor(1056, 1584, kX3W, kX3H, true), (Rect{0, 0, kX3W, kX3H}));  // the screen's shape
  EXPECT_EQ(drawnFor(1200, 900, kX3W, kX3H, true), (Rect{0, 0, kX3W, kX3H}));   // wider
  EXPECT_EQ(drawnFor(600, 2000, kX4W, kX4H, true), (Rect{0, 0, kX4W, kX4H}));   // taller
}

TEST(SleepImagePlacement, CropFillsTheScreenWithAnImageAsWideAndTaller) {
  // #4472's case on the X4: cut to the screen's shape top and bottom, nothing to scale.
  EXPECT_EQ(drawnFor(kX4W, 1000, kX4W, kX4H, true), (Rect{0, 0, kX4W, kX4H}));
}

TEST(SleepImagePlacement, FitScalesAMixedImageDownAndCentresIt) {
  const Rect r = drawnFor(480, 800, kX3W, kX3H, false);
  EXPECT_TRUE(r.scaled);
  EXPECT_EQ(r.h, kX3H);
  EXPECT_NEAR(r.x, (kX3W - r.w) / 2, 1);
  EXPECT_NEAR(r.w, 475, 1);
}

TEST(SleepImagePlacement, ASmallImageIsCentredUnscaledInEitherMode) {
  for (const bool crop : {false, true}) {
    EXPECT_EQ(drawnFor(200, 100, kX3W, kX3H, crop), (Rect{164, 346, 200, 100})) << (crop ? "crop" : "fit");
  }
}

}  // namespace
