#pragma once

#include <I18n.h>

#include "ButtonEventManager.h"
#include "ListRowTap.h"
#include "MappedInputManager.h"
#include "util/ListGrammar.h"

class GfxRenderer;

// The screen's half of a ListController: what the controller asks of the list, and what it tells
// the screen. A screen with two lists (OPDS: catalog and format picker) gives each its own host.
class ListHost {
 public:
  virtual int listCount() const = 0;
  // Rows the screen draws per page, as its last render published them. Called on the loop task: never
  // measure from the renderer's live orientation here.
  virtual int listPageRows() const = 0;
  virtual bool listSelectable(int /*row*/) const { return true; }
  // Whether a declared Left/Right action applies to `row` right now. Asked for declared sides only.
  virtual bool listActionAvailable(ListGrammar::Side /*side*/, int /*row*/) const { return true; }

  virtual void onListSelectionChanged() = 0;
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
// same way (docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md). A member of the
// screen, not a base class; it holds no heap and allocates nothing per tick or per render.
class ListController {
 public:
  ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host, int& selection,
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

 private:
  using Button = MappedInputManager::Button;

  MappedInputManager& input;
  ButtonEventManager& events;
  ListHost& host;
  int& selection;
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
  // A short or double Up/Down press: one step, or a page when it completes a double-tap.
  void tapVertical(ListGrammar::Key key, ListGrammar::Press press, unsigned long pressMs);
  void continuePageRepeat();
};

// The spec's budget for a controller per list screen.
static_assert(sizeof(ListController) <= 100, "ListController must stay under 100 bytes");
