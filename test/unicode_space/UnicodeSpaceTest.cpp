// A Unicode space the font has no glyph for is still a space.
//
// EpdFont::getGlyph() answers a miss with U+FFFD, the box, and that is right for a letter: the reader should see that
// something is missing. A blank has nothing to show, so a book's U+3000 or U+1680 drew a box where it wanted a gap.
// CrossInk 20d74368 (Julia Nguyen). The widths are the Unicode ones.
#include <gtest/gtest.h>

#include "EpdFont.h"
#include "UnicodeSpace.h"
#include "bookerly_14_regular.h"

namespace {

constexpr uint32_t kIdeographicSpace = 0x3000;
constexpr uint32_t kOghamSpace = 0x1680;
constexpr uint32_t kReplacement = 0xFFFD;

const EpdFontData* face() { return &bookerly_14_regular; }

int pixels(const uint16_t advanceFp) { return (advanceFp + 8) >> 4; }

}  // namespace

// What the tests below stand on: the built-in face really has neither of them.
TEST(UnicodeSpace, TheFaceUnderTestLacksTheseSpaces) {
  const EpdFont font(face());
  EXPECT_FALSE(font.findGlyph(kIdeographicSpace));
  EXPECT_FALSE(font.findGlyph(kOghamSpace));
}

TEST(UnicodeSpace, AMissingSpaceDrawsNothing) {
  const EpdFont font(face());
  for (const uint32_t cp : {kIdeographicSpace, kOghamSpace}) {
    const EpdGlyphRef glyph = font.getGlyph(cp);
    ASSERT_TRUE(glyph);
    EXPECT_EQ(glyph.width, 0) << std::hex << cp;
    EXPECT_EQ(glyph.height, 0) << std::hex << cp;
  }
}

TEST(UnicodeSpace, AMissingSpaceIsNotTheBox) {
  const EpdFont font(face());
  EXPECT_NE(font.getGlyph(kIdeographicSpace).index, font.getGlyph(kReplacement).index);
}

TEST(UnicodeSpace, AnIdeographicSpaceIsAnEmWide) {
  const EpdFont font(face());
  const int em = pixels(font.getGlyph('M').advanceX);
  const int width = pixels(font.getGlyph(kIdeographicSpace).advanceX);
  EXPECT_NEAR(width, em, 1);
  EXPECT_GT(width, pixels(font.getGlyph(' ').advanceX) * 2);
}

TEST(UnicodeSpace, AnOghamSpaceIsAWordSpaceWide) {
  const EpdFont font(face());
  EXPECT_EQ(font.getGlyph(kOghamSpace).advanceX, font.getGlyph(' ').advanceX);
}

// A letter the font lacks still shows the box: this is for blanks only.
TEST(UnicodeSpace, AMissingLetterIsStillTheBox) {
  const EpdFont font(face());
  constexpr uint32_t kTibetanLetterKa = 0x0F40;
  ASSERT_FALSE(font.findGlyph(kTibetanLetterKa));
  EXPECT_EQ(font.getGlyph(kTibetanLetterKa).index, font.getGlyph(kReplacement).index);
}

// A font that has the space draws its own, at its own width.
TEST(UnicodeSpace, TheFontsOwnSpaceWins) {
  const EpdFont font(face());
  constexpr uint32_t kThinSpace = 0x2009;
  const EpdGlyphRef own = font.findGlyph(kThinSpace);
  ASSERT_TRUE(own);
  EXPECT_EQ(font.getGlyph(kThinSpace).advanceX, own.advanceX);
}

TEST(UnicodeSpace, TheTableKnowsEverySpaceAndNothingElse) {
  for (const uint32_t cp : {0x1680u, 0x2000u, 0x2001u, 0x2002u, 0x2003u, 0x2004u, 0x2005u, 0x2006u, 0x2007u, 0x2008u,
                            0x2009u, 0x200Au, 0x202Fu, 0x205Fu, 0x3000u}) {
    EXPECT_TRUE(unicode_space::isBlank(cp)) << std::hex << cp;
  }
  for (const uint32_t cp : {0x0020u, 0x00A0u, 0x200Bu, 0x2011u, 0x0041u, 0xFFFDu}) {
    EXPECT_FALSE(unicode_space::isBlank(cp)) << std::hex << cp;
  }
}
