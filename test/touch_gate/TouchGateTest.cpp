#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

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

// --- What can switch Touch Lock off again
// ------------------------------------------------------------------------------

// Stand-ins for the BUTTON_ACTION values: the gate is handed them, so any distinct numbers do.
constexpr uint8_t kConfirm = 40;
constexpr uint8_t kToggleLock = 41;
constexpr uint8_t kPageForward = 1;
constexpr uint8_t kBack = 39;

bool liftable(const bool fixedConfirmKey, std::initializer_list<uint8_t> keyActions, const bool slideSwitchIsTouch) {
  TouchGate::LockEscapes escapes;
  escapes.fixedConfirmKey = fixedConfirmKey;
  escapes.keyActions = keyActions.begin();
  escapes.keyActionCount = keyActions.size();
  escapes.slideSwitchIsTouch = slideSwitchIsTouch;
  return TouchGate::lockCanBeLifted(escapes, kConfirm, kToggleLock);
}

// A Confirm key, or a Home key in its place (X3, X4, X4 Pro, T5S3, Murphy): the Touch Lock row in Settings is always
// a key away, whatever the keys are bound to.
TEST(TouchLockEscape, AFixedConfirmKeyIsAlwaysAWayOut) {
  EXPECT_TRUE(liftable(/*fixedConfirmKey=*/true, {kPageForward, kBack}, /*slideSwitchIsTouch=*/false));
}

// The BOOX Picco: Power, Up and Down, and a short Power is Confirm only by default.
TEST(TouchLockEscape, AKeyBoundToConfirmIsAWayOut) {
  EXPECT_TRUE(liftable(false, {kConfirm, kBack, kPageForward}, false));
}

TEST(TouchLockEscape, AKeyBoundToToggleTouchLockIsAWayOut) {
  EXPECT_TRUE(liftable(false, {kPageForward, kBack, kToggleLock}, false));
}

TEST(TouchLockEscape, TheSlideSwitchSetToTouchIsAWayOut) {
  EXPECT_TRUE(liftable(false, {kPageForward, kBack}, /*slideSwitchIsTouch=*/true));
}

// Short Power bound to something else, no key on Toggle Touch Lock, the switch on the light: once touch is off, nothing
// could switch it on again, and the lock was saved and came back at every boot.
TEST(TouchLockEscape, NoWayOutWithoutAKeyOrTheSwitch) {
  EXPECT_FALSE(liftable(false, {kPageForward, kBack, kPageForward}, false));
  EXPECT_FALSE(liftable(false, {}, false));
}

}  // namespace
