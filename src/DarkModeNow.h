#pragma once

// Dark Mode as the screens follow it now, and the one switch that changes it from outside the settings list: the light
// drawer's (FrontlightPanelActivity). Every render pass asks isDark() (ActivityManager::applyOutputPolarity), so a
// change lands on the next frame whichever way it was made, with the full refresh a polarity change takes.
namespace DarkModeNow {

// Whether the screens go out white on black now.
bool isDark();

// The light drawer's Dark Mode switch: the other of isDark(), saved.
void flip();

}  // namespace DarkModeNow
