// HalClock's temperature drift model and NTP learner in integers (lib/hal/ClockDrift.h), held against the float model
// they replaced, and the float decoder that reads the IDF sensor and older firmware's stored floats (FloatBits.h).
#include <ClockDrift.h>
#include <FloatBits.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using clock_drift::correctedUs;
using clock_drift::kScaleMax;
using clock_drift::kScaleMin;
using clock_drift::kScaleOne;
using clock_drift::kTempUnknown;
using clock_drift::learnedScale;
using clock_drift::ratePpb;

namespace {

// The float model, as HalClock.cpp had it before the integer clock: computeCorrectedElapsedSec() and the learner in
// syncNtp(), with the interval and error converted to float as it did.
namespace legacy {
constexpr float kDriftPerSecondPerDeg = 120.0f / 86400.0f;

double correctedSec(const uint64_t elapsedUs, const float captureC, const float nowC, const float scale) {
  const float avgTemp = (captureC + nowC) * 0.5f;
  const float tempDelta = captureC - avgTemp;
  float tempFactor = 1.0f + tempDelta * kDriftPerSecondPerDeg * scale;
  if (tempFactor < 0.5f) {
    tempFactor = 0.5f;
  } else if (tempFactor > 1.5f) {
    tempFactor = 1.5f;
  }
  return static_cast<double>(elapsedUs) / 1000000.0 * static_cast<double>(tempFactor);
}

float learnedScale(float scale, const int64_t intervalSec, const int64_t errorSec, const float prevC,
                   const float nowC) {
  const auto interval = static_cast<float>(intervalSec);
  const auto error = static_cast<float>(errorSec);
  if (interval >= 60.0f) {
    const float observedDriftPerDay = error * 86400.0f / interval;
    const float tempDelta = nowC - prevC;
    if (std::fabs(tempDelta) > 0.1f) {
      const float modelDriftPerDay = 120.0f * tempDelta;
      if (std::fabs(modelDriftPerDay) > 0.01f) {
        const float measuredScale = observedDriftPerDay / modelDriftPerDay;
        scale = std::max(0.1f, std::min(5.0f, 0.9f * scale + 0.1f * measuredScale));
      }
    }
  }
  return scale;
}
}  // namespace legacy

constexpr uint64_t kSecondUs = 1000000;
constexpr uint64_t kDayUs = 86400 * kSecondUs;

uint64_t corrected(const uint64_t elapsedUs, const int32_t captureCenti, const int32_t nowCenti, const int32_t scale) {
  return correctedUs(elapsedUs, ratePpb(captureCenti, nowCenti, scale));
}

// The exact model in double, which holds every product here to well under a microsecond.
double exactCorrectedUs(const uint64_t elapsedUs, const int32_t captureCenti, const int32_t nowCenti,
                        const int32_t scale) {
  const double factor = std::clamp(1.0 + (captureCenti - nowCenti) / 200.0 / 720.0 * scale / 1e6, 0.5, 1.5);
  return static_cast<double>(elapsedUs) * factor;
}

}  // namespace

// Pinned: a day at 5 degrees cooler is 300 s longer (2 min/day/degree over the 2.5-degree mean difference).
TEST(ClockDrift, CorrectedElapsedPinned) {
  // The rate rounds to 3,472,222 ppb (1/288 is 3,472,222.2): 19 us short of 86,700 s. The float model read
  // 86,699.998856 s.
  EXPECT_EQ(ratePpb(2500, 2000, kScaleOne), 3472222);
  EXPECT_EQ(corrected(kDayUs, 2500, 2000, kScaleOne), 86699999980ull);
  EXPECT_EQ(corrected(kDayUs, 2000, 2500, kScaleOne), 86100000020ull);  // warmed: shorter (float: 86,100.001144 s)
  // Eight hours from 31.5 to 22.25 degrees at a learned 0.8: 148 s, the rate rounded up 0.11 ppb (float: 28,947.999573
  // s).
  EXPECT_EQ(corrected(8 * 3600 * kSecondUs, 3150, 2225, 800000), 28948000003ull);
  // No temperature change, no correction.
  EXPECT_EQ(corrected(600 * kSecondUs, 3000, 3000, kScaleOne), 600 * kSecondUs);
  // The factor stops at 1.5 and 0.5, as the float one did.
  EXPECT_EQ(ratePpb(15000, -2000, kScaleMax), clock_drift::kMaxRatePpb);
  EXPECT_EQ(corrected(3600 * kSecondUs, 15000, -2000, kScaleMax), 5400 * kSecondUs);
  EXPECT_EQ(corrected(3600 * kSecondUs, -2000, 15000, kScaleMax), 1800 * kSecondUs);
}

