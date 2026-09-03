#pragma once

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TouchGestures.h"

class GfxRenderer;

// Turns touch gestures into button actions, so a swipe or a pinch can carry any
// action a physical key can.
//
// The counterpart of ButtonEventManager, and deliberately shaped like it: both
// sit between an input source and main.cpp's action dispatch, and both read
// CrossPointSettings themselves rather than making the caller pass the mapping
// in.
//
// Two rules keep this from disturbing touch handling that is already
// device-validated:
//
//   * It only ever acts on a gesture the user has explicitly bound. With the
//     shipped defaults (every gesture BTN_DEFAULT) update() returns without
//     reading the contact at all, so the reader's tap zones, swipe page turns
//     and menu gesture behave exactly as they did before this class existed.
//   * When it does act, it calls suppressTouchContact() so the same contact
//     cannot ALSO reach the reader. Acting and leaving the contact live would
//     turn one tap into a gesture action plus a page turn.
//
// It reads the contact through MappedInputManager's peeks, which are pure const
// reads of state the SDK latched — nothing here consumes a tap merely by
// looking at it, which is what allows the "look, decide, then maybe suppress"
// order above.
class GestureEventManager {
 public:
  GestureEventManager(MappedInputManager& input, const GfxRenderer& renderer) : input(input), renderer(renderer) {}

  // Classify at most one gesture and report the action bound to it. Returns
  // false when nothing fired, when the board has no touch, or when the gesture
  // that fired is unbound — in the last case the contact is left untouched for
  // the activity to interpret.
  //
  // Call once per loop tick. `inReader` narrows what is considered:
  //
  //   in the reader   every gesture, because the page has no touch targets of
  //                   its own to compete with.
  //   anywhere else   swipes and two-finger gestures only. Taps and long taps
  //                   belong to the screen — a tap on a list row is that row —
  //                   and no swipe is consumed by any non-reader screen, so
  //                   opening those up costs nothing and is what lets the
  //                   reading light be reached from the home screen at night.
  //
  // Outside the reader a gesture bound to a reader-scoped action is left
  // unclaimed rather than swallowed, exactly as the button path does.
  bool consumeAction(CrossPointSettings::BUTTON_ACTION& action, bool inReader);

 private:
  MappedInputManager& input;
  const GfxRenderer& renderer;

  // Resolve `gesture` to its bound action. Returns false for BTN_DEFAULT, and
  // for a reader-scoped action outside the reader — both mean "not ours, leave
  // the contact alone".
  static bool boundAction(TouchGestures::Gesture gesture, CrossPointSettings::BUTTON_ACTION& action, bool inReader);
};
