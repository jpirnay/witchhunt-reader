// The list button scheme (docs/design/list-input-harmonization.md §1):
// which command each button event means, what the front hint boxes say, and the row arithmetic.

#include <gtest/gtest.h>

#include <vector>

#include "util/ListGrammar.h"

namespace {

using ListGrammar::Availability;
using ListGrammar::Command;
using ListGrammar::FrontLabel;
using ListGrammar::Key;
using ListGrammar::Press;
using ListGrammar::Rows;
using ListGrammar::Shape;

Shape shape(bool left, bool right, bool confirmLong = false, bool tabbed = false) {
  Shape s;
  s.leftDeclared = left;
  s.rightDeclared = right;
  s.confirmLong = confirmLong;
  s.tabbed = tabbed;
  return s;
}

Availability available(bool left, bool right) {
  Availability a;
  a.left = left;
  a.right = right;
  return a;
}

const Shape kDefault = shape(false, false);
const Shape kBoth = shape(true, true);
const Availability kNone = available(false, false);
const Availability kAll = available(true, true);

void expectCommand(Key key, Press press, const Shape& s, const Availability& a, Command command, int times) {
  const auto result = ListGrammar::commandFor(key, press, s, a);
  EXPECT_EQ(static_cast<int>(result.command), static_cast<int>(command));
  EXPECT_EQ(result.times, times);
}

struct Mask {
  std::vector<bool> selectable;
};

bool maskSelectable(const void* ctx, const int row) { return static_cast<const Mask*>(ctx)->selectable[row]; }

Rows plainRows(int count, int drawn, int top = 0) {
  Rows rows;
  rows.count = count;
  rows.top = top;
  rows.drawn = drawn;
  return rows;
}

Rows maskedRows(const Mask& mask, int drawn, int top = 0) {
  Rows rows;
  rows.count = static_cast<int>(mask.selectable.size());
  rows.top = top;
  rows.drawn = drawn;
  rows.selectable = &maskSelectable;
  rows.ctx = &mask;
  return rows;
}

void expectTurn(const ListGrammar::PageTurn turn, int row, int top) {
  EXPECT_EQ(turn.row, row);
  EXPECT_EQ(turn.top, top);
}

}  // namespace

// --- Commands -------------------------------------------------------------------------------

TEST(ListGrammarCommand, UpDownStepOnEveryShape) {
  for (const Shape& s : {kDefault, kBoth, shape(false, false, false, true)}) {
    expectCommand(Key::Up, Press::Short, s, kAll, Command::StepPrev, 1);
    expectCommand(Key::Down, Press::Short, s, kAll, Command::StepNext, 1);
  }
}

TEST(ListGrammarCommand, UpDownLongJumpsToTheEnds) {
  expectCommand(Key::Up, Press::Long, kDefault, kNone, Command::First, 1);
  expectCommand(Key::Down, Press::Long, kDefault, kNone, Command::Last, 1);
  expectCommand(Key::Down, Press::Long, kBoth, kAll, Command::Last, 1);
}

TEST(ListGrammarCommand, UpDownLongSwitchesTabsOnTabbedLists) {
  const Shape tabbed = shape(false, false, false, true);
  expectCommand(Key::Up, Press::Long, tabbed, kNone, Command::TabPrev, 1);
  expectCommand(Key::Down, Press::Long, tabbed, kNone, Command::TabNext, 1);
}

TEST(ListGrammarCommand, LeftRightStepByDefault) {
  expectCommand(Key::Left, Press::Short, kDefault, kNone, Command::StepPrev, 1);
  expectCommand(Key::Right, Press::Short, kDefault, kNone, Command::StepNext, 1);
}

TEST(ListGrammarCommand, LeftRightLongAlwaysPages) {
  for (const Shape& s : {kDefault, kBoth, shape(false, true), shape(false, false, false, true)}) {
    expectCommand(Key::Left, Press::Long, s, kNone, Command::PagePrev, 1);
    expectCommand(Key::Right, Press::Long, s, kAll, Command::PageNext, 1);
  }
}

