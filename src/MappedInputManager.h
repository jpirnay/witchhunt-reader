#pragma once

#include <HalGPIO.h>
#include <TouchTransform.h>

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

  // The renderer is held for LIVE orientation: the touch transform must follow
  // what is actually on screen, not the persisted reader setting, or taps land
  // rotated whenever the reader is open. Same discipline as
  // setOrientationProvider below, and for the same reason.
  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

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
  // Same, resolved against an EXPLICIT orientation instead of the live one. For chrome that
  // is drawn in a fixed frame however the screen is rotated -- the button hint strip forces
  // Portrait for its draw, so hit-testing it means asking for the tap in Portrait too.
  // Consumes the tap exactly as wasScreenTapped() does. Takes touchtransform::Orientation
  // rather than GfxRenderer::Orientation so this header keeps its forward declaration; the
  // two enums are static_asserted to agree in GfxRenderer.cpp.
  bool wasScreenTappedIn(touchtransform::Orientation orientation, int& x, int& y) const;
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
  // Duration of the contact just ended, latched at release. Readers use it to
  // tell a tap from a deliberate hold on the same zone.
  unsigned long lastTouchHeldMs() const;
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

  // The same interaction against the list the themes actually painted, rather than against
  // geometry the caller re-derives. `index` is an ITEM index, already resolved through the
  // page or scroll offset, so a caller assigns it straight to its selection.
  //
  // Prefer this to rowTouch() for anything drawn by GUI.drawList: rowTouch cannot express a
  // wrapped list (its rows differ in height), and a screen computing its own band is a second
  // copy of the theme's layout rule. rowTouch stays for the bands no theme draws — option
  // prompts, custom row heights, menus.
  //
  // Returns None when no list was recorded, so it is inert on every screen that draws none.
  RowTouch listTouch(int& index) const;

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

  // Synthesize a press of a RAW hardware button. For inputs that stand in for a button
  // without being one -- a tap on the on-screen hint strip. Raw rather than logical so
  // the user's button remapping still applies, exactly as for the physical key.
  void injectRawPress(uint8_t rawButtonIndex) const;

  // Drain one queued raw button edge from the background sampler (FIFO). Returns
  // false when empty. Used by ButtonEventManager to drive its press-type FSM.
  bool popRawEdge(HalGPIO::ButtonEdge& out) const { return gpio.popButtonEdge(out); }
  // Drop all queued/pending raw edges (activity transitions).
  void flushRawEdges() const { gpio.flushButtonEdges(); }

 private:
  HalGPIO& gpio;
  // Live orientation authority for the touch transform — see the constructor.
  const GfxRenderer& renderer;
  static ScreenOrientation (*orientationProvider)();

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
