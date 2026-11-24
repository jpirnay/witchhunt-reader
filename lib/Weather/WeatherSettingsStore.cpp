#include "WeatherSettingsStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <JsonFile.h>
#include <JsonText.h>
#include <Logging.h>

WeatherSettingsStore WeatherSettingsStore::instance;

namespace {
constexpr char WEATHER_SETTINGS_FILE[] = "/.crosspoint/weather_settings.json";

// A coordinate in millionths of a degree. A file from firmware that held it as a float has only `floatKey`: its value
// is taken as that firmware read it, the float ArduinoJson makes of it, rounded to millionths as its "%.6f" printed it
// in the forecast request and on the keyboard.
weather::MicroDegrees readDegrees(const JsonDocument& doc, const char* key, const char* floatKey) {
  const JsonVariantConst micro = doc[key];
  if (micro.is<int>()) return micro.as<int>();
  return weather::fromFloat(doc[floatKey] | 0.0f, weather::MICRODEGREE_DECIMALS);
}
}  // namespace

bool WeatherSettingsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["latitudeE6"] = latitude;
  doc["longitudeE6"] = longitude;
  // Firmware that held the location as a float reads only these: the same place for it after a downgrade.
  char degrees[16];
  weather::formatDegrees(degrees, sizeof(degrees), latitude, weather::MICRODEGREE_DECIMALS);
  doc["latitude"] = serialized(static_cast<char*>(degrees));  // a char*: ArduinoJson copies it
  weather::formatDegrees(degrees, sizeof(degrees), longitude, weather::MICRODEGREE_DECIMALS);
  doc["longitude"] = serialized(static_cast<char*>(degrees));
  doc["locationConfigured"] = locationConfigured;
  doc["locationName"] = locationName;
  doc["tempUnit"] = static_cast<uint8_t>(tempUnit);
  doc["windUnit"] = static_cast<uint8_t>(windUnit);
  doc["precipUnit"] = static_cast<uint8_t>(precipUnit);
  doc["forecastDays"] = forecastDays;

  return JsonFile::save("WEA", WEATHER_SETTINGS_FILE, doc);
}

bool WeatherSettingsStore::loadFromFile() {
  if (!Storage.exists(WEATHER_SETTINGS_FILE)) {
    LOG_DBG("WEA", "No weather settings file found");
    return false;
  }

  String json = Storage.readFile(WEATHER_SETTINGS_FILE);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  auto error = JsonText::parse(doc, json);
  if (error) {
    LOG_ERR("WEA", "JSON parse error: %s", error.c_str());
    return false;
  }

  latitude = readDegrees(doc, "latitudeE6", "latitude");
  longitude = readDegrees(doc, "longitudeE6", "longitude");
  locationConfigured = doc["locationConfigured"] | (latitude != 0 || longitude != 0);
  locationName = doc["locationName"] | std::string("");
  tempUnit = static_cast<WeatherTempUnit>(doc["tempUnit"] | (uint8_t)0);
  windUnit = static_cast<WeatherWindUnit>(doc["windUnit"] | (uint8_t)0);
  precipUnit = static_cast<WeatherPrecipUnit>(doc["precipUnit"] | (uint8_t)0);
  forecastDays = doc["forecastDays"] | (uint8_t)3;

  if (forecastDays < 1) forecastDays = 1;
  if (forecastDays > 5) forecastDays = 5;

  LOG_DBG("WEA", "Loaded weather settings: %s (%d, %d millionths of a degree)", locationName.c_str(), latitude,
          longitude);
  return true;
}

void WeatherSettingsStore::setLocation(const weather::MicroDegrees lat, const weather::MicroDegrees lon,
                                       const std::string& name) {
  latitude = lat;
  longitude = lon;
  locationConfigured = true;
  locationName = name;
  LOG_DBG("WEA", "Set location: %s (%d, %d millionths of a degree)", name.c_str(), lat, lon);
}

void WeatherSettingsStore::clearLocation() {
  latitude = 0;
  longitude = 0;
  locationConfigured = false;
  locationName.clear();
}

void WeatherSettingsStore::setForecastDays(uint8_t days) {
  if (days < 1) days = 1;
  if (days > 5) days = 5;
  forecastDays = days;
}

const char* WeatherSettingsStore::getTempUnitParam() const {
  return tempUnit == WeatherTempUnit::FAHRENHEIT ? "fahrenheit" : "celsius";
}

const char* WeatherSettingsStore::getWindUnitParam() const {
  switch (windUnit) {
    case WeatherWindUnit::MS:
      return "ms";
    case WeatherWindUnit::MPH:
      return "mph";
    case WeatherWindUnit::KNOTS:
      return "kn";
    case WeatherWindUnit::KMH:
    default:
      return "kmh";
  }
}

const char* WeatherSettingsStore::getPrecipUnitParam() const {
  return precipUnit == WeatherPrecipUnit::INCH ? "inch" : "mm";
}
