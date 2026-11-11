#pragma once

#include "CrossPointSettings.h"
#include "SettingInfo.h"

// The Dark Mode row of the Display tab (see DarkMode.h for what it does). Built here rather than inline in
// SettingsList.h so the host can check what persists it: JsonSettingsIO saves and loads every row of the settings list
// that has a key, by that key, as the row's type bounds it (test/dark_mode).
namespace DarkModeOption {

inline SettingInfo make() {
  return SettingInfo::Toggle(StrId::STR_DARK_MODE, &CrossPointSettings::darkMode, "darkMode", StrId::STR_CAT_DISPLAY);
}

}  // namespace DarkModeOption
