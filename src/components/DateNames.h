#pragma once

#include <I18n.h>

// Short day and month names, indexed as struct tm counts them (tm_wday from Sunday, tm_mon from
// January). The Weather screen, and Home's clock and calendar, name dates with them.
namespace DateNames {

inline constexpr StrId kShortDays[] = {
    StrId::STR_WEATHER_DAY_SUN, StrId::STR_WEATHER_DAY_MON, StrId::STR_WEATHER_DAY_TUE, StrId::STR_WEATHER_DAY_WED,
    StrId::STR_WEATHER_DAY_THU, StrId::STR_WEATHER_DAY_FRI, StrId::STR_WEATHER_DAY_SAT};
inline constexpr StrId kShortMonths[] = {
    StrId::STR_WEATHER_MONTH_JAN, StrId::STR_WEATHER_MONTH_FEB, StrId::STR_WEATHER_MONTH_MAR,
    StrId::STR_WEATHER_MONTH_APR, StrId::STR_WEATHER_MONTH_MAY, StrId::STR_WEATHER_MONTH_JUN,
    StrId::STR_WEATHER_MONTH_JUL, StrId::STR_WEATHER_MONTH_AUG, StrId::STR_WEATHER_MONTH_SEP,
    StrId::STR_WEATHER_MONTH_OCT, StrId::STR_WEATHER_MONTH_NOV, StrId::STR_WEATHER_MONTH_DEC};

}  // namespace DateNames
