#include "ListGrammar.h"

#include <algorithm>

namespace ListGrammar {
namespace {

bool isSelectable(const Rows& rows, const int row) {
  return rows.selectable == nullptr || rows.selectable(rows.ctx, row);
}

int clampRow(const Rows& rows, const int row) { return row < 0 ? 0 : (row >= rows.count ? rows.count - 1 : row); }

// The rows below a list's lead positions, seen as a list of their own: row i is position i + lead.
struct LeadShift {
  const Rows* outer;
  int lead;
};

bool shiftedSelectable(const void* ctx, const int row) {
  const auto* shift = static_cast<const LeadShift*>(ctx);
  return isSelectable(*shift->outer, row + shift->lead);
}

Result once(const Command command) { return {command, 1}; }

// Short Left/Right: the default step, or, once the screen has overloaded the pair, the declared
// action — none when it does not apply. Overloading either side takes the step off both.
Result shortSide(const Side side, const Press press, const Shape& shape, const Availability& available) {
  if (!shape.leftDeclared && !shape.rightDeclared) {
    const Command stepCommand = side == Side::Left ? Command::StepPrev : Command::StepNext;
    return {stepCommand, static_cast<uint8_t>(press == Press::Double ? 2 : 1)};
  }
  const bool declared = side == Side::Left ? shape.leftDeclared : shape.rightDeclared;
  const bool applies = side == Side::Left ? available.left : available.right;
  if (!declared || !applies) return {};
  return once(side == Side::Left ? Command::LeftAction : Command::RightAction);
}

}  // namespace

Result commandFor(const Key key, const Press press, const Shape& shape, const Availability& available) {
  const bool isLong = press == Press::Long;
  switch (key) {
    // Short and double Up/Down are the caller's tapVertical(): single steps and double-tap paging.
    case Key::Up:
      return isLong ? once(shape.tabbed ? Command::TabPrev : Command::First) : Result{};
    case Key::Down:
      return isLong ? once(shape.tabbed ? Command::TabNext : Command::Last) : Result{};
    case Key::Left:
      if (isLong) return once(Command::PagePrev);
      return shortSide(Side::Left, press, shape, available);
    case Key::Right:
      if (isLong) return once(Command::PageNext);
      return shortSide(Side::Right, press, shape, available);
    case Key::Confirm:
      return once(isLong && shape.confirmLong ? Command::ActivateLong : Command::Activate);
    case Key::Back:
      return once(isLong ? Command::Home : Command::Back);
  }
  return {};
}

Labels labelsFor(const Shape& shape, const Availability& available) {
  if (!shape.leftDeclared && !shape.rightDeclared) return {};
  const auto label = [](const bool declared, const bool applies) {
    return declared && applies ? FrontLabel::Action : FrontLabel::PageOnly;
  };
  Labels labels;
  labels.left = label(shape.leftDeclared, available.left);
  labels.right = label(shape.rightDeclared, available.right);
  return labels;
}

int step(const Rows& rows, const int from, const int direction) {
  if (rows.count <= 0) return from;
  const int start = clampRow(rows, from);
  int row = start;
  for (int tried = 0; tried < rows.count; ++tried) {
    row = ((row + direction) % rows.count + rows.count) % rows.count;
    if (isSelectable(rows, row)) return row;
  }
  return from;
}

PageTurn page(const Rows& rows, const int from, const int direction) {
  if (rows.count <= 0) return {from, 0};
  // Lead positions (a tab bar) are not lines of the page: page the rows below them alone, then map
  // the turn back. A rows top of 0 shows the lead as well, so it maps back to position 0.
  const int lead = std::min(rows.lead, rows.count);
  if (lead > 0) {
    const LeadShift shift{&rows, lead};
    Rows inner;
    inner.count = rows.count - lead;
    inner.top = rows.top >= lead ? rows.top - lead : 0;
    inner.drawn = rows.top < lead ? rows.drawn - (lead - rows.top) : rows.drawn;
    inner.selectable = &shiftedSelectable;
    inner.ctx = &shift;
    inner.pageAligned = rows.pageAligned;
    if (inner.count <= 0) return {from, rows.top};
    PageTurn turn;
    if (from < lead) {
      // On the lead: back has nowhere to go; forward pages from the rows' own top.
      if (direction < 0) return {from, rows.top};
      turn = page(inner, inner.top, 1);
    } else {
      turn = page(inner, from - lead, direction);
    }
    // Rows with nothing selectable hand back the row the turn started from: stay where we are.
    if (!isSelectable(rows, turn.row + lead)) return {from, rows.top};
    return {turn.row + lead, turn.top == 0 ? 0 : turn.top + lead};
  }
  const int window = rows.drawn > 0 ? rows.drawn : 1;
  const int start = clampRow(rows, from);
  const int top = clampRow(rows, rows.top);
  // From inside the drawn window a page moves the window and the selection keeps its line on it;
  // from outside it (the window is a render old) the page is measured from the selection itself.
  const bool inWindow = start >= top && start < top + window;
  const int base = inWindow ? top : start;
  const int offset = inWindow ? start - top : 0;
  // The screen never moves past the ends: the last screen is the last full window, or on a screen
  // laid out a page at a time the last page. A forward turn never moves it back, even from a base
  // already past that.
  const int maxTop = rows.pageAligned ? (rows.count - 1) / window * window : std::max(0, rows.count - window);
  int newTop;
  if (direction > 0) {
    newTop = base + window;
    if (newTop > maxTop) newTop = std::max(maxTop, base);
  } else {
    newTop = std::max(0, base - window);
  }
  // A screen that cannot move any further sends the selection to that end instead.
  const bool moved = newTop != base;
  const int target = moved ? std::min(newTop + offset, rows.count - 1) : (direction > 0 ? rows.count - 1 : 0);
  // Settle on the first selectable row at or after the target, so a header never pushes the
  // selection onto another page; failing that, the nearest one before it.
  int row = target;
  while (row < rows.count && !isSelectable(rows, row)) ++row;
  if (row >= rows.count) {
    row = target - 1;
    while (row >= 0 && !isSelectable(rows, row)) --row;
  }
  if (row < 0) return {from, top};
  // Headers directly above the new top open the new screen with it. Forward, never back onto the
  // screen the turn left.
  if (moved) {
    while (newTop > 0 && !isSelectable(rows, newTop - 1) && (direction < 0 || newTop - 1 > base)) --newTop;
  }
  return {row, newTop};
}

int first(const Rows& rows) {
  for (int row = 0; row < rows.count; ++row) {
    if (isSelectable(rows, row)) return row;
  }
  return 0;
}

int last(const Rows& rows) {
  for (int row = rows.count - 1; row >= 0; --row) {
    if (isSelectable(rows, row)) return row;
  }
  return 0;
}

bool fitsOnePage(const Rows& rows) { return rows.count <= (rows.drawn > 0 ? rows.drawn : 1); }

bool completesDoubleTap(const Key key, const unsigned long pressMs, const Key previousKey,
                        const unsigned long previousPressMs, const Rows& rows) {
  if (key != Key::Up && key != Key::Down) return false;
  if (key != previousKey || previousPressMs == 0 || pressMs < previousPressMs) return false;
  if (fitsOnePage(rows)) return false;
  return pressMs - previousPressMs < kDoubleTapMs;
}

}  // namespace ListGrammar
