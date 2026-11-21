#pragma once

#include <cstdint>

// A text scale in fixed point: a font-size multiplier, a font's base scale (a size drawn from another size's faces),
// small caps' 3/4, a word's size percent, or a product of them. raw() counts 2^-20ths of 1: kOne is 1.0, and the
// largest scale is 4096.
//
// Integer only. The C3 boards have no FPU, and text is scaled per glyph and per word, so a float scale cost
// soft-float calls in the reader's hottest loops (docs/future_work/integer-math.md).
//
// A scale is rounded up onto its grid (ratio(), percent(), a product), so v x scale lands on or just above the exact
// v x num / den, never below. Measured against the float expressions this type replaced (test/text_scale): a product
// that is exactly a half (25 x 1.06 = 26.5) rounds up, where the float code rounded about one in eleven of them down
// (1.06f is below 1.06); and the grid, under a millionth, moves no other product of a value below 2048 (a glyph's
// metrics, a word's width, an advance in 12.4) across a rounding step.
class TextScale {
 public:
  static constexpr int kShift = 20;
  static constexpr uint32_t kOne = uint32_t{1} << kShift;

  constexpr TextScale() = default;  // 1

  static constexpr TextScale fromRaw(const uint32_t raw) { return TextScale(raw); }
  // num / den, rounded up onto the grid; 1 for a den at or below zero or a negative num, the largest scale for one
  // above it.
  static constexpr TextScale ratio(const int64_t num, const int64_t den) {
    if (num < 0 || den <= 0) return TextScale();
    auto d = static_cast<uint64_t>(den);
    const uint64_t whole = static_cast<uint64_t>(num) / d;
    if (whole >= (uint64_t{1} << (32 - kShift))) return TextScale(UINT32_MAX);
    uint64_t rest = static_cast<uint64_t>(num) % d;
    // rest << kShift must fit 64 bits: a divisor this large (no scale of a stylesheet's has one) gives up its low bits.
    while (d >= (uint64_t{1} << 43)) {
      rest >>= 1;
      d >>= 1;
    }
    const uint64_t q = (whole << kShift) + ((rest << kShift) + d - 1) / d;
    return TextScale(q > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(q));
  }
  // pct / 100, rounded up onto the grid: percent(80) is 0.8. Per word in layout, so in 32 bits (the C3 divides those
  // in hardware, 64 bits in a library call): fine for any percent a uint8_t or a font size holds.
  static constexpr TextScale percent(const int pct) {
    if (pct < 0) return TextScale();
    if (pct >= 4096) return ratio(pct, 100);
    return TextScale(((static_cast<uint32_t>(pct) << kShift) + 99) / 100);
  }

  constexpr uint32_t raw() const { return raw_; }
  constexpr bool isOne() const { return raw_ == kOne; }

  // The product, rounded up onto the grid.
  constexpr TextScale operator*(const TextScale other) const {
    const uint64_t product = (static_cast<uint64_t>(raw_) * other.raw_ + (kOne - 1)) >> kShift;
    return TextScale(product > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(product));
  }

  // The quotient, rounded up onto the grid; the largest scale for a zero divisor.
  constexpr TextScale operator/(const TextScale other) const {
    if (other.raw_ == 0) return TextScale(UINT32_MAX);
    return saturated((static_cast<uint64_t>(raw_) << kShift) + other.raw_ - 1, other.raw_);
  }

  // v x scale + 1/2, truncated toward zero: the nearest integer, halves up, for any product from -1/2 up, which is all
  // a width, a height or an advance is; one above the nearest below that. It is the rounding the float code wrote as
  // (int)(v * s + 0.5f), and layout measured negative kerns with it, so it stays: a kern rounded otherwise moves words.
  constexpr int apply(const int v) const {
    // C++ division truncates toward zero, as the float-to-int conversion did; a divisor of 2^20 compiles to shifts.
    return static_cast<int>((static_cast<int64_t>(v) * raw_ + kHalf) / kOne);
  }
  // v x scale, nearest, halves away from zero: apply() for any v from 0 up, symmetric below. The float code's
  // (int)(v * s + (v < 0 ? -0.5f : 0.5f)).
  constexpr int applySymmetric(const int v) const {
    const int64_t product = static_cast<int64_t>(v) * raw_;
    return static_cast<int>((product + (product < 0 ? -kHalf : kHalf)) / kOne);
  }
  // v x scale, truncated toward zero: the float code's (int)(v * s).
  constexpr int applyTruncated(const int v) const { return static_cast<int>(static_cast<int64_t>(v) * raw_ / kOne); }

  // 1 / scale in 16.16, rounded to nearest: how far glyph resampling steps through the source per destination pixel
  // (ScaledGlyphResample.h). 0 for a zero scale.
  constexpr int32_t inverse16() const {
    if (raw_ == 0) return 0;
    const uint64_t inverse = ((uint64_t{1} << (16 + kShift)) + raw_ / 2) / raw_;
    return inverse > INT32_MAX ? INT32_MAX : static_cast<int32_t>(inverse);
  }

  friend constexpr bool operator==(const TextScale a, const TextScale b) { return a.raw_ == b.raw_; }
  friend constexpr bool operator!=(const TextScale a, const TextScale b) { return a.raw_ != b.raw_; }
  friend constexpr bool operator<(const TextScale a, const TextScale b) { return a.raw_ < b.raw_; }
  friend constexpr bool operator>(const TextScale a, const TextScale b) { return a.raw_ > b.raw_; }
  friend constexpr bool operator<=(const TextScale a, const TextScale b) { return a.raw_ <= b.raw_; }
  friend constexpr bool operator>=(const TextScale a, const TextScale b) { return a.raw_ >= b.raw_; }

 private:
  constexpr explicit TextScale(const uint32_t raw) : raw_(raw) {}
  static constexpr TextScale saturated(const uint64_t numerator, const uint64_t den) {
    const uint64_t q = numerator / den;
    return TextScale(q > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(q));
  }
  static constexpr int64_t kHalf = int64_t{1} << (kShift - 1);

  uint32_t raw_ = kOne;
};
