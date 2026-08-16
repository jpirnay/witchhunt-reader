#pragma once

#include <HalGPIO.h>

class GfxRenderer;
namespace freeink {
namespace ui {
enum class ScreenEdge : uint8_t;
}
}  // namespace freeink

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  // The renderer is held for LIVE orientation: the touch transform must follow
  // what is actually on screen, not the persisted reader setting, or taps land
  // rotated whenever the reader is open. Same discipline as
  // setStripReversedPredicate below.
  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

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
  const GfxRenderer& getRenderer() const { return renderer; }

  // --- Touch ----------------------------------------------------------------
  // Everything here reports LOGICAL screen pixels, already mapped through the
  // renderer's live orientation — the layer above never sees panel-native
  // coordinates. Names and signatures are verbatim from upstream/develop so
  // screens stay diff-comparable when they convert; see
  // docs/touch-input-migration-2026-08-14.md §1.
  //
  // All of these are inert on non-touch boards (the SDK's touch methods compile
  // to false), so callers need no #ifdefs.
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  // One-shot long-press from the SDK classifier, fired WHILE the finger is
  // still down. Consuming it suppresses the rest of the contact — its continued
  // hold and its release edge — so the ensuing lift can't also tap-dismiss
  // whatever the long-press opened. The SDK owns that latch and self-clears it.
  bool wasScreenLongPress(int& x, int& y) const;
  // Live contact position while the finger is down, with no tap-slop gate —
  // drag tracking.
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports).
  bool wasScreenTouchReleased() const;
  bool wasTapInRect(int x, int y, int width, int height) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers do not cover
  // (custom row heights, option prompts, menus). Down = a held tap-candidate is
  // on a row (move the selection highlight); Tap = a tap released on one
  // (activate). rowHeight limits the hit to the top rowHeight px of each step
  // (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  // Back = left-to-right swipe anchored at the left edge. Public so swipe-mode
  // page turns (reader) can exclude it from a plain SwipeDir::Right.
  bool wasBackGesture() const;
  // Home-key boards (X4 Pro) exit with a short Home-key tap; their bottom-edge
  // swipe is intentionally unused. Other touch boards keep the bottom-edge
  // gesture.
  bool wasHomeGesture() const;
  // A Home-key hold runs the configured long-press action in the reader.
  bool wasHomeKeyHold() const;
  bool wasMenuGesture() const;

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
  // Live orientation authority for the touch transform — see the constructor.
  const GfxRenderer& renderer;
  static bool (*stripReversedPredicate)();

  // SDK edge classification (fui::edgeSwipe) plus the shared decode; the
  // wrappers above give each edge its board meaning.
  bool wasEdgeSwipe(freeink::ui::ScreenEdge edge) const;
  bool wasTopEdgeDownSwipe() const;
  bool wasBottomEdgeUpSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical coords.
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;

  // Left/Right swap when the front-button strip runs bottom-to-top on screen, so
  // "previous" always sits above "next". The side buttons (Up/Down and their
  // PageBack/PageForward aliases) are on a different edge and never swap.
  static Button applyStripOrder(Button button);

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
