#pragma once

#include <cstdint>

/// FreeType's fixed-point arithmetic (ftcalc.c, 64-bit build), bit for bit. The TTF engine scales outlines and
/// metrics with these so its glyphs match FreeType's, and so the PC converter's: a TTF at 14 pt lays out like
/// a .cpfont converted at 14 pt (spec 5.2). Integer only; the engine links no floating point
/// (scripts/check_no_float.py).
namespace ttf {

/// a * b / 65536, rounded half away from zero (FT_MulFix), with the full 64-bit result for callers that
/// range-check it.
constexpr int64_t mulFixWide(const int32_t a, const int32_t b) {
  const int64_t ab = static_cast<int64_t>(a) * b;
  return (ab + 0x8000 - (ab < 0 ? 1 : 0)) >> 16;
}

/// FT_MulFix: 16.16 factors scale font units and 26.6 values.
constexpr int32_t mulFix(const int32_t a, const int32_t b) { return static_cast<int32_t>(mulFixWide(a, b)); }

/// a * 65536 / b, rounded half away from zero (FT_DivFix); 0x7FFFFFFF when b is 0.
constexpr int32_t divFix(const int32_t a, const int32_t b) {
  const bool negative = (a < 0) != (b < 0);
  const uint64_t ua = static_cast<uint64_t>(a < 0 ? -static_cast<int64_t>(a) : a);
  const uint64_t ub = static_cast<uint64_t>(b < 0 ? -static_cast<int64_t>(b) : b);
  const uint64_t q = ub > 0 ? ((ua << 16) + (ub >> 1)) / ub : 0x7FFFFFFFu;
  return static_cast<int32_t>(negative ? -static_cast<int64_t>(q) : static_cast<int64_t>(q));
}

/// a * b / c, rounded half away from zero (FT_MulDiv), with the full 64-bit result; 0x7FFFFFFF when c is 0.
constexpr int64_t mulDivWide(const int32_t a, const int32_t b, const int32_t c) {
  const bool negative = ((a < 0) != (b < 0)) != (c < 0);
  const uint64_t ua = static_cast<uint64_t>(a < 0 ? -static_cast<int64_t>(a) : a);
  const uint64_t ub = static_cast<uint64_t>(b < 0 ? -static_cast<int64_t>(b) : b);
  const uint64_t uc = static_cast<uint64_t>(c < 0 ? -static_cast<int64_t>(c) : c);
  const uint64_t d = uc > 0 ? (ua * ub + (uc >> 1)) / uc : 0x7FFFFFFFu;
  return negative ? -static_cast<int64_t>(d) : static_cast<int64_t>(d);
}

/// FT_MulDiv.
constexpr int32_t mulDiv(const int32_t a, const int32_t b, const int32_t c) {
  return static_cast<int32_t>(mulDivWide(a, b, c));
}

/// FT_SqrtFixed (64-bit build): the square root of an unsigned 16.16 value, by the Babylonian method from a power of
/// two near the root, each division rounded up. The CFF interpreter's `sqrt`.
constexpr uint32_t sqrtFixed(const uint32_t v) {
  if (v == 0) return 0;
  int32_t msb = 0;  // FT_MSB
  for (uint32_t z = v; z >>= 1;) ++msb;
  const uint64_t r = (uint64_t{v} << 16) - 1;
  uint32_t q = uint32_t{1} << ((17 + msb) >> 1);
  uint32_t t = 0;
  do {
    t = q;
    q = (t + static_cast<uint32_t>(r / t) + 1) >> 1;
  } while (q != t);
  return q;
}

}  // namespace ttf
