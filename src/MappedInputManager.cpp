#include "MappedInputManager.h"

#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <TouchTransform.h>

#include "CrossPointSettings.h"
#include "components/themes/ListTouchBand.h"

namespace fui = freeink::ui;

namespace {
// A touch-down only moves a list selection once the finger has rested briefly.
// Without the delay every glancing contact drags the highlight across rows on
// the way to its target, and each move costs an e-paper refresh. Upstream's
// value, kept verbatim.
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
}  // namespace

MappedInputManager::ScreenOrientation (*MappedInputManager::orientationProvider)() = nullptr;

void MappedInputManager::setOrientationProvider(ScreenOrientation (*provider)()) { orientationProvider = provider; }

MappedInputManager::ScreenOrientation MappedInputManager::screenOrientation() {
  return orientationProvider != nullptr ? orientationProvider() : ScreenOrientation::Portrait;
}

bool MappedInputManager::isVerticalStripReversed() {
  return screenOrientation() == ScreenOrientation::LandscapeCounterClockwise;
}

MappedInputManager::Button MappedInputManager::applyStripOrder(const Button button) {
  return applyStripOrder(button, isVerticalStripReversed());
}

// The whole logical-direction table, checked at compile time. Each row is one orientation and
// spells out the button a reader holding the device that way expects to move them that way; the
// LandscapeCounterClockwise row reads "wrong" on purpose, because those answers still have to
// survive the strip reversal that mapButton()/rawIndex() apply on the way to the hardware.
namespace {
using MIM = MappedInputManager;
using SO = MIM::ScreenOrientation;
using Dir = MIM::Direction;
using Btn = MIM::Button;

static_assert(MIM::buttonFor(SO::Portrait, Dir::Left) == Btn::Left, "portrait left");
static_assert(MIM::buttonFor(SO::Portrait, Dir::Right) == Btn::Right, "portrait right");
static_assert(MIM::buttonFor(SO::Portrait, Dir::Up) == Btn::Up, "portrait up");
static_assert(MIM::buttonFor(SO::Portrait, Dir::Down) == Btn::Down, "portrait down");

static_assert(MIM::buttonFor(SO::LandscapeClockwise, Dir::Left) == Btn::Down, "cw left");
static_assert(MIM::buttonFor(SO::LandscapeClockwise, Dir::Right) == Btn::Up, "cw right");
static_assert(MIM::buttonFor(SO::LandscapeClockwise, Dir::Up) == Btn::Left, "cw up");
static_assert(MIM::buttonFor(SO::LandscapeClockwise, Dir::Down) == Btn::Right, "cw down");

static_assert(MIM::buttonFor(SO::PortraitInverted, Dir::Left) == Btn::Right, "inverted left");
static_assert(MIM::buttonFor(SO::PortraitInverted, Dir::Right) == Btn::Left, "inverted right");
static_assert(MIM::buttonFor(SO::PortraitInverted, Dir::Up) == Btn::Down, "inverted up");
static_assert(MIM::buttonFor(SO::PortraitInverted, Dir::Down) == Btn::Up, "inverted down");

// Pre-compensated for the reversal: Up resolves to Left so that rawIndex() lands on the front
// button configured as Right, which is the topmost one when the strip renders bottom-to-top.
static_assert(MIM::buttonFor(SO::LandscapeCounterClockwise, Dir::Left) == Btn::Up, "ccw left");
static_assert(MIM::buttonFor(SO::LandscapeCounterClockwise, Dir::Right) == Btn::Down, "ccw right");
static_assert(MIM::buttonFor(SO::LandscapeCounterClockwise, Dir::Up) == Btn::Left, "ccw up");
static_assert(MIM::buttonFor(SO::LandscapeCounterClockwise, Dir::Down) == Btn::Right, "ccw down");

// Every orientation must reach all four buttons, or one press would drive two directions while
// another button went dead.
constexpr bool coversAllButtons(const SO orientation) {
  return MIM::buttonFor(orientation, Dir::Left) != MIM::buttonFor(orientation, Dir::Right) &&
         MIM::buttonFor(orientation, Dir::Up) != MIM::buttonFor(orientation, Dir::Down) &&
         MIM::buttonFor(orientation, Dir::Left) != MIM::buttonFor(orientation, Dir::Up) &&
         MIM::buttonFor(orientation, Dir::Left) != MIM::buttonFor(orientation, Dir::Down) &&
         MIM::buttonFor(orientation, Dir::Right) != MIM::buttonFor(orientation, Dir::Up) &&
         MIM::buttonFor(orientation, Dir::Right) != MIM::buttonFor(orientation, Dir::Down);
}
static_assert(coversAllButtons(SO::Portrait), "portrait bijection");
static_assert(coversAllButtons(SO::LandscapeClockwise), "cw bijection");
static_assert(coversAllButtons(SO::PortraitInverted), "inverted bijection");
static_assert(coversAllButtons(SO::LandscapeCounterClockwise), "ccw bijection");
}  // namespace

