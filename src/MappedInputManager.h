#pragma once

#include <HalGPIO.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  // Landscape CCW renders the front-button strip bottom-to-top, so the button that is
  // physically *above* the other is the one portrait treats as "next/down". Without a
  // swap the on-screen hints read "Down" above "Up" and pressing the upper button moves
  // the selection down (issue #87). Both the logical Left/Right roles and their hint text
  // follow the strip order — logical buttons survive orientation, raw indices do not.
  //
  // The predicate is installed once at startup (main.cpp) and queried live, so the flag
  // can never go stale against a renderer whose orientation changed. It must answer for
  // the orientation the *user* is holding, which is why the themes' transient flip to
  // Portrait while drawing the hint strip is not allowed to feed it.
  static void setStripReversedPredicate(bool (*predicate)());
  static bool isVerticalStripReversed();

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  // True if any press has been sampled since the last update(). Safe to call
  // mid-stall from within loop() to yield expensive work to button input.
  bool hasPendingInput() const { return gpio.hasPendingInput(); }
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // Raw HalGPIO button index a logical button currently maps to. Used to route a
  // raw edge from the sampler queue to the logical button(s) it drives.
  uint8_t rawIndex(Button button) const;

  // Drain one queued raw button edge from the background sampler (FIFO). Returns
  // false when empty. Used by ButtonEventManager to drive its press-type FSM.
  bool popRawEdge(HalGPIO::ButtonEdge& out) const { return gpio.popButtonEdge(out); }
  // Drop all queued/pending raw edges (activity transitions).
  void flushRawEdges() const { gpio.flushButtonEdges(); }

 private:
  HalGPIO& gpio;
  static bool (*stripReversedPredicate)();

  // Left/Right swap when the front-button strip runs bottom-to-top on screen, so
  // "previous" always sits above "next". The side buttons (Up/Down and their
  // PageBack/PageForward aliases) are on a different edge and never swap.
  static Button applyStripOrder(Button button);

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
