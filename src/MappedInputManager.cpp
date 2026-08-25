#include "MappedInputManager.h"

#include "CrossPointSettings.h"

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