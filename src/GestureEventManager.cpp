#include "GestureEventManager.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <TouchTransform.h>

#include "TouchUi.h"

namespace {
using BA = CrossPointSettings::BUTTON_ACTION;
using TouchGestures::Gesture;
}  // namespace

#if CP_TOUCH_UI

bool GestureEventManager::boundAction(const Gesture gesture, BA& action, const bool inReader) {
  const uint8_t configured = TouchGestures::actionFor(gesture);
  // A reader-scoped action outside the reader would funnel through
  // dispatchButtonAction() and do nothing, having already claimed the contact.
  // Decline instead, so the screen underneath still gets its touch.
  const bool outOfScope = !inReader && CrossPointSettings::isReaderScopedAction(configured);
#if defined(BUTTON_TRACE) && BUTTON_TRACE
  // The other half of the pair with HalGPIO's [TCH] release line: that one says
  // what the SDK classified, this says what the loop did about it. A gesture
  // that appears there and not here was missed by a busy loop; one that appears
  // here as "unbound" is a settings question, not an input one.
  LOG_INF("GEST", "%s -> action=%u (%s)", TouchGestures::nameOf(gesture), configured,
          configured == BA::BTN_DEFAULT ? "unbound, left to the screen"
          : outOfScope                  ? "reader-only action, not in the reader"
                                        : "claimed");
#endif
  if (configured == BA::BTN_DEFAULT || outOfScope) return false;
  action = static_cast<BA>(configured);
  return true;
}

