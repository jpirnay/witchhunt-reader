#pragma once

#include <HalFrontlight.h>
#include <I18nKeys.h>

#include <cstdint>

#include "CrossPointSettings.h"
#include "components/TapZones.h"

// The touch gestures the reader can have actions bound to, and what each of them
// is currently set to do.
//
// A gesture is treated exactly like a button here: it resolves to a
// CrossPointSettings::BUTTON_ACTION through a per-gesture setting, and
// BTN_DEFAULT keeps its usual meaning of "do whatever this input already did".
// That default is what makes the feature safe to add to a device whose touch
// reading controls are device-validated: with nothing bound, GestureEventManager
// never touches the contact and the reader interprets it exactly as before.
//
// Kept separate from MappedInputManager because these are app-level meanings,
// not input-layer facts: MappedInputManager knows a swipe went left, this knows
// that a leftward swipe is a gesture a user can bind.
namespace TouchGestures {

// Every bindable gesture. Order is the order the settings screen lists them in,
// grouped by family; it carries no other meaning (unlike BUTTON_ACTION, whose
// numeric values are persisted, these are never written to disk — the settings
// fields are).
enum class Gesture : uint8_t {
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
  TapLeft,
  TapRight,
  TapCentre,
  TapTop,
  TapBottom,
  LongTapLeft,
  LongTapRight,
  LongTapCentre,
  LongTapTop,
  LongTapBottom,
  PinchIn,
  PinchOut,
  RotateClockwise,
  RotateCounterClockwise,
  Count,
};

// One row per gesture: the setting that stores its action, and the label the
// settings screen shows. Stated once so GestureEventManager and SettingsList
// cannot drift apart — the bug that would otherwise be invisible is a gesture
// the settings screen offers and the input layer never reads.
struct Binding {
  Gesture gesture;
  uint8_t CrossPointSettings::* field;
  StrId label;
  // Heading the settings screen groups this gesture under.
  StrId group;
  // JSON key for the web settings API. Matches the field name.
  const char* key;
  // Needs a controller that reports more than one contact (GT911). A
  // single-contact panel can still swipe and tap.
  bool needsMultiTouch;
};

inline constexpr Binding BINDINGS[] = {
    {Gesture::SwipeLeft, &CrossPointSettings::gestSwipeLeft, StrId::STR_GEST_SWIPE_LEFT, StrId::STR_GEST_SWIPE_GROUP,
     "gestSwipeLeft", false},
    {Gesture::SwipeRight, &CrossPointSettings::gestSwipeRight, StrId::STR_GEST_SWIPE_RIGHT, StrId::STR_GEST_SWIPE_GROUP,
     "gestSwipeRight", false},
    {Gesture::SwipeUp, &CrossPointSettings::gestSwipeUp, StrId::STR_GEST_SWIPE_UP, StrId::STR_GEST_SWIPE_GROUP,
     "gestSwipeUp", false},
    {Gesture::SwipeDown, &CrossPointSettings::gestSwipeDown, StrId::STR_GEST_SWIPE_DOWN, StrId::STR_GEST_SWIPE_GROUP,
     "gestSwipeDown", false},
    {Gesture::TapLeft, &CrossPointSettings::gestTapLeft, StrId::STR_GEST_TAP_LEFT, StrId::STR_GEST_TAP_GROUP,
     "gestTapLeft", false},
    {Gesture::TapRight, &CrossPointSettings::gestTapRight, StrId::STR_GEST_TAP_RIGHT, StrId::STR_GEST_TAP_GROUP,
     "gestTapRight", false},
    {Gesture::TapCentre, &CrossPointSettings::gestTapCentre, StrId::STR_GEST_TAP_CENTRE, StrId::STR_GEST_TAP_GROUP,
     "gestTapCentre", false},
    {Gesture::TapTop, &CrossPointSettings::gestTapTop, StrId::STR_GEST_TAP_TOP, StrId::STR_GEST_TAP_GROUP, "gestTapTop",
     false},
    {Gesture::TapBottom, &CrossPointSettings::gestTapBottom, StrId::STR_GEST_TAP_BOTTOM, StrId::STR_GEST_TAP_GROUP,
     "gestTapBottom", false},
    {Gesture::LongTapLeft, &CrossPointSettings::gestLongTapLeft, StrId::STR_GEST_LONG_TAP_LEFT,
     StrId::STR_GEST_LONG_TAP_GROUP, "gestLongTapLeft", false},
    {Gesture::LongTapRight, &CrossPointSettings::gestLongTapRight, StrId::STR_GEST_LONG_TAP_RIGHT,
     StrId::STR_GEST_LONG_TAP_GROUP, "gestLongTapRight", false},
    {Gesture::LongTapCentre, &CrossPointSettings::gestLongTapCentre, StrId::STR_GEST_LONG_TAP_CENTRE,
     StrId::STR_GEST_LONG_TAP_GROUP, "gestLongTapCentre", false},
    {Gesture::LongTapTop, &CrossPointSettings::gestLongTapTop, StrId::STR_GEST_LONG_TAP_TOP,
     StrId::STR_GEST_LONG_TAP_GROUP, "gestLongTapTop", false},
    {Gesture::LongTapBottom, &CrossPointSettings::gestLongTapBottom, StrId::STR_GEST_LONG_TAP_BOTTOM,
     StrId::STR_GEST_LONG_TAP_GROUP, "gestLongTapBottom", false},
    {Gesture::PinchIn, &CrossPointSettings::gestPinchIn, StrId::STR_GEST_PINCH_IN, StrId::STR_GEST_MULTI_GROUP,
     "gestPinchIn", true},
    {Gesture::PinchOut, &CrossPointSettings::gestPinchOut, StrId::STR_GEST_PINCH_OUT, StrId::STR_GEST_MULTI_GROUP,
     "gestPinchOut", true},
    {Gesture::RotateClockwise, &CrossPointSettings::gestRotateCw, StrId::STR_GEST_ROTATE_CW,
     StrId::STR_GEST_MULTI_GROUP, "gestRotateCw", true},
    {Gesture::RotateCounterClockwise, &CrossPointSettings::gestRotateCcw, StrId::STR_GEST_ROTATE_CCW,
     StrId::STR_GEST_MULTI_GROUP, "gestRotateCcw", true},
};
static_assert(sizeof(BINDINGS) / sizeof(BINDINGS[0]) == static_cast<size_t>(Gesture::Count),
              "every Gesture needs a settings field and a label");

#if defined(BUTTON_TRACE) && BUTTON_TRACE
// Bring-up only, and compiled out otherwise: an eighteen-entry string table is
// not worth the flash in a shipping build. Indexed by Gesture, so it follows the
// enum automatically.
inline const char* nameOf(const Gesture gesture) {
  static constexpr const char* kNames[] = {
      "swipe-left",  "swipe-right",    "swipe-up",   "swipe-down",   "tap-left",      "tap-right",
      "tap-centre",  "tap-top",        "tap-bottom", "longtap-left", "longtap-right", "longtap-centre",
      "longtap-top", "longtap-bottom", "pinch-in",   "pinch-out",    "rotate-cw",     "rotate-ccw",
  };
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Gesture::Count),
                "every Gesture needs a trace name");
  const auto index = static_cast<size_t>(gesture);
  return index < static_cast<size_t>(Gesture::Count) ? kNames[index] : "?";
}
#endif

