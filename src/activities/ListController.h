#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

#include "ButtonEventManager.h"
#include "ListRowTap.h"
#include "MappedInputManager.h"
#include "util/ListGrammar.h"

class GfxRenderer;

// The rows a screen drew in its last render, from its top row: what a page is (spec R3).
struct ListWindow {
  int top = 0;
  int drawn = 1;
};

// The screen's half of a ListController: what the controller asks of the list, and what it tells
// the screen. A screen with two lists (OPDS: catalog and format picker) gives each its own host.
class ListHost {
 public:
  virtual int listCount() const = 0;
  // The window the last render drew. Published by the render and read here on the loop task: never
  // measure from the renderer's live orientation here.
  virtual ListWindow listWindow() const = 0;
  // Positions in front of row 0 that stay on screen above the rows (a tab bar); not page lines.
  virtual int listLeadPositions() const { return 0; }
  // Whether the screen lays its rows out a whole page at a time, each page at a multiple of drawn.
  virtual bool listPagesAligned() const { return false; }
  virtual bool listSelectable(int /*row*/) const { return true; }
  // Whether a declared Left/Right action applies to `row` right now. Asked for declared sides only.
  virtual bool listActionAvailable(ListGrammar::Side /*side*/, int /*row*/) const { return true; }

  // The selected row, and moving it; listSelect() also asks for the repaint.
  virtual int listSelected() const = 0;
  virtual void listSelect(int row) = 0;
  // A page turn: select `row` with the screen starting at `top`. A screen that lays its rows out a
  // page at a time (listPagesAligned()) gets there by selecting the row, since the grammar then
  // picks a row on the target page, hence the default.
  virtual void listShowPage(int row, int /*top*/) { listSelect(row); }

  virtual void onListActivate(int row, bool longPress) = 0;
  virtual void onListBack() = 0;
  virtual void onListHome() = 0;
  virtual void onListAction(ListGrammar::Side /*side*/, int /*row*/) {}
  virtual void onListTab(int /*direction*/) {}
  // A button the scheme does not use on a list (Power).
  virtual void onListOtherEvent(const ButtonEventManager::ButtonEvent& /*event*/) {}

 protected:
  ~ListHost() = default;
};

// A short Left or Right press the screen takes over from the default step.
struct ListAction {
  bool declared = false;
  StrId label = StrId::STR_DIR_UP;  // shown after the page glyph; unused when not declared
};

// What a list screen states once: its Left/Right pair, whether long Confirm has an action of its
// own, whether long Up/Down switch tabs.
struct ListDeclaration {
  ListAction left;
  ListAction right;
  bool confirmLong = false;
  bool tabbed = false;
};

// Runs one list's buttons, hint strips and touch through ListGrammar, so every list answers the
// same way (docs/design/list-input-harmonization.md). A member of the
// screen, not a base class; it holds no heap and allocates nothing per tick or per render.
class ListController {
 public:
  ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host,
                 const ListDeclaration& declaration);

  // Call from the screen's loop() while this list is on screen. It is the only reader of button
  // events there; it reads events until the screen acts on one, then leaves the rest queued for
  // the next tick, where the screen's new state takes them in order. Activity transitions drain.
  void update();
  // A vertical swipe over the list (Activity::pageList): -1 back, +1 forward.
  void page(int direction);
  // A tap on a row (Activity::selectListRow).
  ListRowTap::Result tapRow(int row);
  // Draws the bottom and the side hint strips; `backLabel` and `confirmLabel` are the screen's own.
  void drawHints(GfxRenderer& renderer, const char* backLabel, const char* confirmLabel) const;
  // Forgets a hold-to-repeat and a half-made double-tap: call when the list is (re)entered.
  void reset();

 private:
  using Button = MappedInputManager::Button;

  MappedInputManager& input;
  ButtonEventManager& events;
  ListHost& host;
  ListDeclaration declaration;

  // Hold-to-repeat paging after a long Left/Right, while that key stays down.
  bool repeating = false;
  Button repeatButton = Button::Right;
  int8_t repeatDirection = 1;
  unsigned long repeatSinceMs = 0;

  // The last Up/Down tap that stepped, so a second tap close behind it can turn the pair into a
  // page jump. lastTapPressMs 0 means there is no tap to pair with.
  ListGrammar::Key lastTapKey = ListGrammar::Key::Down;
  unsigned long lastTapPressMs = 0;
  int rowBeforeTap = 0;

  ListGrammar::Shape shape() const;
  ListGrammar::Availability availability() const;
  ListGrammar::Rows rows() const;
  static bool keyFor(Button button, ListGrammar::Key& key);
  // Applies one command. False once the screen has acted (it may have left this list): stop
  // reading events until the next tick.
  bool apply(ListGrammar::Result result);
  void moveTo(int row);
  // A page turn: select its row with the screen starting at its top, unless that changes nothing.
  void showPage(ListGrammar::PageTurn turn);
  // A short or double Up/Down press: one step, or a page when it completes a double-tap.
  void tapVertical(ListGrammar::Key key, ListGrammar::Press press, unsigned long pressMs);
  void continuePageRepeat();
};

// The spec's budget for a controller per list screen.
static_assert(sizeof(ListController) <= 100, "ListController must stay under 100 bytes");
