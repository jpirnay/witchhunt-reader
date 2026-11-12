#pragma once

#include <string>

#include "CrossPointSettings.h"
#include "DarkMode.h"
#include "SettingInfo.h"

// The Dark Mode rows of the Display tab (see DarkMode.h for what they do). Built here rather than inline in
// SettingsList.h so the host can check what persists them: JsonSettingsIO saves and loads every row of the settings
// list that has a key, by that key, as the row's type bounds it (test/dark_mode).
namespace DarkModeOption {

// Off / On, and Scheduled where the board has the reading light and the clock for it (`scheduleOffered`,
// HalCapabilities::hasFrontlightAndRtc). Elsewhere the switch it always was: a stored Scheduled then loads as Off.
inline SettingInfo make(const bool scheduleOffered) {
  if (!scheduleOffered) {
    return SettingInfo::Toggle(StrId::STR_DARK_MODE, &CrossPointSettings::darkMode, "darkMode", StrId::STR_CAT_DISPLAY);
  }
  return SettingInfo::Enum(StrId::STR_DARK_MODE, &CrossPointSettings::darkMode,
                           {StrId::STR_DARK_MODE_OFF, StrId::STR_DARK_MODE_ON, StrId::STR_DARK_MODE_SCHEDULED},
                           "darkMode", StrId::STR_CAT_DISPLAY);
}

// The hours rows show only under Scheduled.
inline bool scheduled() { return SETTINGS.darkMode == DarkMode::SETTING_SCHEDULED; }

// A time of the day in half-hour steps, picked from a list, in the clock's 12- or 24-hour format.
inline SettingInfo hoursRow(const StrId name, uint8_t CrossPointSettings::* field, const char* key) {
  auto row = SettingInfo::Enum(name, field, {}, key, StrId::STR_CAT_DISPLAY);
  row.withDynamicOptions([](const void*) { return static_cast<uint8_t>(DarkMode::SLOTS_PER_DAY); },
                         [](const void*, const uint8_t slot) {
                           char label[12];
                           DarkMode::formatSlot(label, sizeof(label), slot, SETTINGS.clockFormat12h == 0);
                           return std::string(label);
                         })
      .withSelectorActivity()
      .requiring(SettingRequires::ReadingLightAndClock)
      .shownOnlyWhen(&scheduled);
  return row;
}

inline SettingInfo fromRow() {
  return hoursRow(StrId::STR_DARK_MODE_FROM, &CrossPointSettings::darkModeFrom, "darkModeFrom");
}
inline SettingInfo untilRow() {
  return hoursRow(StrId::STR_DARK_MODE_UNTIL, &CrossPointSettings::darkModeUntil, "darkModeUntil");
}

}  // namespace DarkModeOption
