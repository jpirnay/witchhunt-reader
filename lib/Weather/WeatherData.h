#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "WeatherUnits.h"

struct CurrentWeather {
  weather::Tenths temperature = 0;
  weather::Tenths apparentTemperature = 0;
  int humidity = 0;
  int weatherCode = 0;
  weather::Tenths windSpeed = 0;
  int windDirection = 0;
  weather::Tenths precipitation = 0;
  weather::Hundredths uvIndex = 0;
  bool isDay = true;
};

struct DailyForecast {
  time_t date = 0;
  weather::Tenths tempMax = 0;
  weather::Tenths tempMin = 0;
  int weatherCode = 0;
  weather::Tenths precipSum = 0;
  weather::Hundredths uvIndexMax = 0;
  time_t sunrise = 0;
  time_t sunset = 0;
  weather::Thousandths moonPhase = weather::MOON_PHASE_UNKNOWN;  // 0 new, 500 full
};

struct HourlyForecast {
  time_t time = 0;
  weather::Tenths temperature = 0;
  weather::Tenths precipitation = 0;
  int precipitationProbability = 0;
  int weatherCode = 0;
  bool isDay = true;
};

struct WeatherData {
  CurrentWeather current;
  std::vector<DailyForecast> daily;
  std::vector<HourlyForecast> hourly;
  std::string requestSignature;
  std::string timezone;
  int utcOffsetSeconds = 0;
  time_t fetchedAt = 0;
  bool valid = false;
  std::string errorMessage;
};

struct GeocodingResult {
  std::string name;
  std::string country;
  std::string admin1;  // State/region
  weather::MicroDegrees latitude = 0;
  weather::MicroDegrees longitude = 0;
};
