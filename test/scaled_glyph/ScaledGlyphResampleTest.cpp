// Shrinking a glyph (emitScaledGlyphPixels at a scale below 1): text the book sets smaller than the body, small
// caps, superscripts. Nearest-neighbour point sampling skipped whole source rows and columns (a 0.75 shrink drops
// every fourth), so a one-pixel stroke on a skipped row vanished and stems came out uneven, and it emitted no
// anti-aliasing levels at all. The glyph is integrated over the area each destination pixel covers instead, as
// enlarging already does, and re-quantised into the font's four ink levels.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "GlyphKey.h"
#include "ScaledGlyphResample.h"

namespace {

// A 2-bit glyph from rows of '0'..'3' (ink levels), packed as the fonts store them: four pixels a byte, the first in
// the high bits, rows back to back.
struct Glyph {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> bits;
};

Glyph glyphOf(const std::vector<std::string>& rows) {
  Glyph g;
  g.h = static_cast<int>(rows.size());
  g.w = static_cast<int>(rows[0].size());
  g.bits.assign((g.w * g.h + 3) / 4, 0);
  for (int y = 0; y < g.h; ++y) {
    for (int x = 0; x < g.w; ++x) {
      const int pos = y * g.w + x;
      g.bits[pos >> 2] |= static_cast<uint8_t>((rows[y][x] - '0') << ((3 - (pos & 3)) * 2));
    }
  }
  return g;
}

using Pixels = std::set<std::pair<int, int>>;

// The destination pixels whose ink level is in `drawMask` (bit n: level n), shrunk to `scale`.
Pixels shrink(const Glyph& g, const float scale, const uint8_t drawMask) {
  const int dstW = static_cast<int>(g.w * scale + 0.5f);
  const int dstH = static_cast<int>(g.h * scale + 0.5f);
  Pixels out;
  emitScaledGlyphPixels(g.bits.data(), true, g.w, g.h, dstW, dstH, scale, drawMask,
                        [&out](const int x, const int y) { out.insert({x, y}); });
  return out;
}

constexpr uint8_t kAnyInk = 0x0E;   // levels 1-3: what the black-and-white pass draws
constexpr uint8_t kGreys = 0x06;    // levels 1-2
constexpr uint8_t kFullInk = 0x08;  // level 3

}  // namespace

TEST(ScaledGlyphShrink, AOnePixelStrokeSurvives) {
  // A crossbar on source row 4 of 12. At 0.75 point sampling reads rows 0, 1, 2, 3, 5, 6, 7, 9, 10: never 4 or 8.
  std::vector<std::string> rows(12, "00000000");
  rows[4] = "33333333";
  const Pixels drawn = shrink(glyphOf(rows), 0.75f, kAnyInk);
  EXPECT_FALSE(drawn.empty()) << "the stroke vanished";
}

TEST(ScaledGlyphShrink, AnEdgeAcrossAPixelIsGrey) {
  // A stem three pixels wide, shrunk to 0.75: destination columns 0 and 1 lie inside it, column 2 covers a quarter
  // of stem (source x 2.67 to 4), so it is a grey edge, not black and not missing.
  const Glyph stem = glyphOf(std::vector<std::string>(8, "33300000"));
  const Pixels black = shrink(stem, 0.75f, kFullInk);
  const Pixels grey = shrink(stem, 0.75f, kGreys);
  ASSERT_FALSE(black.empty());
  ASSERT_FALSE(grey.empty()) << "no anti-aliasing levels";
  for (const auto& [x, y] : black) EXPECT_LT(x, 2) << "black at column " << x;
  for (const auto& [x, y] : grey) EXPECT_EQ(x, 2) << "grey at column " << x;
}

// A shaped run's glyph keys share the mask cache with code points. Its key keeps them apart: glyph 0x41 and 'A' have
// the same 16 bits, and one's mask must never be served for the other.
TEST(ScaledGlyphResample, AGlyphKeyHasACacheKeyOfItsOwn) {
  constexpr ScaledGlyphKey glyph = scaledGlyphKeyOf(glyph_key::of(0x41));
  EXPECT_EQ(0x41, glyph.id);
  EXPECT_EQ(1, glyph.kind);
  EXPECT_TRUE(glyph.cacheable);
  constexpr ScaledGlyphKey letter = scaledGlyphKeyOf('A');
  EXPECT_EQ(0x41, letter.id);
  EXPECT_EQ(0, letter.kind);
  EXPECT_TRUE(letter.cacheable);

  EXPECT_EQ(0xFFFF, scaledGlyphKeyOf(glyph_key::kLast).id);  // the last glyph key, and the last BMP code point
  EXPECT_EQ(1, scaledGlyphKeyOf(glyph_key::kLast).kind);
  EXPECT_EQ(0xFFFF, scaledGlyphKeyOf(0xFFFF).id);
  EXPECT_EQ(0, scaledGlyphKeyOf(0xFFFF).kind);

  EXPECT_FALSE(scaledGlyphKeyOf(0x1F600).cacheable);  // above the BMP
  EXPECT_FALSE(scaledGlyphKeyOf(0x10FFFF).cacheable);
  EXPECT_FALSE(scaledGlyphKeyOf(glyph_key::kLast + 1).cacheable);
}

// The cache's own match (GfxRenderer's find): an entry stored for 'A' never serves glyph 0x41 of the same font at the
// same scale, size and drawMask, nor one stored for glyph 0x41 'A'.
TEST(ScaledGlyphResample, AGlyphKeyAndACodePointNeverShareAnEntry) {
  const int fontData = 0;                  // stands for one style's EpdFontData
  constexpr uint32_t kScale = 0x3FC00000;  // 1.5f
  const auto storedFor = [&](const ScaledGlyphKey& key) {
    ScaledGlyphEntry e{};
    e.fontData = &fontData;
    e.scaleBits = kScale;
    e.id = key.id;
    e.kind = key.kind;
    e.sel = 3;
    e.w = 10;
    e.h = 12;
    return e;
  };
  const ScaledGlyphKey letter = scaledGlyphKeyOf('A');
  const ScaledGlyphKey glyph = scaledGlyphKeyOf(glyph_key::of(0x41));
  ASSERT_EQ(letter.id, glyph.id);
  EXPECT_TRUE(scaledGlyphEntryMatches(storedFor(letter), &fontData, letter, kScale, 3, 10, 12));
  EXPECT_TRUE(scaledGlyphEntryMatches(storedFor(glyph), &fontData, glyph, kScale, 3, 10, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(storedFor(letter), &fontData, glyph, kScale, 3, 10, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(storedFor(glyph), &fontData, letter, kScale, 3, 10, 12));
  // Every other part of the key tells entries apart too.
  const int otherFont = 0;
  const ScaledGlyphEntry e = storedFor(letter);
  EXPECT_FALSE(scaledGlyphEntryMatches(e, &otherFont, letter, kScale, 3, 10, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(e, &fontData, scaledGlyphKeyOf('B'), kScale, 3, 10, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(e, &fontData, letter, kScale + 1, 3, 10, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(e, &fontData, letter, kScale, 1, 10, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(e, &fontData, letter, kScale, 3, 11, 12));
  EXPECT_FALSE(scaledGlyphEntryMatches(e, &fontData, letter, kScale, 3, 10, 13));
}
