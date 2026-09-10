// utf8NfcNorm composition.
//
// The function started as a Vietnamese-only helper: a base letter followed by combining
// marks, folded into the precomposed U+1EA0-U+1EF9 range. Conjoining Hangul jamo need the
// same treatment for the same reason -- the fonts carry precomposed syllables and nothing
// else -- but they are not combining marks, so neither the fast-path check nor the absorb
// loop saw them and Korean stored in NFD came through as jamo the fonts cannot draw.
// macOS writes every filename that way, and some EPUBs carry their text that way.
//
// Hangul composition is arithmetic (Unicode 3.12), not a table, so the cases below are
// chosen to pin the arithmetic rather than to sample a list: both syllable shapes, the
// boundaries of each jamo block, and the sequences that must NOT compose.
// crosspoint-reader PR #3036.

#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

namespace {

// Builds a UTF-8 string from codepoints so the test reads as Unicode, not as bytes.
std::string cps(std::initializer_list<uint32_t> list) {
  std::string out;
  for (const uint32_t cp : list) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

}  // namespace

// L + V -> LV. 한 = U+1112 U+1161 U+11AB, so its LV stage is U+D55C's base.
TEST(Utf8Compose, ComposesLeadingPlusVowel) {
  // U+1100 (g) + U+1161 (a) -> U+AC00 (가), the first syllable of the block.
  EXPECT_EQ(utf8NfcNorm(cps({0x1100, 0x1161})), cps({0xAC00}));
}

// L + V + T -> LVT, applied in two steps by the same greedy loop.
TEST(Utf8Compose, ComposesFullSyllable) {
  // U+1112 U+1161 U+11AB -> U+D55C (한)
  EXPECT_EQ(utf8NfcNorm(cps({0x1112, 0x1161, 0x11AB})), cps({0xD55C}));
  // 한국 as macOS stores it: two full syllables back to back.
  EXPECT_EQ(utf8NfcNorm(cps({0x1112, 0x1161, 0x11AB, 0x1100, 0x116E, 0x11A8})), cps({0xD55C, 0xAD6D}));
}

// Block boundaries: the first and last of each jamo range must compose.
TEST(Utf8Compose, ComposesAtBlockBoundaries) {
  EXPECT_EQ(utf8NfcNorm(cps({0x1100, 0x1161})), cps({0xAC00}));  // first L, first V
  EXPECT_EQ(utf8NfcNorm(cps({0x1112, 0x1175})), cps({0xD7A3 - 27}));  // last L, last V, no T
  // Last L + last V + last T is the final syllable of the block, U+D7A3.
  EXPECT_EQ(utf8NfcNorm(cps({0x1112, 0x1175, 0x11C2})), cps({0xD7A3}));
}

// An already-composed syllable takes no second trailing consonant, and a bare trailer with
// nothing to attach to is left alone rather than being folded into the previous syllable.
TEST(Utf8Compose, DoesNotOvercompose) {
  // LVT + another T: the second trailer stays separate.
  EXPECT_EQ(utf8NfcNorm(cps({0x1112, 0x1161, 0x11AB, 0x11AB})), cps({0xD55C, 0x11AB}));
  // A vowel with no leading consonant is not a syllable.
  EXPECT_EQ(utf8NfcNorm(cps({0x1161, 0x11AB})), cps({0x1161, 0x11AB}));
}

// Already-NFC Korean is returned untouched -- the common case, and the one the fast path
// is there to make free.
TEST(Utf8Compose, PrecomposedHangulIsUnchanged) {
  const std::string nfc = cps({0xD55C, 0xAD6D, 0xC5B4});  // 한국어
  EXPECT_EQ(utf8NfcNorm(nfc), nfc);
}

// The Vietnamese behaviour this function was written for still holds.
TEST(Utf8Compose, VietnameseCompositionStillWorks) {
  // U+0065 U+0323 U+0302 -> U+1EC7 (ệ)
  EXPECT_EQ(utf8NfcNorm(cps({0x0065, 0x0323, 0x0302})), cps({0x1EC7}));
  // U+0041 U+0300 -> U+00C0 (À)
  EXPECT_EQ(utf8NfcNorm(cps({0x0041, 0x0300})), cps({0x00C0}));
}

// ASCII and mixed text must not be disturbed, including the case where a Korean run sits
// between Latin words -- the absorb loop has to stop at the right place on both sides.
TEST(Utf8Compose, LeavesSurroundingTextAlone) {
  EXPECT_EQ(utf8NfcNorm("plain ascii.epub"), "plain ascii.epub");
  EXPECT_EQ(utf8NfcNorm(""), "");

  const std::string mixed = "Book " + cps({0x1112, 0x1161, 0x11AB}) + " v2.epub";
  EXPECT_EQ(utf8NfcNorm(mixed), "Book " + cps({0xD55C}) + " v2.epub");
}

// Hangul fillers (U+115F/U+1160) are default-ignorable, not composable jamo. They must not
// be mistaken for a leading consonant or vowel.
TEST(Utf8Compose, FillersAreNotJamo) {
  EXPECT_FALSE(utf8IsConjoiningJamo(0x115F));
  EXPECT_FALSE(utf8IsConjoiningJamo(0x1160));
  EXPECT_TRUE(utf8IsConjoiningJamo(0x1100));
  EXPECT_TRUE(utf8IsConjoiningJamo(0x1161));
  EXPECT_TRUE(utf8IsConjoiningJamo(0x11A8));
  // U+11A7 is the T base, one below the first real trailing consonant.
  EXPECT_FALSE(utf8IsConjoiningJamo(0x11A7));
}
