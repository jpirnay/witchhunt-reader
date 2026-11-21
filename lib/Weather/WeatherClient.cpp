#include "WeatherClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <JsonFile.h>
#include <Logging.h>
#include <TlsPolicy.h>  // for HttpDownloader.h below: names lib/SecureNet to PlatformIO's LDF

#include <cstdio>
#include <ctime>

#include "../../src/network/HttpDownloader.h"

namespace {
constexpr char WEATHER_CACHE_FILE[] = "/.crosspoint/weather_cache.json";

// The cache's numbers are integers in WeatherUnits.h's units since layout 2. The layout is part of the request
// signature, so a cache of the other layout fails the signature check, also in firmware that wrote floats, and neither
// reads the other's numbers in the wrong unit.
constexpr int CACHE_LAYOUT = 2;

// A coordinate with six decimals, as the request and the signature carried the float before.
std::string degreesText(const weather::MicroDegrees value) {
  char text[16];
  weather::formatDegrees(text, sizeof(text), value, weather::MICRODEGREE_DECIMALS);
  return text;
}

// An API number in an integer unit, converted here, once, from the float the screens kept before (`| 0.0f`).
int apiValue(const JsonVariantConst value, const unsigned decimals, const int fallback = 0) {
  return value.is<float>() ? weather::fromFloat(value.as<float>(), decimals) : fallback;
}

std::string buildForecastUrl(const WeatherSettingsStore& settings) {
  std::string url = "https://api.open-meteo.com/v1/forecast?";
  url += "latitude=" + degreesText(settings.getLatitude());
  url += "&longitude=" + degreesText(settings.getLongitude());
  url +=
      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
      "weather_code,wind_speed_10m,wind_direction_10m,is_day,precipitation,uv_index";
  url += "&hourly=temperature_2m,precipitation,precipitation_probability,weather_code,is_day";
  url +=
      "&daily=temperature_2m_max,temperature_2m_min,weather_code,"
      "precipitation_sum,sunrise,sunset,uv_index_max";
  url += "&timezone=auto&timeformat=unixtime";
  url += std::string("&temperature_unit=") + settings.getTempUnitParam();
  url += std::string("&wind_speed_unit=") + settings.getWindUnitParam();
  url += std::string("&precipitation_unit=") + settings.getPrecipUnitParam();
  url += "&forecast_days=" + std::to_string(settings.getForecastDays());
  // The screen graphs 24 hours; the forecast strip covers the days after.
  url += "&forecast_hours=24";
  return url;
}

std::string urlEncode(const std::string& value) {
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out.append(buf);
    }
  }
  return out;
}

std::string buildRequestSignature(const WeatherSettingsStore& settings) {
  std::string signature = "lat=";
  signature += degreesText(settings.getLatitude());
  signature += "|lon=";
  signature += degreesText(settings.getLongitude());
  signature += "|temp=";
  signature += settings.getTempUnitParam();
  signature += "|wind=";
  signature += settings.getWindUnitParam();
  signature += "|precip=";
  signature += settings.getPrecipUnitParam();
  signature += "|days=";
  signature += std::to_string(settings.getForecastDays());
  signature += "|layout=";
  signature += std::to_string(CACHE_LAYOUT);
  return signature;
}
}  // namespace

std::string WeatherClient::buildRequestSignature(const WeatherSettingsStore& settings) {
  return ::buildRequestSignature(settings);
}

WeatherData WeatherClient::getWeather(const WeatherSettingsStore& settings, bool forceRefresh) {
  if (!settings.hasLocation()) {
    WeatherData data;
    data.errorMessage = "No location configured";
    return data;
  }

  const std::string requestSignature = buildRequestSignature(settings);

  if (!forceRefresh) {
    WeatherData cached;
    if (loadCache(cached) && cached.valid) {
      if (cached.requestSignature != requestSignature) {
        LOG_DBG("WEA", "Ignoring cache with mismatched request signature");
        Storage.remove(WEATHER_CACHE_FILE);
      } else {
        // Cache-first: the caller judges the age, because only it knows whether the clock can be trusted.
        return cached;
      }
    }

    LOG_DBG("WEA", "No cache available; caller should establish network and force refresh");
    WeatherData data;
    data.errorMessage = "No cache";
    return data;
  }

  return fetchFromApi(settings);
}

