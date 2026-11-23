#include <gtest/gtest.h>

#include "TouchGate.h"

namespace {

using TouchGate::eventsEnabled;

TEST(TouchGate, TouchLockSilencesEveryScreen) {
  for (const bool navigationOn : {false, true}) {
    for (const bool readerOnTop : {false, true}) {
      EXPECT_FALSE(eventsEnabled(/*touchLocked=*/true, navigationOn, readerOnTop))
          << "navigation " << navigationOn << " reader " << readerOnTop;
    }
  }
}

TEST(TouchGate, TheReaderOnTopKeepsTouchWithNavigationOff) {
  EXPECT_TRUE(eventsEnabled(false, /*touchNavigationOn=*/false, /*readerOnTop=*/true));
}

// Its menu, contents and dictionary are not the reader on top: they follow Touch Navigation.
TEST(TouchGate, OtherScreensFollowTouchNavigation) {
  EXPECT_FALSE(eventsEnabled(false, /*touchNavigationOn=*/false, /*readerOnTop=*/false));
  EXPECT_TRUE(eventsEnabled(false, /*touchNavigationOn=*/true, /*readerOnTop=*/false));
  EXPECT_TRUE(eventsEnabled(false, /*touchNavigationOn=*/true, /*readerOnTop=*/true));
}

}  // namespace
