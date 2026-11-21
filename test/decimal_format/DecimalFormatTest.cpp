// formatDecimal (src/util/DecimalFormat.h): fixed-point values written as printf("%.Nf") wrote the floats they were,
// for CMD:IMU's reply and the clock drift rate on the Sync Time screen.
#include <gtest/gtest.h>
#include <util/DecimalFormat.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

std::string fmt(const int64_t num, const int64_t den, const int places, const bool plus = false) {
  char buf[48];
  formatDecimal(buf, sizeof(buf), num, den, places, plus);
  return buf;
}

std::string printfFormat(const char* format, const double value) {
  char buf[48];
  snprintf(buf, sizeof(buf), format, value);
  return buf;
}

// num / den lies exactly halfway between two values `places` decimals apart: there formatDecimal rounds away from
// zero and printf to even, on whatever binary value the double holds.
bool exactTie(const int64_t num, const int64_t den, const int places) {
  int64_t scaled = 2 * (num < 0 ? -num : num);
  for (int i = 0; i < places; ++i) scaled *= 10;
  return scaled % den == 0 && (scaled / den) % 2 == 1;
}

}  // namespace

TEST(DecimalFormat, Pinned) {
  EXPECT_EQ(fmt(-1030, 1000, 3), "-1.030");  // CMD:IMU's ax for a Picco held normally
  EXPECT_EQ(fmt(20, 1000, 3), "0.020");
  EXPECT_EQ(fmt(312500, 1000, 1), "312.5");  // mdps as dps
  EXPECT_EQ(fmt(-12350, 1000, 1), "-12.4");  // a tie, away from zero
  EXPECT_EQ(fmt(999, 1000, 0), "1");
  EXPECT_EQ(fmt(-999999, 1000, 3), "-999.999");
  EXPECT_EQ(fmt(-999999, 1000, 2), "-1000.00");  // the carry into the whole part
  EXPECT_EQ(fmt(0, 7, 2, true), "+0.00");
  EXPECT_EQ(fmt(-4, 1000, 2, true), "-0.00");  // printf keeps the sign of -0.004
  EXPECT_EQ(fmt(-1217 * 3600LL, 2689, 2, true), "-1629.30");
  EXPECT_EQ(fmt(INT64_MIN, 1, 0), "-9223372036854775808");
}

// Against printf over many values and divisors: the same text but on exact ties.
TEST(DecimalFormat, MatchesPrintfOffTies) {
  const int64_t dens[] = {1, 3, 7, 8, 1000, 3600, 86399};
  const char* formats[] = {"%.0f", "%.1f", "%.2f", "%.3f"};
  int ties = 0;
  for (int64_t num = -20000; num <= 20000; ++num) {
    for (const int64_t den : dens) {
      for (int places = 0; places < 4; ++places) {
        const std::string got = fmt(num, den, places);
        if (got == printfFormat(formats[places], static_cast<double>(num) / static_cast<double>(den))) continue;
        ASSERT_TRUE(exactTie(num, den, places)) << num << "/" << den << " at " << places << ": " << got;
        ++ties;
      }
    }
  }
  EXPECT_GT(ties, 0);
}

// The Sync Time screen's drift rate, s/hr: drift x 3600 / elapsed against the double it divided before.
TEST(DecimalFormat, SyncTimeDriftRateMatchesTheDoubleOffTies) {
  const int64_t elapsed[] = {61, 2689, 3599, 3600, 7200, 86400, 100000, 604800, 2592000};
  for (int64_t drift = -3000; drift <= 3000; ++drift) {
    for (const int64_t e : elapsed) {
      const double hours = static_cast<double>(e) / 3600.0;
      const std::string want = printfFormat("%+.2f", static_cast<double>(drift) / hours);
      const std::string got = fmt(drift * 3600, e, 2, true);
      if (got != want) {
        ASSERT_TRUE(exactTie(drift * 3600, e, 2)) << drift << " over " << e << ": " << got;
      }
    }
  }
}
