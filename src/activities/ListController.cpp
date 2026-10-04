#include "ListController.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include <cstdio>

#include "components/UITheme.h"

namespace {

// How often a held Left/Right keeps paging once its long press has fired.
constexpr unsigned long kPageRepeatMs = 500;

// One gesture, one definition of how quick a double press is.
static_assert(ListGrammar::kDoubleTapMs == ButtonEventManager::DOUBLE_WINDOW_MS,
              "a list double-tap uses the button manager's double-click window");

// "«" and "»": the page glyphs that open the front Left/Right labels.
constexpr const char* kPrevGlyph = "\xC2\xAB";
constexpr const char* kNextGlyph = "\xC2\xBB";

bool hostSelectable(const void* ctx, const int row) { return static_cast<const ListHost*>(ctx)->listSelectable(row); }

ListGrammar::Press pressFor(const ButtonEventManager::PressType type) {
  switch (type) {
    case ButtonEventManager::PressType::Long:
      return ListGrammar::Press::Long;
    case ButtonEventManager::PressType::Double:
      return ListGrammar::Press::Double;
    case ButtonEventManager::PressType::Short:
      break;
  }
  return ListGrammar::Press::Short;
}

// "« Search", "» Down", or the glyph alone for a declared side that does not apply. The glyph goes
// first because hint boxes cut long labels off at the end.
void composeFront(char* out, const size_t size, const char* glyph, const ListGrammar::FrontLabel label,
                  const StrId action, const StrId stepLabel) {
  switch (label) {
    case ListGrammar::FrontLabel::PageOnly:
      snprintf(out, size, "%s", glyph);
      return;
    case ListGrammar::FrontLabel::Action:
      snprintf(out, size, "%s %s", glyph, I18n::getInstance().get(action));
      return;
    case ListGrammar::FrontLabel::Step:
      snprintf(out, size, "%s %s", glyph, I18n::getInstance().get(stepLabel));
      return;
  }
}

}  // namespace

ListController::ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host, int& selection,
                               const ListDeclaration& declaration)
    : input(input), events(events), host(host), selection(selection), declaration(declaration) {}

ListGrammar::Shape ListController::shape() const {
  ListGrammar::Shape s;
  s.leftDeclared = declaration.left.declared;
  s.rightDeclared = declaration.right.declared;
  s.confirmLong = declaration.confirmLong;
  s.tabbed = declaration.tabbed;
  return s;
}

ListGrammar::Availability ListController::availability() const {
  ListGrammar::Availability a;
  a.left = declaration.left.declared && host.listActionAvailable(ListGrammar::Side::Left, selection);
  a.right = declaration.right.declared && host.listActionAvailable(ListGrammar::Side::Right, selection);
  return a;
}

ListGrammar::Rows ListController::rows() const {
  ListGrammar::Rows r;
  r.count = host.listCount();
  r.pageRows = host.listPageRows();
  r.selectable = &hostSelectable;
  r.ctx = &host;
  return r;
}

bool ListController::keyFor(const Button button, ListGrammar::Key& key) {
  using Direction = MappedInputManager::Direction;
  if (button == Button::Confirm) {
    key = ListGrammar::Key::Confirm;
  } else if (button == Button::Back) {
    key = ListGrammar::Key::Back;
  } else if (MappedInputManager::isDirection(button, Direction::Up)) {
    key = ListGrammar::Key::Up;
  } else if (MappedInputManager::isDirection(button, Direction::Down)) {
    key = ListGrammar::Key::Down;
  } else if (MappedInputManager::isDirection(button, Direction::Left)) {
    key = ListGrammar::Key::Left;
  } else if (MappedInputManager::isDirection(button, Direction::Right)) {
    key = ListGrammar::Key::Right;
  } else {
    return false;
  }
  return true;
}

void ListController::update() {
  continuePageRepeat();

  ButtonEventManager::ButtonEvent event;
  while (events.consumeEvent(event)) {
    // PageBack/PageForward are the reader's names for the Up/Down keys: one press emits an event
    // under each name, and a list answers to Up/Down only.
    if (event.button == Button::PageBack || event.button == Button::PageForward) continue;

    ListGrammar::Key key = ListGrammar::Key::Confirm;
    if (!keyFor(event.button, key)) {
      lastTapPressMs = 0;  // a press between two taps ends the pair
      host.onListOtherEvent(event);
      continue;
    }

    const auto press = pressFor(event.type);
    const bool vertical = key == ListGrammar::Key::Up || key == ListGrammar::Key::Down;
    if (vertical && press != ListGrammar::Press::Long) {
      tapVertical(key, press, event.pressMs);
      continue;
    }
    // Any other press ends a double-tap in progress.
    lastTapPressMs = 0;

    const bool side = key == ListGrammar::Key::Left || key == ListGrammar::Key::Right;
    const auto result =
        ListGrammar::commandFor(key, press, shape(), side ? availability() : ListGrammar::Availability{});
    if (side && event.type == ButtonEventManager::PressType::Long) {
      repeating = true;
      repeatButton = event.button;
      repeatDirection = key == ListGrammar::Key::Left ? -1 : 1;
      repeatSinceMs = millis();
    }
    if (!apply(result)) return;
  }
}

