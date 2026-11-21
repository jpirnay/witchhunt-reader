// FixedDecimal (lib/Serialization/FixedDecimal.h): integers in 10^-n units, as text and from a float, without floating
// point on the device. Each is compared with the float code it replaced, over a wide range: printf's "%.*f" (a tie to
// even on the exact value), lround (a tie away from zero) and strtod.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "FixedDecimal.h"

using fixed_decimal::Tie;

namespace {

std::string formatted(const int32_t value, const unsigned decimals, const unsigned shown, const Tie tie) {
  char text[32];
  fixed_decimal::format(text, sizeof(text), value, decimals, shown, tie);
  return text;
}

std::string printfFloat(const float value, const unsigned shown) {
  char text[64];
  snprintf(text, sizeof(text), "%.*f", static_cast<int>(shown), static_cast<double>(value));
  return text;
}

TEST(FixedDecimal, Rescale) {
  EXPECT_EQ(fixed_decimal::rescale(125, 1, 0, Tie::AwayFromZero), 13);
  EXPECT_EQ(fixed_decimal::rescale(-125, 1, 0, Tie::AwayFromZero), -13);
  EXPECT_EQ(fixed_decimal::rescale(125, 1, 0, Tie::ToEven), 12);
  EXPECT_EQ(fixed_decimal::rescale(135, 1, 0, Tie::ToEven), 14);
  EXPECT_EQ(fixed_decimal::rescale(-135, 1, 0, Tie::ToEven), -14);
  EXPECT_EQ(fixed_decimal::rescale(-4, 1, 0, Tie::AwayFromZero), 0);
  EXPECT_EQ(fixed_decimal::rescale(INT32_MAX, 9, 0, Tie::AwayFromZero), 2);
  EXPECT_EQ(fixed_decimal::rescale(INT32_MIN + 1, 9, 0, Tie::AwayFromZero), -2);
  // Whole numbers from tenths, as lround rounded the float: every tenth from -100.0 to 100.0.
  for (int t = -1000; t <= 1000; t++) {
    ASSERT_EQ(fixed_decimal::rescale(t, 1, 0, Tie::AwayFromZero), std::lround(t / 10.0)) << t;
  }
}

TEST(FixedDecimal, FormatMatchesPrintfOfTheExactValue) {
  EXPECT_EQ(formatted(-1224194, 4, 1, Tie::ToEven), "-122.4");
  EXPECT_EQ(formatted(-4, 2, 1, Tie::ToEven), "-0.0");  // printf keeps the sign of what rounds to zero
  EXPECT_EQ(formatted(0, 6, 4, Tie::ToEven), "0.0000");
  EXPECT_EQ(formatted(250, 3, 1, Tie::ToEven), "0.2");
  EXPECT_EQ(formatted(250, 3, 1, Tie::AwayFromZero), "0.3");
  EXPECT_EQ(formatted(12345, 3, 0, Tie::ToEven), "12");
  EXPECT_EQ(formatted(INT32_MIN + 1, 6, 6, Tie::ToEven), "-2147.483647");
  // Thousandths shown with one decimal, against printf of the same value in binary: equal wherever the double holds the
  // value exactly, which is every value whose thousandths end in 000, 125, 250, ..., 875; elsewhere no tie can arise.
  for (int v = -20000; v <= 20000; v++) {
    char expected[32];
    snprintf(expected, sizeof(expected), "%.1f", v / 1000.0);
    // A decimal tie the double cannot hold: printf saw it off the tie.
    if (std::abs(v) % 100 == 50 && std::abs(v) % 125 != 0) continue;
    ASSERT_EQ(formatted(v, 3, 1, Tie::ToEven), expected) << v;
  }
}

TEST(FixedDecimal, ParseAcceptsWhatStrtofDidForPlainNumbers) {
  int32_t v = 0;
  ASSERT_TRUE(fixed_decimal::parse("52.52", 6, v));
  EXPECT_EQ(v, 52520000);
  ASSERT_TRUE(fixed_decimal::parse("-122.4194155", 6, v));  // the 7th decimal rounds, a half away from zero
  EXPECT_EQ(v, -122419416);
  ASSERT_TRUE(fixed_decimal::parse("  +.5", 6, v));
  EXPECT_EQ(v, 500000);
  ASSERT_TRUE(fixed_decimal::parse("7.", 6, v));
  EXPECT_EQ(v, 7000000);
  ASSERT_TRUE(fixed_decimal::parse("-0.0000001", 6, v));
  EXPECT_EQ(v, 0);
  ASSERT_TRUE(fixed_decimal::parse("2147.483647", 6, v));
  EXPECT_EQ(v, INT32_MAX);
  EXPECT_FALSE(fixed_decimal::parse("2147.4836475", 6, v));  // rounds past int32
  EXPECT_FALSE(fixed_decimal::parse("2147.483648", 6, v));
  for (const char* bad : {"", " ", ".", "-", "+.", "1.2.3", "1,5", "12a", "1e3", "0x10", "nan", "inf", "5 "}) {
    EXPECT_FALSE(fixed_decimal::parse(bad, 6, v)) << '"' << bad << '"';
  }
  // Decimal text with up to nine decimals, against strtod rounded to millionths.
  std::mt19937 random(20261121);
  for (int i = 0; i < 100000; i++) {
    char text[32];
    const int whole = static_cast<int>(random() % 361) - 180;
    const unsigned fraction = random() % 1000000000u;
    const int places = 1 + static_cast<int>(random() % 9);
    unsigned shown = fraction;
    for (int k = places; k < 9; k++) shown /= 10;
    snprintf(text, sizeof(text), "%s%d.%0*u", whole == 0 && random() % 2 ? "-" : "", whole, places, shown);
    ASSERT_TRUE(fixed_decimal::parse(text, 6, v)) << text;
    const double exact = strtod(text, nullptr) * 1e6;
    // A half-way digit string ("...5" right after the 6th decimal) rounds away from zero; strtod's binary value may sit
    // a hair either side of it, so compare with a tolerance of one where the text is a tie.
    ASSERT_NEAR(static_cast<double>(v), std::round(exact), 1.0) << text;
    if (std::fabs(exact - std::trunc(exact)) < 0.4999 || std::fabs(exact - std::trunc(exact)) > 0.5001) {
      ASSERT_EQ(v, static_cast<int32_t>(std::lround(exact))) << text;
    }
  }
}

TEST(FixedDecimal, FromFloatIsTheExactlyRoundedFloat) {
  EXPECT_EQ(fixed_decimal::fromFloat(52.52f, 6, Tie::ToEven), 52520000);
  EXPECT_EQ(fixed_decimal::fromFloat(-122.419418f, 6, Tie::ToEven), -122419418);
  EXPECT_EQ(fixed_decimal::fromFloat(0.0078125f, 6, Tie::ToEven), 7812);  // 7812.5 exactly
  EXPECT_EQ(fixed_decimal::fromFloat(0.0078125f, 6, Tie::AwayFromZero), 7813);
  EXPECT_EQ(fixed_decimal::fromFloat(-0.0f, 1, Tie::ToEven), 0);
  EXPECT_EQ(fixed_decimal::fromFloat(1e-40f, 9, Tie::ToEven), 0);  // subnormal
  EXPECT_EQ(fixed_decimal::fromFloat(1e30f, 1, Tie::ToEven), INT32_MAX);
  EXPECT_EQ(fixed_decimal::fromFloat(-INFINITY, 1, Tie::ToEven), -INT32_MAX);
  EXPECT_EQ(fixed_decimal::fromFloat(NAN, 1, Tie::ToEven), 0);
  EXPECT_EQ(fixed_decimal::fromFloat(3e9f, 0, Tie::ToEven), INT32_MAX);

  // Millionths of a coordinate: the "%.6f" printf made of the float, which the forecast request carried.
  std::mt19937 random(20261121);
  std::uniform_real_distribution<float> degrees(-180.0f, 180.0f);
  for (int i = 0; i < 300000; i++) {
    const float f = i < 1000 ? static_cast<float>(i - 500) / 128.0f : degrees(random);  // exact ties first
    char text[32];
    fixed_decimal::format(text, sizeof(text), fixed_decimal::fromFloat(f, 6, Tie::ToEven), 6, 6, Tie::ToEven);
    ASSERT_EQ(text, printfFloat(f, 6)) << f;
  }
  // Tenths, hundredths and thousandths, a tie away from zero: what llround makes of the exact double product.
  std::uniform_real_distribution<float> values(-1000.0f, 1000.0f);
  for (int i = 0; i < 300000; i++) {
    const float f = values(random);
    for (unsigned decimals = 0; decimals <= 3; decimals++) {
      const double exact = static_cast<double>(f) * static_cast<double>(fixed_decimal::pow10(decimals));
      ASSERT_EQ(fixed_decimal::fromFloat(f, decimals, Tie::AwayFromZero), std::llround(exact)) << f;
    }
  }
}

}  // namespace
