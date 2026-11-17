#pragma once

#include <cstdint>

// The anti-aliasing of the page a chapter build draws (Background-C's displayBuildPage(): a cold open, a turn into a
// chapter not laid out yet). The build draws it in black and white, and when the build ends on it the page stays on
// the panel as drawn (keepBuildPageOnScreen()). Its gray came only then, as the deferred pass: Picco, 2.53 bench, cold
// open in two passes: page on the panel at 1.46 s, chapter laid out at 2.86 s, gray at 3.22 s.
//
// Where the build leaves the frame buffers alone, the gray comes while the build goes on: on a panel that puts a page
// and its gray up in one push, with the page's own push; elsewhere from the deferred pass, once the page's refresh is
// over and ahead of the build's next slice. That is the S3 boards (HalCapabilities::buildsInPsram()): the build
// lays out in PSRAM, in a region of its own or in the arena lent in place of the secondary buffer, so the frame on
// screen stays in the secondary buffer for the pass's restore, the write buffer is free for its planes, and its glyphs
// take nothing from the heap the build runs on. Not the C3s: the X3's build holds the secondary buffer itself, and the
// X4's resident build runs on a heap its low-heap guard watches.
//
// A page with pictures is drawn again once the build is done, with them and their gray, as before.
//
// The decisions alone, apart from the reader, so the host can test them.
namespace BuildPageGray {

// Where the gray of the page the build drew stands.
enum class Gray : uint8_t {
  None,   // drawn in black and white: the build's end asks for its gray
  Asked,  // the deferred pass was asked for while the build goes on; one that was preempted is asked for again at the
          // build's end, when the page is still the one on screen
  Shown,  // on the panel: that pass ran, or the page went up with its gray in one push
};

// The page's gray comes while the build goes on.
constexpr bool whileBuilding(const bool buildLeavesFramesAlone, const bool antiAliasing, const bool pageHasImages,
                             const bool somethingOverPage) {
  return buildLeavesFramesAlone && antiAliasing && !pageHasImages && !somethingOverPage;
}

// The deferred pass is asked for once a page, once the page's refresh is over: the pass waits for the panel, and the
// build waits for the pass while it is owed. Never while another pass is owed.
constexpr bool asksPassNow(const Gray gray, const bool refreshPending, const bool passOwed) {
  return gray == Gray::None && !refreshPending && !passOwed;
}

// The build's end asks for the page's gray unless it is on the panel already.
constexpr bool owedAtBuildEnd(const Gray gray) { return gray != Gray::Shown; }

}  // namespace BuildPageGray
