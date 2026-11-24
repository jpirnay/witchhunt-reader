#pragma once

#include "CrossPointSettings.h"
#include "SettingInfo.h"

// The day the week starts on, as the calendar and the week's reading on Home count it. One row for
// the device's Clock screen and the settings list the web UI and persistence read, so the two
// cannot offer different days (see TimezoneOptions.h for what that cost once). The values are
// positions in the list: append a day, never reorder (CrossPointSettings::FIRST_DAY_OF_WEEK).
namespace WeekStartOption {

inline SettingInfo make(const StrId category) {
  return SettingInfo::Enum(StrId::STR_FIRST_DAY_OF_WEEK, &CrossPointSettings::firstDayOfWeek,
                           {StrId::STR_MONDAY, StrId::STR_SUNDAY, StrId::STR_SATURDAY}, "firstDayOfWeek", category);
}

}  // namespace WeekStartOption
