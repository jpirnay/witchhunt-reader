#pragma once

#include <algorithm>
#include <cstdint>

// The anti-aliasing of the page a chapter build draws (Background-C's displayBuildPage(): a cold open, a turn into a
// chapter not laid out yet). The build draws it in black and white, and when the build ends on it the page stays on
// the panel as drawn (keepBuildPageOnScreen()). Its gray came only then, as the deferred pass: Picco, 2.53 bench, cold
// open in two passes: page on the panel at 1.46 s, chapter laid out at 2.86 s, gray at 3.22 s. X3, master fbee8fa6f,
// 2026-11-17: page at 3.40 s, chapter laid out at 6.12 s, gray at 6.56 s.
//
// The gray comes while the build goes on wherever its pass can run beside the build (Pass):
//  - Restore: the build leaves the frame buffers alone. That is the S3 boards (HalCapabilities::buildsInPsram()): the
//    build lays out in PSRAM, in a region of its own or in the arena lent in place of the secondary buffer, so the
//    frame on screen stays in the secondary buffer for the pass's restore, the write buffer is free for its planes,
//    and its glyphs take nothing from the heap the build runs on. On a panel that puts a page and its gray up in one
//    push (one-pass text, the T5S3) the gray goes up with the page's own push; elsewhere it comes from the deferred
//    pass, once the page's refresh is over and ahead of the build's next slice.
//  - Redraw: the build holds the secondary buffer as its arena (the C3s' borrowed build). The gray needs neither that
//    buffer nor the heap (docs/future_work/reader-priority-scheme.md, section 4): the pass renders its planes over the
//    write buffer, which holds the page on screen, draws the page there again and hands that to the cleanup. The
//    page, its glyphs and the status bar's pixels are kept in the build's arena for the length of the pass, between
//    two of its slices. The status bar is not drawn again: its clock, battery and page count may have moved since,
//    and the controller's baseline must be the frame on the glass (chromeBands()).
//  - AtBuildEnd: neither, so the gray waits for the build's end. The X4's resident build, which keeps the secondary
//    buffer and runs on the heap its low-heap guard watches: the page and its glyphs would come from that heap.
//
// A page with pictures is drawn again once the build is done, with them and their gray, as before.
//
// The decisions alone, apart from the reader, so the host can test them.
namespace BuildPageGray {

// Where the gray of the page the build drew stands.
enum class Gray : uint8_t {
  None,   // drawn in black and white: the build's end asks for its gray
  Asked,  // its pass was asked for while the build goes on; one that was preempted is asked for again at the build's
          // end, when the page is still the one on screen
  Shown,  // on the panel: that pass ran, or the page went up with its gray in one push
};

// How the page's gray can come while the build goes on (see above).
enum class Pass : uint8_t { AtBuildEnd, Restore, Redraw };

// buildsInPsram: HalCapabilities::buildsInPsram(). keepsPreviousFrame: the frame on screen is still in the secondary
// buffer (false on an S3 only where the PSRAM arena could not be had and the buffer itself was lent).
// buildHoldsSecondary: the live build lays out in the lent secondary buffer, as its arena.
constexpr Pass passFor(const bool buildsInPsram, const bool keepsPreviousFrame, const bool buildHoldsSecondary) {
  if (buildsInPsram) return keepsPreviousFrame ? Pass::Restore : Pass::AtBuildEnd;
  return buildHoldsSecondary ? Pass::Redraw : Pass::AtBuildEnd;
}

// The page's gray comes while the build goes on.
constexpr bool whileBuilding(const Pass pass, const bool antiAliasing, const bool pageHasImages,
                             const bool somethingOverPage) {
  return pass != Pass::AtBuildEnd && antiAliasing && !pageHasImages && !somethingOverPage;
}

// The deferred pass is asked for once a page, once the page's refresh is over: the pass waits for the panel, and the
// build waits for the pass while it is owed. Never while another pass is owed.
constexpr bool asksPassNow(const Gray gray, const bool refreshPending, const bool passOwed) {
  return gray == Gray::None && !refreshPending && !passOwed;
}

// The build's end asks for the page's gray unless it is on the panel already.
constexpr bool owedAtBuildEnd(const Gray gray) { return gray != Gray::Shown; }

// --- Redraw: what is kept of the frame instead of drawn again ----------------------------------------------------

// A rectangle in the reader's frame, as GfxRenderer's region helpers take it.
struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// The status bar's bands, kept as pixels through the pass and put back over the page drawn again: the full width,
// from the screen's edge (the bar fills the bezel, fillMargin) to the bar's inner edge and kChromePad beyond it. Their
// pixels are the frame that was pushed, so a band larger than the bar costs bytes, not the result. A side with no bar
// (height 0) has no band.
constexpr int kChromePad = 8;
struct ChromeBands {
  Rect top;
  Rect bottom;
};
constexpr ChromeBands chromeBands(const int screenWidth, const int screenHeight, const int bezelTop, const int barTop,
                                  const int bezelBottom, const int barBottom) {
  ChromeBands bands;
  if (barTop > 0) bands.top = {0, 0, screenWidth, std::min(screenHeight, bezelTop + barTop + kChromePad)};
  if (barBottom > 0) {
    const int height = std::min(screenHeight, bezelBottom + barBottom + kChromePad);
    bands.bottom = {0, screenHeight - height, screenWidth, height};
  }
  return bands;
}

}  // namespace BuildPageGray
