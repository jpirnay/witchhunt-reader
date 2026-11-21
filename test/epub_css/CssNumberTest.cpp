// CSS numbers and lengths in fixed point (lib/Epub/Epub/css/CssNumber.h, CssLength in CssStyle.h): read without strtof,
// resolved to pixels as the float code resolved them.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "../../lib/Epub/Epub/css/CssStyle.h"

namespace {

CssNumber parsed(const char* text, size_t* consumed = nullptr, const bool exponent = true) {
  CssNumber n;
  const size_t taken = CssNumber::parse(text, n, exponent);
  if (consumed) *consumed = taken;
  return n;
}

}  // namespace

TEST(CssNumber, ReadsWhatStrtofRead) {
  size_t taken = 0;
  CssNumber n = parsed("1.5em", &taken);
  EXPECT_EQ(15, n.num);
  EXPECT_EQ(10, n.den);
  EXPECT_EQ(3u, taken);
  n = parsed("  .5", &taken);
  EXPECT_EQ(5, n.num);
  EXPECT_EQ(10, n.den);
  EXPECT_EQ(4u, taken);
  n = parsed("-0.35", &taken);
  EXPECT_EQ(-35, n.num);
  EXPECT_EQ(100, n.den);
  n = parsed("+2", &taken);
  EXPECT_EQ(2, n.num);
  EXPECT_EQ(2u, taken);
  n = parsed("5.", &taken);  // the point is taken, as strtof takes it
  EXPECT_EQ(5, n.num);
  EXPECT_EQ(2u, taken);
}

TEST(CssNumber, NoDigitIsNoNumber) {
  for (const char* text : {"", ".", "-", "+.", "em", "auto", "-.e1"}) {
    size_t taken = 1;
    const CssNumber n = parsed(text, &taken);
    EXPECT_EQ(0u, taken) << text;
    EXPECT_EQ(0, n.num) << text;
  }
}

TEST(CssNumber, AnExponentNeedsItsDigits) {
  size_t taken = 0;
  EXPECT_EQ(100, parsed("1e2%", &taken).num);
  EXPECT_EQ(3u, taken);
  CssNumber n = parsed("15e-1", &taken);
  EXPECT_EQ(15, n.num);
  EXPECT_EQ(10, n.den);
  n = parsed("1em", &taken);  // an e that starts the unit
  EXPECT_EQ(1, n.num);
  EXPECT_EQ(1u, taken);
  n = parsed("1e2", &taken, /*exponent=*/false);
  EXPECT_EQ(1, n.num);
  EXPECT_EQ(1u, taken);
}

TEST(CssNumber, LongNumbersStopAtNineDecimals) {
  const CssNumber n = parsed("0.1234567891234");
  EXPECT_EQ(123456789, n.num);
  EXPECT_EQ(1000000000, n.den);
  EXPECT_EQ(int64_t{100000000000000000}, parsed("123456789012345678901234567890").num);  // saturates
}

TEST(CssNumber, RoundsHalvesAwayFromZero) {
  EXPECT_EQ(3, parsed("2.5").rounded());
  EXPECT_EQ(-3, parsed("-2.5").rounded());
  EXPECT_EQ(2, parsed("2.49").rounded());
  EXPECT_TRUE(parsed("4.0").whole());
  EXPECT_FALSE(parsed("4.5").whole());
}

TEST(CssNumber, ScaleIsTheValueOverTheDivisor) {
  EXPECT_EQ(TextScale::percent(160), parsed("160").scale(100));
  EXPECT_EQ(TextScale::ratio(3, 4), parsed("12").scale(16));
  EXPECT_EQ(0u, parsed("-1").scale().raw());
  EXPECT_EQ(0u, parsed("0").scale().raw());
}

TEST(CssLength, HoldsSixteenSixteenItsMagnitudeRoundedUp) {
  EXPECT_EQ(65536, CssLength::of(1, 1, CssUnit::Pixels).value);
  EXPECT_EQ(22938, CssLength::of(35, 100, CssUnit::Em).value);                      // 0.35 x 65536 = 22937.6
  EXPECT_EQ(-19661, CssLength::of(-3, 10, CssUnit::Em).value);                      // 19660.8
  EXPECT_EQ(INT32_MAX, CssLength::of(int64_t{1} << 40, 1, CssUnit::Pixels).value);  // saturates
}

TEST(CssLength, ResolvesToPixels) {
  EXPECT_EQ(6, CssLength::of(3, 10, CssUnit::Em).toPixels(20));    // exactly 6: not 5
  EXPECT_EQ(-6, CssLength::of(-3, 10, CssUnit::Em).toPixels(20));  // nor -5
  EXPECT_EQ(24, CssLength::of(15, 10, CssUnit::Em).toPixels(16));
  EXPECT_EQ(230, CssLength::of(50, 1, CssUnit::Percent).toPixels(16, 460));
  EXPECT_EQ(15, CssLength::of(12, 1, CssUnit::Points).toPixels(16));    // 15.96
  EXPECT_EQ(133, CssLength::of(100, 1, CssUnit::Points).toPixels(16));  // 1.33 px a point
  EXPECT_EQ(3, CssLength::of(25, 10, CssUnit::Pixels).toPixelsRounded(16));
  EXPECT_EQ(-2, CssLength::of(-25, 10, CssUnit::Pixels).toPixelsRounded(16));  // (int)(-2.5 + 0.5f)
}

// For the decimals stylesheets write (two places, -5 to 5) at every em size and in every container a page has: the
// pixels the float code resolved, but where the length is a whole number of pixels and float landed just short of it.
TEST(CssLength, ResolvesAsTheFloatCodeDidButAtWholePixels) {
  int whole = 0;
  int wholeThatDiffer = 0;
  for (int hundredths = -500; hundredths <= 500; ++hundredths) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", hundredths / 100.0);
    const float f = std::strtof(text, nullptr);
    for (const CssUnit unit : {CssUnit::Em, CssUnit::Percent}) {
      const CssLength length = CssLength::of(parsed(text), unit);
      for (int size = 8; size <= (unit == CssUnit::Em ? 60 : 800); size += unit == CssUnit::Em ? 1 : 13) {
        const float px = unit == CssUnit::Em ? f * static_cast<float>(size) : f * static_cast<float>(size) / 100.0f;
        const int expected = static_cast<int>(px);
        const int expectedRounded = static_cast<int>(px + 0.5f);
        const int em = unit == CssUnit::Em ? size : 16;
        const int container = unit == CssUnit::Em ? 0 : size;
        const int64_t exact = static_cast<int64_t>(hundredths) * size;  // the pixels x 100 (x 10000 for %)
        const int64_t per = unit == CssUnit::Em ? 100 : 10000;
        if (exact % per == 0) {
          ++whole;
          EXPECT_EQ(exact / per, length.toPixels(em, container)) << text << " at " << size;
          if (expected != length.toPixels(em, container)) ++wholeThatDiffer;
          continue;
        }
        ASSERT_EQ(expected, length.toPixels(em, container)) << text << " at " << size;
        if ((2 * exact) % per != 0) {  // not a half either
          ASSERT_EQ(expectedRounded, length.toPixelsRounded(em, container)) << text << " at " << size;
        }
      }
    }
  }
  EXPECT_GT(whole, 0);
  EXPECT_LT(wholeThatDiffer * 4, whole);
}