TEST(ListGrammarCommand, DeclaredPairRunsItsActions) {
  expectCommand(Key::Left, Press::Short, kBoth, kAll, Command::LeftAction, 1);
  expectCommand(Key::Right, Press::Short, kBoth, kAll, Command::RightAction, 1);
}

TEST(ListGrammarCommand, DeclaringOneSideTakesTheStepOffBoth) {
  const Shape rightOnly = shape(false, true);
  expectCommand(Key::Left, Press::Short, rightOnly, kAll, Command::None, 0);
  expectCommand(Key::Right, Press::Short, rightOnly, kAll, Command::RightAction, 1);
  const Shape leftOnly = shape(true, false);
  expectCommand(Key::Left, Press::Short, leftOnly, kAll, Command::LeftAction, 1);
  expectCommand(Key::Right, Press::Short, leftOnly, kAll, Command::None, 0);
}

TEST(ListGrammarCommand, ActionThatDoesNotApplyIsNoneNeverAStep) {
  expectCommand(Key::Left, Press::Short, kBoth, kNone, Command::None, 0);
  expectCommand(Key::Right, Press::Short, kBoth, available(true, false), Command::None, 0);
  expectCommand(Key::Left, Press::Short, kBoth, available(true, false), Command::LeftAction, 1);
}

TEST(ListGrammarCommand, DoubleStepsTwiceButRunsActionsOnce) {
  expectCommand(Key::Down, Press::Double, kDefault, kNone, Command::StepNext, 2);
  expectCommand(Key::Up, Press::Double, kBoth, kAll, Command::StepPrev, 2);
  expectCommand(Key::Right, Press::Double, kDefault, kNone, Command::StepNext, 2);
  expectCommand(Key::Right, Press::Double, kBoth, kAll, Command::RightAction, 1);
  expectCommand(Key::Confirm, Press::Double, kDefault, kNone, Command::Activate, 1);
  expectCommand(Key::Back, Press::Double, kDefault, kNone, Command::Back, 1);
}

TEST(ListGrammarCommand, ConfirmLongActivatesUnlessDeclared) {
  expectCommand(Key::Confirm, Press::Short, kDefault, kNone, Command::Activate, 1);
  expectCommand(Key::Confirm, Press::Long, kDefault, kNone, Command::Activate, 1);
  expectCommand(Key::Confirm, Press::Long, shape(false, false, true), kNone, Command::ActivateLong, 1);
}

TEST(ListGrammarCommand, BackShortGoesBackLongGoesHome) {
  expectCommand(Key::Back, Press::Short, kDefault, kNone, Command::Back, 1);
  expectCommand(Key::Back, Press::Long, kBoth, kAll, Command::Home, 1);
}

// --- Labels ---------------------------------------------------------------------------------

TEST(ListGrammarLabels, DefaultPairShowsTheStep) {
  const auto labels = ListGrammar::labelsFor(kDefault, kNone);
  EXPECT_EQ(labels.left, FrontLabel::Step);
  EXPECT_EQ(labels.right, FrontLabel::Step);
}

TEST(ListGrammarLabels, DeclaredActionsShowWhenTheyApply) {
  const auto labels = ListGrammar::labelsFor(kBoth, kAll);
  EXPECT_EQ(labels.left, FrontLabel::Action);
  EXPECT_EQ(labels.right, FrontLabel::Action);
}

TEST(ListGrammarLabels, DeclaredSideThatDoesNotApplyShowsTheGlyphAlone) {
  auto labels = ListGrammar::labelsFor(kBoth, available(false, true));
  EXPECT_EQ(labels.left, FrontLabel::PageOnly);
  EXPECT_EQ(labels.right, FrontLabel::Action);
  labels = ListGrammar::labelsFor(shape(false, true), kAll);
  EXPECT_EQ(labels.left, FrontLabel::PageOnly);  // undeclared side of an overloaded pair
  EXPECT_EQ(labels.right, FrontLabel::Action);
}

