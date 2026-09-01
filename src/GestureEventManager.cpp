#include "GestureEventManager.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <TouchTransform.h>

namespace {
using BA = CrossPointSettings::BUTTON_ACTION;
using TouchGestures::Gesture;
}  // namespace

bool GestureEventManager::boundAction(const Gesture gesture, BA& action) {
  const uint8_t configured = TouchGestures::actionFor(gesture);
#if defined(BUTTON_TRACE) && BUTTON_TRACE
  // The other half of the pair with HalGPIO's [TCH] release line: that one says
  // what the SDK classified, this says what the loop did about it. A gesture
  // that appears there and not here was missed by a busy loop; one that appears
  // here as "unbound" is a settings question, not an input one.
  LOG_INF("GEST", "%s -> action=%u (%s)", TouchGestures::nameOf(gesture), configured,
          configured == BA::BTN_DEFAULT ? "unbound, left to the reader" : "claimed");
#endif
  if (configured == BA::BTN_DEFAULT) return false;
  action = static_cast<BA>(configured);
  return true;
}

bool GestureEventManager::consumeAction(BA& action) {
  if (!input.hasTouch()) return false;
  // Nothing bound: do not so much as look at the contact. This is the guarantee
  // that an unconfigured device behaves exactly as it did before gestures
  // existed — see the class comment.
  if (!TouchGestures::anyBound()) {
#if defined(BUTTON_TRACE) && BUTTON_TRACE
    // Logged once per contact rather than per tick: this is the state a device
    // whose settings predate the shipped defaults comes up in, and it looks
    // exactly like a broken gesture layer from the outside.
    if (input.wasScreenTouchReleased()) {
      LOG_INF("GEST", "no gesture is bound to an action; every contact is left to the reader");
    }
#endif
    return false;
  }

  // ONE orientation read for the whole classification. getScreenWidth() and
  // tapToLogical() both sample the LIVE draw orientation, which the themes flip
  // to Portrait mid-pass to put the hint strips on the panel edge — so reading
  // width, then height, then the tap position could resolve each against a
  // different frame and zone a landscape tap as though it were portrait. Held
  // rather than live for the same reason MappedInputManager's direction mapping
  // is (see setOrientationProvider, issue #87): this runs on the loop task,
  // outside any render pass, and must answer for the way the user is holding the
  // device.
  const auto orientation = renderer.getHeldOrientation();
  const auto touchOrientation = static_cast<touchtransform::Orientation>(orientation);
  const int width = renderer.getScreenWidth(orientation);
  const int height = renderer.getScreenHeight(orientation);
  int x = 0;
  int y = 0;

  // Multi-touch first. It comes off its own latched queue, so it can neither be
  // confused with a single-contact gesture nor steal one; popping is the only
  // way to clear it, so an unbound pinch is dropped here rather than being left
  // to arrive late.
  switch (input.popMultiTouch(x, y)) {
    case MappedInputManager::MultiTouch::PinchIn:
      return boundAction(Gesture::PinchIn, action);
    case MappedInputManager::MultiTouch::PinchOut:
      return boundAction(Gesture::PinchOut, action);
    case MappedInputManager::MultiTouch::RotateClockwise:
      return boundAction(Gesture::RotateClockwise, action);
    case MappedInputManager::MultiTouch::RotateCounterClockwise:
      return boundAction(Gesture::RotateCounterClockwise, action);
    case MappedInputManager::MultiTouch::None:
      break;
  }

  // Long press before tap: it fires while the finger is still down, and the lift
  // that follows would otherwise also read as a tap. Claiming it suppresses the
  // rest of the contact, so the tap never happens. Leaving it unbound
  // deliberately does NOT suppress — an unbound long tap then degrades to
  // whatever the tap in that zone does, which is the least surprising thing for
  // a reader who holds a finger down a little too long.
  if (input.peekScreenLongPressIn(touchOrientation, x, y)) {
    const Gesture gesture = TouchGestures::longTapGestureFor(TapZones::zoneFor(x, y, width, height));
    if (boundAction(gesture, action)) {
      input.suppressTouchContact();
      return true;
    }
  }

  // Swipe before tap: a swipe ends in a release like a tap does, and the SDK
  // reports both for the same contact when the travel sits near the threshold.
  // Direction is resolved in the orientation sampled above, so it is the way the
  // page moved, not the way the panel is wired.
  //
  // Vertical swipes are split by the half of the screen they START in, which is
  // the question a phone asks to tell the notification shade from quick
  // settings. The start point rather than the end: a downward swipe travels, and
  // where it ends up says nothing about which control the reader reached for.
  // Horizontal swipes are not split — they are the page turn in Swipe mode, and
  // a page turn does not care which half of the page it began on.
  int swipeStartX = 0;
  int swipeStartY = 0;  // unused: the split is horizontal, but decodeSwipe reports both
  Gesture swipe = Gesture::Count;
  switch (input.wasSwipeIn(touchOrientation, swipeStartX, swipeStartY)) {
    case MappedInputManager::SwipeDir::Left:
      swipe = Gesture::SwipeLeft;
      break;
    case MappedInputManager::SwipeDir::Right:
      swipe = Gesture::SwipeRight;
      break;
    case MappedInputManager::SwipeDir::Up:
      swipe = swipeStartX < width / 2 ? Gesture::SwipeUpLeft : Gesture::SwipeUpRight;
      break;
    case MappedInputManager::SwipeDir::Down:
      swipe = swipeStartX < width / 2 ? Gesture::SwipeDownLeft : Gesture::SwipeDownRight;
      break;
    case MappedInputManager::SwipeDir::None:
      break;
  }
  if (swipe != Gesture::Count && boundAction(swipe, action)) {
    input.suppressTouchContact();
    return true;
  }

  if (input.wasScreenTappedIn(touchOrientation, x, y)) {
    const Gesture gesture = TouchGestures::tapGestureFor(TapZones::zoneFor(x, y, width, height));
    if (boundAction(gesture, action)) {
      input.suppressTouchContact();
      return true;
    }
  }

  return false;
}