MappedInputManager::DirectionPair MappedInputManager::frontStripDirections() {
  switch (screenOrientation()) {
    case ScreenOrientation::Portrait:
    case ScreenOrientation::PortraitInverted:
      return {Direction::Left, Direction::Right};
    case ScreenOrientation::LandscapeClockwise:
    case ScreenOrientation::LandscapeCounterClockwise:
      return {Direction::Up, Direction::Down};
  }
  return {Direction::Left, Direction::Right};
}

MappedInputManager::DirectionPair MappedInputManager::sideButtonDirections() {
  const DirectionPair front = frontStripDirections();
  return front.previous == Direction::Left ? DirectionPair{Direction::Up, Direction::Down}
                                           : DirectionPair{Direction::Left, Direction::Right};
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
  // The movement labels follow the buttons that actually move the selection. In landscape the
  // front strip stands vertically on screen, so it carries logical Up/Down rather than Left/Right,
  // and in the orientations that render the strip in reverse the two swap ends. Resolving the hint
  // through buttonFor()/rawIndex() is what keeps it on the very button the handler listens to —
  // spelling the swap out here again is how the label and the action drift apart.
  const DirectionPair front = frontStripDirections();
  const uint8_t backHw = rawIndex(Button::Back);
  const uint8_t confirmHw = rawIndex(Button::Confirm);
  const uint8_t previousHw = rawIndex(buttonFor(front.previous));
  const uint8_t nextHw = rawIndex(buttonFor(front.next));

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](const uint8_t hw) -> const char* {
    if (hw == backHw) {
      return back;
    }
    if (hw == confirmHw) {
      return confirm;
    }
    if (hw == previousHw) {
      return previous;
    }
    if (hw == nextHw) {
      return next;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

MappedInputManager::Hints MappedInputManager::mapHints(const char* back, const char* confirm, const char* left,
                                                       const char* right, const char* up, const char* down) const {
  // Portrait hands the front strip the Left/Right pair and the side buttons the Up/Down one;
  // landscape swaps them, because the strip is what stands vertically on screen there.
  const DirectionPair front = frontStripDirections();
  const bool frontIsHorizontal = front.previous == Direction::Left;
  const char* const frontPrevious = frontIsHorizontal ? left : up;
  const char* const frontNext = frontIsHorizontal ? right : down;
  const char* const sidePrevious = frontIsHorizontal ? up : left;
  const char* const sideNext = frontIsHorizontal ? down : right;

  // The side hints are drawn in panel order (BTN_UP's box above BTN_DOWN's), so they have to be
  // handed over in that order — and BTN_UP is not always the button that moves backwards.
  const DirectionPair side = sideButtonDirections();
  const SideLabels sideLabels =
      buttonFor(side.previous) == Button::Up ? SideLabels{sidePrevious, sideNext} : SideLabels{sideNext, sidePrevious};
  return {mapLabels(back, confirm, frontPrevious, frontNext), sideLabels};
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

void MappedInputManager::setTouchEventsEnabled(const bool enabled) {
  if (enabled == touchEventsEnabled_) return;
  touchEventsEnabled_ = enabled;
  // Going quiet drops what is already queued. The single-contact events expire
  // on their own within a sampler pass, but a multi-touch gesture sits in the
  // ring until something pops it — without this, turning touch back on would
  // replay a pinch made while it was off.
  if (!enabled) gpio.flushTouchGestures();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!rawTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTappedIn(const touchtransform::Orientation orientation, int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!rawTap(nx, ny)) return false;
  renderer.tapToLogical(static_cast<GfxRenderer::Orientation>(orientation), nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!rawTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!rawLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::peekScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!rawLongPress(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::peekScreenLongPressIn(const touchtransform::Orientation orientation, int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!rawLongPress(nx, ny)) return false;
  renderer.tapToLogical(static_cast<GfxRenderer::Orientation>(orientation), nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!rawHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

void MappedInputManager::injectRawPress(const uint8_t rawButtonIndex) const { gpio.injectPress(rawButtonIndex); }

bool MappedInputManager::wasScreenTouchReleased() const { return rawReleased(); }

unsigned long MappedInputManager::lastTouchHeldMs() const { return gpio.lastTouchHeldMs(); }

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  // Rows band along y, bounded on x. Arithmetic lives in touchtransform so it
  // can be host-tested (see test/touch_transform).
  const auto hit = [&](const int x, const int y) {
    return touchtransform::bandHit(y, x, top, rowStep, rowCount, xStart, xEnd, rowHeight, row);
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
  // Columns are the same test with the axes swapped: band along x, bounded on y.
  const auto hit = [&](const int x, const int y) {
    return touchtransform::bandHit(x, y, left, colStep, colCount, yStart, yEnd, colWidth, col);
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::listTouch(int& index) const {
  // Live-orientation coordinates, unlike the hint strip: drawList() paints in whatever
  // orientation the renderer is in rather than forcing Portrait, so that is the frame its rows
  // were recorded in. See ListTouchBand.h.
  const auto hit = [&](const int x, const int y) {
    const int item = ListTouchBand::hitTest(x, y);
    if (item < 0) return false;
    index = item;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::MultiTouch MappedInputManager::popMultiTouch(int& x, int& y) const {
  HalGPIO::TouchGesture gesture;
  if (!rawPopGesture(gesture)) return MultiTouch::None;
  renderer.tapToLogical(gesture.nx, gesture.ny, x, y);
  switch (gesture.kind) {
    case HalGPIO::TouchGesture::Kind::Pinch:
      // magnitude is end separation over start separation, and the SDK rejects
      // anything inside 0.8..1.2, so it is never ambiguously 1.
      return gesture.magnitude < 1.0f ? MultiTouch::PinchIn : MultiTouch::PinchOut;
    case HalGPIO::TouchGesture::Kind::Rotate:
      // Positive degrees is clockwise: the panel's Y axis points down, so a
      // positive cross product is a clockwise turn as seen by the reader.
      return gesture.magnitude > 0.0f ? MultiTouch::RotateClockwise : MultiTouch::RotateCounterClockwise;
  }
  return MultiTouch::None;
}

bool MappedInputManager::decodeSwipe(const touchtransform::Orientation orientation, int& sx, int& sy, int& ex,
                                     int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!rawSwipeEndpoints(nxs, nys, nxe, nye)) return false;
  const auto rendererOrientation = static_cast<GfxRenderer::Orientation>(orientation);
  renderer.tapToLogical(rendererOrientation, nxs, nys, sx, sy);
  renderer.tapToLogical(rendererOrientation, nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  return wasSwipeIn(static_cast<touchtransform::Orientation>(renderer.getOrientation()));
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipeIn(const touchtransform::Orientation orientation) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(orientation, sx, sy, ex, ey)) return SwipeDir::None;
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
  // One orientation read for the decode AND the screen size it is tested
  // against, so the two cannot disagree about which way the screen is turned.
  const auto orientation = renderer.getOrientation();
  if (!decodeSwipe(static_cast<touchtransform::Orientation>(orientation), sx, sy, ex, ey)) return false;
  return fui::edgeSwipe(edge, sx, sy, ex, ey, renderer.getScreenWidth(orientation),
                        renderer.getScreenHeight(orientation));
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
