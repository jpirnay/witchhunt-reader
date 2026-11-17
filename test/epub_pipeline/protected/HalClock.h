#pragma once

// The loan clock for the protected-book pipeline test (lib/BookProtection/BookSession.cpp asks
// HalClock::loanNow() alone): the epoch a test sets, 0 for a clock never set.

#include <cstdint>

namespace HalClock {
inline int64_t& testLoanNow() {
  static int64_t now = 0;
  return now;
}
inline int64_t loanNow() { return testLoanNow(); }
}  // namespace HalClock