// What "Built-in" actually DOES for a gesture, named the way the per-button rows
// name theirs — a row reading "Built-in" tells the reader nothing about whether
// leaving it alone means a page turn or means silence.
//
// Mode-dependent on purpose, and that is the whole value: with Swipe reading
// controls on, a leftward swipe IS the page turn, and with them off the same
// gesture does nothing at all. The list is rebuilt whenever the settings screen
// is opened, so the label follows Touch Reading Controls as the user changes it.
//
// Only the four gestures the reader already acts on can say anything else: this
// mirrors ReaderUtils::detectTouchPageTurn and isTouchMenuGesture, and has to be
// kept honest against them.
inline StrId builtinLabelFor(const Gesture gesture) {
  const uint8_t mode = SETTINGS.touchReaderControls;
  const bool readerTouchOn = mode != CrossPointSettings::TOUCH_READER_OFF;
  const bool tapTurnsPages =
      mode == CrossPointSettings::TOUCH_READER_ON || mode == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
  const bool invertedTaps = mode == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
  const bool swipeTurnsPages = mode == CrossPointSettings::TOUCH_READER_SWIPE;

  switch (gesture) {
    case Gesture::SwipeLeft:
      return swipeTurnsPages ? StrId::STR_BTN_DEF_NEXT_PAGE : StrId::STR_BTN_DEF_NOTHING;
    case Gesture::SwipeRight:
      return swipeTurnsPages ? StrId::STR_BTN_DEF_PREV_PAGE : StrId::STR_BTN_DEF_NOTHING;
    case Gesture::SwipeUp:
    case Gesture::SwipeDown: {
      // One edge opens the reader menu, and which one depends on whether the
      // board has a light — see MappedInputManager::wasMenuGesture(). Only the
      // reading controls gate it; unlike the centre tap it does not consult
      // tapForReaderMenu.
      //
      // Said as "Reader Menu" on the direction that owns the edge, because that
      // is what leaving the row alone gets you there. The gesture is still
      // bindable, and a binding still works everywhere except that edge.
      const Gesture menuSwipe = Frontlight.present() ? Gesture::SwipeUp : Gesture::SwipeDown;
      return (readerTouchOn && gesture == menuSwipe) ? StrId::STR_BTN_DEF_READER_MENU : StrId::STR_BTN_DEF_NOTHING;
    }
    case Gesture::TapLeft:
      if (!tapTurnsPages) return StrId::STR_BTN_DEF_NOTHING;
      return invertedTaps ? StrId::STR_BTN_DEF_NEXT_PAGE : StrId::STR_BTN_DEF_PREV_PAGE;
    case Gesture::TapRight:
      if (!tapTurnsPages) return StrId::STR_BTN_DEF_NOTHING;
      return invertedTaps ? StrId::STR_BTN_DEF_PREV_PAGE : StrId::STR_BTN_DEF_NEXT_PAGE;
    case Gesture::TapCentre:
      return (readerTouchOn && SETTINGS.tapForReaderMenu != 0) ? StrId::STR_BTN_DEF_READER_MENU
                                                               : StrId::STR_BTN_DEF_NOTHING;
    default:
      // Everything else was dead before gestures existed, so leaving it alone
      // genuinely does nothing.
      return StrId::STR_BTN_DEF_NOTHING;
  }
}

