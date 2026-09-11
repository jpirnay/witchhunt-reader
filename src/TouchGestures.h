#pragma once

#include <I18nKeys.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

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
  // Vertical swipes are anchored to the left/right EDGE COLUMN they START in —
  // the left column adjusts brightness, the right one warmth — and exclude the
  // top and bottom bands, which belong to the light panel and the reader menu.
  // TapZones::edgeColumnFor() owns that geometry and explains the exclusion.
  // The names keep their original "Left"/"Right" spelling: a column IS on the
  // left, so renaming the enum, the settings fields and the JSON keys would
  // ripple through BINDINGS and ACTION_FIELDS for no behavioural gain.
  //
  // Horizontal swipes are not split — they are the page turn in Swipe mode, and
  // a page turn does not care where on the page it began.
  SwipeUpLeft,
  SwipeUpRight,
  SwipeDownLeft,
  SwipeDownRight,
  // The OUTWARD horizontal swipes: leftward in the left page-turn third, rightward
  // in the right one — travelling toward the edge the finger started at. Those
  // thirds already MEAN back and forward (a tap there turns one page), so flicking
  // the same way in the same zone jumping ten is one rule rather than two.
  // KOReader ships the same +/-10 magnitude on double_tap_left_side /
  // double_tap_right_side; the trigger differs because this firmware has no
  // double-tap detector, and an outward swipe costs two gestures rather than a
  // whole event class. Every inward and centre swipe keeps the plain page turn.
  SwipeLeftInLeft,
  SwipeRightInRight,
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
    {Gesture::SwipeUpLeft, &CrossPointSettings::gestSwipeUpLeft, StrId::STR_GEST_SWIPE_UP_LEFT,
     StrId::STR_GEST_SWIPE_GROUP, "gestSwipeUpLeft", false},
    {Gesture::SwipeUpRight, &CrossPointSettings::gestSwipeUpRight, StrId::STR_GEST_SWIPE_UP_RIGHT,
     StrId::STR_GEST_SWIPE_GROUP, "gestSwipeUpRight", false},
    {Gesture::SwipeDownLeft, &CrossPointSettings::gestSwipeDownLeft, StrId::STR_GEST_SWIPE_DOWN_LEFT,
     StrId::STR_GEST_SWIPE_GROUP, "gestSwipeDownLeft", false},
    {Gesture::SwipeDownRight, &CrossPointSettings::gestSwipeDownRight, StrId::STR_GEST_SWIPE_DOWN_RIGHT,
     StrId::STR_GEST_SWIPE_GROUP, "gestSwipeDownRight", false},
    {Gesture::SwipeLeftInLeft, &CrossPointSettings::gestSwipeLeftInLeft, StrId::STR_GEST_SWIPE_LEFT_IN_LEFT,
     StrId::STR_GEST_SWIPE_GROUP, "gestSwipeLeftInLeft", false},
    {Gesture::SwipeRightInRight, &CrossPointSettings::gestSwipeRightInRight, StrId::STR_GEST_SWIPE_RIGHT_IN_RIGHT,
     StrId::STR_GEST_SWIPE_GROUP, "gestSwipeRightInRight", false},
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
      "swipe-left",      "swipe-right",      "swipe-up-left",      "swipe-up-right",
      "swipe-down-left", "swipe-down-right", "swipe-left-in-left", "swipe-right-in-right",
      "tap-left",        "tap-right",        "tap-centre",         "tap-top",
      "tap-bottom",      "longtap-left",     "longtap-right",      "longtap-centre",
      "longtap-top",     "longtap-bottom",   "pinch-in",           "pinch-out",
      "rotate-cw",       "rotate-ccw",
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
    case Gesture::SwipeLeftInLeft:
      // Left alone these are the plain page-turn swipe, because that is what the
      // contact falls through to: an unbound gesture is never claimed, so the
      // reader's own detectTouchPageTurn sees it as an ordinary horizontal swipe.
      return swipeTurnsPages ? StrId::STR_BTN_DEF_NEXT_PAGE : StrId::STR_BTN_DEF_NOTHING;
    case Gesture::SwipeRightInRight:
      return swipeTurnsPages ? StrId::STR_BTN_DEF_PREV_PAGE : StrId::STR_BTN_DEF_NOTHING;
    // SwipeDownLeft/SwipeDownRight now fall through to NOTHING below, and that is a
    // correction rather than a regression. They used to claim the reader menu, because a
    // down-swipe starting at the TOP EDGE opened it and these rows fired anywhere in
    // their half — so the row overlapped the built-in and had to name it.
    //
    // Since they are anchored to the edge COLUMNS, which exclude the top band
    // (TapZones::edgeColumnFor), they no longer overlap it at all: a top-edge down-swipe
    // is the reader's own isTouchMenuGesture and never reaches this table.
    //
    // That also fixes a live bug. The shipped gestSwipeDownRight = Light Dimmer used to
    // claim a top-edge down-swipe in the right half and suppressTouchContact() it, which
    // zeroes the snapshot wasSwipe() reads — so the menu swipe silently did nothing on
    // half the screen. It works on both halves again.
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
  return std::any_of(std::begin(BINDINGS), std::end(BINDINGS), [](const Binding& binding) {
    return SETTINGS.*(binding.field) != CrossPointSettings::BTN_DEFAULT;
  });
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
