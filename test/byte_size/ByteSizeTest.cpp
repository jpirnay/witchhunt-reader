// ByteSize (src/util/ByteSize.h): byte counts as "12.3 MB" in integers. The screens and the web pages printed them with
// printf("%.1f") (and "%.0f") of bytes / 1024.0 or / 1048576.0, which a double holds exactly; these compare the integer
// text with that printf over a wide range, the halfway points included, where printf rounds a tie to even.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "ByteSize.h"

namespace {

std::string tenths(const uint64_t bytes, const unsigned shift) {
  char text[32];
  byte_size::formatTenths(text, sizeof(text), bytes, shift);
  return text;
}

std::string printfTenths(const uint64_t bytes, const unsigned shift) {
  char text[32];
  snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / static_cast<double>(uint64_t{1} << shift));
  return text;
}

// The byte counts whose tenths (or whole units) are exactly halfway, and their neighbours: bytes * scale is an odd
// multiple of half a unit.
void checkHalfways(const unsigned shift, const unsigned scale, const uint64_t limit) {
  const uint64_t half = uint64_t{1} << (shift - 1);
  for (uint64_t odd = 1; odd * half / scale <= limit; odd += 2) {
    if ((odd * half) % scale != 0) continue;
    const uint64_t tie = odd * half / scale;
    for (uint64_t bytes = tie - (tie > 0 ? 1 : 0); bytes <= tie + 1; bytes++) {
      char expected[32];
      char got[32];
      if (scale == 10) {
        snprintf(expected, sizeof(expected), "%.1f",
                 static_cast<double>(bytes) / static_cast<double>(uint64_t{1} << shift));
        byte_size::formatTenths(got, sizeof(got), bytes, shift);
      } else {
        snprintf(expected, sizeof(expected), "%.0f",
                 static_cast<double>(bytes) / static_cast<double>(uint64_t{1} << shift));
        snprintf(got, sizeof(got), "%llu", static_cast<unsigned long long>(byte_size::rounded(bytes, shift, 1)));
      }
      ASSERT_STREQ(got, expected) << bytes << " bytes, shift " << shift;
    }
  }
}

TEST(ByteSize, TenthsOfAKilobyteMatchPrintfForEveryCountUpToAMegabyte) {
  for (uint64_t bytes = 0; bytes <= 1u << 20; bytes++) {
    ASSERT_EQ(tenths(bytes, byte_size::KB), printfTenths(bytes, byte_size::KB)) << bytes;
  }
}

TEST(ByteSize, TenthsOfAMegabyteMatchPrintf) {
  for (uint64_t bytes = 0; bytes <= 64u << 20; bytes += 61) {
    ASSERT_EQ(tenths(bytes, byte_size::MB), printfTenths(bytes, byte_size::MB)) << bytes;
  }
  std::mt19937_64 random(20261121);
  for (int i = 0; i < 200000; i++) {
    const uint64_t bytes = random() % (uint64_t{1} << 41);  // up to a 2 TB card
    ASSERT_EQ(tenths(bytes, byte_size::MB), printfTenths(bytes, byte_size::MB)) << bytes;
    ASSERT_EQ(tenths(bytes, byte_size::GB), printfTenths(bytes, byte_size::GB)) << bytes;
  }
}

TEST(ByteSize, HalfwayPointsRoundToEvenAsPrintfDoes) {
  // 0.25 MB is "0.2" and 0.75 MB is "0.8": printf's tie to even, not a half up.
  EXPECT_EQ(tenths(262144, byte_size::MB), "0.2");
  EXPECT_EQ(tenths(786432, byte_size::MB), "0.8");
  checkHalfways(byte_size::KB, 10, 64u << 20);
  checkHalfways(byte_size::MB, 10, uint64_t{64} << 30);
  checkHalfways(byte_size::GB, 10, uint64_t{2} << 40);
  checkHalfways(byte_size::KB, 1, 64u << 20);  // FontDownloadActivity's whole KB, "%.0f"
  checkHalfways(byte_size::MB, 1, uint64_t{64} << 30);
}

TEST(ByteSize, WholeKilobytesMatchPrintf) {
  for (uint64_t bytes = 1024; bytes < 1u << 20; bytes++) {
    char expected[32];
    snprintf(expected, sizeof(expected), "%.0f", static_cast<double>(bytes) / 1024.0);
    ASSERT_EQ(std::to_string(byte_size::rounded(bytes, byte_size::KB, 1)), expected) << bytes;
  }
}

TEST(ByteSize, BookInfoFloatQuotientAgreesWhereAFloatHoldsTheCount) {
  // BookInfoActivity divided a float, exact up to 2^24 bytes: there it printed what the double did.
  for (uint64_t bytes = 1024; bytes < uint64_t{1} << 24; bytes += 13) {
    char expected[32];
    if (bytes < 1u << 20) {
      snprintf(expected, sizeof(expected), "%.1f KB", static_cast<double>(static_cast<size_t>(bytes) / 1024.0f));
    } else {
      snprintf(expected, sizeof(expected), "%.1f MB",
               static_cast<double>(static_cast<size_t>(bytes) / (1024.0f * 1024.0f)));
    }
    char got[32];
    byte_size::formatTenths(got, sizeof(got), bytes, bytes < 1u << 20 ? byte_size::KB : byte_size::MB,
                            bytes < 1u << 20 ? "KB" : "MB");
    ASSERT_STREQ(got, expected) << bytes;
  }
}

TEST(ByteSize, Unit) {
  char text[32];
  byte_size::formatTenths(text, sizeof(text), 3565158, byte_size::MB, "MB");
  EXPECT_STREQ(text, "3.4 MB");
  byte_size::formatTenths(text, sizeof(text), 81920, byte_size::KB, "KB");
  EXPECT_STREQ(text, "80.0 KB");
}

}  // namespace
