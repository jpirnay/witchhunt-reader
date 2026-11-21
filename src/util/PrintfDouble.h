#pragma once

#include <bit>
#include <cstdint>

// A fixed-point number as the double that printf's "%.Nf" prints back digit for digit.
//
// A few translated strings format a percent with "%.0f" or "%.2f" (STR_OVERLAY_READING_PROGRESS,
// STR_PAGE_OVERALL_FORMAT, ...). A key's printf arguments are fixed for its name, since a language pack on the
// card serves older firmware too (docs/i18n.md), so those keep taking a double. It is built from its bits here:
// on the C3, which has no FPU, even converting an integer to a double is a soft-float call.
//
// The value is `units` / `scale`, with `scale` 1 for "%.0f" and 100 for "%.2f". The double is exact for a whole
// number, and otherwise less than 2^-32 below the value: far inside the half unit printf rounds at, so the digits
// come back as they went in.
inline double printfDouble(const uint32_t units, const uint32_t scale = 1) {
  if (units == 0 || scale == 0) return 0.0;
  uint64_t mantissa = (static_cast<uint64_t>(units) << 32) / scale;  // value = mantissa * 2^-32
  int exponent = -32;
  // Normalise the top bit to bit 52, where a double's implicit bit sits. Bits shifted out on the right are zeros
  // for a whole number.
  const int top = 63 - std::countl_zero(mantissa);
  if (top > 52) {
    mantissa >>= top - 52;
    exponent += top - 52;
  } else {
    mantissa <<= 52 - top;
    exponent -= 52 - top;
  }
  const uint64_t biased = static_cast<uint64_t>(exponent + 52 + 1023);
  return std::bit_cast<double>((biased << 52) | (mantissa & ((uint64_t{1} << 52) - 1)));
}
