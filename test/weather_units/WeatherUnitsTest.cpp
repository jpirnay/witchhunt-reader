// The weather in integers (lib/Weather/WeatherUnits.h). The screens used to keep the API's numbers as the floats
// ArduinoJson makes of them and print them with lroundf and printf; these run the API's number texts through both
// paths and pin the integer text to the float text. The settings file keeps its float keys beside the new integer ones,
// so each firmware reads the other's file.
#include <ArduinoJson.h>
#include <WeatherSettingsStore.h>
#include <WeatherUnits.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

#include "HalStorage.h"

namespace {

constexpr char kSettingsPath[] = "/.crosspoint/weather_settings.json";

// A JSON number as the firmware held it before: ArduinoJson's float, read with `| 0.0f`.
float apiFloat(const std::string& number) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, "{\"v\":" + number + "}"), DeserializationError::Ok) << number;
  return doc["v"] | 0.0f;
}

std::string decimalText(const int value, const int decimals) {
  char text[32];
  const int unit = fixed_decimal::pow10(static_cast<unsigned>(decimals));
  const int magnitude = std::abs(value);
  snprintf(text, sizeof(text), "%s%d.%0*d", value < 0 ? "-" : "", magnitude / unit, decimals, magnitude % unit);
  return text;
}

std::string oldWhole(const float f) {
  char text[24];
  snprintf(text, sizeof(text), "%ld", lroundf(f));
  return text;
}

std::string newWhole(const int value, const unsigned decimals) {
  char text[24];
  snprintf(text, sizeof(text), "%d", weather::whole(value, decimals));
  return text;
}

// Temperature, feels like, the days' highs and lows and the wind: the API's one decimal, shown whole.
TEST(WeatherUnits, WholeTemperaturesAndWindAsBefore) {
  for (int tenths = -600; tenths <= 600; tenths++) {
    const std::string text = decimalText(tenths, 1);
    const float f = apiFloat(text);
    const weather::Tenths t = weather::fromFloat(f, weather::TENTHS_DECIMALS);
    ASSERT_EQ(t, tenths) << text;
    ASSERT_EQ(newWhole(t, weather::TENTHS_DECIMALS), oldWhole(f)) << text;
  }
  // Whole numbers come without a point.
  for (int whole = -60; whole <= 60; whole++) {
    const float f = apiFloat(std::to_string(whole));
    ASSERT_EQ(newWhole(weather::fromFloat(f, weather::TENTHS_DECIMALS), weather::TENTHS_DECIMALS), oldWhole(f));
  }
}

TEST(WeatherUnits, WholeUvIndexAsBefore) {
  for (int hundredths = 0; hundredths <= 1500; hundredths++) {
    const std::string text = decimalText(hundredths, 2);
    const float f = apiFloat(text);
    const weather::Hundredths uv = weather::fromFloat(f, weather::HUNDREDTHS_DECIMALS);
    ASSERT_EQ(uv, hundredths) << text;
    ASSERT_EQ(newWhole(uv, weather::HUNDREDTHS_DECIMALS), oldWhole(f)) << text;
  }
}

// Today's precipitation, "%.1f", for every value Open-Meteo sends: two decimals in mm, three in inches. Kept in the
// tenths it is shown in, rounded from the API's float as printf rounded it, so also a value exactly halfway prints as
// it did by the side its float lies on: 0.35 (0.34999999) "0.3", 0.15 (0.15000001) "0.2".
TEST(WeatherUnits, PrecipitationAsBefore) {
  char before[24];
  char after[24];
  for (const int decimals : {1, 2, 3}) {
    const int limit = decimals == 3 ? 20000 : 30000;
    for (int value = 0; value <= limit; value++) {
      const std::string text = decimalText(value, decimals);
      const float f = apiFloat(text);
      snprintf(before, sizeof(before), "%.1f", static_cast<double>(f));
      weather::formatPrecipitation(after, sizeof(after), weather::fromFloat(f, weather::TENTHS_DECIMALS));
      ASSERT_STREQ(after, before) << text;
    }
  }
  weather::formatPrecipitation(after, sizeof(after), weather::fromFloat(apiFloat("0.35"), weather::TENTHS_DECIMALS));
  EXPECT_STREQ(after, "0.3");
}

// The hourly graph's axis: whole degrees below and above the curve.
TEST(WeatherUnits, GraphAxisAsBefore) {
  for (int tenths = -600; tenths <= 600; tenths++) {
    const float f = apiFloat(decimalText(tenths, 1));
    ASSERT_EQ(weather::floorWhole(tenths), static_cast<int>(std::floor(f))) << tenths;
    ASSERT_EQ(weather::ceilWhole(tenths), static_cast<int>(std::ceil(f))) << tenths;
  }
}

std::string degreesText(const weather::MicroDegrees v, const unsigned shown) {
  char text[24];
  weather::formatDegrees(text, sizeof(text), v, shown);
  return text;
}

// Coordinates as the geocoder sends them (up to five decimals) and as a settings file from before held them: the
// request's and the keyboard's six decimals stay the float's to the digit. The settings rows' four do too, but where
// the float rounds to a millionth exactly halfway between two ten-thousandths (about 0.4 % of these): there the float's
// text went by its noise, and the exact value now rounds to even.
TEST(WeatherUnits, CoordinatesAsBefore) {
  std::mt19937 random(20261121);
  int ties = 0;
  for (int i = 0; i < 200000; i++) {
    const int places = 2 + static_cast<int>(random() % 4);
    const int limit = (i % 2 == 0 ? 90 : 180) * fixed_decimal::pow10(static_cast<unsigned>(places));
    const int scaled = static_cast<int>(random() % static_cast<unsigned>(2 * limit + 1)) - limit;
    const std::string text = decimalText(scaled, places);
    const float f = apiFloat(text);
    const weather::MicroDegrees micro = weather::fromFloat(f, weather::MICRODEGREE_DECIMALS);
    ASSERT_EQ(degreesText(micro, 6), std::to_string(f)) << text;  // the request URL, the keyboard
    char before[24];
    snprintf(before, sizeof(before), "%.4f", static_cast<double>(f));
    if (std::abs(micro) % 100 == 50) {
      ties++;
      continue;
    }
    ASSERT_EQ(degreesText(micro, 4), before) << text;  // the settings rows
  }
  EXPECT_LT(ties, 200000 / 100);
}