WeatherData WeatherClient::fetchFromApi(const WeatherSettingsStore& settings) {
  WeatherData data;
  data.requestSignature = buildRequestSignature(settings);
  std::string url = buildForecastUrl(settings);
  LOG_DBG("WEA", "fetchFromApi[1] start");
  LOG_DBG("WEA", "fetchFromApi[2] url length=%zu", url.size());
  LOG_DBG("WEA", "Fetching weather from API");

  std::string response;
  LOG_DBG("WEA", "fetchFromApi[3] HttpDownloader::fetchUrl before");
  // Public forecast data, no credentials sent: verifying the chain would cost a
  // multi-second ECDSA walk to protect nothing. Unverified and honest about it.
  if (!HttpDownloader::fetchUrl(url, response, "", "", HttpDownloader::TlsPolicy::Unverified)) {
    data.errorMessage = "Network error";
    LOG_ERR("WEA", "Failed to fetch weather data");
    return data;
  }
  LOG_DBG("WEA", "fetchFromApi[4] HttpDownloader::fetchUrl after; bytes=%zu", response.size());

  LOG_DBG("WEA", "fetchFromApi[5] parseWeatherJson before");
  if (!parseWeatherJson(response, data)) {
    LOG_ERR("WEA", "Failed to parse weather JSON");
    return data;
  }
  LOG_DBG("WEA", "fetchFromApi[6] parseWeatherJson after");

  data.valid = true;
  time(&data.fetchedAt);
  saveCache(data);
  LOG_DBG("WEA", "fetchFromApi[7] Weather data fetched and cached");
  return data;
}

bool WeatherClient::parseWeatherJson(const std::string& json, WeatherData& data) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    data.errorMessage = "JSON parse error";
    LOG_ERR("WEA", "JSON parse error: %s", error.c_str());
    return false;
  }

  // Check for API error
  if (doc["error"] | false) {
    data.errorMessage = doc["reason"] | "API error";
    LOG_ERR("WEA", "API error: %s", data.errorMessage.c_str());
    return false;
  }

  data.timezone = doc["timezone"] | std::string("");
  data.utcOffsetSeconds = doc["utc_offset_seconds"] | 0;

  // Parse current weather
  JsonObject current = doc["current"];
  if (current) {
    data.current.temperature = apiValue(current["temperature_2m"], weather::TENTHS_DECIMALS);
    data.current.apparentTemperature = apiValue(current["apparent_temperature"], weather::TENTHS_DECIMALS);
    data.current.humidity = current["relative_humidity_2m"] | 0;
    data.current.weatherCode = current["weather_code"] | 0;
    data.current.windSpeed = apiValue(current["wind_speed_10m"], weather::TENTHS_DECIMALS);
    data.current.windDirection = current["wind_direction_10m"] | 0;
    data.current.precipitation = apiValue(current["precipitation"], weather::TENTHS_DECIMALS);
    data.current.uvIndex = apiValue(current["uv_index"], weather::HUNDREDTHS_DECIMALS);
    data.current.isDay = (current["is_day"] | 1) != 0;
  }

  // Parse daily forecast
  JsonObject daily = doc["daily"];
  if (daily) {
    JsonArray times = daily["time"];
    JsonArray tempMax = daily["temperature_2m_max"];
    JsonArray tempMin = daily["temperature_2m_min"];
    JsonArray codes = daily["weather_code"];
    JsonArray precip = daily["precipitation_sum"];
    JsonArray sunrise = daily["sunrise"];
    JsonArray sunset = daily["sunset"];
    JsonArray uvMax = daily["uv_index_max"];

    size_t count = times.size();
    data.daily.reserve(count);
    for (size_t i = 0; i < count; i++) {
      DailyForecast day;
      day.date = times[i] | (time_t)0;
      day.tempMax = apiValue(tempMax[i], weather::TENTHS_DECIMALS);
      day.tempMin = apiValue(tempMin[i], weather::TENTHS_DECIMALS);
      day.weatherCode = codes[i] | 0;
      day.precipSum = apiValue(precip[i], weather::TENTHS_DECIMALS);
      day.sunrise = sunrise[i] | (time_t)0;
      day.sunset = sunset[i] | (time_t)0;
      day.uvIndexMax = apiValue(uvMax[i], weather::HUNDREDTHS_DECIMALS);
      day.moonPhase = apiValue(daily["moon_phase"][i], weather::THOUSANDTHS_DECIMALS, weather::MOON_PHASE_UNKNOWN);
      data.daily.push_back(day);
    }
  }

  // Parse hourly forecast
  JsonObject hourly = doc["hourly"];
  if (hourly) {
    JsonArray times = hourly["time"];
    JsonArray temp = hourly["temperature_2m"];
    JsonArray precip = hourly["precipitation"];
    JsonArray precipProb = hourly["precipitation_probability"];
    JsonArray codes = hourly["weather_code"];
    JsonArray isDay = hourly["is_day"];

    size_t count = times.size();
    data.hourly.reserve(count);
    for (size_t i = 0; i < count; i++) {
      HourlyForecast hour;
      hour.time = times[i] | (time_t)0;
      hour.temperature = apiValue(temp[i], weather::TENTHS_DECIMALS);
      hour.precipitation = apiValue(precip[i], weather::TENTHS_DECIMALS);
      hour.precipitationProbability = precipProb[i] | 0;
      hour.weatherCode = codes[i] | 0;
      hour.isDay = (isDay[i] | 1) != 0;
      data.hourly.push_back(hour);
    }
  }

  return true;
}