TEST(ListGrammarLabels, LabelsAgreeWithCommands) {
  // A side labelled Step steps, Action acts, PageOnly does nothing on a short press.
  const Shape shapes[] = {kDefault, kBoth, shape(true, false), shape(false, true)};
  const Availability avails[] = {kNone, kAll, available(true, false), available(false, true)};
  for (const Shape& s : shapes) {
    for (const Availability& a : avails) {
      const auto labels = ListGrammar::labelsFor(s, a);
      const auto left = ListGrammar::commandFor(Key::Left, Press::Short, s, a).command;
      const auto right = ListGrammar::commandFor(Key::Right, Press::Short, s, a).command;
      EXPECT_EQ(labels.left == FrontLabel::Step, left == Command::StepPrev);
      EXPECT_EQ(labels.left == FrontLabel::Action, left == Command::LeftAction);
      EXPECT_EQ(labels.left == FrontLabel::PageOnly, left == Command::None);
      EXPECT_EQ(labels.right == FrontLabel::Step, right == Command::StepNext);
      EXPECT_EQ(labels.right == FrontLabel::Action, right == Command::RightAction);
      EXPECT_EQ(labels.right == FrontLabel::PageOnly, right == Command::None);
    }
  }
}

// --- Row arithmetic -------------------------------------------------------------------------

TEST(ListGrammarRows, StepWraps) {
  const Rows rows = plainRows(3, 10);
  EXPECT_EQ(ListGrammar::step(rows, 0, 1), 1);
  EXPECT_EQ(ListGrammar::step(rows, 2, 1), 0);
  EXPECT_EQ(ListGrammar::step(rows, 0, -1), 2);
}

TEST(ListGrammarRows, StepSkipsUnselectableRows) {
  const Mask mask{{false, true, true, false}};
  const Rows rows = maskedRows(mask, 10);
  EXPECT_EQ(ListGrammar::step(rows, 2, 1), 1);
  EXPECT_EQ(ListGrammar::step(rows, 1, -1), 2);
}

TEST(ListGrammarRows, StepWithNothingSelectableStays) {
  const Mask mask{{false, false}};
  EXPECT_EQ(ListGrammar::step(maskedRows(mask, 10), 0, 1), 0);
}

TEST(ListGrammarRows, EmptyListDoesNotMove) {
  const Rows rows = plainRows(0, 10);
  EXPECT_EQ(ListGrammar::step(rows, 0, 1), 0);
  expectTurn(ListGrammar::page(rows, 0, 1), 0, 0);
  expectTurn(ListGrammar::page(rows, 0, -1), 0, 0);
  EXPECT_EQ(ListGrammar::first(rows), 0);
  EXPECT_EQ(ListGrammar::last(rows), 0);
}

TEST(ListGrammarRows, PageForwardMovesTheScreenAndKeepsTheLine) {
  expectTurn(ListGrammar::page(plainRows(50, 23, 0), 5, 1), 28, 23);
  expectTurn(ListGrammar::page(plainRows(50, 23, 23), 30, 1), 34, 27);  // the screen stops at the last full one
  expectTurn(ListGrammar::page(plainRows(50, 23, 27), 34, 1), 49, 27);  // last screen: clamps on the last row
}

TEST(ListGrammarRows, PageBackShowsTheWindowAbove) {
  expectTurn(ListGrammar::page(plainRows(50, 23, 23), 30, -1), 7, 0);
  expectTurn(ListGrammar::page(plainRows(50, 23, 27), 40, -1), 17, 4);
  expectTurn(ListGrammar::page(plainRows(50, 23, 0), 5, -1), 0, 0);  // first screen: clamps on the first row
}

