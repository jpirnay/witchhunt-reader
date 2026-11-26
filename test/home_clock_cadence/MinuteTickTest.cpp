// Which screens the clock's minute tick redraws (src/activities/MinuteTick.h): every screen with Use
// Clock on, and a screen that shows the time of its own (Home's Clock widget) with it off.
#include <gtest/gtest.h>

#include "activities/MinuteTick.h"

namespace {

// A screen's shouldSkipPeriodicUpdate(), counting how often it is asked.
struct Skips {
  bool answer = false;
  int asked = 0;
  bool operator()() {
    ++asked;
    return answer;
  }
};

}  // namespace

TEST(MinuteTick, AClockWidgetTicksWithUseClockOff) {
  Skips skips;
  EXPECT_TRUE(MinuteTick::redraws(false, true, skips));
  EXPECT_EQ(skips.asked, 1);
}

TEST(MinuteTick, NoClockOfItsOwnAndUseClockOffDoesNotTick) {
  Skips skips;
  EXPECT_FALSE(MinuteTick::redraws(false, false, skips));
  // Not even asked: the reader's answer records a refresh it then owes.
  EXPECT_EQ(skips.asked, 0);
}

TEST(MinuteTick, UseClockTicksEveryScreen) {
  Skips skips;
  EXPECT_TRUE(MinuteTick::redraws(true, false, skips));
  EXPECT_TRUE(MinuteTick::redraws(true, true, skips));
  EXPECT_EQ(skips.asked, 2);
}

// Home's idle pace (HomeClockCadence) still holds for the widget: a skipped minute is not drawn,
// Use Clock on or off.
TEST(MinuteTick, TheScreenMaySkipAMinuteEitherWay) {
  Skips skips{true};
  EXPECT_FALSE(MinuteTick::redraws(false, true, skips));
  EXPECT_FALSE(MinuteTick::redraws(true, false, skips));
  EXPECT_FALSE(MinuteTick::redraws(true, true, skips));
  EXPECT_EQ(skips.asked, 3);
}

// A frame the minute tick alone asked for is clock-only (it does not spend the menus' ghost-clearing count);
// any other request for the same frame makes it an ordinary one.
TEST(ClockOnlyFrame, TheTickAloneIsClockOnly) {
  MinuteTick::ClockOnlyFrame frame;
  frame.tick(/*othersAsked=*/false);
  EXPECT_TRUE(frame.takeForPass());
}

TEST(ClockOnlyFrame, AFrameAskedForBeforeTheTickIsNot) {
  MinuteTick::ClockOnlyFrame frame;
  frame.other();
  frame.tick(/*othersAsked=*/true);
  EXPECT_FALSE(frame.takeForPass());
}

TEST(ClockOnlyFrame, ARequestAfterTheTickMakesItOrdinary) {
  MinuteTick::ClockOnlyFrame frame;
  frame.tick(/*othersAsked=*/false);
  frame.other();  // a tab switch before the render task took the pass
  EXPECT_FALSE(frame.takeForPass());
}

TEST(ClockOnlyFrame, EachTickAnswersOnePass) {
  MinuteTick::ClockOnlyFrame frame;
  frame.tick(/*othersAsked=*/false);
  EXPECT_TRUE(frame.takeForPass());
  EXPECT_FALSE(frame.takeForPass());  // the next pass, for a button, counts
}

TEST(ClockOnlyFrame, NothingAskedIsNotClockOnly) {
  MinuteTick::ClockOnlyFrame frame;
  EXPECT_FALSE(frame.takeForPass());
}
