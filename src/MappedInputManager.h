#pragma once

#include <HalGPIO.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  // Screen orientation as the input layer sees it. Mirrors GfxRenderer::Orientation value for
  // value; it is duplicated rather than included because the input layer sits below the renderer
  // and must not depend on it. main.cpp installs the provider that bridges the two.
  enum class ScreenOrientation : uint8_t { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  // A movement direction *on screen*, as the reader perceives it while holding the device however
  // they hold it. Which physical button produces it depends on the orientation — see buttonFor().
  enum class Direction : uint8_t { Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  // Hint text for the two side buttons, in physical order: `up` belongs on BTN_UP, `down` on
  // BTN_DOWN. Which movement each of them performs depends on the orientation — see mapHints().
  struct SideLabels {
    const char* up;
    const char* down;
  };

  // Every hint a screen draws, already routed to the physical buttons that perform them.
  struct Hints {
    Labels front;
    SideLabels side;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  // Landscape CCW renders the front-button strip bottom-to-top, so the button that is
  // physically *above* the other is the one portrait treats as "next/down". Without a
  // swap the on-screen hints read "Down" above "Up" and pressing the upper button moves
  // the selection down (issue #87). Both the logical Left/Right roles and their hint text
  // follow the strip order — logical buttons survive orientation, raw indices do not.
  //
  // The provider is installed once at startup (main.cpp) and queried live, so the orientation
  // can never go stale against a renderer whose orientation changed. It must answer for the
  // orientation the *user* is holding, which is why it reads GfxRenderer::getHeldOrientation():
  // rendering runs on its own task, and the themes flip the renderer to Portrait mid-pass to put
  // the hint strips on the panel edge. Sampling plain getOrientation() from loop() therefore
  // catches Portrait now and then and resolves a direction to the wrong physical button — which
  // showed up as a landscape page jump occasionally followed by a stray single step.
  static void setOrientationProvider(ScreenOrientation (*provider)());
  static ScreenOrientation screenOrientation();
  static bool isVerticalStripReversed();

  // ---- Logical (orientation-aware) directions ---------------------------------
  //
  // The four front buttons sit on one edge of the panel and the two side buttons on another, so
  // rotating the device rotates what each of them *means*. In portrait the front strip runs
  // horizontally (Left/Right) and the side buttons vertically (Up/Down); hold the device in
  // landscape and those two axes trade places. Navigation must follow the screen, not the panel:
  //
  //   orientation | logical Left | logical Right | logical Up  | logical Down
  //   portrait    | phys Left    | phys Right    | phys Up     | phys Down
  //   landscape CW| phys Down    | phys Up       | phys Left   | phys Right
  //   inverted    | phys Right   | phys Left     | phys Down   | phys Up
  //   landscape CCW| phys Up     | phys Down     | phys Right  | phys Left
  //
  // Use these ONLY for movement and navigation (list stepping/paging, cursors, selectors). Yes/no
  // prompts, the reader's page turns and the generic short/double/long press dispatch keep their
  // fixed physical buttons: those are gestures on a known button, not travel across a screen.
  [[nodiscard]] static Button buttonFor(Direction direction) { return buttonFor(screenOrientation(), direction); }
  // Same, for an orientation named outright. Pure and compile-time evaluable, so the whole 4x4
  // table is checked by static_assert in MappedInputManager.cpp.
  [[nodiscard]] static constexpr Button buttonFor(const ScreenOrientation orientation, const Direction direction) {
    // mapButton()/rawIndex() run applyStripOrder() on their way to the hardware index, and it is an
    // involution (it only ever swaps Left and Right). Applying it here therefore *cancels* the one
    // downstream, which is exactly what we want: the geometric answer is already in physical terms,
    // so it must reach the hardware untouched, while every non-direction caller keeps the reversal.
    return applyStripOrder(geometricButtonFor(orientation, direction),
                           orientation == ScreenOrientation::LandscapeCounterClockwise);
  }
  [[nodiscard]] bool wasLogicalPressed(Direction direction) const { return wasPressed(buttonFor(direction)); }
  [[nodiscard]] bool wasLogicalReleased(Direction direction) const { return wasReleased(buttonFor(direction)); }
  [[nodiscard]] bool isLogicalPressed(Direction direction) const { return isPressed(buttonFor(direction)); }
  // True when `button` (e.g. a ButtonEventManager event's button) is the one that currently moves
  // the selection in `direction`.
  [[nodiscard]] static bool isDirection(const Button button, const Direction direction) {
    return button == buttonFor(direction);
  }

  // The two front-strip buttons that are not Back/Confirm, in the order they appear ON SCREEN:
  // `frontStripPrevious()` is the leftmost in portrait and the topmost in landscape. For actions
  // that must never leave the front strip — a yes/no prompt, a viewer's two labelled commands —
  // because that is the only strip their hints are drawn on. mapLabels() hands its `previous` /
  // `next` text to exactly these two, so a handler written against them can never end up on a
  // different button than its own label.
  [[nodiscard]] static Button frontStripPrevious() { return buttonFor(frontStripDirections().previous); }
  [[nodiscard]] static Button frontStripNext() { return buttonFor(frontStripDirections().next); }

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
  // Front-strip hints for a screen that labels only the front buttons. `previous`/`next` name the
  // movement the strip performs — which physical pair that is, and in which order, follows the
  // orientation.
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Both hint strips for a screen that labels all six buttons. `left`/`right` are whatever is bound
  // to logical Left/Right (a list's page jump, a screen's rename/delete), `up`/`down` whatever is
  // bound to logical Up/Down (usually the step). Rotating the device moves a pair from the front
  // strip to the side buttons and back; the hints move with it, so a label is always drawn beside
  // the button that performs it.
  Hints mapHints(const char* back, const char* confirm, const char* left, const char* right, const char* up,
                 const char* down) const;
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
  static ScreenOrientation (*orientationProvider)();

  // Left/Right swap when the front-button strip runs bottom-to-top on screen, so
  // "previous" always sits above "next". The side buttons (Up/Down and their
  // PageBack/PageForward aliases) are on a different edge and never swap.
  static Button applyStripOrder(Button button);
  static constexpr Button applyStripOrder(const Button button, const bool stripReversed) {
    if (!stripReversed) {
      return button;
    }
    // Only the four front buttons sit on the reversed strip. Up/Down (and their
    // PageBack/PageForward aliases) are the side buttons on a different edge — their
    // physical order is unchanged by rotation, so they must not swap.
    switch (button) {
      case Button::Left:
        return Button::Right;
      case Button::Right:
        return Button::Left;
      default:
        return button;
    }
  }

  // The table in the comment above buttonFor(), before applyStripOrder() has had its say. Pure
  // geometry: which button the reader's finger reaches for when they mean `direction`, given how
  // they are holding the device. Knows nothing of the strip order or the settings remap.
  static constexpr Button geometricButtonFor(const ScreenOrientation orientation, const Direction direction) {
    switch (orientation) {
      case ScreenOrientation::Portrait:
        switch (direction) {
          case Direction::Left:
            return Button::Left;
          case Direction::Right:
            return Button::Right;
          case Direction::Up:
            return Button::Up;
          case Direction::Down:
            return Button::Down;
        }
        break;
      case ScreenOrientation::LandscapeClockwise:
        switch (direction) {
          case Direction::Left:
            return Button::Down;
          case Direction::Right:
            return Button::Up;
          case Direction::Up:
            return Button::Left;
          case Direction::Down:
            return Button::Right;
        }
        break;
      case ScreenOrientation::PortraitInverted:
        switch (direction) {
          case Direction::Left:
            return Button::Right;
          case Direction::Right:
            return Button::Left;
          case Direction::Up:
            return Button::Down;
          case Direction::Down:
            return Button::Up;
        }
        break;
      case ScreenOrientation::LandscapeCounterClockwise:
        switch (direction) {
          case Direction::Left:
            return Button::Up;
          case Direction::Right:
            return Button::Down;
          case Direction::Up:
            return Button::Right;
          case Direction::Down:
            return Button::Left;
        }
        break;
    }
    return Button::Down;
  }

  // The two logical directions a button pair carries, named after what they do to a list.
  struct DirectionPair {
    Direction previous;
    Direction next;
  };
  // In portrait the front strip runs horizontally (logical Left/Right) and the side buttons
  // vertically (logical Up/Down); in landscape the panel edges have traded places and so do
  // these. Which of the two pairs steps and which pages is the caller's business, not ours.
  static DirectionPair frontStripDirections();
  static DirectionPair sideButtonDirections();

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
