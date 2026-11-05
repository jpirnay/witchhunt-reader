#include <gtest/gtest.h>

#include "PasswordMask.h"

namespace {

constexpr auto NONE = std::string::npos;

TEST(PasswordMask, HidesEveryByteWithNothingToReveal) {
  EXPECT_EQ(maskPassword("secret", NONE), "******");
  EXPECT_EQ(maskPassword("secret", 0), "******");
  EXPECT_EQ(maskPassword("", NONE), "");
}

TEST(PasswordMask, RevealsTheCharacterJustTyped) {
  EXPECT_EQ(maskPassword("abc", 3), "**c");
  EXPECT_EQ(maskPassword("abc", 2), "*b*");
}

// A lone byte of a UTF-8 character is not text: drawn, it is a garbled glyph.
TEST(PasswordMask, RevealsAWholeTwoByteCharacter) { EXPECT_EQ(maskPassword("a\xC3\xBA", 3), "*\xC3\xBA"); }

TEST(PasswordMask, RevealsAWholeThreeByteCharacter) { EXPECT_EQ(maskPassword("a\xE2\x82\xAC", 4), "*\xE2\x82\xAC"); }

TEST(PasswordMask, RevealsAWholeFourByteCharacter) {
  EXPECT_EQ(maskPassword("\xF0\x9F\x98\x80z", 4), "\xF0\x9F\x98\x80*");
}

TEST(PasswordMask, KeepsTheTextsLengthSoIndexesStayValid) {
  const std::string text = "a\xC3\xBA\xE2\x82\xACz";
  for (size_t end = 0; end <= text.size(); end++) EXPECT_EQ(maskPassword(text, end).size(), text.size()) << end;
  EXPECT_EQ(maskPassword(text, NONE).size(), text.size());
}

}  // namespace
