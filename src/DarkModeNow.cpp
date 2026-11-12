#include "DarkModeNow.h"

#include "CrossPointSettings.h"

namespace DarkModeNow {

bool isDark() { return SETTINGS.darkMode != 0; }

// The settings row's own field and save: the drawer is a second way to the same setting, not a second dark mode.
void flip() {
  SETTINGS.darkMode = isDark() ? 0 : 1;
  SETTINGS.saveToFile();
}

}  // namespace DarkModeNow
