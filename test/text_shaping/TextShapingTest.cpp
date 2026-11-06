#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "IndicShaper.h"
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
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x05F3, 0}}));
}

TEST(TextShapingTest, ThePointedNameDropsItsMarks) {
  Pairs out;
  ASSERT_TRUE(run({YOD, 0x05B0, HE, VAV, 0x05B8, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x05F3, 0}}));
}

TEST(TextShapingTest, APrefixStays) {
  Pairs out;
  ASSERT_TRUE(run({0x05DC, 0x05B7, YOD, HE, VAV, 0x05B8, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05DC, 0}, {0x05B7, 2}, {0x05D4, 4}, {0x05F3, 4}}));
}

TEST(TextShapingTest, AMarkAfterTheLastHeBelongsToTheName) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, HE, 0x0591}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x05F3, 0}}));
}

TEST(TextShapingTest, PunctuationAfterTheNameKeepsItsOffset) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, HE, 0x002E}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x05F3, 0}, {0x002E, 8}}));
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
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x05F3, 0}, {0x05BE, 8}, {0x05D4, 10}, {0x05F3, 10}}));
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

TEST(TextShapingTest, AQamatsQatanStyleMarkAfterTheVavStillMatches) {
  Pairs out;
  ASSERT_TRUE(run({YOD, HE, VAV, 0x05BA, HE}, out));
  EXPECT_EQ(out, (Pairs{{0x05D4, 0}, {0x05F3, 0}}));
}

TEST(TextShapingTest, ALoneLeadByteAfterTheNameIsOneReplacementAtItsOffset) {
  const std::string s = utf8({YOD, HE, VAV, HE}) + "\xD7";
  TextShaping::Unit units[16];
  size_t count = 0;
  ASSERT_TRUE(TextShaping::shape(s.data(), s.size(), units, 16, count));
  ASSERT_EQ(count, 3u);
  EXPECT_EQ(units[0].cp, 0x05D4u);
  EXPECT_EQ(units[1].cp, 0x05F3u);
  EXPECT_EQ(units[2].cp, 0xFFFDu);
  EXPECT_EQ(units[2].cluster, 8);
}

TEST(TextShapingTest, ABadContinuationConsumesTheLeadAndTheGoodBytesOnly) {
  // E2 82 then 'a': utf8NextCodepoint skips the two bytes read so far as one U+FFFD, then 'a'.
  const std::string s = utf8({YOD, HE, VAV, HE}) + std::string("\xE2\x82") + "a";
  TextShaping::Unit units[16];
  size_t count = 0;
  ASSERT_TRUE(TextShaping::shape(s.data(), s.size(), units, 16, count));
  ASSERT_EQ(count, 4u);
  EXPECT_EQ(units[2].cp, 0xFFFDu);
  EXPECT_EQ(units[2].cluster, 8);
  EXPECT_EQ(units[3].cp, static_cast<uint32_t>('a'));
  EXPECT_EQ(units[3].cluster, 10);
}

TEST(TextShapingTest, ANameCutByTheLengthBeforeItsLastHeIsNotAName) {
  const std::string s = utf8({YOD, HE, VAV, HE});
  TextShaping::Unit units[16];
  size_t count = 0;
  EXPECT_FALSE(TextShaping::shape(s.data(), 6, units, 16, count));
}

namespace {

constexpr uint32_t DEVA_KA = 0x0915, DEVA_SSA = 0x0937, DEVA_MA = 0x092E, DEVA_RA = 0x0930, DEVA_HA = 0x0939,
                   DEVA_NA = 0x0928, DEVA_DA = 0x0926, DEVA_I = 0x093F, DEVA_II = 0x0940, VIRAMA = 0x094D, ZWJ = 0x200D,
                   DANDA = 0x0964, BENG_KA = 0x0995, BENG_I = 0x09BF;

TextShaping::GroupToken token(const std::string& text, uint32_t key) { return {text, key}; }

bool merges(const std::string& previous, uint32_t previousKey, const std::string& next, uint32_t nextKey,
            bool indicGroup = true) {
  return TextShaping::mergesWithPrevious(token(previous, previousKey), token(next, nextKey), indicGroup);
}

}  // namespace

TEST(TextShapingMerge, ASpanSplitOfOneStyleMerges) {
  EXPECT_TRUE(merges(utf8({DEVA_HA, DEVA_I}), 1, utf8({DEVA_NA, VIRAMA, DEVA_DA, DEVA_II}), 1));
}

