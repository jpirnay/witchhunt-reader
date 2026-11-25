#pragma once

#include <GfxRenderer.h>
#include <WeatherIcons.h>
#include <WeatherSettingsStore.h>

#include <cstdint>

// The weather drawing the Weather screen and Home's weather widget share.
namespace WeatherDraw {

// U+00B0, in every UI face. Split from a following letter: "\xB0C" would be one hex escape.
inline constexpr char kDegree[] = "\xC2\xB0";
inline constexpr char kCelsius[] =
    "\xC2\xB0"
    "C";
inline constexpr char kFahrenheit[] =
    "\xC2\xB0"
    "F";

// "°C" or "°F", the unit the weather settings fetch in.
inline const char* tempUnitLabel() {
  return WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? kCelsius : kFahrenheit;
}

// A large icon (getWeatherIconLarge, WEATHER_ICON_SIZE square, 1 bit, MSB first, 0 is ink) at `scale`.
inline void drawIcon(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y, const int scale) {
  constexpr int kRowBytes = WEATHER_ICON_SIZE / 8;
  for (int srcY = 0; srcY < WEATHER_ICON_SIZE; srcY++) {
    for (int srcX = 0; srcX < WEATHER_ICON_SIZE; srcX++) {
      if (icon[srcY * kRowBytes + srcX / 8] & static_cast<uint8_t>(0x80 >> (srcX % 8))) continue;
      if (scale == 1) {
        renderer.drawPixel(x + srcX, y + srcY);
      } else {
        renderer.fillRect(x + srcX * scale, y + srcY * scale, scale, scale);
      }
    }
  }
}

}  // namespace WeatherDraw
