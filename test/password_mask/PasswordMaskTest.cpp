#include <gtest/gtest.h>

#include "PasswordMask.h"

namespace {

constexpr auto NONE = std::string::npos;

TEST(PasswordMask, HidesEveryCharacterWithNothingToReveal) {
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

// A letter of another script is one star, as a Latin one is, not one per byte of its UTF-8.
TEST(PasswordMask, OneStarPerCharacterInEveryScript) {
  EXPECT_EQ(maskPassword("שלום", NONE), "****");  // Hebrew, two bytes a letter
  EXPECT_EQ(maskPassword("καλή", NONE), "****");  // Greek
  EXPECT_EQ(maskPassword("мир", NONE), "***");    // Cyrillic
  EXPECT_EQ(maskPassword("a€😀", NONE), "***");   // three and four bytes
  EXPECT_EQ(maskPassword("שלום", 8), "***ם");     // the last letter typed, whole
  EXPECT_EQ(maskPassword("καλή", 4), "*α**");
  EXPECT_EQ(maskPassword("мир1", 6), "**р*");
}

// A position in the text is carried to the mask and back, whichever character is shown.
TEST(PasswordMask, OffsetsCarryAPositionBetweenTextAndMask) {
  const std::string text = "aש€😀z";  // characters at bytes 0, 1, 3, 6, 10; 11 bytes
  const size_t starts[] = {0, 1, 3, 6, 10, 11};
  for (const size_t reveal : {NONE, size_t{1}, size_t{3}, size_t{6}, size_t{10}, size_t{11}}) {
    const std::string masked = maskPassword(text, reveal);
    for (const size_t pos : starts) {
      const size_t m = maskedOffset(text, reveal, pos);
      EXPECT_LE(m, masked.size()) << reveal << " " << pos;
      EXPECT_EQ(unmaskedOffset(text, reveal, m), pos) << reveal << " " << pos;
    }
    EXPECT_EQ(maskedOffset(text, reveal, text.size()), masked.size()) << reveal;
  }
  // Nothing shown: one byte a character.
  EXPECT_EQ(maskedOffset(text, NONE, 6), 3u);
  EXPECT_EQ(unmaskedOffset(text, NONE, 3), 6u);
  // ש shown: it keeps its two bytes, and a position inside it goes on to the next character.
  EXPECT_EQ(maskPassword(text, 3), "*ש***");
  EXPECT_EQ(maskedOffset(text, 3, 3), 3u);
  EXPECT_EQ(unmaskedOffset(text, 3, 2), 3u);
}

}  // namespace