bool GestureEventManager::consumeAction(BA& action, const bool inReader) {
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
      return boundAction(Gesture::PinchIn, action, inReader);
    case MappedInputManager::MultiTouch::PinchOut:
      return boundAction(Gesture::PinchOut, action, inReader);
    case MappedInputManager::MultiTouch::RotateClockwise:
      return boundAction(Gesture::RotateClockwise, action, inReader);
    case MappedInputManager::MultiTouch::RotateCounterClockwise:
      return boundAction(Gesture::RotateCounterClockwise, action, inReader);
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
    // Corner before zone — a corner sits inside Left or Right, so the other order would
    // never reach one.
    const TapZones::Corner corner = TapZones::cornerFor(x, y, width, height);

    // The CORNERS are live on every screen; the five ZONES stay reader-only. The line is
    // drawn on size, not on taste: a zone is a third of the glass and would sit on top of
    // whatever a screen has drawn there, while a corner is an eighth of the shorter edge at
    // an extremity, and outside the reader the only other long-press consumer is the button
    // hint strip along the bottom (ActivityManager::dispatchHintStripTap) — every other
    // screen resolves lists, covers, the tab bar and the keyboard on TAPS.
    //
    // Opening the corners up is what makes the reading-light toggle mean the same thing
    // everywhere. A control that works in a book and silently does nothing on the home
    // screen is worse than one that does not exist, and the light is exactly the control
    // someone reaches for in the dark without looking at the screen.
    //
    // Claiming here suppresses the contact, so a bound corner wins over anything the screen
    // underneath would have made of the press. That is deliberate: the corner is the more
    // specific target.
    const bool haveGesture = corner != TapZones::Corner::None || inReader;
    if (haveGesture) {
      const Gesture gesture = corner != TapZones::Corner::None
                                  ? TouchGestures::longTapCornerGestureFor(corner)
                                  : TouchGestures::longTapGestureFor(TapZones::zoneFor(x, y, width, height));
      // boundAction() still declines a reader-SCOPED action outside the reader, so a corner
      // bound to e.g. Next Chapter stays inert on the home screen rather than being eaten.
      if (boundAction(gesture, action, inReader)) {
        input.suppressTouchContact();
        return true;
      }
    }
  }

  // Swipe before tap: a swipe ends in a release like a tap does, and the SDK
  // reports both for the same contact when the travel sits near the threshold.
  // Direction is resolved in the orientation sampled above, so it is the way the
  // page moved, not the way the panel is wired.
  //
  // Vertical swipes are anchored to the left/right EDGE COLUMN they START in, the way
  // KOReader anchors its frontlight swipes (DSWIPE_ZONE_LEFT_EDGE / RIGHT_EDGE). The start
  // point rather than the end: an end-anchored split would be a function of swipe LENGTH
  // rather than of a place the reader picks, and e-paper cannot animate a drag, so there
  // would be nothing to aim at while the finger is moving.
  //
  // edgeColumnFor() also excludes the top and bottom bands, which belong to the reader menu
  // and the light panel — see the note there on the overlap that creates. A vertical swipe
  // anywhere else on the page is deliberately nobody's: it used to be brightness on
  // whichever half it fell in, which meant a thumb resting mid-page could dim the screen.
  //
  // Horizontal swipes are not split — they are the page turn in Swipe mode, and a page turn
  // does not care where on the page it began.
  int swipeStartX = 0;
  int swipeStartY = 0;
  Gesture swipe = Gesture::Count;
  const auto swipeDir = input.wasSwipeIn(touchOrientation, swipeStartX, swipeStartY);
  const TapZones::EdgeColumn startColumn = TapZones::edgeColumnFor(swipeStartX, swipeStartY, width, height);
  // The page-turn third the swipe STARTED in, for the outward horizontal swipes. Only the
  // horizontal position matters, and zoneFor() answers Left/Right for any y once x is in an
  // outer third, so this is the same zone a tap there would hit.
  const TapZones::Zone startZone = TapZones::zoneFor(swipeStartX, swipeStartY, width, height);
  switch (swipeDir) {
    // A horizontal swipe that STARTS in an outer third is the ten-page jump for that third,
    // WHICHEVER WAY IT TRAVELS. The direction is deliberately not part of the test.
    //
    // It used to require travelling OUTWARD -- leftward in the left third -- and that was
    // reported unreliable on a T5S3, working about one attempt in ten on the left while the
    // right worked. Every code path here is symmetric, so the cause is not in the software:
    // an outward swipe has to START inside the third and still find 60 px
    // (InputManager::TOUCH_SWIPE_MIN_PX) of room to travel FURTHER outward, which is a narrow
    // window hard against the bezel, and how reliably a thumb lands in it depends on which
    // hand is holding the device. Direction-agnostic removes the window entirely: begin
    // anywhere in the third and flick either way.
    //
    // The cost is confined to Swipe reading mode, where the outer thirds lose the one-page
    // swipe and keep the one-page TAP. In the shipped default (TOUCH_READER_ON, i.e. taps)
    // horizontal swipes are unused, so there is no cost at all.
    case MappedInputManager::SwipeDir::Left:
      swipe = startZone == TapZones::Zone::Left    ? Gesture::SwipeInLeftZone
              : startZone == TapZones::Zone::Right ? Gesture::SwipeInRightZone
                                                   : Gesture::SwipeLeft;
      break;
    case MappedInputManager::SwipeDir::Right:
      swipe = startZone == TapZones::Zone::Right  ? Gesture::SwipeInRightZone
              : startZone == TapZones::Zone::Left ? Gesture::SwipeInLeftZone
                                                  : Gesture::SwipeRight;
      break;
    case MappedInputManager::SwipeDir::Up:
      if (startColumn == TapZones::EdgeColumn::Left) {
        swipe = Gesture::SwipeUpLeft;
      } else if (startColumn == TapZones::EdgeColumn::Right) {
        swipe = Gesture::SwipeUpRight;
      }
      break;
    case MappedInputManager::SwipeDir::Down:
      if (startColumn == TapZones::EdgeColumn::Left) {
        swipe = Gesture::SwipeDownLeft;
      } else if (startColumn == TapZones::EdgeColumn::Right) {
        swipe = Gesture::SwipeDownRight;
      }
      break;
    case MappedInputManager::SwipeDir::None:
      break;
  }
  if (swipe != Gesture::Count && boundAction(swipe, action, inReader)) {
    input.suppressTouchContact();
    return true;
  }

  if (inReader && input.wasScreenTappedIn(touchOrientation, x, y)) {
    const Gesture gesture = TouchGestures::tapGestureFor(TapZones::zoneFor(x, y, width, height));
    if (boundAction(gesture, action, inReader)) {
      input.suppressTouchContact();
      return true;
    }
  }

  return false;
}

#else  // !CP_TOUCH_UI

// main.cpp still calls this every tick, so it keeps its symbol and answers "no
// gesture". boundAction() is not defined at all, and that is what makes
// TouchGestures::BINDINGS (400 B of .rodata) and builtinLabelFor() unreachable --
// SettingsList's gesture loop being the only other user of either.
bool GestureEventManager::consumeAction(BA& action, const bool inReader) {
  (void)action;
  (void)inReader;
  return false;
}

#endif  // CP_TOUCH_UI
