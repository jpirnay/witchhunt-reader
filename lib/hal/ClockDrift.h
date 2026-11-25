#pragma once

#include <cstdint>

// HalClock's temperature drift model and its NTP learner, in integers: the C3 has no FPU. Pure, so the host tests hold
// it against the float model it replaced (test/clock_drift).
//
// The chip's low-power (RC) clock runs fast when warm and slow when cool, by about 2 minutes a day per degree C off
// the temperature it was captured at (120 s / 86400 s = 1/720 per degree), times a per-board scale that each NTP sync
// nudges toward what it measured. Experimental source:
// https://www.reddit.com/r/esp32/comments/11cikkp/the_clock_on_the_esp_is_wrong/
namespace clock_drift {

// Temperatures are hundredths of a degree C. Unknown: the chip sensor failed (Arduino's temperatureRead() returns
// NaN) or a stored value was not a number. The float model carried the NaN along and every comparison failed; the
// functions below skip the same steps.
constexpr int32_t kTempUnknown = INT32_MIN;

// A captured temperature the RTC-memory snapshot may hold: -80..150 degrees. kTempUnknown is outside.
inline bool temperatureSane(const int32_t centi) { return centi >= -8000 && centi <= 15000; }

// The learned scale in millionths of the model: kScaleOne trusts 2 min/day/degree as it is.
constexpr int32_t kScaleOne = 1000000;
constexpr int32_t kScaleMin = 100000;   // 0.1
constexpr int32_t kScaleMax = 5000000;  // 5.0

// The rate correction, parts per billion, for a capture at `captureCenti` read again at `nowCenti`: positive when the
// device cooled down, as the RC oscillator then under-counted and the true interval is longer. The interval's mean
// temperature is taken as the average of its ends (the trapezoidal rule), so the difference that counts is half of
// capture - now. Clamped to +-0.5 (a factor of 0.5..1.5), as the float factor was. Both temperatures must be known.
constexpr int32_t kMaxRatePpb = 500000000;
inline int32_t ratePpb(const int32_t captureCenti, const int32_t nowCenti, const int32_t scaleMicro) {
  // (capture - now) / 200 degrees x 1/720 per degree x scale / 10^6, in 10^-9: (capture - now) x scale / 144, rounded
  // half away from zero.
  const int64_t product = (static_cast<int64_t>(captureCenti) - nowCenti) * scaleMicro;
  const int64_t ppb = (product >= 0 ? product + 72 : product - 72) / 144;
  if (ppb < -kMaxRatePpb) return -kMaxRatePpb;
  if (ppb > kMaxRatePpb) return kMaxRatePpb;
  return static_cast<int32_t>(ppb);
}

// `elapsedUs` of LP-timer time corrected by `ppb`: elapsedUs x (1 + ppb / 10^9), the correction truncated toward zero.
// An interval past kMaxElapsedUs (35 years) is taken as that long: no real one is, and restore() rejects past 5 years.
constexpr uint64_t kMaxElapsedUs = uint64_t{1} << 50;
inline uint64_t correctedUs(const uint64_t elapsedUs, const int32_t ppb) {
  const auto e = static_cast<int64_t>(elapsedUs < kMaxElapsedUs ? elapsedUs : kMaxElapsedUs);
  // Split at 10^9 so neither product passes 2^63: e / 10^9 < 2^21, e % 10^9 < 2^30, |ppb| < 2^29.
  const int64_t correction = e / 1000000000 * ppb + e % 1000000000 * ppb / 1000000000;
  return static_cast<uint64_t>(e + correction);
}

namespace detail {
// error x 72,000,000,000 / (interval x delta), truncated toward zero and saturated at +-10^8, for |error| <= 2^40,
// 60 <= interval <= 2^37 and 0 < |delta| <= 2^22.
inline int64_t measuredScaleMicro(const int64_t errorSec, const int64_t intervalSec, const int64_t deltaCenti) {
  constexpr int64_t kLimit = 100000000;
  const bool negative = (errorSec < 0) != (deltaCenti < 0);
  const uint64_t numerator = (errorSec < 0 ? 0 - static_cast<uint64_t>(errorSec) : errorSec) * 720u;  // < 2^50
  const uint64_t denominator = static_cast<uint64_t>(intervalSec) *
                               (deltaCenti < 0 ? 0 - static_cast<uint64_t>(deltaCenti) : deltaCenti);  // < 2^59
  // numerator / denominator is the quotient in units of 10^8 millionths: at 1 or more it saturates.
  if (numerator >= denominator) return negative ? -kLimit : kLimit;
  // Below 1: its first eight decimals by long division, each step under 10 x 2^59.
  uint64_t remainder = numerator;
  int64_t quotient = 0;
  for (int digit = 0; digit < 8; ++digit) {
    remainder *= 10;
    quotient = quotient * 10 + static_cast<int64_t>(remainder / denominator);
    remainder %= denominator;
  }
  return negative ? -quotient : quotient;
}
}  // namespace detail

// The NTP learner: the scale after a sync that found the clock `errorSec` off (local minus NTP; negative when it was
// behind) `intervalSec` after the previous sync, the temperature having gone from `prevCenti` to `nowCenti` between
// them. The measured scale is the drift seen per day over the drift the model expects for that change (120 s/day per
// degree); it is blended in at a tenth and the result clamped to 0.1..5.0.
//
// Unchanged when the interval is under a minute, a temperature is unknown, or the temperature moved 0.1 degree or
// less. Also when an input is no measurement at all (an error past 2^40 s, an interval past 2^37 s, a change past
// 2^22 hundredths of a degree), which keeps the arithmetic inside 64 bits; the float model would have moved the scale
// on such garbage.
//
// The measured quotient saturates at +-100: past it the clamp decides whatever the old scale was (0.9 x 5.0 + 10 is
// above 5.0, 0.9 x 0.1 - 10 below 0.1).
inline int32_t learnedScale(const int32_t scaleMicro, const int64_t intervalSec, const int64_t errorSec,
                            const int32_t prevCenti, const int32_t nowCenti) {
  if (intervalSec < 60 || prevCenti == kTempUnknown || nowCenti == kTempUnknown) return scaleMicro;
  const int64_t delta = static_cast<int64_t>(nowCenti) - prevCenti;
  if (delta >= -10 && delta <= 10) return scaleMicro;
  constexpr int64_t kMaxError = int64_t{1} << 40;
  if (intervalSec > (int64_t{1} << 37) || delta < -(int64_t{1} << 22) || delta > (int64_t{1} << 22) ||
      errorSec < -kMaxError || errorSec > kMaxError) {
    return scaleMicro;
  }
  // measured = (error x 86400 / interval) / (120 x delta / 100) = error x 72000 / (interval x delta), in millionths.
  const int64_t tenTimes =
      9 * static_cast<int64_t>(scaleMicro) + detail::measuredScaleMicro(errorSec, intervalSec, delta);
  const int64_t blended = (tenTimes >= 0 ? tenTimes + 5 : tenTimes - 5) / 10;
  if (blended < kScaleMin) return kScaleMin;
  if (blended > kScaleMax) return kScaleMax;
  return static_cast<int32_t>(blended);
}

// ---- The wall clock to the microsecond ----
// An RTC-less board (the X4) keeps its clock through sleeps and restarts as a snapshot: an epoch and the LP timer's
// reading at it. Set and read in whole seconds, it lost the fraction at every capture and again at every wake (about a
// second per sleep, which the NTP learner then took for temperature drift), and a drift nudge stepped the clock back by
// its fraction and dropped any correction under a second.

constexpr int64_t kUsPerSecond = 1000000;

// The snapshot's pair: at LP time `lpUs` the wall clock read `epochSec` exactly.
struct Anchor {
  int64_t epochSec = 0;
  uint64_t lpUs = 0;
};

// The anchor for a wall clock of `epochUs` (0 or more) read at LP time `lpUs`: its second, at the LP time that second
// began. So the snapshot carries the fraction in the fields it always had, and firmware that reads them as before still
// reads a consistent pair. Within the LP timer's first second there is no earlier LP time to carry it in (0 reads as no
// snapshot), and the fraction is dropped as it always was.
inline Anchor anchorAt(const int64_t epochUs, const uint64_t lpUs) {
  const auto fraction = static_cast<uint64_t>(epochUs % kUsPerSecond);
  return {epochUs / kUsPerSecond, lpUs > fraction ? lpUs - fraction : lpUs};
}

// An NTP timestamp's fraction (2^-32 s) in microseconds, rounded down.
inline uint32_t ntpFractionUs(const uint32_t fraction) {
  return static_cast<uint32_t>((static_cast<uint64_t>(fraction) * kUsPerSecond) >> 32);
}

}  // namespace clock_drift
