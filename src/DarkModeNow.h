#pragma once

// Dark Mode as the screens follow it now, and the one switch that changes it from outside the settings list: the light
// drawer's (FrontlightPanelActivity). Every render pass asks isDark() (ActivityManager::applyOutputPolarity), so a
// change lands on the next frame whichever way it was made, with the full refresh a polarity change takes. That
// includes the night hours: under Scheduled the clock is read at each frame's start, and nothing else wakes for it.
namespace DarkModeNow {

// Whether the screens go out white on black now: the setting, and under Scheduled the clock (DarkMode::isDark).
bool isDark();

// The light drawer's Dark Mode switch: the other of isDark(), saved (DarkMode::flipped).
void flip();

}  // namespace DarkModeNow
