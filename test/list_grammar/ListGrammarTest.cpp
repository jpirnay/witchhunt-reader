// The list button scheme (docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md §1):
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

Rows plainRows(int count, int pageRows) {
  Rows rows;
  rows.count = count;
  rows.pageRows = pageRows;
  return rows;
}

Rows maskedRows(const Mask& mask, int pageRows) {
  Rows rows;
  rows.count = static_cast<int>(mask.selectable.size());
  rows.pageRows = pageRows;
  rows.selectable = &maskSelectable;
  rows.ctx = &mask;
  return rows;
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
  EXPECT_EQ(ListGrammar::page(rows, 0, 1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 0, -1), 0);
  EXPECT_EQ(ListGrammar::first(rows), 0);
  EXPECT_EQ(ListGrammar::last(rows), 0);
}

TEST(ListGrammarRows, PageForwardGoesToTheNextPageAndClampsOnTheLast) {
  const Rows rows = plainRows(50, 23);
  EXPECT_EQ(ListGrammar::page(rows, 0, 1), 23);
  EXPECT_EQ(ListGrammar::page(rows, 10, 1), 23);
  EXPECT_EQ(ListGrammar::page(rows, 23, 1), 46);
  EXPECT_EQ(ListGrammar::page(rows, 46, 1), 49);
  EXPECT_EQ(ListGrammar::page(rows, 49, 1), 49);
}

TEST(ListGrammarRows, PageBackGoesToThePreviousPageAndClampsOnTheFirst) {
  const Rows rows = plainRows(50, 23);
  EXPECT_EQ(ListGrammar::page(rows, 49, -1), 23);
  EXPECT_EQ(ListGrammar::page(rows, 30, -1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 23, -1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 5, -1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 0, -1), 0);
}

TEST(ListGrammarRows, PageOnOnePageListGoesToTheEnds) {
  const Rows rows = plainRows(5, 23);
  EXPECT_EQ(ListGrammar::page(rows, 2, 1), 4);
  EXPECT_EQ(ListGrammar::page(rows, 2, -1), 0);
}

TEST(ListGrammarRows, PageTreatsANonPositivePageAsOneRow) { EXPECT_EQ(ListGrammar::page(plainRows(5, 0), 0, 1), 1); }

TEST(ListGrammarRows, PageSkipsUnselectableRows) {
  // Rows 4 and 8 are headers; four rows per page.
  const Mask mask{{true, true, true, true, false, true, true, true, false, true}};
  const Rows rows = maskedRows(mask, 4);
  EXPECT_EQ(ListGrammar::page(rows, 0, 1), 5);   // lands on header 4, walks on
  EXPECT_EQ(ListGrammar::page(rows, 5, 1), 9);   // lands on header 8, walks on
  EXPECT_EQ(ListGrammar::page(rows, 9, -1), 5);  // lands on header 4, settles on the page it opens
}

TEST(ListGrammarRows, OutOfRangeStartIsClampedFirst) {
  const Rows rows = plainRows(5, 2);
  EXPECT_EQ(ListGrammar::page(rows, 9, -1), 2);
  EXPECT_EQ(ListGrammar::page(rows, 9, 1), 4);
  EXPECT_EQ(ListGrammar::step(rows, 9, 1), 0);
  EXPECT_EQ(ListGrammar::step(rows, -3, -1), 4);
}

TEST(ListGrammarRows, PageAtAHeaderAtTheEndSettlesBesideIt) {
  const Mask mask{{true, true, true, true, true, false}};
  EXPECT_EQ(ListGrammar::page(maskedRows(mask, 3), 3, 1), 4);
  const Mask leading{{false, true, true, true}};
  EXPECT_EQ(ListGrammar::page(maskedRows(leading, 2), 3, -1), 1);
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