TEST(ListGrammarRows, RowsOfDifferentHeightsPageByWhatWasDrawn) {
  // Wrapped titles: only 7 of 30 rows fit, and the screen shows rows 7-13.
  expectTurn(ListGrammar::page(plainRows(30, 7, 7), 9, 1), 16, 14);
  expectTurn(ListGrammar::page(plainRows(30, 7, 7), 9, -1), 2, 0);
}

TEST(ListGrammarRows, ASelectionOutsideTheWindowPagesFromItself) {
  // The window is a render old: the selection has already stepped below it, and lands on the new top.
  expectTurn(ListGrammar::page(plainRows(50, 23, 0), 23, 1), 27, 27);
  expectTurn(ListGrammar::page(plainRows(50, 23, 0), 30, -1), 7, 7);
}

TEST(ListGrammarRows, PageOnOnePageListGoesToTheEnds) {
  const Rows rows = plainRows(5, 23);
  expectTurn(ListGrammar::page(rows, 2, 1), 4, 0);
  expectTurn(ListGrammar::page(rows, 2, -1), 0, 0);
}

TEST(ListGrammarRows, PageTreatsANonPositiveWindowAsOneRow) {
  expectTurn(ListGrammar::page(plainRows(5, 0), 0, 1), 1, 1);
}

TEST(ListGrammarRows, PageSkipsUnselectableRows) {
  // Rows 4 and 8 are headers; four rows fit on a screen.
  const Mask mask{{true, true, true, true, false, true, true, true, false, true}};
  expectTurn(ListGrammar::page(maskedRows(mask, 4, 0), 0, 1), 5, 4);  // lands on header 4, walks on
  // Screen clamps at the end; the selection keeps its line.
  expectTurn(ListGrammar::page(maskedRows(mask, 4, 4), 5, 1), 7, 6);
  expectTurn(ListGrammar::page(maskedRows(mask, 4, 8), 9, -1), 5, 4);  // keeps its line; header 4 opens the screen
}

TEST(ListGrammarRows, PageAtAHeaderAtTheEndSettlesBesideIt) {
  const Mask mask{{true, true, true, true, true, false}};
  expectTurn(ListGrammar::page(maskedRows(mask, 3, 3), 3, 1), 4, 3);
  const Mask leading{{false, true, true, true}};
  expectTurn(ListGrammar::page(maskedRows(leading, 2, 2), 3, -1), 1, 0);
}

TEST(ListGrammarRows, PageTurnsKeepTheSelectionsLine) {
  // The reported round trip: 13 rows, eight on a screen, the selection on the fourth line.
  expectTurn(ListGrammar::page(plainRows(13, 8, 0), 3, 1), 8, 5);
  expectTurn(ListGrammar::page(plainRows(13, 8, 5), 8, -1), 3, 0);
}

TEST(ListGrammarRows, AHeaderEndingTheScreenOpensTheNext) {
  // Row 2 is a header; three rows fit. The new screen would start at row 3, under it.
  const Mask mask{{true, true, false, true, true, true, true, true}};
  expectTurn(ListGrammar::page(maskedRows(mask, 3, 0), 1, 1), 4, 2);
}

TEST(ListGrammarRows, PageTurnsOnAlignedPages) {
  // OPDS lays out pages 0-22, 23-45 and 46-49: the last screen starts at 46, not at 27.
  const auto aligned = [](int count, int drawn, int top) {
    Rows rows = plainRows(count, drawn, top);
    rows.pageAligned = true;
    return rows;
  };
  expectTurn(ListGrammar::page(aligned(50, 23, 23), 30, 1), 49, 46);  // onto the short last page
  expectTurn(ListGrammar::page(aligned(50, 23, 46), 49, -1), 26, 23);
  expectTurn(ListGrammar::page(aligned(50, 23, 46), 47, 1), 49, 46);  // last page: clamps on the last row
}

