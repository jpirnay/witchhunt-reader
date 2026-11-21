#pragma once

#include <cstdint>
#include <optional>

// A book's place in its series as the book writes it ("3", "2.5"), read as a number in millionths: 2.5 is
// 2,500,000. The Library orders a series by it, the end-of-book screen finds the sequel by it, and KOReader sync
// sends it.
//
// Read without floating point (the C3 has no FPU), and like strtof for any decimal a book writes: leading blanks,
// a sign, digits with a decimal point, an exponent ("1e2"). Digits past the sixth decimal are dropped. Hexadecimal,
// "inf" and "nan", which strtof also takes, are no number here.
struct SeriesIndex {
  // Up to 10^7: beyond it ArduinoJson would write the float as an exponent, which no book's index needs.
  static constexpr int64_t kLimit = int64_t{10000000} * 1000000;

  int64_t millionths = 0;

  // The number at the start of `text`; `end`, when given, receives where it stops, as strtof's does. nullopt when
  // no digits begin the text or its magnitude reaches 10^7.
  static std::optional<SeriesIndex> parse(const char* const text, const char** const end = nullptr) {
    if (end != nullptr) *end = text;
    const char* p = text;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
    bool negative = false;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    int64_t value = 0;  // millionths
    bool digits = false;
    bool tooLarge = false;
    for (; *p >= '0' && *p <= '9'; ++p) {
      digits = true;
      if (!tooLarge) {
        value = value * 10 + (*p - '0') * int64_t{1000000};
        tooLarge = value >= kLimit;
      }
    }
    if (*p == '.') {
      ++p;
      int64_t unit = 100000;
      for (; *p >= '0' && *p <= '9'; ++p) {
        digits = true;
        value += (*p - '0') * unit;
        unit /= 10;
      }
    }
    if (!digits) return std::nullopt;
    // An exponent counts only with a digit after its letter and sign, as in strtof.
    if (*p == 'e' || *p == 'E') {
      const char* q = p + 1;
      bool negativeExponent = false;
      if (*q == '+' || *q == '-') negativeExponent = *q++ == '-';
      if (*q >= '0' && *q <= '9') {
        int exponent = 0;
        for (; *q >= '0' && *q <= '9'; ++q) {
          if (exponent < 100) exponent = exponent * 10 + (*q - '0');
        }
        p = q;
        for (int i = 0; i < exponent && value != 0 && !tooLarge; ++i) {
          if (negativeExponent) {
            value /= 10;
          } else {
            value *= 10;
            tooLarge = value >= kLimit;
          }
        }
      }
    }
    if (end != nullptr) *end = p;
    if (tooLarge) return std::nullopt;
    return SeriesIndex{negative ? -value : value};
  }

  friend constexpr auto operator<=>(SeriesIndex, SeriesIndex) = default;
};
