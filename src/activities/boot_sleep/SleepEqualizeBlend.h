#pragma once

#include <AdaptiveTone.h>

// How strongly Equalize spreads a sleep image's tones, by the levels the image actually reaches the panel at. Free of
// Arduino so the host can test it (test/sleep_equalize_blend).
//
// The conservative strength exists because a four-level target breaks a flattened gradient into dither banding; a
// target that resolves more has the range to use a stronger curve, and measurably wants one
// (adaptive_tone::EQ_BLEND_NUM_DEEP). That is the render path's to say, not the panel's: the X4 Pro with UC8179 and the
// Picco report 16 levels, but a sleep PNG still reaches them through the framebuffer's planes at four, where the deep
// curve banded.
namespace SleepEqualizeBlend {

constexpr int forLevels(const int drawnLevels) {
  return drawnLevels > 4 ? adaptive_tone::EQ_BLEND_NUM_DEEP : adaptive_tone::EQ_BLEND_NUM;
}

// A sleep BMP is painted into the panel's 8-bit canvas wherever the panel resolves more than four levels, and drawn as
// a gray-level plan's passes where the panel has a plan instead (SleepActivity's renderBitmapSleepScreen).
constexpr int forBmp(const int panelLevels, const bool hasLevelPlan) {
  return hasLevelPlan ? adaptive_tone::EQ_BLEND_NUM_DEEP : forLevels(panelLevels);
}

// A sleep PNG reaches the canvas only in a build whose PNG decoder paints it (HalDisplay::kMayLendGray8Canvas, the
// T5S3). Everywhere else it is dithered into the framebuffer's planes, four levels, however many the panel has.
constexpr int forPng(const bool decoderPaintsCanvas, const int panelLevels) {
  return forLevels(decoderPaintsCanvas ? panelLevels : 4);
}

}  // namespace SleepEqualizeBlend
