#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Whether touch reaches the screen on top this tick: main.cpp hands this to
// MappedInputManager::setTouchEventsEnabled() every tick. Apart from the code that
// acts on it so the host can test it (test/touch_gate).
namespace TouchGate {

// Whether the screen on top counts as the reader: the reader itself, or an overlay pulled over it
// (Activity::overlaysScreenBelow(): the light drawer). An overlay is part of the screen it covers, so
// over a book it takes touch as the book does; the top-edge swipe that opens the drawer comes from the
// page. The screens the reader opens (its menu, contents, dictionary) are screens of their own and
// do not count.
constexpr bool readerOwnsScreen(const bool topIsReader, const bool topIsOverlay, const bool belowIsReader) {
  return topIsReader || (topIsOverlay && belowIsReader);
}

// Touch Lock (CrossPointSettings::touchLock) silences every screen. Otherwise the reader on top
// (readerOwnsScreen()) keeps touch whatever Touch Navigation says, because Touch Page Turn governs
// it there; the screens the reader opens (its menu, contents, dictionary) follow Touch Navigation
// like any other.
constexpr bool eventsEnabled(const bool touchLocked, const bool touchNavigationOn, const bool readerOnTop) {
  return !touchLocked && (touchNavigationOn || readerOnTop);
}

// What can still switch Touch Lock off once touch is gone. Any one of them is enough.
struct LockEscapes {
  // A key gives Confirm whatever the settings say (a Confirm key, or a Home key in its place), so the Touch Lock row
  // in Settings is a key away.
  bool fixedConfirmKey = false;
  // The actions of the keys the board has, every press kind of each, as the settings store them.
  const uint8_t* keyActions = nullptr;
  size_t keyActionCount = 0;
  // The slide switch is set to Touch: switched on, it lifts the lock.
  bool slideSwitchIsTouch = false;
};

// Whether a lock switched on now could be switched off again (CrossPointSettings::touchLockCanBeLifted()). `confirm`
// and `toggleLock` are the BUTTON_ACTION values of Confirm and Toggle Touch Lock. A gesture bound to Toggle Touch Lock
// is no way out: the lock silences it.
inline bool lockCanBeLifted(const LockEscapes& escapes, const uint8_t confirm, const uint8_t toggleLock) {
  if (escapes.fixedConfirmKey || escapes.slideSwitchIsTouch) return true;
  return std::any_of(escapes.keyActions, escapes.keyActions + escapes.keyActionCount,
                     [&](const uint8_t action) { return action == confirm || action == toggleLock; });
}

}  // namespace TouchGate
