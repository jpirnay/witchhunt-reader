#pragma once

#include <cstdint>

// When CMD:STATE reports a popup as on screen (SerialTestApi, the `popup=` field). Noted when drawPopup() draws it:
// the screen (ActivityManager::activityChangeCount()), the render pass (renderPassCount()) and the frames the panel had
// been sent (HalDisplay::framesShipped()), all before the popup's own ship.
//
// A popup is on screen while its screen is, no later render pass has drawn, and no frame has gone to the panel since
// its own. The last part is new: the screen and the pass alone kept a popup reported for good where the same pass went
// on to put a page up, and nothing drew afterwards. A chapter build that ends while its Indexing popup paints
// (IndexingPopupGate: the build runs through the popup's waveform) is laid out and shown in that very pass, and so is a
// TXT page after its index. Seen on the Murphy: `popup=Indexing` for minutes over a page at rest (2026-11-17).
namespace PopupProbe {

struct Mark {
  uint32_t activity = 0;
  uint32_t renderPass = 0;
  uint32_t frames = 0;
};

// `noted`: the marks when the popup was drawn, before its ship (or since its last own frame, notePopupContinued).
constexpr bool onScreen(const Mark& noted, const Mark& now) {
  return noted.activity == now.activity && noted.renderPass == now.renderPass && now.frames <= noted.frames + 1;
}

}  // namespace PopupProbe
