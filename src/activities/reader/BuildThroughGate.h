#pragma once

#include <cstdint>

// Whether Background-B may build the next chapter through page turns (BG_BUILD_THROUGH_PAGES in
// EpubReaderActivity.cpp): the pages of a build-through draw while B holds the framebuffer it borrowed, so the panel
// has to keep its FAST refreshes without that buffer. EpubReaderActivity asks with what its renderer and B's borrow
// say.
namespace BuildThroughGate {

struct Panel {
  bool fastRefreshNeedsHostFrame = false;  // GfxRenderer::fastRefreshNeedsHostFrame(): the X4's RED, not the X3's DTM1
  bool lendsPsramArena = false;            // GfxRenderer::lendsPsramArena(): B's lend is PSRAM, not the framebuffer
};

// The X3 diffs its FAST refresh against its controller's copy of the frame on screen. The X4 diffs against RED, which
// B's borrow seeds from the frame on screen (seedsRed) and a build-through then keeps (single-buffer fast
// differential). A board that lends a PSRAM arena keeps its framebuffer and is left out: there a build-through would
// give up AA and the pre-render on its pages for a buffer the display does not lose.
constexpr bool keepsFastRefresh(const Panel& panel) {
  return !panel.fastRefreshNeedsHostFrame || !panel.lendsPsramArena;
}

// B's borrow seeds RED from the secondary buffer: on a panel that needs the host frame, when the lend is that buffer
// and it still holds the frame on screen (FreeInkDisplay::secondaryHoldsFrameOnScreen; after an image decode has lent
// it and given it back, it holds the write buffer's contents).
constexpr bool seedsRed(const Panel& panel, const bool secondaryHoldsFrameOnScreen) {
  return panel.fastRefreshNeedsHostFrame && !panel.lendsPsramArena && secondaryHoldsFrameOnScreen;
}

// B's build may go on through page turns now. Where the panel needs the host frame, RED has to be the frame on screen
// still: seeded by the borrow, with no frame shipped since (framesShipped is HalDisplay::framesShipped()).
constexpr bool mayBegin(const Panel& panel, const bool redSeeded, const uint32_t seedFrame,
                        const uint32_t framesShipped) {
  return keepsFastRefresh(panel) && (!panel.fastRefreshNeedsHostFrame || (redSeeded && framesShipped == seedFrame));
}

}  // namespace BuildThroughGate
