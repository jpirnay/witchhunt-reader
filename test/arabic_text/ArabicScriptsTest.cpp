// ArabicScriptsTest: the Arabic run rules of lib/Utf8/ArabicScripts.h.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ArabicScripts.h"
#include "Utf8.h"

extern "C" {
#include "minibidi.h"
}

namespace {

std::string encode(const uint32_t cp) {
  std::string s;
  utf8AppendCodepoint(cp, s);
  return s;
}

// The 13 ranges of the run members.
const std::vector<std::pair<uint32_t, uint32_t>> kMemberRanges = {
    {0x0608, 0x0608}, {0x060B, 0x060B}, {0x060D, 0x060D}, {0x0610, 0x061B}, {0x061D, 0x065F},
    {0x066D, 0x06DC}, {0x06DF, 0x06E8}, {0x06EA, 0x06EF}, {0x06FA, 0x06FF}, {0x0750, 0x077F},
    {0x0870, 0x088F}, {0x0897, 0x08E1}, {0x08E3, 0x08FF},
};

bool inRanges(const uint32_t cp) {
  for (const auto& [lo, hi] : kMemberRanges) {
    if (cp >= lo && cp <= hi) return true;
  }
  return false;
}

}  // namespace

TEST(ArabicScripts, RunMembersAreTheThirteenRanges) {
  ASSERT_EQ(kMemberRanges.size(), 13u);
  for (const auto& [lo, hi] : kMemberRanges) {
    EXPECT_TRUE(arabic::isRunMember(lo)) << std::hex << lo;
    EXPECT_TRUE(arabic::isRunMember(hi)) << std::hex << hi;
    if (!inRanges(lo - 1)) EXPECT_FALSE(arabic::isRunMember(lo - 1)) << std::hex << lo - 1;
    if (!inRanges(hi + 1)) EXPECT_FALSE(arabic::isRunMember(hi + 1)) << std::hex << hi + 1;
  }
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    ASSERT_EQ(arabic::isRunMember(cp), inRanges(cp)) << std::hex << cp;
  }
  // The exclusions by name.
  for (const uint32_t cp : {0x0600u, 0x0605u, 0x0606u, 0x0607u, 0x0609u, 0x060Au, 0x060Cu, 0x060Eu, 0x060Fu, 0x061Cu,
                            0x0660u, 0x0669u, 0x066Au, 0x066Bu, 0x066Cu, 0x06DDu, 0x06DEu, 0x06E9u, 0x06F0u, 0x06F9u,
                            0x0890u, 0x0891u, 0x0896u, 0x08E2u, 0xFB50u, 0xFDF2u, 0xFE70u, 0x200Cu, 0x200Du, 0x0028u}) {
    EXPECT_FALSE(arabic::isRunMember(cp)) << std::hex << cp;
  }
  // AL punctuation and symbols are members.
  for (const uint32_t cp :
       {0x0608u, 0x060Bu, 0x060Du, 0x061Bu, 0x061Du, 0x061Eu, 0x061Fu, 0x066Du, 0x06D4u, 0x06FDu, 0x06FEu, 0x0888u}) {
    EXPECT_TRUE(arabic::isRunMember(cp)) << std::hex << cp;
  }
}

TEST(ArabicScripts, MembersAreExactlyTheBidiALAndNSMOfTheBlocks) {
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (!arabic::inBlocks(cp)) {
      ASSERT_FALSE(arabic::isRunMember(cp)) << std::hex << cp;
      continue;
    }
    if (cp >= 0x0892 && cp <= 0x0896) continue;  // unassigned: the UCD defaults them to AL
    if (cp == 0x061C) continue;                  // ALM: bidi class AL, a control the line pass consumes
    const uchar cls = bidi_class(cp);
    ASSERT_EQ(arabic::isRunMember(cp), cls == AL || cls == NSM) << std::hex << cp;
    ASSERT_EQ(arabic::isMark(cp), cls == NSM) << std::hex << cp;
  }
}

TEST(ArabicScripts, NoMirroredCodePointIsAMember) {
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (mirror(cp) != cp) ASSERT_FALSE(arabic::isRunMember(cp)) << std::hex << cp;
  }
}

