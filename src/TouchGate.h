#pragma once

// Whether touch reaches the screen on top this tick: main.cpp hands this to
// MappedInputManager::setTouchEventsEnabled() every tick. Apart from the code that
// acts on it so the host can test it (test/touch_gate).
namespace TouchGate {

// Touch Lock (CrossPointSettings::touchLock) silences every screen. Otherwise the reader on top
// keeps touch whatever Touch Navigation says, because Touch Page Turn governs it there; the
// screens the reader opens (its menu, contents, dictionary) follow Touch Navigation like any
// other.
constexpr bool eventsEnabled(const bool touchLocked, const bool touchNavigationOn, const bool readerOnTop) {
  return !touchLocked && (touchNavigationOn || readerOnTop);
}

}  // namespace TouchGate
