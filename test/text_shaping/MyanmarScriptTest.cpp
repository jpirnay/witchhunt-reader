// lib/Utf8/MyanmarScript.h: the run members, the letters and digits against the shaper's own Myanmar categories, the
// byte scan, and the rule that cuts a Myanmar word between syllables.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "MyanmarScript.h"
#include "OtShaperInternal.h"

namespace {

std::string utf8(const uint32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

constexpr uint32_t kBlocks[][2] = {{0x1000, 0x109F}, {0xA9E0, 0xA9FF}, {0xAA60, 0xAA7F}};

}  // namespace

TEST(MyanmarScript, TheRunMembersAreTheThreeBlocks) {
  for (const auto& block : kBlocks) {
    EXPECT_FALSE(myanmar::isRunMember(block[0] - 1));
    for (uint32_t cp = block[0]; cp <= block[1]; ++cp) EXPECT_TRUE(myanmar::isRunMember(cp)) << std::hex << cp;
    EXPECT_FALSE(myanmar::isRunMember(block[1] + 1));
  }
  for (const uint32_t cp : {0x200Cu, 0x200Du, 0x25CCu, 0x0020u, 0x0E01u, 0x0915u, 0x116D0u}) {
    EXPECT_FALSE(myanmar::isRunMember(cp)) << std::hex << cp;
  }
  EXPECT_TRUE(myanmar::isShared(0x200C) && myanmar::isShared(0x200D) && myanmar::isShared(0x25CC));
  EXPECT_FALSE(myanmar::isShared(0x1039));
}

// The consonants and independent vowels are the shaper's: its Myanmar categories C and Ra, and IV, for every code point
// of the blocks. The digits are placeholders (GB) to the shaper, as the section marks are.
TEST(MyanmarScript, TheLettersAreTheShapersConsonantsAndIndependentVowels) {
  int consonants = 0;
  int vowels = 0;
  for (const auto& block : kBlocks) {
    for (uint32_t cp = block[0]; cp <= block[1]; ++cp) {
      const uint8_t category = ot::charData(cp).indicCategory;
      EXPECT_EQ(myanmar::isConsonant(cp), category == ot::icat::C || category == ot::icat::Ra) << std::hex << cp;
      EXPECT_EQ(myanmar::isIndependentVowel(cp), category == ot::mcat::IV) << std::hex << cp;
      if (myanmar::isDigit(cp)) {
        EXPECT_EQ(category, ot::mcat::GB) << std::hex << cp;
      }
      consonants += myanmar::isConsonant(cp);
      vowels += myanmar::isIndependentVowel(cp);
    }
  }
  EXPECT_EQ(consonants, 102);
  EXPECT_EQ(vowels, 14);
  for (uint32_t cp = 0x1040; cp <= 0x1049; ++cp) EXPECT_TRUE(myanmar::isDigit(cp) && myanmar::startsSyllable(cp));
  EXPECT_FALSE(myanmar::startsSyllable(0x104A));  // little section
  EXPECT_FALSE(myanmar::startsSyllable(0x104B));  // section
  for (const uint32_t cp : {0x104Cu, 0x104Du, 0x104Eu, 0x104Fu}) EXPECT_TRUE(myanmar::startsSyllable(cp));
}

TEST(MyanmarScript, AWordIsCutBeforeASyllableStartOnly) {
  using myanmar::breakAllowed;
  constexpr uint32_t ka = 0x1000, kha = 0x1001, nga = 0x1004, ma = 0x1019, na = 0x1014, a = 0x1021;
  EXPECT_TRUE(breakAllowed(ka, kha, 0));       // two syllables
  EXPECT_TRUE(breakAllowed(0x102C, ma, 0));    // after a vowel sign
  EXPECT_TRUE(breakAllowed(0x1038, a, 0));     // after visarga, before an independent vowel
  EXPECT_FALSE(breakAllowed(ka, 0x102D, 0));   // before a vowel sign
  EXPECT_FALSE(breakAllowed(ka, 0x103B, 0));   // before a medial
  EXPECT_FALSE(breakAllowed(ka, 0x1031, 0));   // before e, though it is drawn before its consonant
  EXPECT_FALSE(breakAllowed(ka, 0x103A, 0));   // before asat
  EXPECT_FALSE(breakAllowed(0x1039, kha, 0));  // a stacked consonant stays under the one before it
  EXPECT_FALSE(breakAllowed(0x200D, kha, 0));  // nor after a ZWJ
  EXPECT_TRUE(breakAllowed(0x200C, kha, 0));   // a ZWNJ is no glue
  // A killed final consonant stays with its syllable: မြန် is one, and so is ကြောင့် with the dot below before asat
  // (the caller passes the code point after the dot below).
  EXPECT_FALSE(breakAllowed(0x103C, na, 0x103A));
  EXPECT_FALSE(breakAllowed(0x102C, nga, 0x103A));  // ကြောင့်: nga, the dot below, then asat
  // Kinzi (nga, asat, stacker) stays with the syllable before it, and the consonant after it with kinzi: သင်္ဘော.
  EXPECT_FALSE(breakAllowed(0x101E, nga, 0x103A));
  EXPECT_FALSE(breakAllowed(0x1039, 0x1018, 0x1031));
  EXPECT_FALSE(breakAllowed(ka, kha, 0x1039));  // the first consonant of a stack
  // Digits: a number is never cut, a digit after a letter starts a syllable.
  EXPECT_FALSE(breakAllowed(0x1041, 0x1042, 0));
  EXPECT_TRUE(breakAllowed(0x1038, 0x1041, 0));
  EXPECT_FALSE(breakAllowed(0x1091, 0x1092, 0));  // Shan digits
  EXPECT_FALSE(breakAllowed(ka, 0x104B, 0));      // the section mark ends its phrase
}

TEST(MyanmarScript, ACutElsewhereIsMyanmarsBusinessOnlyBesideItsLetters) {
  using myanmar::cutAllowed;
  EXPECT_TRUE(cutAllowed('a', 'b', 0));
  EXPECT_TRUE(cutAllowed(0x1000, 'b', 0));      // a Latin letter after a Myanmar one
  EXPECT_TRUE(cutAllowed('a', 0x1000, 0));      // a Myanmar syllable after a Latin letter
  EXPECT_FALSE(cutAllowed('a', 0x102D, 0));     // a sign never starts a line
  EXPECT_FALSE(cutAllowed(0x1000, 0x200D, 0));  // the joiners belong to the letter before them
  EXPECT_FALSE(cutAllowed(0x1000, 0x200C, 0));
  EXPECT_TRUE(cutAllowed('a', 0x200D, 0));  // an emoji or Latin joiner is not Myanmar's
}

TEST(MyanmarScript, TheByteScanFindsAMemberOfEachBlockWithinItsBounds) {
  for (const auto& block : kBlocks) {
    for (const uint32_t cp : {block[0], block[1]}) {
      const std::string text = "ab" + utf8(cp);
      EXPECT_TRUE(myanmar::holdsMyanmar(text.data(), text.size())) << std::hex << cp;
      EXPECT_FALSE(myanmar::holdsMyanmar(text.data(), text.size() - 1)) << std::hex << cp;  // cut short
    }
    for (const uint32_t cp : {block[0] - 1, block[1] + 1}) {
      const std::string text = utf8(cp);
      EXPECT_FALSE(myanmar::holdsMyanmar(text.data(), text.size())) << std::hex << cp;
    }
  }
  const std::string thai = utf8(0x0E01) + utf8(0x10A0) + utf8(0x1E00);  // Thai, Georgian, Latin Extended Additional
  EXPECT_FALSE(myanmar::holdsMyanmar(thai.data(), thai.size()));
  EXPECT_FALSE(myanmar::holdsMyanmar(nullptr, 0));
}
