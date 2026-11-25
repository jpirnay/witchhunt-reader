#include <gtest/gtest.h>

#include "TouchGate.h"

namespace {

using TouchGate::eventsEnabled;
using TouchGate::readerOwnsScreen;

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

TEST(TouchGate, TheReaderOwnsTheScreenWhateverIsBelowIt) {
  for (const bool belowIsReader : {false, true}) {
    EXPECT_TRUE(readerOwnsScreen(/*topIsReader=*/true, /*topIsOverlay=*/false, belowIsReader));
  }
}

// The light drawer pulled down over a page: Touch Navigation off must not leave it without touch, as it did when the
// gate asked only for the reader on top.
TEST(TouchGate, AnOverlayOverTheReaderTakesTouchWithNavigationOff) {
  const bool readerOnTop = readerOwnsScreen(/*topIsReader=*/false, /*topIsOverlay=*/true, /*belowIsReader=*/true);
  EXPECT_TRUE(readerOnTop);
  EXPECT_TRUE(eventsEnabled(false, /*touchNavigationOn=*/false, readerOnTop));
}

TEST(TouchGate, TouchLockSilencesAnOverlayOverTheReader) {
  const bool readerOnTop = readerOwnsScreen(/*topIsReader=*/false, /*topIsOverlay=*/true, /*belowIsReader=*/true);
  for (const bool navigationOn : {false, true}) {
    EXPECT_FALSE(eventsEnabled(/*touchLocked=*/true, navigationOn, readerOnTop)) << "navigation " << navigationOn;
  }
}

// The same drawer over Home or a menu is part of that screen, which Touch Navigation governs.
TEST(TouchGate, AnOverlayElsewhereFollowsTouchNavigation) {
  const bool readerOnTop = readerOwnsScreen(/*topIsReader=*/false, /*topIsOverlay=*/true, /*belowIsReader=*/false);
  EXPECT_FALSE(readerOnTop);
  EXPECT_FALSE(eventsEnabled(false, /*touchNavigationOn=*/false, readerOnTop));
  EXPECT_TRUE(eventsEnabled(false, /*touchNavigationOn=*/true, readerOnTop));
}

// The screens the reader opens (menu, contents, dictionary) are no overlays: Touch Navigation off still silences them.
TEST(TouchGate, TheReadersOwnScreensAreNotItsOverlays) {
  const bool readerOnTop = readerOwnsScreen(/*topIsReader=*/false, /*topIsOverlay=*/false, /*belowIsReader=*/true);
  EXPECT_FALSE(readerOnTop);
  EXPECT_FALSE(eventsEnabled(false, /*touchNavigationOn=*/false, readerOnTop));
}

}  // namespace
