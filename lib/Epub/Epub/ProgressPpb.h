#pragma once

#include <compare>
#include <cstdint>

// How far through a book or a chapter, in parts per billion: 0 at its start, 1,000,000,000 at its end.
//
// Integer, because the C3 boards have no FPU and a float there is a soft-float call per operation
// (docs/design/integer-math.md). Every fraction is floored, and a percent rounded half up from a floored
// billionth is the exact fraction's rounding: floor((floor(x * 10^9) + 5 * 10^6) / 10^7) == floor(x * 100 + 1/2).
//
// Why billionths and not millionths: some screens round as printf's "%.0f" and "%.2f" did, a half to the even
// neighbour, and that needs to know an exact half from a value just above one. A floored millionth reads 0.38503 %
// as the half 0.385 % one time in a hundred; a floored billionth does so one time in a hundred thousand, while an
// exact half (a chapter starting 12.5 % into the book) stays exact. KOReader's records carry six decimals; the
// wire rounds to those (KOReaderJsonNumber.h).
class ProgressPpb {
 public:
  static constexpr uint32_t kWhole = 1000000000;

  constexpr ProgressPpb() = default;
  // Past the whole is the whole.
  constexpr explicit ProgressPpb(const uint32_t ppb) : ppb_(ppb < kWhole ? ppb : kWhole) {}

  // `part` of `whole`, floored, in [0, kWhole]: page 3 of 10 is 300,000,000. 0 for a `whole` of 0 or less or a
  // `part` below 0. Exact for any part and whole below 2^33.
  static constexpr ProgressPpb of(const int64_t part, const int64_t whole) {
    if (whole <= 0 || part <= 0) return ProgressPpb();
    if (part >= whole) return ProgressPpb(kWhole);
    return ProgressPpb(static_cast<uint32_t>(static_cast<uint64_t>(part) * kWhole / static_cast<uint64_t>(whole)));
  }

  // A point `inPart` of the way through a part of `partSize` units that starts `before` units into a whole of
  // `whole`: a chapter's place in a book by bytes. Two 32-bit sizes times a billion stay below 2^63.
  static constexpr ProgressPpb across(const uint32_t before, const uint32_t partSize, const uint32_t whole,
                                      const ProgressPpb inPart) {
    if (whole == 0) return ProgressPpb();
    const uint64_t billionths = static_cast<uint64_t>(before) * kWhole + static_cast<uint64_t>(partSize) * inPart.ppb_;
    const uint64_t ppb = billionths / whole;
    return ProgressPpb(ppb < kWhole ? static_cast<uint32_t>(ppb) : kWhole);
  }

  constexpr uint32_t ppb() const { return ppb_; }

  // floor(v * this): where this point falls in `v` units (bytes, pages).
  constexpr uint32_t partOf(const uint32_t v) const {
    return static_cast<uint32_t>(static_cast<uint64_t>(v) * ppb_ / kWhole);
  }
  // The same rounded to the nearest unit, a half up.
  constexpr uint32_t roundedPartOf(const uint32_t v) const {
    return static_cast<uint32_t>((static_cast<uint64_t>(v) * ppb_ + kWhole / 2) / kWhole);
  }

  // Whole percents. Each display keeps the rounding it had as a float: `(int)(x * 100 + 0.5f)` and lround() are
  // half up, printf's "%.0f" sends a half to the even neighbour, `(int)(x * 100)` floors.
  constexpr int percentHalfUp() const { return static_cast<int>((ppb_ + kPercent / 2) / kPercent); }
  constexpr int percentHalfEven() const { return static_cast<int>(halfEven(ppb_, kPercent)); }
  constexpr int percentFloor() const { return static_cast<int>(ppb_ / kPercent); }
  // Hundredths of a percent (basis points), 0..10000: floored, and as printf's "%.2f" rounds a percent.
  constexpr uint32_t basisPoints() const { return ppb_ / kBasisPoint; }
  constexpr uint32_t basisPointsHalfEven() const { return halfEven(ppb_, kBasisPoint); }
  // Millionths, rounded half up: the six decimals of a KOReader record.
  constexpr uint32_t millionths() const { return (ppb_ + 500) / 1000; }

  // How far apart two points are, in billionths.
  friend constexpr uint32_t distance(const ProgressPpb a, const ProgressPpb b) {
    return a.ppb_ > b.ppb_ ? a.ppb_ - b.ppb_ : b.ppb_ - a.ppb_;
  }
  friend constexpr auto operator<=>(ProgressPpb, ProgressPpb) = default;

 private:
  static constexpr uint32_t kPercent = kWhole / 100;
  static constexpr uint32_t kBasisPoint = kWhole / 10000;

  // v / d to the nearest, a half to the even quotient.
  static constexpr uint32_t halfEven(const uint32_t v, const uint32_t d) {
    const uint32_t q = v / d;
    const uint32_t r = v % d;
    return q + (r > d - r || (r == d - r && (q & 1u)) ? 1 : 0);
  }

  uint32_t ppb_ = 0;
};
