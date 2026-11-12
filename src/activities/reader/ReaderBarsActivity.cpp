#include "ReaderBarsActivity.h"

#include <GfxRenderer.h>

#include <utility>

#include "MappedInputManager.h"

ReaderBarsActivity::ReaderBarsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Info info)
    : Activity("ReaderBars", renderer, mappedInput), bars(renderer, std::move(info)) {}

void ReaderBarsActivity::onEnter() {
  Activity::onEnter();
  bars.begin(mappedInput.hasTouch(), /*live=*/false);
  requestUpdate();
}

void ReaderBarsActivity::onExit() {
  bars.end();
  Activity::onExit();
}

void ReaderBarsActivity::loop() {
  switch (bars.loop(mappedInput)) {
    case ReaderBarsController::Event::None:
      return;
    case ReaderBarsController::Event::Redraw:
    case ReaderBarsController::Event::LayoutChanged:
      requestUpdate();
      return;
    case ReaderBarsController::Event::Closed:
      setResult(ActivityResult(bars.result()));
      finish();
      return;
  }
}

void ReaderBarsActivity::render(RenderLock&&) {
  // The bars cover only their bands and leave the page between them standing, so the rest of the frame must hold what
  // is on the panel. NOT on the opening pass: EpubReaderActivity::openReaderBars() put the page there, and a sync
  // would copy whatever the secondary buffer held instead (a pre-rendered next page, or nothing while it is lent).
  // Every later pass needs it: displayBuffer() below ends in a swap, so the write buffer is two frames behind.
  if (!firstRender) renderer.syncWriteBufferFromDisplayed();
  bars.draw();
  renderer.displayBuffer();
  firstRender = false;
}
