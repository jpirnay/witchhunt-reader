#pragma once
#include <string>

#include "WeatherData.h"
#include "WeatherSettingsStore.h"

class WeatherClient {
 public:
  /// How long a fetched forecast is shown without asking the API again, when the clock can vouch for its age.
  static constexpr int CACHE_TTL_SECONDS = 30 * 60;

  /// Without forceRefresh: the cached forecast whatever its age (the caller decides whether to refresh, see
  /// CACHE_TTL_SECONDS), or an invalid result when there is none for these settings. With it: a fetch from the API.
  static WeatherData getWeather(const WeatherSettingsStore& settings, bool forceRefresh = false);

  /// Search for cities by name via Open-Meteo geocoding API.
  /// Returns up to 5 results.
  static std::vector<GeocodingResult> searchCity(const std::string& query);

 private:
  static WeatherData fetchFromApi(const WeatherSettingsStore& settings);
  static bool parseWeatherJson(const std::string& json, WeatherData& data);
  static std::string buildRequestSignature(const WeatherSettingsStore& settings);
  static bool saveCache(const WeatherData& data);
  static bool loadCache(WeatherData& data);
};
