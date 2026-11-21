#pragma once

#include <FixedDecimal.h>

#include <cstddef>
#include <cstdint>

// The weather's numbers as integers in a stated unit, since the C3 has no FPU (docs/future_work/integer-math.md).
// WeatherClient converts each where it reads the API's JSON, so that the screen shows what it showed when they were
// floats. Open-Meteo sends temperature and wind speed with one decimal and the UV index with two (seen 2026-11-21):
// kept whole, they round to the shown whole number as lroundf did. Precipitation comes with two decimals in mm and
// three in inches, and is shown with one, where a value such as 0.35 printed by the side its float happened to lie on
// (0.34999999, "0.3"); so it is kept in the tenths it is shown in, rounded from that float as printf rounded it.
namespace weather {

// int, 32 bits on every target: int32_t is long on the RISC-V toolchain, and ArduinoJson would build its readers again
// for long.
static_assert(sizeof(int) >= 4, "the weather's units need 32-bit ints");
using Tenths = int;        // temperature (°C or °F), wind speed, precipitation (mm or inch)
using Hundredths = int;    // UV index
using Thousandths = int;   // moon phase, of a cycle
using MicroDegrees = int;  // latitude, longitude: millionths of a degree, up to 180° within 32 bits

constexpr unsigned TENTHS_DECIMALS = 1;
constexpr unsigned HUNDREDTHS_DECIMALS = 2;
constexpr unsigned THOUSANDTHS_DECIMALS = 3;
constexpr unsigned MICRODEGREE_DECIMALS = 6;

constexpr Thousandths MOON_PHASE_UNKNOWN = -1000;  // -1 of a cycle

// A value in 10^-decimals units as the whole number the screen shows: rounded as lroundf rounded the float it
// replaced, a half away from zero.
inline int whole(const int value, const unsigned decimals) {
  return fixed_decimal::rescale(value, decimals, 0, fixed_decimal::Tie::AwayFromZero);
}

// The whole degrees at or below, and at or above, a temperature: the hourly graph's axis.
constexpr int floorWhole(const Tenths t) { return t >= 0 ? t / 10 : -((-t + 9) / 10); }
constexpr int ceilWhole(const Tenths t) { return t >= 0 ? (t + 9) / 10 : -(-t / 10); }

// A precipitation with its one decimal: "2.4".
inline int formatPrecipitation(char* out, const size_t outSize, const Tenths value) {
  return fixed_decimal::format(out, outSize, value, TENTHS_DECIMALS, TENTHS_DECIMALS, fixed_decimal::Tie::ToEven);
}

// A coordinate with `shown` decimals (up to 6): "-122.4194", as printf("%.*f") printed the float it replaced. But for a
// value on a tie at the shown precision (...xxxx50 millionths at four decimals): the float's text went by which side
// of it the float happened to lie on, and the exact value now rounds to even.
inline int formatDegrees(char* out, const size_t outSize, const MicroDegrees value, const unsigned shown) {
  return fixed_decimal::format(out, outSize, value, MICRODEGREE_DECIMALS, shown, fixed_decimal::Tie::ToEven);
}

// A coordinate typed in: "52.52", "-0.5". False for anything that is not a plain decimal number.
inline bool parseDegrees(const char* text, MicroDegrees& out) {
  int32_t parsed = 0;
  if (!fixed_decimal::parse(text, MICRODEGREE_DECIMALS, parsed)) return false;
  out = parsed;
  return true;
}

// A number ArduinoJson hands over as a float, in an integer unit: exactly the float the screens used to round
// (fixed_decimal::fromFloat reads its bits), a tie to even as printf rounded it.
inline int fromFloat(const float value, const unsigned decimals) {
  return fixed_decimal::fromFloat(value, decimals, fixed_decimal::Tie::ToEven);
}

}  // namespace weather