TEST(TextShapingMerge, InsideASyllableAnotherStyleMerges) {
  EXPECT_TRUE(merges(utf8({DEVA_KA}), 1, utf8({DEVA_I}), 2));
  EXPECT_TRUE(merges(utf8({DEVA_KA, VIRAMA}), 1, utf8({DEVA_SSA}), 2));
  EXPECT_TRUE(merges(utf8({DEVA_RA, VIRAMA}), 1, utf8({DEVA_KA}), 2));
  EXPECT_TRUE(merges(utf8({DEVA_KA, VIRAMA}), 1, utf8({ZWJ, DEVA_SSA}), 2));
}

TEST(TextShapingMerge, BetweenSyllablesAnotherStyleStaysApart) {
  EXPECT_FALSE(merges(utf8({DEVA_KA}), 1, utf8({DEVA_MA}), 2));
}

TEST(TextShapingMerge, AGroupWithoutAShapedLetterMergesNothing) { EXPECT_FALSE(merges("foo", 1, "bar", 1, false)); }

TEST(TextShapingMerge, TheDivineNameTakesAMarkOnlyTokenGluedToIt) {
  const std::string name = utf8({YOD, 0x05B0, HE, VAV, 0x05B8, HE});
  EXPECT_TRUE(merges(name, 1, utf8({0x0591}), 1, false));
  EXPECT_TRUE(merges(name, 1, utf8({0x0591}), 2, false));
}

TEST(TextShapingMerge, AMarkOnlyTokenAfterAnotherWordStaysApart) {
  EXPECT_FALSE(merges(utf8({0x05E9, 0x05DC, 0x05D5, 0x05DD}), 1, utf8({0x0591}), 1, false));
}

TEST(IndicRuns, ARunIsOneScriptAndTheSharedCodePointsAroundIt) {
  IndicShaper::Run runs[8];
  std::string s = utf8({DEVA_KA, DEVA_I, DANDA});
  ASSERT_EQ(IndicShaper::scanRuns(s, runs), 1u);
  EXPECT_EQ(runs[0].begin, 0);
  EXPECT_EQ(runs[0].end, 9);
  ASSERT_NE(runs[0].script, nullptr);
  EXPECT_STREQ(runs[0].script->name, "Devanagari");

  s = "(" + utf8({DEVA_KA, DEVA_I}) + "),";
  ASSERT_EQ(IndicShaper::scanRuns(s, runs), 1u);
  EXPECT_EQ(runs[0].begin, 1);
  EXPECT_EQ(runs[0].end, 7);

  s = utf8({BENG_KA, BENG_I, DEVA_KA, DEVA_I});
  ASSERT_EQ(IndicShaper::scanRuns(s, runs), 2u);
  EXPECT_STREQ(runs[0].script->name, "Bengali");
  EXPECT_STREQ(runs[1].script->name, "Devanagari");
  EXPECT_EQ(runs[0].end, runs[1].begin);

  s = utf8({DANDA});
  ASSERT_EQ(IndicShaper::scanRuns(s, runs), 1u);
  EXPECT_EQ(runs[0].script, nullptr);
  EXPECT_EQ(runs[0].end, 3);

  EXPECT_EQ(IndicShaper::scanRuns("abc", runs), 0u);
  EXPECT_TRUE(IndicShaper::holdsIndic(utf8({DEVA_KA})));
  EXPECT_FALSE(IndicShaper::holdsIndic("abc"));
  EXPECT_FALSE(IndicShaper::holdsIndic(std::string_view(utf8({DEVA_KA}).data(), 2)));
}

TEST(IndicRuns, MaxRunsHoldsEveryToken) {
  // Two scripts abut with no byte between them: letters of 3 bytes, alternating, are one run each.
  std::string s;
  for (int i = 0; i < 33; i++) s += utf8({DEVA_KA, BENG_KA});
  s += utf8({DEVA_KA});  // 67 letters, 201 bytes
  ASSERT_EQ(s.size(), 201u);
  static_assert(IndicShaper::maxRuns(201) == 68);
  IndicShaper::Run runs[IndicShaper::maxRuns(201)];
  const size_t count = IndicShaper::scanRuns(s, runs);
  EXPECT_EQ(count, 67u);
  EXPECT_LE(count, IndicShaper::maxRuns(s.size()));
  EXPECT_EQ(runs[66].end, 201);
  // A token of 200 bytes (66 letters and 2 more bytes) fits the array maxRuns(200) gives.
  static_assert(IndicShaper::maxRuns(200) == 67);
  EXPECT_LE(IndicShaper::scanRuns(std::string_view(s.data(), 200), runs), IndicShaper::maxRuns(200));
}