bool WeatherClient::saveCache(const WeatherData& data) {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["requestSignature"] = data.requestSignature;
  doc["fetchedAt"] = data.fetchedAt;
  doc["timezone"] = data.timezone;
  doc["utcOffsetSeconds"] = data.utcOffsetSeconds;

  // Current. The numbers in WeatherUnits.h's units (CACHE_LAYOUT).
  JsonObject cur = doc["current"].to<JsonObject>();
  cur["temperature"] = data.current.temperature;
  cur["apparentTemperature"] = data.current.apparentTemperature;
  cur["humidity"] = data.current.humidity;
  cur["weatherCode"] = data.current.weatherCode;
  cur["windSpeed"] = data.current.windSpeed;
  cur["windDirection"] = data.current.windDirection;
  cur["precipitation"] = data.current.precipitation;
  cur["uvIndex"] = data.current.uvIndex;
  cur["isDay"] = data.current.isDay;

  // Daily
  JsonArray dailyArr = doc["daily"].to<JsonArray>();
  for (const auto& day : data.daily) {
    JsonObject d = dailyArr.add<JsonObject>();
    d["date"] = day.date;
    d["tempMax"] = day.tempMax;
    d["tempMin"] = day.tempMin;
    d["weatherCode"] = day.weatherCode;
    d["precipSum"] = day.precipSum;
    d["uvIndexMax"] = day.uvIndexMax;
    d["sunrise"] = day.sunrise;
    d["sunset"] = day.sunset;
    d["moonPhase"] = day.moonPhase;
    d["moonPhaseApiName"] = "moon_phase";  // for compatibility/tracing
  }

  // Hourly
  JsonArray hourlyArr = doc["hourly"].to<JsonArray>();
  for (const auto& hour : data.hourly) {
    JsonObject h = hourlyArr.add<JsonObject>();
    h["time"] = hour.time;
    h["temperature"] = hour.temperature;
    h["precipitation"] = hour.precipitation;
    h["precipProb"] = hour.precipitationProbability;
    h["weatherCode"] = hour.weatherCode;
    h["isDay"] = hour.isDay;
  }

  return JsonFile::save("WEA", WEATHER_CACHE_FILE, doc);
}

