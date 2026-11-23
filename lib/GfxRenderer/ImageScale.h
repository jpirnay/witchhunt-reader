#pragma once

#include <cstdint>

// An image's scale as an exact ratio: `num` destination pixels for every `den` source pixels.
//
// Integer only. The C3 boards have no FPU, so a float scale costs soft-float calls per pixel, and its
// rounding can land a pixel short of a whole result: 340.0f / 1020 * 1020 is 339.99997, which floors to
// 339. A ratio of two sizes gives the exact floor.
class ImageScale {
 public:
  constexpr ImageScale() = default;  // 1:1

  // `num` destination pixels per `den` source pixels. A `num` of 0 scales everything to nothing; a negative
  // `num`, or a `den` at or below zero, gives 1:1.
  constexpr ImageScale(const int num, const int den)
      : num_(num >= 0 && den > 0 ? num : 1), den_(num >= 0 && den > 0 ? den : 1) {}

  // The scale at which a `srcW` x `srcH` image just fits a `boxW` x `boxH` box: the smaller of the two ratios.
  static constexpr ImageScale fit(const int srcW, const int srcH, const int boxW, const int boxH) {
    return smaller(ImageScale(boxW, srcW), ImageScale(boxH, srcH));
  }
  // The scale at which it just covers the box: the larger of the two ratios.
  static constexpr ImageScale fill(const int srcW, const int srcH, const int boxW, const int boxH) {
    return larger(ImageScale(boxW, srcW), ImageScale(boxH, srcH));
  }

  static constexpr ImageScale smaller(const ImageScale a, const ImageScale b) { return b < a ? b : a; }
  static constexpr ImageScale larger(const ImageScale a, const ImageScale b) { return a < b ? b : a; }

  constexpr int num() const { return num_; }
  constexpr int den() const { return den_; }
  constexpr bool isIdentity() const { return num_ == den_; }
  constexpr bool isDownscale() const { return num_ < den_; }
  // At most `n`/`d` (both above zero), e.g. atMost(1, 8) for "1/8 or smaller".
  constexpr bool atMost(const int n, const int d) const {
    return static_cast<int64_t>(num_) * d <= static_cast<int64_t>(n) * den_;
  }

  // floor(v * num / den), negative `v` included: where source offset `v` lands.
  constexpr int apply(const int v) const {
    if (v >= 0) {
      const auto u = static_cast<uint32_t>(v);
      // Both factors below 2^16 keep the product in 32 bits, which the C3 multiplies and divides in hardware.
      if (u <= 0xFFFFu && num_ <= 0xFFFF) return static_cast<int>(u * static_cast<uint32_t>(num_) / den_);
      return static_cast<int>(static_cast<uint64_t>(u) * num_ / den_);
    }
    // C++ division truncates towards zero; step down by den - 1 first to floor.
    return static_cast<int>((static_cast<int64_t>(v) * num_ - (den_ - 1)) / den_);
  }

  // How many destination pixels `n` source pixels cover: the last of them lands at apply(n - 1).
  constexpr int extent(const int n) const { return n <= 0 ? 0 : apply(n - 1) + 1; }

  friend constexpr bool operator<(const ImageScale a, const ImageScale b) {
    return static_cast<int64_t>(a.num_) * b.den_ < static_cast<int64_t>(b.num_) * a.den_;
  }
  friend constexpr bool operator==(const ImageScale a, const ImageScale b) {
    return static_cast<int64_t>(a.num_) * b.den_ == static_cast<int64_t>(b.num_) * a.den_;
  }

 private:
  int num_ = 1;
  int den_ = 1;
};

// What GfxRenderer::drawBitmap() shows of an image, and the bounds it scales that part down to fit.
struct BoxCut {
  int maxWidth = 0;       // 0: the width does not bound the scale
  int maxHeight = 0;      // 0: the height does not
  int visibleWidth = 0;   // centred columns shown; 0: all of them
  int visibleHeight = 0;  // centred rows shown; 0: all of them
};

// A `srcW` x `srcH` image filling a `boxW` x `boxH` box: cut, centred, to the box's shape, then scaled
// down to cover it. The cut rounds up and only the uncut side bounds the scale, so the cut covers the
// box exactly: its last column (row) lands at floor((shown - 1) * scale), which is at least side - 1
// and below side. An image already of the box's shape is not cut; one smaller than the box is not scaled
// up, and covers what it covers.
constexpr BoxCut cutToBox(const int srcW, const int srcH, const int boxW, const int boxH) {
  BoxCut cut{boxW, boxH, 0, 0};
  if (srcW <= 0 || srcH <= 0 || boxW <= 0 || boxH <= 0) return cut;
  const int64_t wide = static_cast<int64_t>(srcW) * boxH;
  const int64_t tall = static_cast<int64_t>(boxW) * srcH;
  if (wide > tall) {
    cut.visibleWidth = static_cast<int>((tall + boxH - 1) / boxH);  // boxW * srcH / boxH, rounded up
    cut.maxWidth = 0;
  } else if (tall > wide) {
    cut.visibleHeight = static_cast<int>((wide + boxW - 1) / boxW);  // boxH * srcW / boxW, rounded up
    cut.maxHeight = 0;
  }
  return cut;
}

// round(a / b) for b above zero, halves away from zero: integer stand-in for std::round of a quotient.
constexpr int roundedQuotient(const int64_t a, const int64_t b) {
  return static_cast<int>(a >= 0 ? (2 * a + b) / (2 * b) : -((-2 * a + b) / (2 * b)));
}
