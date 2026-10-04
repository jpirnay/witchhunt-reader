#include "ListGrammar.h"

namespace ListGrammar {
namespace {

bool isSelectable(const Rows& rows, const int row) {
  return rows.selectable == nullptr || rows.selectable(rows.ctx, row);
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
  const uint8_t steps = press == Press::Double ? 2 : 1;
  switch (key) {
    case Key::Up:
      if (isLong) return once(shape.tabbed ? Command::TabPrev : Command::First);
      return {Command::StepPrev, steps};
    case Key::Down:
      if (isLong) return once(shape.tabbed ? Command::TabNext : Command::Last);
      return {Command::StepNext, steps};
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
  const int start = from < 0 ? 0 : (from >= rows.count ? rows.count - 1 : from);
  int row = start;
  for (int tried = 0; tried < rows.count; ++tried) {
    row = ((row + direction) % rows.count + rows.count) % rows.count;
    if (isSelectable(rows, row)) return row;
  }
  return from;
}

int page(const Rows& rows, const int from, const int direction) {
  if (rows.count <= 0) return from;
  const int perPage = rows.pageRows > 0 ? rows.pageRows : 1;
  const int start = from < 0 ? 0 : (from >= rows.count ? rows.count - 1 : from);
  const int currentPage = start / perPage;
  const int lastPage = (rows.count - 1) / perPage;
  int target = 0;
  if (direction > 0) {
    target = currentPage < lastPage ? (currentPage + 1) * perPage : rows.count - 1;
  } else {
    target = currentPage > 0 ? (currentPage - 1) * perPage : 0;
  }
  // Settle on the first selectable row at or after the target, so a header that opens a page
  // never pushes the selection onto another page; failing that, the nearest one before it.
  for (int row = target; row < rows.count; ++row) {
    if (isSelectable(rows, row)) return row;
  }
  for (int row = target - 1; row >= 0; --row) {
    if (isSelectable(rows, row)) return row;
  }
  return from;
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

bool fitsOnePage(const Rows& rows) { return rows.count <= (rows.pageRows > 0 ? rows.pageRows : 1); }

bool completesDoubleTap(const Key key, const unsigned long pressMs, const Key previousKey,
                        const unsigned long previousPressMs, const Rows& rows) {
  if (key != Key::Up && key != Key::Down) return false;
  if (key != previousKey || previousPressMs == 0 || pressMs < previousPressMs) return false;
  if (fitsOnePage(rows)) return false;
  return pressMs - previousPressMs < kDoubleTapMs;
}

}  // namespace ListGrammar