// The action bound to `gesture`, or BTN_DEFAULT when the user has left it alone.
inline uint8_t actionFor(const Gesture gesture) { return SETTINGS.*(BINDINGS[static_cast<size_t>(gesture)].field); }

// True when any gesture at all carries a user-chosen action. The classifier
// short-circuits on this: with nothing bound it must not so much as look at the
// contact, so that a device nobody has configured behaves bit-identically to one
// built before gestures existed.
inline bool anyBound() {
  for (const auto& binding : BINDINGS) {
    if (SETTINGS.*(binding.field) != CrossPointSettings::BTN_DEFAULT) return true;
  }
  return false;
}

// The tap and long-tap gestures for a zone. Two small switches rather than
// arithmetic on the enum, so reordering Gesture cannot silently misroute a tap.
inline Gesture tapGestureFor(const TapZones::Zone zone) {
  switch (zone) {
    case TapZones::Zone::Left:
      return Gesture::TapLeft;
    case TapZones::Zone::Right:
      return Gesture::TapRight;
    case TapZones::Zone::Top:
      return Gesture::TapTop;
    case TapZones::Zone::Bottom:
      return Gesture::TapBottom;
    case TapZones::Zone::Centre:
      break;
  }
  return Gesture::TapCentre;
}

inline Gesture longTapGestureFor(const TapZones::Zone zone) {
  switch (zone) {
    case TapZones::Zone::Left:
      return Gesture::LongTapLeft;
    case TapZones::Zone::Right:
      return Gesture::LongTapRight;
    case TapZones::Zone::Top:
      return Gesture::LongTapTop;
    case TapZones::Zone::Bottom:
      return Gesture::LongTapBottom;
    case TapZones::Zone::Centre:
      break;
  }
  return Gesture::LongTapCentre;
}

}  // namespace TouchGestures
