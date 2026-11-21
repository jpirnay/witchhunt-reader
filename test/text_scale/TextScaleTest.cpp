// TextScale (lib/EpdFont/TextScale.h), the text scale in fixed point, checked against the float expressions it
// replaced: (int)(v * s + 0.5f), (int)(v * s + (v < 0 ? -0.5f : 0.5f)) and (int)(v * s), for the scales text is set at
// (a CSS multiplier, a built-in size drawn from the 18 pt faces, small caps' 3/4, a word's size percent, products of
// them) and for every value from -2047 to 2047 (a glyph's metrics, a word's width, an advance or a kern in 12.4).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "TextScale.h"

namespace {

// A scale as the float code had it (`f`, made as it made it) and as the fraction it stood for.
struct Case {
  float f;
  int64_t num;
  int64_t den;
  std::string name;
};

std::vector<Case> textScales() {
  std::vector<Case> cases;
  // A CSS multiplier, parsed from its decimal digits: 0.30 to 3.00.
  for (int c = 30; c <= 300; ++c) {
    cases.push_back({static_cast<float>(c) / 100.0f, c, 100, "css " + std::to_string(c) + "%"});
  }
  // The built-in sizes above the 18 pt master (CrossPointSettings::builtinFaceScale), alone and under a CSS size.
  for (const int points : {20, 22, 24, 26}) {
    const float base = static_cast<float>(points) / 18;
    cases.push_back({base, points, 18, std::to_string(points) + " pt"});
    for (int c = 50; c <= 200; c += 5) {
      cases.push_back({base * (static_cast<float>(c) / 100.0f), int64_t{points} * c, 1800,
                       std::to_string(points) + " pt x " + std::to_string(c) + "%"});
    }
  }
  // Small caps under a CSS size.
  for (int c = 50; c <= 200; c += 5) {
    cases.push_back(
        {(static_cast<float>(c) / 100.0f) * 0.75f, int64_t{c} * 3, 400, "small caps x " + std::to_string(c) + "%"});
  }
  return cases;
}

TextScale fixedOf(const Case& c) { return TextScale::ratio(c.num, c.den); }

// Whether v x num / den is an exact half (k + 1/2): the one place the float code's rounding was its float's, not the
// value's.
bool exactHalf(const int v, const Case& c) {
  const int64_t twice = 2 * static_cast<int64_t>(v) * c.num;
  return twice % (2 * c.den) == c.den || twice % (2 * c.den) == -c.den;
}

// Whether v x num / den is a whole number.
bool whole(const int v, const Case& c) { return (static_cast<int64_t>(v) * c.num) % c.den == 0; }

}  // namespace

TEST(TextScale, OneIsTwoToTheTwentieth) {
  EXPECT_EQ(uint32_t{1} << 20, TextScale().raw());
  EXPECT_TRUE(TextScale().isOne());
  EXPECT_TRUE(TextScale::percent(100).isOne());
  EXPECT_TRUE(TextScale::ratio(18, 18).isOne());
  EXPECT_EQ(TextScale::ratio(3, 4).raw(), 3u << 18);  // a binary fraction is exact
  EXPECT_EQ(TextScale::ratio(5, 4), TextScale::percent(125));
}

// Rounded up, so v x scale is never below the exact value: an exact half rounds up.
TEST(TextScale, AScaleIsRoundedUpOntoItsGrid) {
  for (const Case& c : textScales()) {
    const uint64_t raw = TextScale::ratio(c.num, c.den).raw();
    EXPECT_GE(raw * c.den, static_cast<uint64_t>(c.num) << 20) << c.name;
    EXPECT_LT((raw - 1) * c.den, static_cast<uint64_t>(c.num) << 20) << c.name;
  }
  EXPECT_EQ(943719u, TextScale::percent(90).raw());  // 0.9 x 2^20 = 943718.4
}

TEST(TextScale, PercentIsTheRatioOfAHundred) {
  for (int pct = 0; pct <= 5000; ++pct) EXPECT_EQ(TextScale::ratio(pct, 100), TextScale::percent(pct)) << pct;
}

TEST(TextScale, AProductRoundsUpAndOneLeavesAScaleAsItIs) {
  const TextScale s = TextScale::percent(90);
  EXPECT_EQ(s, s * TextScale());
  EXPECT_EQ(s, TextScale() * s);
  EXPECT_EQ(TextScale::ratio(9, 16), TextScale::ratio(3, 4) * TextScale::ratio(3, 4));
  // 0.9 x 1.1 = 0.99: the product of two rounded-up scales, rounded up, is not below it.
  const uint64_t product = (TextScale::percent(90) * TextScale::percent(110)).raw();
  EXPECT_GE(product * 100, uint64_t{99} << 20);
}

TEST(TextScale, AQuotientRoundsUp) {
  EXPECT_TRUE((TextScale::percent(90) / TextScale::percent(90)).isOne());
  EXPECT_EQ(TextScale::ratio(5, 4), TextScale::ratio(3, 2) / TextScale::percent(120));
  // 1.6 / 1.28, both rounded up already: the quotient is not below the 1.25 it stands for.
  EXPECT_GE((TextScale::percent(160) / TextScale::percent(128)).raw(), TextScale::ratio(5, 4).raw());
  EXPECT_EQ(UINT32_MAX, (TextScale() / TextScale::fromRaw(0)).raw());
}