// Over temperatures, scales and intervals from a second to a year: the integer correction is the exact model to a
// nanosecond per second (its rate is whole ppb) and a microsecond, and within the float model's own rounding of it
// (the float factor carries about 1e-7).
TEST(ClockDrift, CorrectedElapsedMatchesTheFloatModel) {
  const uint64_t elapsed[] = {kSecondUs,       600 * kSecondUs + 123, 3600 * kSecondUs, kDayUs + 999999,
                              7 * kDayUs + 17, 30 * kDayUs,           365 * kDayUs + 1};
  const int32_t scales[] = {kScaleMin, 500000, kScaleOne, 1234567, 2500000, kScaleMax};
  for (int32_t capture = -2000; capture <= 6000; capture += 137) {
    for (int32_t now = -2000; now <= 6000; now += 211) {
      for (const int32_t scale : scales) {
        for (const uint64_t e : elapsed) {
          const auto got = static_cast<double>(corrected(e, capture, now, scale));
          const double exact = exactCorrectedUs(e, capture, now, scale);
          ASSERT_LE(std::fabs(got - exact), static_cast<double>(e) * 0.5e-9 + 1.0)
              << e << " " << capture << " " << now << " " << scale;
          const double floatUs =
              legacy::correctedSec(e, capture / 100.0f, now / 100.0f, scale / 1e6f) * static_cast<double>(kSecondUs);
          ASSERT_LE(std::fabs(got - floatUs), static_cast<double>(e) * 2e-7 + 1.0)
              << e << " " << capture << " " << now << " " << scale;
        }
      }
    }
  }
}

TEST(ClockDrift, IntervalsPastThirtyFiveYearsAreHeldThere) {
  EXPECT_EQ(correctedUs(UINT64_MAX, 0), clock_drift::kMaxElapsedUs);
  EXPECT_EQ(correctedUs(UINT64_MAX, clock_drift::kMaxRatePpb), clock_drift::kMaxElapsedUs / 2 * 3);
}

TEST(ClockDrift, TemperatureSanity) {
  EXPECT_TRUE(clock_drift::temperatureSane(-8000));
  EXPECT_TRUE(clock_drift::temperatureSane(2537));
  EXPECT_TRUE(clock_drift::temperatureSane(15000));
  EXPECT_FALSE(clock_drift::temperatureSane(-8001));
  EXPECT_FALSE(clock_drift::temperatureSane(15001));
  EXPECT_FALSE(clock_drift::temperatureSane(kTempUnknown));
}

// Pinned learner steps.
TEST(ClockDrift, LearnedScalePinned) {
  // A day, 30 s fast, 2 degrees warmer: measured 30 x 72000 / (86400 x 200) = 0.125, blended at a tenth.
  EXPECT_EQ(learnedScale(kScaleOne, 86400, 30, 2250, 2450), 912500);  // float: 0.912499964
  EXPECT_EQ(learnedScale(2000000, 86400, -30, 2250, 2450), 1787500);  // float: 1.787499905
  // The incident in HalClock::syncNtp()'s comment: 1217 s slow over 2689 s drove the scale to the floor.
  EXPECT_EQ(learnedScale(kScaleOne, 2689, -1217, 2500, 3500), kScaleMin);
  // Saturated either way, whatever the scale was.
  EXPECT_EQ(learnedScale(kScaleMax, 60, -100000, 2500, 2600), kScaleMin);
  EXPECT_EQ(learnedScale(kScaleMin, 60, 100000, 2500, 2600), kScaleMax);
}

TEST(ClockDrift, LearnedScaleSkipsWhatTheFloatModelSkipped) {
  EXPECT_EQ(learnedScale(1234567, 59, 30, 2250, 2450), 1234567);             // under a minute
  EXPECT_EQ(learnedScale(1234567, 86400, 30, 2250, 2255), 1234567);          // 0.05 degree
  EXPECT_EQ(learnedScale(1234567, 86400, 30, 2250, 2260), 1234567);          // 0.1 degree: not more than 0.1
  EXPECT_NE(learnedScale(1234567, 86400, 30, 2250, 2261), 1234567);          // 0.11 degree
  EXPECT_EQ(learnedScale(1234567, 86400, 30, kTempUnknown, 2450), 1234567);  // NaN in the float model
  EXPECT_EQ(learnedScale(1234567, 86400, 30, 2250, kTempUnknown), 1234567);
  // No measurement at all: an error past 2^40 s.
  EXPECT_EQ(learnedScale(1234567, 86400, (int64_t{1} << 40) + 1, 2250, 2450), 1234567);
}

