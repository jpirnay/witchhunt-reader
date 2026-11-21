#pragma once

#include <TextScale.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// A number as CSS writes it, read exactly and without strtof: [+-] digits [. digits] [e [+-] digits], its value
// num / den. Integer only (the C3 has no FPU), and not locale-sensitive, as strtof is.
struct CssNumber {
  int64_t num = 0;
  int64_t den = 1;

  // Reads the number `text` starts with, after any white space (strtof skipped it). Returns how many characters it
  // took, 0 when no number starts there (`out` is then 0). Digits past nine decimals or past 10^17 are not read, nor
  // an exponent with no digit after it ("1em" is 1 and "em"). `exponent` false stops at an `e` as well.
  static size_t parse(std::string_view text, CssNumber& out, bool exponent = true);

  // Its value, rounded up onto TextScale's grid; 0 for a value at or below 0. `divisor` divides it first, exactly:
  // a percentage by 100, a pixel size by CSS's 16 px medium.
  [[nodiscard]] TextScale scale(const int64_t divisor = 1) const {
    if (num <= 0 || divisor <= 0) return TextScale::fromRaw(0);
    return TextScale::ratio(num, den * divisor);
  }
  // Whether it is a whole number.
  [[nodiscard]] bool whole() const { return num % den == 0; }
  // Rounded to the nearest whole number, halves away from zero (std::lround).
  [[nodiscard]] int64_t rounded() const {
    return num >= 0 ? (2 * num + den) / (2 * den) : -((-2 * num + den) / (2 * den));
  }
};

inline size_t CssNumber::parse(const std::string_view text, CssNumber& out, const bool exponent) {
  constexpr int64_t kMaxNum = 100000000000000000;  // 10^17: room for one more digit in an int64
  constexpr int64_t kMaxDen = 1000000000;          // nine decimals, more than any stylesheet means
  const auto digit = [](const char c) { return c >= '0' && c <= '9'; };
  out = CssNumber{};
  size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r'))) ++i;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
  int64_t num = 0;
  int64_t den = 1;
  bool any = false;
  for (; i < text.size() && digit(text[i]); ++i) {
    any = true;
    num = num < kMaxNum ? num * 10 + (text[i] - '0') : kMaxNum;
  }
  if (i < text.size() && text[i] == '.') {
    size_t j = i + 1;
    for (; j < text.size() && digit(text[j]); ++j) {
      any = true;
      if (den < kMaxDen && num < kMaxNum) {
        num = num * 10 + (text[j] - '0');
        den *= 10;
      }
    }
    if (any) i = j;  // "5." is 5, its point taken as strtof takes it; "." is no number
  }
  if (!any) return 0;
  if (exponent && i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    size_t j = i + 1;
    bool down = false;
    if (j < text.size() && (text[j] == '+' || text[j] == '-')) down = text[j++] == '-';
    if (j < text.size() && digit(text[j])) {
      int e = 0;
      for (; j < text.size() && digit(text[j]); ++j) e = e < 100 ? e * 10 + (text[j] - '0') : 100;
      for (; e > 0; --e) {
        if (down) {
          if (den >= kMaxDen * 1000) break;  // further down it is nothing anyway
          den *= 10;
        } else {
          num = num < kMaxNum ? num * 10 : kMaxNum;
        }
      }
      i = j;
    }
  }
  out.num = negative ? -num : num;
  out.den = den;
  return i;
}
