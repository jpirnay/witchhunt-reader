// BidiClassTest — the second class table (bidiclasses_ext.t) that bidi_class()
// searches when upstream's table has no entry: the scripts the firmware draws.

#include <gtest/gtest.h>

#include "BidiUtils.h"

extern "C" {
#include "minibidi.h"
}

TEST(BidiClassTest, LeftToRightScriptsReadAsL) {
  EXPECT_EQ(bidi_class(0x03B1), L);   // Greek alpha
  EXPECT_EQ(bidi_class(0x0915), L);   // Devanagari ka
  EXPECT_EQ(bidi_class(0x094D), L);   // Devanagari virama, NSM in the UCD
  EXPECT_EQ(bidi_class(0x3042), L);   // hiragana a
  EXPECT_EQ(bidi_class(0x3099), L);   // combining dakuten, NSM in the UCD
  EXPECT_EQ(bidi_class(0x4E00), L);   // CJK ideograph
  EXPECT_EQ(bidi_class(0xAC00), L);   // Hangul syllable
  EXPECT_EQ(bidi_class(0x10D0), L);   // Georgian an
  EXPECT_EQ(bidi_class(0x0561), L);   // Armenian ayb
  EXPECT_EQ(bidi_class(0x1200), L);   // Ethiopic ha
  EXPECT_EQ(bidi_class(0xFF21), L);   // fullwidth A
  EXPECT_EQ(bidi_class(0x20000), L);  // CJK extension B
  EXPECT_EQ(bidi_class(0x1000), L);   // Myanmar ka
  EXPECT_EQ(bidi_class(0x1031), L);   // Myanmar vowel sign e, a spacing mark
  EXPECT_EQ(bidi_class(0x1039), L);   // Myanmar virama (stacker), NSM in the UCD
  EXPECT_EQ(bidi_class(0x103A), L);   // Myanmar asat, NSM in the UCD
  EXPECT_EQ(bidi_class(0x1041), L);   // Myanmar digit one
  EXPECT_EQ(bidi_class(0xAA60), L);   // Myanmar Extended-A khamti ga
  EXPECT_EQ(bidi_class(0xA9E0), L);   // Myanmar Extended-B shan gha
  EXPECT_EQ(bidi_class(0x104A), L);   // Myanmar little section (U+104A, L in the UCD)
}

TEST(BidiClassTest, PunctuationSpacesAndNumbers) {
  EXPECT_EQ(bidi_class(0x037E), ON);  // Greek question mark
  EXPECT_EQ(bidi_class(0x3001), ON);  // ideographic comma
  EXPECT_EQ(bidi_class(0x3000), WS);  // ideographic space
  EXPECT_EQ(bidi_class(0xFF10), EN);  // fullwidth zero
  EXPECT_EQ(bidi_class(0x20AC), ET);  // euro sign
  EXPECT_EQ(bidi_class(0x2212), ES);  // minus sign
}

TEST(BidiClassTest, HebrewPresentationForms) {
  EXPECT_EQ(bidi_class(0xFB2A), R);    // shin with shin dot
  EXPECT_EQ(bidi_class(0xFB1E), NSM);  // point judeo-spanish varika
  EXPECT_EQ(bidi_class(0xFB29), ES);   // alternative plus sign
}

TEST(BidiClassTest, UpstreamEntriesUnchanged) {
  EXPECT_EQ(bidi_class(0x05D0), R);
  EXPECT_EQ(bidi_class(0x0627), AL);
  EXPECT_EQ(bidi_class(0x0041), L);
  EXPECT_EQ(bidi_class(0x0300), NSM);
  EXPECT_EQ(bidi_class(0x200F), R);
}

TEST(BidiClassTest, IsTransparentMark) {
  EXPECT_FALSE(BidiUtils::isTransparentMark(0x094D));
  EXPECT_FALSE(BidiUtils::isTransparentMark(0x3099));
  EXPECT_TRUE(BidiUtils::isTransparentMark(0xFB1E));
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x05B0));
}

TEST(BidiClassTest, HebrewPointsUpstreamLacks) {
  EXPECT_EQ(bidi_class(0x05C7), NSM);
  EXPECT_EQ(bidi_class(0x05BA), NSM);
  EXPECT_EQ(bidi_class(0x05A2), NSM);
  EXPECT_EQ(bidi_class(0x05C5), NSM);
  EXPECT_EQ(bidi_class(0x05C6), R);
  EXPECT_EQ(bidi_class(0x05EF), R);
  EXPECT_TRUE(BidiUtils::isTransparentMark(0x05C7));
}

TEST(BidiClassTest, LigaturesScriptsAndLetterlikeSymbols) {
  EXPECT_EQ(bidi_class(0xFB01), L);
  EXPECT_EQ(bidi_class(0x2074), EN);
  EXPECT_EQ(bidi_class(0x2160), L);
}
