#pragma once

#include <GfxRenderer.h>

// SPIKE, not for merge: vertical-rl text laid out as a horizontal page in a frame turned 90 degrees clockwise
// (docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md, section 5.2). The reader sets this
// per book; layout swaps the viewport's width and height, Page::render draws in the turned frame, and
// TextBlock::render draws upright characters through the reader's own frame.
struct VerticalSpike {
  static bool active;
  // The reader's viewport width in its own frame: the content frame's block extent.
  static int readerViewportWidth;

  // The content frame is one 90-degree step counter-clockwise in panel-rotation terms from the reader's
  // (Portrait 90 -> LandscapeCounterClockwise 0, ...), so a content point (x, y) is the reader point
  // (readerWidth - 1 - y, x): inline runs down, block runs leftward.
  static GfxRenderer::Orientation contentOrientation(const GfxRenderer::Orientation reader) {
    switch (reader) {
      case GfxRenderer::Portrait:
        return GfxRenderer::LandscapeCounterClockwise;
      case GfxRenderer::LandscapeClockwise:
        return GfxRenderer::Portrait;
      case GfxRenderer::PortraitInverted:
        return GfxRenderer::LandscapeClockwise;
      case GfxRenderer::LandscapeCounterClockwise:
        return GfxRenderer::PortraitInverted;
    }
    return reader;
  }

  // Set by Page::render while it draws in the content frame.
  static GfxRenderer::Orientation readerOrientation;
  static int readerWidth;
};
