#include "UiAppHost.h"

#include "UiAppHelpers.h"

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void UiAppHost::resetUi() {
  uiReady = false;
  themeTokens = uiThemeTokens(uiTarget);
  themeCell.store(&themeTokens, std::memory_order_release);
  app.setThemeRef(&themeCell);
}

void UiAppHost::renderUi() {
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
}

UiAppHost::TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, const bool routeHeld,
                                            const bool withLongPress) {
  TouchRoute result;
  if (!uiReady) return result;

  int longX = 0;
  int longY = 0;
  if (withLongPress && input.peekScreenLongPress(longX, longY)) {
    // The finger is still down, so this goes in as the release FreeInkUI reads a long press from.
    result.snap.touchReleased = true;
    result.snap.longPress = true;
    result.snap.touchX = static_cast<int16_t>(longX);
    result.snap.touchY = static_cast<int16_t>(longY);
    result.routed = true;
    result.event = app.route(result.snap);
    if (result.event) input.suppressTouchContact();
    return result;
  }

  result.snap = touchSnapshotFrom(input);
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }

  result.routed = true;
  result.event = app.route(result.snap);
  return result;
}