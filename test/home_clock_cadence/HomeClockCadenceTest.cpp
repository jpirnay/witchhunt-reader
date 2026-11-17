// How often Home redraws its clock (src/activities/home/HomeClockCadence.h).
#include <gtest/gtest.h>

#include "activities/home/HomeClockCadence.h"

namespace {

constexpr unsigned long kMinuteMs = 60UL * 1000UL;

}  // namespace

TEST(HomeClockCadence, EveryMinuteRedrawsWhileTheDeviceIsInUse) {
  for (int minute = 0; minute < 60; ++minute) {
    EXPECT_TRUE(HomeClockCadence::redraws(0, minute)) << minute;
    EXPECT_TRUE(HomeClockCadence::redraws(9 * kMinuteMs, minute)) << minute;
  }
}

TEST(HomeClockCadence, TenIdleMinutesLeaveOneRedrawInTen) {
  const unsigned long idle = HomeClockCadence::kIdleAfterMs;
  int redraws = 0;
  for (int minute = 0; minute < 60; ++minute) {
    const bool redraw = HomeClockCadence::redraws(idle, minute);
    EXPECT_EQ(minute % 10 == 0, redraw) << minute;
    redraws += redraw ? 1 : 0;
  }
  EXPECT_EQ(6, redraws);
  // Hours of idling change nothing: still the ten-minute marks.
  EXPECT_TRUE(HomeClockCadence::redraws(5 * 60 * kMinuteMs, 40));
  EXPECT_FALSE(HomeClockCadence::redraws(5 * 60 * kMinuteMs, 41));
}

TEST(HomeClockCadence, TheCadenceChangesAtTenIdleMinutes) {
  EXPECT_TRUE(HomeClockCadence::redraws(HomeClockCadence::kIdleAfterMs - 1, 3));
  EXPECT_FALSE(HomeClockCadence::redraws(HomeClockCadence::kIdleAfterMs, 3));
}