TEST(ListGrammarRows, PageTurnsBelowALeadingTabBar) {
  // Position 0 is a tab bar above 30 rows. The first screen shows it and nine rows, later ones nine
  // rows: the bar is not a line of the page.
  const auto tabbed = [](int top, int drawn) {
    Rows rows = plainRows(31, drawn, top);
    rows.lead = 1;
    return rows;
  };
  expectTurn(ListGrammar::page(tabbed(0, 10), 3, 1), 12, 10);
  expectTurn(ListGrammar::page(tabbed(10, 9), 12, -1), 3, 0);  // the round trip
  expectTurn(ListGrammar::page(tabbed(0, 10), 0, 1), 10, 10);  // from the bar: the first row below the screen
  expectTurn(ListGrammar::page(tabbed(0, 10), 0, -1), 0, 0);   // from the bar: nowhere to go
  expectTurn(ListGrammar::page(tabbed(0, 10), 3, -1), 1, 0);   // first screen: clamps on the first row

  // A tab whose rows are all headers: from the bar there is nothing to page to.
  const Mask headersOnly{{true, false, false, false}};
  Rows rows = maskedRows(headersOnly, 4);
  rows.lead = 1;
  expectTurn(ListGrammar::page(rows, 0, 1), 0, 0);
}

TEST(ListGrammarRows, OutOfRangeStartIsClampedFirst) {
  const Rows rows = plainRows(5, 2);
  expectTurn(ListGrammar::page(rows, 9, -1), 2, 2);
  expectTurn(ListGrammar::page(rows, 9, 1), 4, 4);
  EXPECT_EQ(ListGrammar::step(rows, 9, 1), 0);
  EXPECT_EQ(ListGrammar::step(rows, -3, -1), 4);
}

TEST(ListGrammarRows, FirstAndLastSkipUnselectableRows) {
  const Mask mask{{false, true, true, false}};
  const Rows rows = maskedRows(mask, 10);
  EXPECT_EQ(ListGrammar::first(rows), 1);
  EXPECT_EQ(ListGrammar::last(rows), 2);
  const Mask none{{false, false}};
  EXPECT_EQ(ListGrammar::first(maskedRows(none, 10)), 0);
  EXPECT_EQ(ListGrammar::last(maskedRows(none, 10)), 0);
}

// --- Double-tap ----------------------------------------------------------------------------

TEST(ListGrammarDoubleTap, SameKeyInsideTheWindowOnALongListPages) {
  const Rows rows = plainRows(50, 23);
  EXPECT_TRUE(ListGrammar::completesDoubleTap(Key::Down, 1299, Key::Down, 1000, rows));
  EXPECT_TRUE(ListGrammar::completesDoubleTap(Key::Up, 1000, Key::Up, 1000, rows));
}

TEST(ListGrammarDoubleTap, TheWindowEndsAtThreeHundredMilliseconds) {
  const Rows rows = plainRows(50, 23);
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Down, 1300, Key::Down, 1000, rows));
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Down, 5000, Key::Down, 1000, rows));
}

TEST(ListGrammarDoubleTap, ADifferentKeyIsANewTap) {
  const Rows rows = plainRows(50, 23);
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Up, 1100, Key::Down, 1000, rows));
}

TEST(ListGrammarDoubleTap, OnlyUpAndDownDoubleTap) {
  const Rows rows = plainRows(50, 23);
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Right, 1100, Key::Right, 1000, rows));
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Confirm, 1100, Key::Confirm, 1000, rows));
}

TEST(ListGrammarDoubleTap, AListThatFitsOnePageKeepsTwoSteps) {
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Down, 1100, Key::Down, 1000, plainRows(23, 23)));
  EXPECT_TRUE(ListGrammar::completesDoubleTap(Key::Down, 1100, Key::Down, 1000, plainRows(24, 23)));
}

TEST(ListGrammarDoubleTap, NoEarlierTapOrAnEarlierTimeIsNotADoubleTap) {
  const Rows rows = plainRows(50, 23);
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Down, 100, Key::Down, 0, rows));
  EXPECT_FALSE(ListGrammar::completesDoubleTap(Key::Down, 900, Key::Down, 1000, rows));
}
