#pragma once

#include <cstddef>
#include <cstdint>

// Byte counts as the screens and the web pages print them, in units of 1024: "12.3 MB". In integers (the C3 has no
// FPU, docs/design/integer-math.md), rounded the way printf rounded the quotient they were printed from before: to
// nearest, a tie to even ("%.1f" of 0.25 is "0.2"). bytes / 1024.0 is exact in a double, so printf saw the exact value
// and these give the same text; test/byte_size compares the two over a wide range.
namespace byte_size {

// Units of 1024, as shifts.
constexpr unsigned KB = 10;
constexpr unsigned MB = 20;
constexpr unsigned GB = 30;

// `bytes` in units of 2^shift (shift >= 1), times `scale` (1 for whole units, 10 for tenths), rounded to nearest, a tie
// to even.
constexpr uint64_t rounded(const uint64_t bytes, const unsigned shift, const unsigned scale) {
  const uint64_t scaled = bytes * scale;
  const uint64_t whole = scaled >> shift;
  const uint64_t rest = scaled & ((uint64_t{1} << shift) - 1);
  const uint64_t half = uint64_t{1} << (shift - 1);
  return whole + ((rest > half || (rest == half && (whole & 1) != 0)) ? 1 : 0);
}

// `bytes` in units of 2^shift to one decimal, then " `unit`" when one is given: "12.3 MB", as
// printf("%.1f MB", bytes / 1048576.0) wrote it. Returns what snprintf returns.
int formatTenths(char* out, size_t outSize, uint64_t bytes, unsigned shift, const char* unit = nullptr);

}  // namespace byte_size
