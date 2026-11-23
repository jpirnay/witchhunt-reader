// Where a custom sleep BMP lands (SleepImagePlacement.h), checked by what GfxRenderer::drawBitmap()
// makes of the placement: it shows the centred visible part, scales it down to the bounds but never up,
// and clips at the screen's edges.
//
// The bug this pins (reported against CrossInk as its #925): in Crop mode an image larger than the
// screen one way and smaller the other was cut to the screen's shape on the assumption that it would
// then be scaled up to fill it. drawBitmap does not scale up, so it came out unscaled in the top-left
// corner -- an X4-sized 480x800 image on the X3's 528x792 as 480x720 at (0, 0).

#include <gtest/gtest.h>

#include <algorithm>

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

// The screen pixels drawBitmap(bitmap, p.x, p.y, maxWidth, maxHeight, visibleWidth, visibleHeight) covers.
Rect drawn(const int w, const int h, const int pageWidth, const int pageHeight, const Placement& p) {
  const BoxCut& d = p.draw;
  const int shownW = (d.visibleWidth > 0 && d.visibleWidth < w) ? d.visibleWidth : w;
  const int shownH = (d.visibleHeight > 0 && d.visibleHeight < h) ? d.visibleHeight : h;
  ImageScale fit;
  if (d.maxWidth > 0) fit = ImageScale(d.maxWidth, shownW);
  if (d.maxHeight > 0)
    fit = d.maxWidth > 0 ? ImageScale::smaller(fit, ImageScale(d.maxHeight, shownH)) : ImageScale(d.maxHeight, shownH);
  Rect r;
  r.scaled = fit.isDownscale();
  const ImageScale scale = r.scaled ? fit : ImageScale();
  // Each source pixel's offset is scaled and floored: the last lands at floor((n - 1) * scale).
  const int x1 = std::min(p.x + scale.extent(shownW), pageWidth);
  const int y1 = std::min(p.y + scale.extent(shownH), pageHeight);
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

// Shapes that do not divide evenly: the cut rounds up and only the uncut side bounds the scale, so the
// image covers the screen to the last pixel, and no further.
TEST(SleepImagePlacement, CropCoversTheScreenExactlyWhateverTheShape) {
  for (const int w : {529, 600, 601, 777, 1001, 1201, 1999}) {
    for (const int h : {793, 800, 901, 1203, 1600, 2401}) {
      const Placement p = place(w, h, kX3W, kX3H, true);
      const Rect r = drawn(w, h, kX3W, kX3H, p);
      EXPECT_EQ(r, (Rect{0, 0, kX3W, kX3H})) << w << "x" << h;
      // Not clipped by the screen either: the scaled cut ends on its last column and row.
      const BoxCut& d = p.draw;
      const int shownW = d.visibleWidth > 0 ? d.visibleWidth : w;
      const int shownH = d.visibleHeight > 0 ? d.visibleHeight : h;
      const ImageScale scale = d.maxWidth > 0 ? ImageScale(d.maxWidth, shownW) : ImageScale(d.maxHeight, shownH);
      EXPECT_EQ(scale.extent(shownW), kX3W) << w << "x" << h;
      EXPECT_EQ(scale.extent(shownH), kX3H) << w << "x" << h;
    }
  }
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