TEST(ArabicScripts, TheBytePreTestMatchesTheBlocks) {
  auto holds = [](const uint32_t cp) {
    const std::string s = encode(cp);
    return arabic::holdsArabic(s.data(), s.size());
  };
  for (uint32_t cp = 0x0590; cp <= 0x0DFF; cp++) {
    const bool expected = arabic::inBlocks(cp);
    ASSERT_EQ(holds(cp), expected) << std::hex << cp;
  }
  for (uint32_t cp = 0x0600; cp <= 0x06FF; cp++) ASSERT_TRUE(holds(cp)) << std::hex << cp;
  for (uint32_t cp = 0x0750; cp <= 0x077F; cp++) ASSERT_TRUE(holds(cp)) << std::hex << cp;
  for (uint32_t cp = 0x0870; cp <= 0x08FF; cp++) ASSERT_TRUE(holds(cp)) << std::hex << cp;
  for (uint32_t cp = 0x0700; cp <= 0x074F; cp++) ASSERT_FALSE(holds(cp)) << std::hex << cp;  // Syriac
  EXPECT_FALSE(arabic::holdsArabic("\xDD\x80", 2));                                          // Syriac, DD 80-8F
  EXPECT_FALSE(arabic::holdsArabic("\xDD\x8F", 2));
  EXPECT_TRUE(arabic::holdsArabic("\xDD\x90", 2));
  EXPECT_FALSE(arabic::holdsArabic("\xDD", 1));  // cut short
  EXPECT_FALSE(arabic::holdsArabic("\xE0\xA1", 2));
  EXPECT_FALSE(arabic::holdsArabic("", 0));
  // A digit-only token passes: its scan finds no run.
  EXPECT_TRUE(holds(0x0661));
  // Anywhere in the text, and no byte past `len` is read.
  const std::string mixed = "ab " + encode(0x0628) + " cd";
  EXPECT_TRUE(arabic::holdsArabic(mixed.data(), mixed.size()));
  EXPECT_FALSE(arabic::holdsArabic(mixed.data(), 3));
  EXPECT_FALSE(arabic::holdsArabic("hello", 5));
}

TEST(ArabicScripts, ClusterBreaks) {
  const uint32_t beh = 0x0628, fatha = 0x064E, zwj = 0x200D, zwnj = 0x200C;
  EXPECT_TRUE(arabic::isClusterBreak(beh, beh));
  EXPECT_FALSE(arabic::isClusterBreak(beh, fatha));
  EXPECT_FALSE(arabic::isClusterBreak(beh, zwj));
  EXPECT_FALSE(arabic::isClusterBreak(beh, zwnj));
  EXPECT_FALSE(arabic::isClusterBreak(zwj, beh));
  EXPECT_TRUE(arabic::isClusterBreak(zwnj, beh));
  EXPECT_TRUE(arabic::isClusterBreak(fatha, beh));
}

TEST(ArabicScripts, U088FIsALetter) {
  EXPECT_TRUE(arabic::isRunMember(0x088F));
  EXPECT_FALSE(arabic::isMark(0x088F));
  EXPECT_TRUE(arabic::isLetterOrMark(0x088F));
  const std::string s = encode(0x088F);
  EXPECT_TRUE(arabic::endsWithLetter(s.data(), s.size()));
}

TEST(ArabicScripts, LettersAndMarks) {
  EXPECT_TRUE(arabic::isMark(0x064E));
  EXPECT_TRUE(arabic::isMark(0x0670));
  EXPECT_TRUE(arabic::isMark(0x08CA));
  EXPECT_TRUE(arabic::isMark(0x0897));
  EXPECT_FALSE(arabic::isMark(0x0628));
  EXPECT_FALSE(arabic::isMark(0x0640));
  EXPECT_TRUE(arabic::isLetterOrMark(0x0628));
  EXPECT_TRUE(arabic::isLetterOrMark(0x064E));
  EXPECT_FALSE(arabic::isLetterOrMark(0x0661));
  EXPECT_FALSE(arabic::isLetterOrMark(0x200D));
  EXPECT_EQ(arabic::kProbe, 0x0628u);
}

TEST(ArabicScripts, EndsWithLetter) {
  auto ends = [](const std::string& s) { return arabic::endsWithLetter(s.data(), s.size()); };
  EXPECT_TRUE(ends(encode(0x0628)));
  EXPECT_TRUE(ends(encode(0x0640)));                    // tatweel
  EXPECT_TRUE(ends(encode(0x0628) + encode(0x064E)));   // a mark is skipped
  EXPECT_FALSE(ends(encode(0x064E)));                   // only a mark
  EXPECT_FALSE(ends(encode(0x0628) + encode(0x200C)));  // ZWNJ is not a letter
  EXPECT_FALSE(ends(encode(0x0628) + encode(0x0661)));  // a digit
  EXPECT_FALSE(ends(encode(0x0628) + "."));
  EXPECT_FALSE(ends("abc"));
  EXPECT_FALSE(ends(""));
  EXPECT_TRUE(ends("ab" + encode(0x0628)));
}

