#pragma once

#include <cstdint>

// An IEEE-754 single, given as its bits, times `scale` (1..INT32_MAX) and rounded half away from zero, as
// std::lround(value * scale) would give in exact arithmetic. For the one conversion where a float arrives from outside
// (the IDF temperature sensor, the values older firmware left in NVS and RTC memory): it uses no float arithmetic,
// which the C3 (no FPU) would run as soft-float calls. False for NaN, infinity, and a result outside int32.
inline bool fixedFromFloatBits(const uint32_t bits, const int32_t scale, int32_t& out) {
  const uint32_t exponent = (bits >> 23) & 0xFF;
  if (exponent == 0xFF) return false;  // infinity or NaN
  if (exponent == 0) {
    // Zero or subnormal: under 2^-126, which no int32 scale lifts to half a unit.
    out = 0;
    return true;
  }
  // value = significand x 2^(exponent - 150), the significand 24 bits with its implicit leading one.
  const uint64_t product = static_cast<uint64_t>((bits & 0x7FFFFFu) | 0x800000u) * static_cast<uint32_t>(scale);
  const int shift = static_cast<int>(exponent) - 150;
  uint64_t magnitude = 0;
  if (shift >= 0) {
    // product is at least 2^23, so any shift of 8 or more leaves int32 behind.
    if (shift >= 32 || product > (uint64_t{INT32_MAX} >> shift)) return false;
    magnitude = product << shift;
  } else if (shift > -64) {
    // product < 2^55, so adding the half below 2^62 cannot wrap.
    const int right = -shift;
    magnitude = (product + (uint64_t{1} << (right - 1))) >> right;
  }
  if (magnitude > uint64_t{INT32_MAX}) return false;
  const auto value = static_cast<int32_t>(magnitude);
  out = (bits >> 31) != 0 ? -value : value;
  return true;
}
