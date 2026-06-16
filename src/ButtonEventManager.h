#pragma once

#include <Arduino.h>

#include "MappedInputManager.h"

// Forward declaration for the global accessor used by Activity.h.
// Defined in main.cpp alongside the ButtonEventManager instance.
class ButtonEventManager;
ButtonEventManager& globalButtonEvents();

// Classifies raw button edges into Short, Double, and Long press events.
//
// Edges are produced by the background sampler (HalGPIO) and queued with the
// millis() timestamp at which they were detected. update() drains that queue and
// drives a per-button state machine from each discrete edge, so a complete tap
// that began and ended while the loop task was busy (a single drained edge pair)
// is still classified correctly. The key latency rule:
//   - If no double-click action is configured for a button, Short fires immediately
//     on release (zero extra wait).
//   - If a double-click action IS configured, Short is delayed by DOUBLE_WINDOW_MS
//     to allow disambiguation.
//   - Long fires as soon as hold time >= LONG_PRESS_MS; release is not required.
//   - Double fires on the second release within DOUBLE_WINDOW_MS.
//
// Activities query consumeEvent() each loop tick to receive pending events.
// drain() resets all state machines — call it on activity transitions.

class ButtonEventManager {
 public:
  using Button = MappedInputManager::Button;

  enum class PressType { Short, Double, Long };

  struct ButtonEvent {
    Button button;
    PressType type;
  };

  // Timing constants (milliseconds)
  static constexpr unsigned long LONG_PRESS_MS = 1000;
  static constexpr unsigned long DOUBLE_WINDOW_MS = 300;

  explicit ButtonEventManager(MappedInputManager& input) : input(input) {}

  // Call once per main loop tick, after MappedInputManager::update().
  void update();

  // Returns the next pending event, or false if none. Call repeatedly until
  // false to drain all events for this tick.
  bool consumeEvent(ButtonEvent& out);

  // Reset all per-button FSMs. Call on activity transitions to prevent bleed-through.
  void drain();

  // Temporarily force double-click detection for a button (adds latency to Short press).
  // Call this in the Activity's transition setup or loop.
  void forceDoubleAction(Button button, bool enable = true) {
    if (enable) {
      forcedDoubleMask |= (1 << static_cast<int>(button));
    } else {
      forcedDoubleMask &= ~(1 << static_cast<int>(button));
    }
  }

  // Preserve a default event for activity processing after main loop dispatch.
  // This is used when the configured action is BTN_DEFAULT.
  void pushEventFront(Button button, PressType type);

  // Returns true while a button's first release is waiting for the
  // double-click decision window to expire (i.e. a Short is pending).
  bool isShortPending(Button button) const;

  // Returns true if a double-click action is configured for this button.
  // ButtonEventManager queries CrossPointSettings internally.
  bool hasDoubleAction(Button button) const;

  // Called by the global dispatcher (main.cpp) when a Long event is consumed for a
  // non-default action on a navigation button. Suppresses the release-based page turn
  // that would otherwise fire when the button is released after the long action.
  void markLongPressDispatched(Button button) { longPressDispatchedMask |= (1u << static_cast<int>(button)); }

  // Returns true if markLongPressDispatched was called for this button since the last
  // update(). detectPageTurn() uses this to skip wasReleased-based page turns.
  bool wasLongPressDispatched(Button button) const {
    return (longPressDispatchedMask & (1u << static_cast<int>(button))) != 0;
  }

 private:
  static constexpr int NUM_BUTTONS = 9;
  static constexpr Button ALL_BUTTONS[NUM_BUTTONS] = {
      Button::Back, Button::Confirm,  Button::Left,        Button::Right, Button::Up,
      Button::Down, Button::PageBack, Button::PageForward, Button::Power,
  };

  uint32_t forcedDoubleMask = 0;
  uint32_t longPressDispatchedMask = 0;

  enum class State { Idle, Pressed, ReleasedOnce, DoublePressed };

  struct PerButton {
    State state = State::Idle;
    unsigned long pressDownTime = 0;  // when the current (or first) press started
    unsigned long releaseTime = 0;    // when the first release happened (for double-click window)
  };

  PerButton buttons[NUM_BUTTONS];

  // Pending events ring buffer (small — at most one event per button per tick)
  static constexpr int EVENT_BUF = 16;
  ButtonEvent eventBuf[EVENT_BUF] = {};
  int eventHead = 0;
  int eventTail = 0;

  MappedInputManager& input;

  void pushEvent(Button button, PressType type);
  // Advance one button's FSM on a discrete press/release edge captured at time t.
  void applyEdge(int idx, Button btn, bool pressed, unsigned long t);
  // Advance one button's FSM on elapsed time (long-press-while-held, double-window
  // expiry), evaluated at `now` against whether the button is currently held.
  void applyTimeout(int idx, Button btn, unsigned long now, bool heldNow);
  static int buttonToIndex(Button button);
};