TEST(ArabicScripts, PunctuationMembersAreExactlyTheTwelve) {
  const uint32_t expected[] = {0x0608, 0x060B, 0x060D, 0x061B, 0x061D, 0x061E,
                               0x061F, 0x066D, 0x06D4, 0x06FD, 0x06FE, 0x0888};
  for (uint32_t cp = 0; cp < 0x1000; cp++) {
    bool listed = false;
    for (const uint32_t e : expected) listed = listed || e == cp;
    EXPECT_EQ(arabic::isPunctuationMember(cp), listed) << std::hex << cp;
    if (listed) EXPECT_TRUE(arabic::isRunMember(cp)) << std::hex << cp;
  }
}

TEST(ArabicScripts, TakesJoinerExcludesPunctuationIncludingU0888) {
  EXPECT_FALSE(arabic::takesJoiner(0x0888));
  EXPECT_FALSE(arabic::takesJoiner(0x061F));
  EXPECT_FALSE(arabic::takesJoiner(0x06D4));
  EXPECT_TRUE(arabic::takesJoiner(0x0628));
  EXPECT_TRUE(arabic::takesJoiner(0x064E));
  EXPECT_TRUE(arabic::takesJoiner(0x0640));
  EXPECT_FALSE(arabic::takesJoiner(0x0661));
}

TEST(ArabicScripts, EndsWithLetterIsFalseForPunctuation) {
  auto ends = [](const std::string& s) { return arabic::endsWithLetter(s.data(), s.size()); };
  EXPECT_FALSE(ends(encode(0x0628) + encode(0x061F)));
  EXPECT_FALSE(ends(encode(0x0628) + encode(0x06D4)));
  EXPECT_FALSE(ends(encode(0x0628) + encode(0x0888)));
  EXPECT_FALSE(ends(encode(0x0628) + encode(0x061F) + encode(0x064E)));  // a mark is skipped, the punctuation is not
  EXPECT_TRUE(ends(encode(0x061F) + encode(0x0628)));
}

// The number signs are Unicode's Prepended_Concatenation_Mark of the three blocks (PropList.txt: U+0600-0605, 06DD,
// 0890-0891, 08E2; U+070F is Syriac and U+110BD, 110CD Kaithi), none of them a run member: a sign is a number, and its
// run is a number run (IndicShaper's ArabicNumber).
TEST(ArabicScripts, NumberSignsAreThePrependedConcatenationMarksOfTheBlocks) {
  const std::vector<uint32_t> expected = {0x0600, 0x0601, 0x0602, 0x0603, 0x0604,
                                          0x0605, 0x06DD, 0x0890, 0x0891, 0x08E2};
  std::vector<uint32_t> found;
  for (uint32_t cp = 0x0600; cp <= 0x08FF; ++cp) {
    if (arabic::isNumberSign(cp)) found.push_back(cp);
  }
  EXPECT_EQ(expected, found);
  for (const uint32_t cp : expected) EXPECT_FALSE(arabic::isRunMember(cp)) << std::hex << cp;
}

TEST(ArabicScripts, NumberSignDigitsAreTheThreeDigitSets) {
  size_t digits = 0;
  for (uint32_t cp = 0; cp <= 0x08FF; ++cp) {
    if (!arabic::isNumberSignDigit(cp)) continue;
    ++digits;
    EXPECT_TRUE((cp >= '0' && cp <= '9') || (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9))
        << std::hex << cp;
  }
  EXPECT_EQ(30u, digits);
}

TEST(ArabicScripts, ADigitStaysWithTheSignBeforeIt) {
  EXPECT_FALSE(arabic::isClusterBreak(0x06DD, 0x0661));
  EXPECT_FALSE(arabic::isClusterBreak(0x0601, '1'));
  EXPECT_TRUE(arabic::isClusterBreak(0x06DD, 0x0628));  // a letter after it is a cluster of its own
  EXPECT_TRUE(arabic::isClusterBreak(0x0628, 0x06DD));
  EXPECT_TRUE(arabic::isClusterBreak(0x0661, 0x0662));
}

TEST(ArabicScripts, HoldsNumberSignWithDigit) {
  const auto holds = [](const std::string& s) { return arabic::holdsNumberSignWithDigit(s.data(), s.size()); };
  EXPECT_TRUE(holds("\xDB\x9D\xD9\xA1"));  // U+06DD U+0661
  EXPECT_TRUE(
      holds("\xD8\xA8 \xD8\x80"
            "12"));                          // beh, space, U+0600, 1, 2
  EXPECT_FALSE(holds("\xDB\x9D"));           // the sign alone
  EXPECT_FALSE(holds("\xDB\x9D \xD9\xA1"));  // a space between
  EXPECT_FALSE(holds("\xD9\xA1\xDB\x9D"));   // the digit first
  EXPECT_FALSE(holds("12"));
  EXPECT_FALSE(arabic::holdsRunMember("\xDB\x9D\xD9\xA1", 4));  // no run member: the paragraph gate asks both
}
