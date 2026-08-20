// Default_Ignorable codepoints must never produce ink.
//
// The bug this guards against was not in firmware logic — it was baked into the generated font
// tables. fontconvert.py accepted any glyph the font's cmap offered, and Bookerly maps
// U+200C..U+200F to real outlines with a ZERO advance. The result was a glyph that paints and
// does not move the pen, so the next character overprints it. 41 of 46 generated builtin font
// headers carried at least one. It only became visible on a book watermarked with zero-width
// steganography, where a single chapter held 2233 of them.
//
// Two independent things are checked, because the fix needed both:
//
//   1. The PREDICATE (utf8IsDefaultIgnorable) — the text layer's definition of "carries no
//      text", including the deliberate exclusion of U+00AD SOFT HYPHEN.
//   2. The SHIPPED FONT DATA — every default-ignorable glyph in every builtin font is empty.
//      This is the real regression guard: it reads the committed tables, so a future font
//      regeneration with an unpatched converter fails here rather than on a user's screen.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "EpdFontData.h"
#include "Utf8.h"

// Reading fonts (the ones that render book text, where the bug was visible) plus one UI font.
#include "bookerly_14_regular.h"
#include "bookerly_18_bolditalic.h"
#include "inter_ui_12_regular.h"
#include "notosans_14_regular.h"

namespace {

struct NamedFont {
  const char* name;
  const EpdUnicodeInterval* intervals;
  uint32_t intervalCount;
  const EpdGlyph* glyphs;
};

std::vector<NamedFont> shippedFonts() {
  return {
      {"bookerly_14_regular", bookerly_14_regularIntervals,
       sizeof(bookerly_14_regularIntervals) / sizeof(bookerly_14_regularIntervals[0]), bookerly_14_regularGlyphs},
      {"bookerly_18_bolditalic", bookerly_18_bolditalicIntervals,
       sizeof(bookerly_18_bolditalicIntervals) / sizeof(bookerly_18_bolditalicIntervals[0]),
       bookerly_18_bolditalicGlyphs},
      {"notosans_14_regular", notosans_14_regularIntervals,
       sizeof(notosans_14_regularIntervals) / sizeof(notosans_14_regularIntervals[0]), notosans_14_regularGlyphs},
      {"inter_ui_12_regular", inter_ui_12_regularIntervals,
       sizeof(inter_ui_12_regularIntervals) / sizeof(inter_ui_12_regularIntervals[0]), inter_ui_12_regularGlyphs},
  };
}

}  // namespace

TEST(DefaultIgnorable, ClassifiesTheZeroWidthWatermarkCodepoints) {
  // The three that watermarking tools actually emit, in the quantities that made this matter.
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x200B));  // ZERO WIDTH SPACE
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x200C));  // ZERO WIDTH NON-JOINER
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x200D));  // ZERO WIDTH JOINER
}

TEST(DefaultIgnorable, ClassifiesTheRemainingFormatControls) {
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x034F));  // combining grapheme joiner
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x061C));  // Arabic letter mark
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x200E));  // LRM
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x200F));  // RLM
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x202A));  // bidi embedding, first
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x202E));  // bidi override, last
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x2060));  // word joiner
  EXPECT_TRUE(utf8IsDefaultIgnorable(0x206F));  // deprecated format, last
  EXPECT_TRUE(utf8IsDefaultIgnorable(0xFE0F));  // variation selector 16
  EXPECT_TRUE(utf8IsDefaultIgnorable(0xFEFF));  // BOM / zero-width no-break space
}

TEST(DefaultIgnorable, SoftHyphenIsExcludedBecauseTheLineBreakerDrawsIt) {
  // U+00AD *is* Default_Ignorable in Unicode, but this firmware breaks lines on it and must
  // render a hyphen when it does. Treating it as ignorable would silently drop every
  // publisher-supplied break hint and the hyphen that goes with it.
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x00AD));
}

TEST(DefaultIgnorable, VisibleNeighboursAreNotSweptUp) {
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x0020));  // space
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x0041));  // 'A'
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x00A0));  // NBSP — a visible space
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x2010));  // hyphen, just past U+200F
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x2019));  // right single quote
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x202F));  // narrow no-break space — visible
  EXPECT_FALSE(utf8IsDefaultIgnorable(0x2066));  // isolate initiator, just past U+2065
  EXPECT_FALSE(utf8IsDefaultIgnorable(0xFFFD));  // replacement character — must stay drawable
}

// The regression guard. Reads the committed font tables directly.
TEST(DefaultIgnorable, NoShippedFontDrawsAnIgnorableGlyph) {
  size_t inspected = 0;
  for (const auto& font : shippedFonts()) {
    for (uint32_t i = 0; i < font.intervalCount; ++i) {
      const EpdUnicodeInterval& interval = font.intervals[i];
      for (uint32_t cp = interval.first; cp <= interval.last; ++cp) {
        if (!utf8IsDefaultIgnorable(cp)) continue;
        const EpdGlyph& g = font.glyphs[interval.offset + (cp - interval.first)];
        ++inspected;
        // width/height 0 and no bitmap bytes: nothing to draw. advanceX is 0 for these in the
        // source font too, so this assertion does not constrain layout — only ink.
        EXPECT_EQ(g.width, 0) << font.name << " U+" << std::hex << cp << " has a bitmap";
        EXPECT_EQ(g.height, 0) << font.name << " U+" << std::hex << cp << " has a bitmap";
        EXPECT_EQ(g.dataLength, 0) << font.name << " U+" << std::hex << cp << " has bitmap bytes";
        EXPECT_EQ(g.advanceX, 0) << font.name << " U+" << std::hex << cp << " advances the pen";
      }
    }
  }
  // Guard the guard: if the interval tables ever stop covering these codepoints, the loop above
  // would pass vacuously while getGlyph() fell back to U+FFFD and drew a box instead.
  EXPECT_GT(inspected, 40u) << "font intervals no longer cover the default-ignorable range";
}
