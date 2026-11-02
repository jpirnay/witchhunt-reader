#pragma once

#include <GfxRenderer.h>

#include "Page.h"
#include "TurnedFrame.h"

// The turned frame's orientation numbers are the renderer's.
static_assert(turnedframe::Portrait == GfxRenderer::Portrait);
static_assert(turnedframe::LandscapeClockwise == GfxRenderer::LandscapeClockwise);
static_assert(turnedframe::PortraitInverted == GfxRenderer::PortraitInverted);
static_assert(turnedframe::LandscapeCounterClockwise == GfxRenderer::LandscapeCounterClockwise);

// A vertical page's draw scope: the reader's offsets become the content frame's, the renderer draws in that frame, and
// the orientation found on entry is back on every exit. A horizontal page takes none of it.
class PageFrameScope {
 public:
  PageFrameScope(const GfxRenderer& renderer, const Page& page, int& xOffset, int& yOffset)
      : renderer_(renderer), entry_(renderer.getOrientation()), turned_(page.isVertical()) {
    if (!turned_) return;
    const GfxRenderer::Orientation reader = renderer.getHeldOrientation();
    const turnedframe::Offsets offsets =
        turnedframe::contentOffsets(renderer.getScreenWidth(reader), xOffset, yOffset, page.verticalBlockExtent);
    xOffset = offsets.x;
    yOffset = offsets.y;
    renderer.setDrawOrientation(static_cast<GfxRenderer::Orientation>(turnedframe::contentOf(reader)));
  }
  ~PageFrameScope() {
    if (turned_) renderer_.setDrawOrientation(entry_);
  }
  PageFrameScope(const PageFrameScope&) = delete;
  PageFrameScope& operator=(const PageFrameScope&) = delete;

 private:
  const GfxRenderer& renderer_;
  GfxRenderer::Orientation entry_;
  bool turned_;
};
