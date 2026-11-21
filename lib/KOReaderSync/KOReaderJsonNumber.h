#pragma once

#include <Epub/ProgressPpb.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

// Numbers on the KOReader sync wire, without floating point.
//
// A record's "percentage" is a JSON number. ArduinoJson parses one into a float and writes a float with six
// decimals, and both ends of that stay in the library. Our side meets them here: a parsed float is read from its
// bits, and an outgoing number is written as digits (ArduinoJson's serialized()) that match what it wrote for the
// float before, byte for byte.

// A parsed percentage (0..1) as a ProgressPpb, rounded to the nearest billionth. Below 0 is the start, 1 and above
// the end; an infinity or a NaN is no usable percentage at all.
inline std::optional<ProgressPpb> progressFromJsonFloat(const float value) {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint32_t exponent = (bits >> 23) & 0xFFu;
  if (exponent == 0xFFu) return std::nullopt;
  if ((bits >> 31) != 0 || exponent == 0) return ProgressPpb();  // negative, zero or subnormal
  if (exponent >= 127) return ProgressPpb(ProgressPpb::kWhole);
  // value = mantissa * 2^(exponent - 150), the mantissa with its hidden bit below 2^24, so the billionths fit in
  // 54 bits before the shift.
  const uint64_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
  const uint32_t shift = 150 - exponent;  // 24 and up
  if (shift >= 64) return ProgressPpb();
  return ProgressPpb(static_cast<uint32_t>((mantissa * ProgressPpb::kWhole + (uint64_t{1} << (shift - 1))) >> shift));
}

namespace koreader_json_detail {

inline size_t writeUnsigned(uint64_t v, char* const out) {
  char digits[20];
  size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  for (size_t i = 0; i < count; ++i) out[i] = digits[count - 1 - i];
  return count;
}

// `millionths` / 10^6 in ArduinoJson's plain form: six decimals, one fewer for each integral digit past the first
// (seven significant digits in all), rounded half up, trailing zeros dropped.
inline size_t writeDecimal(const uint64_t millionths, char* const out) {
  uint64_t integral = millionths / 1000000;
  uint32_t decimal = static_cast<uint32_t>(millionths % 1000000);
  uint32_t places = 6;
  uint32_t dropped = 1;  // 10^(6 - places)
  for (uint64_t tmp = integral; tmp >= 10 && places > 0; tmp /= 10) {
    --places;
    dropped *= 10;
  }
  if (dropped > 1) {
    const uint32_t rest = decimal % dropped;
    decimal /= dropped;
    if (rest * 2 >= dropped) ++decimal;
    uint32_t limit = 1;
    for (uint32_t i = 0; i < places; ++i) limit *= 10;
    if (decimal >= limit) {
      decimal = 0;
      ++integral;
    }
  }
  while (places > 0 && decimal % 10 == 0) {
    decimal /= 10;
    --places;
  }
  size_t n = writeUnsigned(integral, out);
  if (places > 0) {
    out[n++] = '.';
    for (uint32_t i = places; i > 0; --i) {
      out[n + i - 1] = static_cast<char>('0' + decimal % 10);
      decimal /= 10;
    }
    n += places;
  }
  return n;
}

// `mantissa` * 10^`exponent` in ArduinoJson's exponent form, for a number at or below 0.00001: the leading digit,
// the others after a point with trailing zeros dropped, then the power of ten ("4.403e-6").
inline size_t writeExponent(const uint32_t mantissa, const int exponent, char* const out) {
  char digits[10];
  int count = 0;
  for (uint32_t v = mantissa; v != 0; v /= 10) digits[count++] = static_cast<char>('0' + v % 10);
  int last = 0;  // the lowest digit that is not a trailing zero
  while (digits[last] == '0') ++last;
  size_t n = 0;
  out[n++] = digits[count - 1];
  if (last < count - 1) {
    out[n++] = '.';
    for (int i = count - 2; i >= last; --i) out[n++] = digits[i];
  }
  out[n++] = 'e';
  out[n++] = '-';
  n += writeUnsigned(static_cast<uint64_t>(-(exponent + count - 1)), out + n);
  return n;
}

}  // namespace koreader_json_detail

// `millionths` / 10^6 as the JSON text ArduinoJson 7 wrote for the float nearest to it (a series index). `out` takes
// at least 24 bytes; returns the length written, without the NUL it adds. Exact for any value whose decimals fit in
// seven significant digits, which a book's series index's do. Magnitudes of 10^7 and up, which ArduinoJson writes
// as an exponent, come out as plain digits.
inline size_t formatJsonMillionths(const int64_t millionths, char* const out) {
  size_t n = 0;
  uint64_t u = static_cast<uint64_t>(millionths);
  if (millionths < 0) {
    out[n++] = '-';
    u = 0 - u;
  }
  // ArduinoJson writes 0.00001 and below as an exponent.
  n += u > 0 && u <= 10 ? koreader_json_detail::writeExponent(static_cast<uint32_t>(u), -6, out + n)
                        : koreader_json_detail::writeDecimal(u, out + n);
  out[n] = '\0';
  return n;
}

// A percentage as it goes on the wire, as ArduinoJson wrote the float of it: six decimals, rounded half up as
// ArduinoJson rounded; at 0.00001 and below, where it switched to an exponent, every digit the billionths have
// ("4.403e-6"). `out` takes at least 24 bytes.
inline size_t formatJsonPercentage(const ProgressPpb percentage, char* const out) {
  const uint32_t ppb = percentage.ppb();
  const size_t n = ppb > 0 && ppb <= 10000 ? koreader_json_detail::writeExponent(ppb, -9, out)
                                           : koreader_json_detail::writeDecimal(percentage.millionths(), out);
  out[n] = '\0';
  return n;
}
