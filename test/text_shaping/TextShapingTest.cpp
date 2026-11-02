#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "TextShaping.h"

namespace {

using Pairs = std::vector<std::pair<uint32_t, uint16_t>>;

std::string utf8(const std::vector<uint32_t>& cps) {
  std::string s;
  for (const uint32_t cp : cps) {
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return s;
}

// Shapes the logical code points; returns false when drawn as is, else fills `out`.
bool run(const std::vector<uint32_t>& cps, Pairs& out, size_t capacity = 16) {
  const std::string s = utf8(cps);
  TextShaping::Unit units[16];
  size_t count = 0;
  if (!TextShaping::shape(s.data(), s.size(), units, capacity, count)) return false;
  out.clear();
  for (size_t i = 0; i < count; i++) out.emplace_back(units[i].cp, units[i].cluster);
  return true;
}

constexpr uint32_t YOD = 0x05D9, HE = 0x05D4, VAV = 0x05D5;

}  // namespace

TEST(TextShapingTest, TheNameBecomesHeAndGeresh) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x0027, 0}}));
}

TEST(TextShapingTest, ThePointedNameDropsItsMarks) {
  Pairs out;
  ASSERT_TRUE(run({YOD, 0x05B0, HE, VAV, 0x05B8, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x0027, 0}}));
}

TEST(TextShapingTest, APrefixStays) {
  Pairs out;
  ASSERT_TRUE(run({0x05DC, 0x05B7, YOD, HE, VAV, 0x05B8, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05DC, 0}, {0x05B7, 2}, {0x05D4, 4}, {0x0027, 4}}));
}

TEST(TextShapingTest, AMarkAfterTheLastHeBelongsToTheName) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, HE, 0x0591}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x0027, 0}}));
}

TEST(TextShapingTest, PunctuationAfterTheNameKeepsItsOffset) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, HE, 0x002E}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x0027, 0}, {0x002E, 8}}));
}

TEST(TextShapingTest, NearMissesAreDrawnAsTheyAre) {
  Pairs out;
  EXPECT_FALSE(run({YOD, HE, VAV}, out));
  EXPECT_FALSE(run({YOD, HE, VAV, YOD}, out));
  EXPECT_FALSE(run({0x05E9, 0x05DC, 0x05D5, 0x05DD}, out));
}

TEST(TextShapingTest, LatinAndCjkAreDrawnAsTheyAre) {
  Pairs out;
  EXPECT_FALSE(run({'h', 'e', 'l', 'l', 'o'}, out));
  EXPECT_FALSE(run({0x65E5, 0x672C}, out));
}

TEST(TextShapingTest, TwoNamesInOneToken) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, HE, 0x05BE, YOD, HE, VAV, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x0027, 0}, {0x05BE, 8}, {0x05D4, 10}, {0x0027, 10}}));
}

namespace {
// Fills a buffer with a sentinel, runs shape, and checks a false result touched neither count nor units.
void expectUntouched(const std::string& s, size_t capacity) {
  constexpr uint32_t kSentinel = 0xDEADBEEF;
  TextShaping::Unit units[4];
  for (auto& u : units) u = {kSentinel, 0xABCD};
  size_t count = 7;
  EXPECT_FALSE(TextShaping::shape(s.data(), s.size(), units, capacity, count));
  EXPECT_EQ(count, 7u);
  for (const auto& u : units) {
    EXPECT_EQ(u.cp, kSentinel);
    EXPECT_EQ(u.cluster, 0xABCD);
  }
}
}  // namespace

TEST(TextShapingTest, ABufferTooSmallDrawsAsIs) { expectUntouched(utf8({YOD, HE, VAV, HE, 0x002E}), 2); }

TEST(TextShapingTest, AHebrewWordWithoutTheNameLeavesTheBufferAlone) {
  expectUntouched(utf8({0x05E9, 0x05DC, 0x05D5, 0x05DD}), 4);
}
