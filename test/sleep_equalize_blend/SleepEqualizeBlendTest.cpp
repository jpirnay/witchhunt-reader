// The Equalize strength a sleep image gets (SleepEqualizeBlend.h), by the levels its path draws it at.
//
// The bug this pins: since #869 the X4 Pro with UC8179 and the Picco report 16 levels, which their BMP covers use. A
// sleep PNG is still dithered into the framebuffer's planes there, at four levels, but its strength followed the
// panel's 16 and took the deep curve, which bands at four; its .pxc cache kept the banding.
#include <gtest/gtest.h>

#include "activities/boot_sleep/SleepEqualizeBlend.h"

namespace {

using adaptive_tone::EQ_BLEND_NUM;
using adaptive_tone::EQ_BLEND_NUM_DEEP;

static_assert(EQ_BLEND_NUM != EQ_BLEND_NUM_DEEP, "the tests below tell the two strengths apart");

TEST(SleepEqualizeBlend, APngThroughThePlanesGetsTheFourLevelStrength) {
  // The X4 Pro with UC8179 and the Picco: 16 levels for a BMP, but no PNG decoder that paints their canvas.
  EXPECT_EQ(SleepEqualizeBlend::forPng(/*decoderPaintsCanvas=*/false, /*panelLevels=*/16), EQ_BLEND_NUM);
  // The four-level panels.
  EXPECT_EQ(SleepEqualizeBlend::forPng(/*decoderPaintsCanvas=*/false, /*panelLevels=*/4), EQ_BLEND_NUM);
}

TEST(SleepEqualizeBlend, APngOnTheCanvasGetsTheDeepStrength) {
  // The T5S3: its PNG decoder paints the canvas, at the panel's depth.
  EXPECT_EQ(SleepEqualizeBlend::forPng(/*decoderPaintsCanvas=*/true, /*panelLevels=*/16), EQ_BLEND_NUM_DEEP);
  // A canvas build whose panel answers four levels draws the PNG through the planes all the same.
  EXPECT_EQ(SleepEqualizeBlend::forPng(/*decoderPaintsCanvas=*/true, /*panelLevels=*/4), EQ_BLEND_NUM);
}

TEST(SleepEqualizeBlend, ABmpGoesOutAtThePanelsDepth) {
  // The canvas, wherever the panel resolves more than four levels.
  EXPECT_EQ(SleepEqualizeBlend::forBmp(/*panelLevels=*/16, /*hasLevelPlan=*/false), EQ_BLEND_NUM_DEEP);
  // A gray-level plan's passes, on a panel whose canvas has four.
  EXPECT_EQ(SleepEqualizeBlend::forBmp(/*panelLevels=*/4, /*hasLevelPlan=*/true), EQ_BLEND_NUM_DEEP);
  // The planes.
  EXPECT_EQ(SleepEqualizeBlend::forBmp(/*panelLevels=*/4, /*hasLevelPlan=*/false), EQ_BLEND_NUM);
}

}  // namespace
