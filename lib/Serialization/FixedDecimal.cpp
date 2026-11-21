#include "FixedDecimal.h"

#include <bit>
#include <cstdio>

namespace fixed_decimal {

int32_t rescale(const int32_t value, const unsigned from, const unsigned to, const Tie tie) {
  const int32_t divisor = pow10(from - to);
  int32_t quotient = value / divisor;
  const int32_t rest = value % divisor;  // takes value's sign
  const int32_t magnitude = rest < 0 ? -rest : rest;
  const int32_t beyondHalf = magnitude - (divisor - magnitude);  // 2 * magnitude - divisor, without the overflow
  if (beyondHalf > 0 || (beyondHalf == 0 && (tie == Tie::AwayFromZero || (quotient & 1) != 0))) {
    quotient += value < 0 ? -1 : 1;
  }
  return quotient;
}

int format(char* out, const size_t outSize, const int32_t value, const unsigned decimals, const unsigned shown,
           const Tie tie) {
  const int32_t rounded = rescale(value, decimals, shown, tie);
  const unsigned magnitude = rounded < 0 ? 0u - static_cast<unsigned>(rounded) : static_cast<unsigned>(rounded);
  const char* sign = value < 0 ? "-" : "";
  if (shown == 0) return snprintf(out, outSize, "%s%u", sign, magnitude);
  const auto unit = static_cast<unsigned>(pow10(shown));
  return snprintf(out, outSize, "%s%u.%0*u", sign, magnitude / unit, static_cast<int>(shown), magnitude % unit);
}

bool parse(const char* text, const unsigned decimals, int32_t& out) {
  const char* p = text;
  while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
  const bool negative = *p == '-';
  if (*p == '-' || *p == '+') p++;
  int32_t magnitude = 0;
  unsigned kept = 0;  // decimals taken in
  bool roundUp = false;
  bool anyDigit = false;
  bool inFraction = false;
  for (;; p++) {
    if (*p == '.' && !inFraction) {
      inFraction = true;
      continue;
    }
    if (*p < '0' || *p > '9') break;
    anyDigit = true;
    const int digit = *p - '0';
    if (!inFraction || kept < decimals) {
      if (magnitude > (INT32_MAX - digit) / 10) return false;
      magnitude = magnitude * 10 + digit;
      if (inFraction) kept++;
    } else if (kept == decimals) {
      roundUp = digit >= 5;  // the first decimal dropped decides: from a 5 on, a tie or above it
      kept++;
    }
  }
  if (*p != '\0' || !anyDigit) return false;
  for (; kept < decimals; kept++) {
    if (magnitude > INT32_MAX / 10) return false;
    magnitude *= 10;
  }
  if (roundUp) {
    if (magnitude == INT32_MAX) return false;
    magnitude++;
  }
  out = negative ? -magnitude : magnitude;
  return true;
}

int32_t fromFloat(const float value, const unsigned decimals, const Tie tie) {
  const auto bits = std::bit_cast<uint32_t>(value);
  const bool negative = (bits >> 31) != 0;
  const unsigned biased = (bits >> 23) & 0xFF;
  uint64_t mantissa = bits & 0x7FFFFF;
  if (biased == 0xFF) {
    if (mantissa != 0) return 0;  // NaN
    return negative ? -INT32_MAX : INT32_MAX;
  }
  int exponent = -149;  // a subnormal's
  if (biased != 0) {
    mantissa |= 0x800000;
    exponent = static_cast<int>(biased) - 150;
  }
  // |value| = mantissa * 2^exponent: the result is mantissa * 10^decimals * 2^exponent, under 2^54 before the shift.
  const uint64_t scaled = mantissa * static_cast<uint64_t>(pow10(decimals));
  uint64_t magnitude;
  if (exponent >= 10) {
    magnitude = UINT64_MAX;  // past 2^33: clamped below
  } else if (exponent >= 0) {
    magnitude = scaled << exponent;
  } else if (exponent < -62) {
    magnitude = 0;  // under 2^54 / 2^63: less than half a unit
  } else {
    const auto shift = static_cast<unsigned>(-exponent);
    magnitude = scaled >> shift;
    const uint64_t rest = scaled & ((uint64_t{1} << shift) - 1);
    const uint64_t half = uint64_t{1} << (shift - 1);
    if (rest > half || (rest == half && (tie == Tie::AwayFromZero || (magnitude & 1) != 0))) magnitude++;
  }
  if (magnitude > INT32_MAX) magnitude = INT32_MAX;
  return negative ? -static_cast<int32_t>(magnitude) : static_cast<int32_t>(magnitude);
}

}  // namespace fixed_decimal
