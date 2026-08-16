#include "MappedInputManager.h"

#include <FreeInkUICore.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"

namespace fui = freeink::ui;

namespace {
// A touch-down only moves a list selection once the finger has rested briefly.
// Without the delay every glancing contact drags the highlight across rows on
// the way to its target, and each move costs an e-paper refresh. Upstream's
// value, kept verbatim.
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
}  // namespace

bool (*MappedInputManager::stripReversedPredicate)() = nullptr;

void MappedInputManager::setStripReversedPredicate(bool (*predicate)()) { stripReversedPredicate = predicate; }

bool MappedInputManager::isVerticalStripReversed() {
  return stripReversedPredicate != nullptr && stripReversedPredicate();
}

MappedInputManager::Button MappedInputManager::applyStripOrder(const Button button) {
  if (!isVerticalStripReversed()) {
    return button;
  }
  // Only the four front buttons sit on the reversed strip. Up/Down (and their
  // PageBack/PageForward aliases) are the side buttons on a different edge — their
  // physical order is unchanged by rotation, so they must not swap.
  switch (button) {
    case Button::Left:
      return Button::Right;
    case Button::Right:
      return Button::Left;
    default:
      return button;
  }
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  switch (applyStripOrder(button)) {
    case Button::Back:
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::PageForward:
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
  }
  return false;
}

uint8_t MappedInputManager::rawIndex(const Button button) const {
  switch (applyStripOrder(button)) {
    case Button::Back:
      return SETTINGS.frontButtonBack;
    case Button::Confirm:
      return SETTINGS.frontButtonConfirm;
    case Button::Left:
      return SETTINGS.frontButtonLeft;
    case Button::Right:
      return SETTINGS.frontButtonRight;
    case Button::Up:
      return HalGPIO::BTN_UP;
    case Button::Down:
      return HalGPIO::BTN_DOWN;
    case Button::Power:
      return HalGPIO::BTN_POWER;
    case Button::PageBack:
      return HalGPIO::BTN_UP;
    case Button::PageForward:
      return HalGPIO::BTN_DOWN;
  }
  return 0xFF;
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // On a reversed strip the Left/Right roles are swapped (see applyStripOrder), so the
  // hint text has to follow — otherwise the label would contradict what the button does.
  const bool reversed = isVerticalStripReversed();
  const char* previousLabel = reversed ? next : previous;
  const char* nextLabel = reversed ? previous : next;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return previousLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return nextLabel;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
// --- Touch -------------------------------------------------------------------
// Thin interpretation layer over HalGPIO's raw passthrough: map panel-native
// normalized coordinates into logical pixels via the renderer's live
// orientation, then give the result app meaning. Ported from upstream/develop;
// see docs/touch-input-migration-2026-08-14.md §1.

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left:
      return SwipeDir::Left;
    case fui::SwipeDir::Right:
      return SwipeDir::Right;
    case fui::SwipeDir::Up:
      return SwipeDir::Up;
    case fui::SwipeDir::Down:
      return SwipeDir::Down;
    default:
      return SwipeDir::None;
  }
}

// Edge classification (which swipe counts as an edge gesture) lives in the SDK;
// only the MEANING of each edge — back, menu, home — is decided here.
bool MappedInputManager::wasEdgeSwipe(const freeink::ui::ScreenEdge edge) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  return fui::edgeSwipe(edge, sx, sy, ex, ey, renderer.getScreenWidth(), renderer.getScreenHeight());
}

bool MappedInputManager::wasBackGesture() const {
  // Edge-anchored so mid-screen horizontal swipes stay available to activities
  // that consume SwipeDir::Left/Right (percent selection, image viewer).
  return wasEdgeSwipe(fui::ScreenEdge::Left);
}

bool MappedInputManager::wasTopEdgeDownSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Top); }

bool MappedInputManager::wasBottomEdgeUpSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Bottom); }

bool MappedInputManager::wasMenuGesture() const { return wasTopEdgeDownSwipe(); }

bool MappedInputManager::wasHomeGesture() const {
  return gpio.hasHomeKey() ? gpio.wasHomeKeyTapped() : wasBottomEdgeUpSwipe();
}

bool MappedInputManager::wasHomeKeyHold() const { return gpio.hasHomeKey() && gpio.wasHomeKeyLongPressed(); }