// Over intervals, errors, temperature changes and scales: the exact model to a millionth, and the float learner to
// its own rounding. That is mostly the temperatures, which float holds to about 2e-6 degree (33.28 is
// 33.279998779): over a change of 0.11 degree, 2e-5 of the measured scale. Changes of exactly 0.1 degree are left
// out: the float model decided those by how the two temperatures rounded.
TEST(ClockDrift, LearnedScaleMatchesTheFloatModel) {
  const int64_t intervals[] = {60, 61, 600, 2689, 3600, 86400, 7 * 86400, 30 * 86400, 400 * 86400};
  const int64_t errors[] = {-100000, -3600, -1217, -100, -7, -1, 0, 1, 3, 42, 900, 3600, 100000};
  const int32_t scales[] = {kScaleMin, 333333, kScaleOne, 1500000, 4200000, kScaleMax};
  int compared = 0;
  for (const int64_t interval : intervals) {
    for (const int64_t error : errors) {
      for (const int32_t scale : scales) {
        for (int32_t prev = -1000; prev <= 5000; prev += 1500) {
          for (int32_t delta = -3000; delta <= 3000; delta += 7) {
            if (delta == 10 || delta == -10) continue;
            const int32_t now = prev + delta;
            const int32_t got = learnedScale(scale, interval, error, prev, now);
            double exact = scale / 1e6;
            double measured = 0;
            if (delta > 10 || delta < -10) {
              measured = static_cast<double>(error) * 72000.0 / (static_cast<double>(interval) * delta);
              exact = std::clamp(0.9 * exact + 0.1 * measured, 0.1, 5.0);
            }
            ASSERT_NEAR(got, exact * 1e6, 1.0) << interval << " " << error << " " << scale << " " << prev << " " << now;
            const float want = legacy::learnedScale(scale / 1e6f, interval, error, prev / 100.0f, now / 100.0f);
            const double floatRounding = 2.0 + 0.1 * std::fabs(measured) * 1e6 * (3e-4 / std::abs(delta) + 1e-6);
            ASSERT_NEAR(got, want * 1e6, floatRounding)
                << interval << " " << error << " " << scale << " " << prev << " " << now;
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 1000000);
}

// The decoder against exact arithmetic: double holds a float times any scale here (24 + 20 bits) exactly, and llround
// rounds half away from zero.
TEST(FloatBits, MatchesExactRounding) {
  const int32_t scales[] = {1, 100, 1000, kScaleOne};
  uint32_t state = 0x12345678u;
  for (int i = 0; i < 2000000; ++i) {
    state = state * 1664525u + 1013904223u;
    // Half of them with an exponent near the units, where the interesting roundings are.
    uint32_t bits = state;
    if (i % 2 == 0) bits = (bits & 0x807FFFFFu) | ((110u + (state >> 27)) << 23);
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    for (const int32_t scale : scales) {
      int32_t got = 0;
      const bool ok = fixedFromFloatBits(bits, scale, got);
      if (!std::isfinite(value)) {
        ASSERT_FALSE(ok) << bits;
        continue;
      }
      const double product = static_cast<double>(value) * scale;
      if (std::fabs(product) > 4e9) {
        ASSERT_FALSE(ok) << bits << " x " << scale;
        continue;
      }
      const long long magnitude = std::llround(std::fabs(product));
      ASSERT_EQ(ok, magnitude <= INT32_MAX) << bits << " x " << scale;
      if (ok) {
        ASSERT_EQ(got, product < 0 ? -magnitude : magnitude) << bits << " x " << scale;
      }
    }
  }
}

TEST(FloatBits, SpecialValues) {
  const auto bitsOf = [](const float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
  };
  int32_t out = 7;
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(0.0f), 100, out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(-0.0f), 100, out));
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(25.37f), 100, out));
  EXPECT_EQ(out, 2537);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(-12.345f), 100, out));  // -12.3450002670288...
  EXPECT_EQ(out, -1235);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(0.005f), 100, out));  // 0.00499999989: below the half
  EXPECT_EQ(out, 0);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(0.125f), 100, out));  // exactly 12.5: away from zero
  EXPECT_EQ(out, 13);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(-0.125f), 100, out));
  EXPECT_EQ(out, -13);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(1.0f), kScaleOne, out));  // the stored drift scale's default
  EXPECT_EQ(out, kScaleOne);
  EXPECT_TRUE(fixedFromFloatBits(bitsOf(0.1f), kScaleOne, out));
  EXPECT_EQ(out, kScaleMin);
  EXPECT_TRUE(fixedFromFloatBits(0x00000001u, kScaleOne, out));  // the smallest subnormal
  EXPECT_EQ(out, 0);
  EXPECT_FALSE(fixedFromFloatBits(bitsOf(NAN), 100, out));
  EXPECT_FALSE(fixedFromFloatBits(bitsOf(INFINITY), 100, out));
  EXPECT_FALSE(fixedFromFloatBits(bitsOf(-INFINITY), 100, out));
  EXPECT_FALSE(fixedFromFloatBits(bitsOf(3e7f), 100, out));  // past int32
  EXPECT_FALSE(fixedFromFloatBits(bitsOf(-3e38f), 100, out));
}