// A ratio of large numbers is the scale they stand for, not a saturated one.
TEST(TextScale, ARatioOfLargeNumbersIsExact) {
  EXPECT_EQ(TextScale::ratio(5, 4), TextScale::ratio(int64_t{5} << 40, int64_t{4} << 40));
  EXPECT_EQ(TextScale::ratio(29 * 140, 100 * 42), TextScale::ratio(int64_t{29 * 140} << 30, int64_t{100 * 42} << 30));
}

TEST(TextScale, OutOfRangeSaturatesAndNonsenseIsOne) {
  EXPECT_EQ(UINT32_MAX, TextScale::ratio(int64_t{1} << 40, 1).raw());
  EXPECT_EQ(UINT32_MAX, (TextScale::ratio(4000, 1) * TextScale::ratio(4000, 1)).raw());
  EXPECT_TRUE(TextScale::ratio(5, 0).isOne());
  EXPECT_TRUE(TextScale::ratio(-5, 4).isOne());
  EXPECT_TRUE(TextScale::percent(-1).isOne());
  EXPECT_EQ(0u, TextScale::percent(0).raw());
}

// apply() is (int)(v * s + 0.5f): for every value and scale the same, but where v x s is an exact half and the float
// code's own error rounded it down (25 x 1.06f = 26.4999986 -> 26). There the fixed point rounds the half up, as the
// value says.
TEST(TextScale, ApplyIsTheFloatRoundingButAtExactHalves) {
  int halves = 0;
  int halvesThatDiffer = 0;
  for (const Case& c : textScales()) {
    const TextScale s = fixedOf(c);
    for (int v = -2047; v <= 2047; ++v) {
      const int expected = static_cast<int>(static_cast<float>(v) * c.f + 0.5f);
      const int got = s.apply(v);
      if (exactHalf(v, c)) {
        ++halves;
        // The half plus a half, truncated toward zero: v x s + 1/2 is whole.
        const int64_t twice = 2 * static_cast<int64_t>(v) * c.num + c.den;
        EXPECT_EQ(static_cast<int>(twice / (2 * c.den)), got) << c.name << " v=" << v;
        if (got != expected) ++halvesThatDiffer;
        continue;
      }
      ASSERT_EQ(expected, got) << c.name << " v=" << v;
    }
  }
  // How often the float code rounded a half against the value: about one in eleven (572 of 6,068 for the values 0 to
  // 511).
  EXPECT_GT(halves, 0);
  EXPECT_LT(halvesThatDiffer * 8, halves);
}

TEST(TextScale, ApplySymmetricIsTheFloatRoundingButAtExactHalves) {
  for (const Case& c : textScales()) {
    const TextScale s = fixedOf(c);
    for (int v = -2047; v <= 2047; ++v) {
      const float x = static_cast<float>(v) * c.f;
      const int expected = static_cast<int>(x + (x < 0 ? -0.5f : 0.5f));
      const int got = s.applySymmetric(v);
      if (exactHalf(v, c)) {
        // Away from zero.
        const int64_t twice = 2 * static_cast<int64_t>(v) * c.num;
        EXPECT_EQ(static_cast<int>((twice + (twice < 0 ? -c.den : c.den)) / (2 * c.den)), got) << c.name << " v=" << v;
        continue;
      }
      ASSERT_EQ(expected, got) << c.name << " v=" << v;
    }
  }
}

// applyTruncated() is (int)(v * s): the same but where v x s is whole and the float code landed just short of it.
TEST(TextScale, ApplyTruncatedIsTheFloatTruncationButAtWholeProducts) {
  for (const Case& c : textScales()) {
    const TextScale s = fixedOf(c);
    for (int v = -2047; v <= 2047; ++v) {
      const int expected = static_cast<int>(static_cast<float>(v) * c.f);
      const int got = s.applyTruncated(v);
      if (whole(v, c)) {
        EXPECT_EQ(static_cast<int>(static_cast<int64_t>(v) * c.num / c.den), got) << c.name << " v=" << v;
        continue;
      }
      ASSERT_EQ(expected, got) << c.name << " v=" << v;
    }
  }
}

// The small-caps scale is a binary fraction: every rounding is exactly the float code's.
TEST(TextScale, SmallCapsMatchTheFloatCodeExactly) {
  const TextScale s = TextScale::ratio(3, 4);
  for (int v = -4096; v <= 4096; ++v) {
    EXPECT_EQ(static_cast<int>(v * 0.75f + 0.5f), s.apply(v)) << v;
    EXPECT_EQ(static_cast<int>(v * 0.75f), s.applyTruncated(v)) << v;
    const float x = v * 0.75f;
    EXPECT_EQ(static_cast<int>(x + (x < 0 ? -0.5f : 0.5f)), s.applySymmetric(v)) << v;
  }
}

// The resampler's source step: 1 / scale in 16.16, at most a unit from the float code's 65536.0f / s + 0.5f.
TEST(TextScale, InverseIsTheResamplersStep) {
  int differ = 0;
  for (const Case& c : textScales()) {
    const int32_t expected = static_cast<int32_t>(65536.0f / c.f + 0.5f);
    const int32_t got = fixedOf(c).inverse16();
    EXPECT_LE(std::abs(expected - got), 1) << c.name;
    if (expected != got) ++differ;
  }
  EXPECT_EQ(65536, TextScale().inverse16());
  EXPECT_EQ(131072, TextScale::ratio(1, 2).inverse16());
  EXPECT_EQ(0, TextScale::fromRaw(0).inverse16());
  EXPECT_LT(differ * 20, static_cast<int>(textScales().size()));  // measured: 10 of 430
}