// Typed in: kept to the millionth. A value exactly halfway at four decimals rounds to even on the settings row, where
// its float printed by where it fell (52.52435 was 52.524349..., "52.5243").
TEST(WeatherUnits, ParseDegrees) {
  weather::MicroDegrees v = 0;
  ASSERT_TRUE(weather::parseDegrees("52.52435", v));
  EXPECT_EQ(v, 52524350);
  EXPECT_EQ(degreesText(v, 4), "52.5244");
  EXPECT_EQ(degreesText(v, 6), "52.524350");
  ASSERT_TRUE(weather::parseDegrees("-122.4194", v));
  EXPECT_EQ(degreesText(v, 4), "-122.4194");
  EXPECT_FALSE(weather::parseDegrees("north", v));
}

class WeatherSettingsFile : public ::testing::Test {
 protected:
  void SetUp() override { json_file_fixture::state = {}; }
  void TearDown() override {
    WEATHER_SETTINGS.clearLocation();
    json_file_fixture::state = {};
  }
};

// A file from firmware that held the location as floats: the location it shows and requests stays to the digit.
TEST_F(WeatherSettingsFile, ReadsTheFloatFileOfOlderFirmware) {
  JsonDocument old;
  old["latitude"] = 52.52437f;  // as the old saveToFile wrote its floats
  old["longitude"] = -122.41942f;
  old["locationConfigured"] = true;
  old["locationName"] = "Berlin, Germany";
  old["forecastDays"] = 4;
  std::string text;
  serializeJson(old, text);
  json_file_fixture::state.files[kSettingsPath] = text;

  // What the old firmware read back from that file.
  JsonDocument reread;
  ASSERT_EQ(deserializeJson(reread, text), DeserializationError::Ok);
  const float lat = reread["latitude"] | 0.0f;
  const float lon = reread["longitude"] | 0.0f;

  ASSERT_TRUE(WEATHER_SETTINGS.loadFromFile());
  EXPECT_TRUE(WEATHER_SETTINGS.hasLocation());
  EXPECT_EQ(WEATHER_SETTINGS.getForecastDays(), 4);
  EXPECT_EQ(degreesText(WEATHER_SETTINGS.getLatitude(), 6), std::to_string(lat)) << text;
  EXPECT_EQ(degreesText(WEATHER_SETTINGS.getLongitude(), 6), std::to_string(lon)) << text;
  char before[24];
  snprintf(before, sizeof(before), "%.4f", static_cast<double>(lat));
  EXPECT_EQ(degreesText(WEATHER_SETTINGS.getLatitude(), 4), before);
  snprintf(before, sizeof(before), "%.4f", static_cast<double>(lon));
  EXPECT_EQ(degreesText(WEATHER_SETTINGS.getLongitude(), 4), before);
}

// The file written now: read back to the millionth, and with float keys an older firmware reads the same place from.
TEST_F(WeatherSettingsFile, WritesIntegersAndTheFloatKeysForOlderFirmware) {
  WEATHER_SETTINGS.setLocation(-33867850, 151207320, "Sydney, Australia");
  ASSERT_TRUE(WEATHER_SETTINGS.saveToFile());
  const std::string text = json_file_fixture::state.files[kSettingsPath];
  EXPECT_NE(text.find("\"latitudeE6\":-33867850"), std::string::npos) << text;
  EXPECT_NE(text.find("\"latitude\":-33.867850"), std::string::npos) << text;

  // An older firmware reads the float nearest the place, as from the geocoder.
  JsonDocument older;
  ASSERT_EQ(deserializeJson(older, text), DeserializationError::Ok);
  EXPECT_FLOAT_EQ(older["latitude"] | 0.0f, -33.86785f);
  EXPECT_FLOAT_EQ(older["longitude"] | 0.0f, 151.20732f);
  EXPECT_TRUE(older["locationConfigured"] | false);

  WEATHER_SETTINGS.clearLocation();
  ASSERT_TRUE(WEATHER_SETTINGS.loadFromFile());
  EXPECT_EQ(WEATHER_SETTINGS.getLatitude(), -33867850);
  EXPECT_EQ(WEATHER_SETTINGS.getLongitude(), 151207320);
  EXPECT_EQ(WEATHER_SETTINGS.getLocationName(), "Sydney, Australia");
}

// No location at all, as an old file without the flag had it: 0, 0 is "not configured".
TEST_F(WeatherSettingsFile, NoLocation) {
  json_file_fixture::state.files[kSettingsPath] = "{\"tempUnit\":1}";
  ASSERT_TRUE(WEATHER_SETTINGS.loadFromFile());
  EXPECT_FALSE(WEATHER_SETTINGS.hasLocation());
  EXPECT_EQ(WEATHER_SETTINGS.getLatitude(), 0);
  EXPECT_EQ(WEATHER_SETTINGS.getTempUnit(), WeatherTempUnit::FAHRENHEIT);
}

}  // namespace
