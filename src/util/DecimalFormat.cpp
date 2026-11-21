#include "DecimalFormat.h"

#include <cstdio>

// Out of line: the 64-bit divisions would otherwise be inlined at every caller, and none of them is hot.
int formatDecimal(char* buf, const size_t size, const int64_t num, const int64_t den, const int places,
                  const bool plus) {
  const bool negative = num < 0;
  const uint64_t n = negative ? 0 - static_cast<uint64_t>(num) : static_cast<uint64_t>(num);
  const auto d = static_cast<uint64_t>(den);
  uint64_t unit = 1;
  for (int i = 0; i < places; ++i) unit *= 10;
  uint64_t whole = n / d;
  // (2 x remainder x unit + d) / 2d is remainder x unit / d rounded half up: below 2^41 x 10^6.
  uint64_t fraction = (2 * (n % d) * unit + d) / (2 * d);
  if (fraction == unit) {
    ++whole;
    fraction = 0;
  }
  const char* sign = negative ? "-" : plus ? "+" : "";
  if (places == 0) return snprintf(buf, size, "%s%llu", sign, static_cast<unsigned long long>(whole));
  return snprintf(buf, size, "%s%llu.%0*llu", sign, static_cast<unsigned long long>(whole), places,
                  static_cast<unsigned long long>(fraction));
}
