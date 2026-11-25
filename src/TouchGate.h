#pragma once

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

}  // namespace TouchGate
