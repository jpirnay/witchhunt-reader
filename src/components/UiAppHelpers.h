#pragma once

#include <BoardConfig.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "MappedInputManager.h"
#include "components/UIThemeTokens.h"
#include "fontIds.h"

inline freeink::ui::GfxRendererTarget makeUiTarget(const GfxRenderer& renderer) {
  freeink::ui::GfxRendererTarget target(renderer, BoardConfig::isMetalioEInk4Plus() && BoardConfig::hasTouch());
  target.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, UI_10_FONT_ID);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_TITLE, UI_12_FONT_ID);
  return target;
}

inline freeink::ui::InputSnapshot touchSnapshotFrom(const MappedInputManager& mappedInput) {
  freeink::ui::InputSnapshot snapshot{};
  int touchX = 0;
  int touchY = 0;

  if (mappedInput.isScreenTouchHeld(touchX, touchY)) {
    snapshot.touchHeld = true;
    snapshot.touchX = static_cast<int16_t>(touchX);
    snapshot.touchY = static_cast<int16_t>(touchY);
  }
  if (mappedInput.wasScreenTouchPressed(touchX, touchY)) {
    snapshot.touchPressed = true;
    snapshot.touchX = static_cast<int16_t>(touchX);
    snapshot.touchY = static_cast<int16_t>(touchY);
  }
  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    snapshot.touchReleased = true;
    snapshot.touchX = static_cast<int16_t>(touchX);
    snapshot.touchY = static_cast<int16_t>(touchY);
  } else if (mappedInput.wasScreenTouchReleased()) {
    snapshot.touchReleased = true;
    snapshot.touchX = -1;
    snapshot.touchY = -1;
  }
  // Some touch backends publish the swipe before the raw release edge. Cancel the pressed row
  // now so it cannot remain armed after the list scrolls and interfere with the next tap.
  if (!snapshot.touchReleased && mappedInput.wasSwipe() != MappedInputManager::SwipeDir::None) {
    snapshot.touchReleased = true;
    snapshot.touchX = -1;
    snapshot.touchY = -1;
  }
  return snapshot;
}