bool WeatherClient::loadCache(WeatherData& data) {
  if (!Storage.exists(WEATHER_CACHE_FILE)) {
    return false;
  }

  String json = Storage.readFile(WEATHER_CACHE_FILE);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WEA", "Cache parse error: %s", error.c_str());
    return false;
  }

  data.fetchedAt = doc["fetchedAt"] | (time_t)0;
  data.requestSignature = doc["requestSignature"] | std::string("");
  data.timezone = doc["timezone"] | std::string("");
  data.utcOffsetSeconds = doc["utcOffsetSeconds"] | 0;

  // Current
  JsonObject cur = doc["current"];
  if (cur) {
    data.current.temperature = cur["temperature"] | 0;
    data.current.apparentTemperature = cur["apparentTemperature"] | 0;
    data.current.humidity = cur["humidity"] | 0;
    data.current.weatherCode = cur["weatherCode"] | 0;
    data.current.windSpeed = cur["windSpeed"] | 0;
    data.current.windDirection = cur["windDirection"] | 0;
    data.current.precipitation = cur["precipitation"] | 0;
    data.current.uvIndex = cur["uvIndex"] | 0;
    data.current.isDay = cur["isDay"] | true;
  }

  // Daily
  JsonArray dailyArr = doc["daily"].as<JsonArray>();
  for (JsonObject d : dailyArr) {
    DailyForecast day;
    day.date = d["date"] | (time_t)0;
    day.tempMax = d["tempMax"] | 0;
    day.tempMin = d["tempMin"] | 0;
    day.weatherCode = d["weatherCode"] | 0;
    day.precipSum = d["precipSum"] | 0;
    day.uvIndexMax = d["uvIndexMax"] | 0;
    day.sunrise = d["sunrise"] | (time_t)0;
    day.sunset = d["sunset"] | (time_t)0;
    day.moonPhase = d["moonPhase"] | weather::MOON_PHASE_UNKNOWN;
    data.daily.push_back(day);
  }

  // Hourly
  JsonArray hourlyArr = doc["hourly"].as<JsonArray>();
  for (JsonObject h : hourlyArr) {
    HourlyForecast hour;
    hour.time = h["time"] | (time_t)0;
    hour.temperature = h["temperature"] | 0;
    hour.precipitation = h["precipitation"] | 0;
    hour.precipitationProbability = h["precipProb"] | 0;
    hour.weatherCode = h["weatherCode"] | 0;
    hour.isDay = h["isDay"] | true;
    data.hourly.push_back(hour);
  }

  data.valid = true;
  LOG_DBG("WEA", "Cache loaded, fetchedAt=%ld", (long)data.fetchedAt);
  return true;
}

std::vector<GeocodingResult> WeatherClient::searchCity(const std::string& query) {
  std::vector<GeocodingResult> results;

  std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(query);
  url += "&count=5&language=en";

  std::string response;
  // Public geocoding lookup; see fetchFromApi() for why this is not verified.
  if (!HttpDownloader::fetchUrl(url, response, "", "", HttpDownloader::TlsPolicy::Unverified)) {
    LOG_ERR("WEA", "Geocoding request failed");
    return results;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, response);
  if (error) {
    LOG_ERR("WEA", "Geocoding parse error: %s", error.c_str());
    return results;
  }

  JsonArray arr = doc["results"].as<JsonArray>();
  for (JsonObject obj : arr) {
    GeocodingResult r;
    r.name = obj["name"] | std::string("");
    r.country = obj["country"] | std::string("");
    r.admin1 = obj["admin1"] | std::string("");
    r.latitude = apiValue(obj["latitude"], weather::MICRODEGREE_DECIMALS);
    r.longitude = apiValue(obj["longitude"], weather::MICRODEGREE_DECIMALS);
    results.push_back(r);
  }

  LOG_DBG("WEA", "Geocoding found %zu results for '%s'", results.size(), query.c_str());
  return results;
}