bool ListController::apply(const ListGrammar::Result result) {
  using ListGrammar::Command;
  switch (result.command) {
    case Command::None:
      return true;
    case Command::StepPrev:
    case Command::StepNext: {
      const auto r = rows();
      int row = selection;
      for (int i = 0; i < result.times; ++i)
        row = ListGrammar::step(r, row, result.command == Command::StepNext ? 1 : -1);
      moveTo(row);
      return true;
    }
    case Command::PagePrev:
    case Command::PageNext:
      moveTo(ListGrammar::page(rows(), selection, result.command == Command::PageNext ? 1 : -1));
      return true;
    case Command::First:
      moveTo(ListGrammar::first(rows()));
      return true;
    case Command::Last:
      moveTo(ListGrammar::last(rows()));
      return true;
    default:
      break;
  }

  // The screen may leave this list, so stop here and leave later presses queued
  // for the next tick, where the new state takes them in order.
  repeating = false;
  switch (result.command) {
    case Command::TabPrev:
      host.onListTab(-1);
      break;
    case Command::TabNext:
      host.onListTab(1);
      break;
    case Command::Activate:
      host.onListActivate(selection, false);
      break;
    case Command::ActivateLong:
      host.onListActivate(selection, true);
      break;
    case Command::Back:
      host.onListBack();
      break;
    case Command::Home:
      host.onListHome();
      break;
    case Command::LeftAction:
      host.onListAction(ListGrammar::Side::Left, selection);
      break;
    case Command::RightAction:
      host.onListAction(ListGrammar::Side::Right, selection);
      break;
    default:
      break;
  }
  return false;
}

void ListController::moveTo(const int row) {
  if (row == selection) return;
  selection = row;
  host.onListSelectionChanged();
}

void ListController::tapVertical(const ListGrammar::Key key, const ListGrammar::Press press,
                                 const unsigned long pressMs) {
  const auto r = rows();
  const int direction = key == ListGrammar::Key::Down ? 1 : -1;

  // A Double event is a whole double-tap in one: it arrives only when the user bound a double
  // action to the page-turn keys and it fell through to the list.
  if (press == ListGrammar::Press::Double) {
    lastTapPressMs = 0;
    moveTo(ListGrammar::fitsOnePage(r) ? ListGrammar::step(r, ListGrammar::step(r, selection, direction), direction)
                                       : ListGrammar::page(r, selection, direction));
    return;
  }

  // The first tap of the pair has already stepped. Page from where it started instead, so the pair
  // moves one page, not a page and a row. A third tap starts a new pair.
  if (ListGrammar::completesDoubleTap(key, pressMs, lastTapKey, lastTapPressMs, r)) {
    lastTapPressMs = 0;
    moveTo(ListGrammar::page(r, rowBeforeTap, direction));
    return;
  }

  rowBeforeTap = selection;
  lastTapKey = key;
  lastTapPressMs = pressMs;
  moveTo(ListGrammar::step(r, selection, direction));
}

void ListController::continuePageRepeat() {
  if (!repeating) return;
  // An injected long press (a long tap on a hint box) never holds the live level, so it pages once.
  if (!input.isPressed(repeatButton)) {
    repeating = false;
    return;
  }
  const unsigned long now = millis();
  if (now - repeatSinceMs < kPageRepeatMs) return;
  repeatSinceMs = now;
  moveTo(ListGrammar::page(rows(), selection, repeatDirection));
}

void ListController::page(const int direction) {
  repeating = false;
  lastTapPressMs = 0;
  moveTo(ListGrammar::page(rows(), selection, direction));
}

ListRowTap::Result ListController::tapRow(const int row) {
  lastTapPressMs = 0;
  const int count = host.listCount();
  if (row >= 0 && row < count && !host.listSelectable(row)) return ListRowTap::Result::Rejected;
  return ListRowTap::apply(row, count, selection);
}

void ListController::drawHints(GfxRenderer& renderer, const char* backLabel, const char* confirmLabel) const {
  const auto labels = ListGrammar::labelsFor(shape(), availability());
  char left[48];
  char right[48];
  composeFront(left, sizeof(left), kPrevGlyph, labels.left, declaration.left.label, StrId::STR_DIR_UP);
  composeFront(right, sizeof(right), kNextGlyph, labels.right, declaration.right.label, StrId::STR_DIR_DOWN);
  const auto hints = input.mapHints(backLabel, confirmLabel, left, right, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);
}
