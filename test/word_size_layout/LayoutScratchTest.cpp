// One layout scratch for a build (ParsedText::LayoutScratch): the parser lends it to every block it makes, a pass runs
// in it and marks it in use, and a pass that starts inside another's (a block laid out from another block's line
// callback) takes a scratch of its own instead of overwriting the outer pass's DP tables, widths and line positions.
// Every block lays out exactly as it does with a scratch of its own.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;
constexpr uint16_t kWidth = 120;

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xs;
  bool operator==(const Line& other) const { return words == other.words && xs == other.xs; }
};

Line lineOf(const TextBlock& block) {
  Line line;
  for (uint16_t w = 0; w < block.wordCount(); ++w) {
    line.words.emplace_back(block.wordText(w));
    line.xs.push_back(block.wordXpos(w));
  }
  return line;
}

BlockStyle justified() {
  BlockStyle bs;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  bs.alignment = CssTextAlign::Justify;
  bs.textAlignDefined = true;
  return bs;
}

// A paragraph long enough for several justified lines, so the pass fills every table it has.
void fill(ParsedText& text, const char* stem, const int count) {
  for (int i = 0; i < count; ++i) text.addWord(std::string(stem) + std::to_string(i * 7 % 13), EpdFontFamily::REGULAR);
}

std::vector<Line> layOut(ParsedText& text, const GfxRenderer& renderer) {
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, kWidth,
      [&lines](std::unique_ptr<TextBlock> tb, bool, bool) {
        lines.push_back(lineOf(*tb));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

}  // namespace

TEST(LayoutScratch, ABlockLaysOutTheSameInALentScratchAsInItsOwn) {
  GfxRenderer renderer;
  ParsedText own(false, false, justified());
  fill(own, "own", 40);
  const std::vector<Line> expected = layOut(own, renderer);
  ASSERT_GT(expected.size(), 3u);

  ParsedText::LayoutScratch scratch;
  ParsedText lent(false, false, justified());
  lent.lendLayoutScratch(&scratch);
  fill(lent, "own", 40);
  EXPECT_EQ(layOut(lent, renderer), expected);
  EXPECT_FALSE(scratch.inUse);
}

TEST(LayoutScratch, TwoBlocksTakeTurnsInOneScratch) {
  GfxRenderer renderer;
  ParsedText a(false, false, justified());
  ParsedText b(false, false, justified());
  fill(a, "alpha", 30);
  fill(b, "be", 50);
  const std::vector<Line> expectedA = layOut(a, renderer);
  const std::vector<Line> expectedB = layOut(b, renderer);

  ParsedText::LayoutScratch scratch;
  ParsedText lentA(false, false, justified());
  ParsedText lentB(false, false, justified());
  lentA.lendLayoutScratch(&scratch);
  lentB.lendLayoutScratch(&scratch);
  fill(lentA, "alpha", 30);
  fill(lentB, "be", 50);
  EXPECT_EQ(layOut(lentA, renderer), expectedA);
  EXPECT_EQ(layOut(lentB, renderer), expectedB);
}

// The nesting the guard exists for: block B laid out from inside block A's line callback, both lent one scratch. B
// finds it in use and takes its own; A's pass, which still reads its tables after the callback returns, is unharmed.
TEST(LayoutScratch, APassInsideAnotherTakesAScratchOfItsOwn) {
  GfxRenderer renderer;
  ParsedText a(false, false, justified());
  ParsedText b(false, false, justified());
  fill(a, "alpha", 30);
  fill(b, "be", 50);
  const std::vector<Line> expectedA = layOut(a, renderer);
  const std::vector<Line> expectedB = layOut(b, renderer);

  ParsedText::LayoutScratch scratch;
  ParsedText outer(false, false, justified());
  ParsedText inner(false, false, justified());
  outer.lendLayoutScratch(&scratch);
  inner.lendLayoutScratch(&scratch);
  fill(outer, "alpha", 30);
  fill(inner, "be", 50);

  std::vector<Line> outerLines;
  std::vector<Line> innerLines;
  bool sawScratchInUse = false;
  outer.layoutAndExtractLines(
      renderer, kFontId, kWidth,
      [&](std::unique_ptr<TextBlock> tb, bool, bool) {
        outerLines.push_back(lineOf(*tb));
        if (outerLines.size() == 2) {
          sawScratchInUse = scratch.inUse;
          innerLines = layOut(inner, renderer);
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);

  EXPECT_TRUE(sawScratchInUse);
  EXPECT_EQ(outerLines, expectedA);
  EXPECT_EQ(innerLines, expectedB);
  EXPECT_FALSE(scratch.inUse);
}

TEST(LayoutScratch, ReleasingABlocksScratchGivesTheLentOneBackWhole) {
  GfxRenderer renderer;
  ParsedText::LayoutScratch scratch;
  ParsedText text(false, false, justified());
  text.lendLayoutScratch(&scratch);
  fill(text, "w", 40);
  layOut(text, renderer);
  EXPECT_GT(scratch.lineXPosScratch_.capacity(), 0u);
  text.releaseLayoutScratch();
  EXPECT_EQ(scratch.lineXPosScratch_.capacity(), 0u);
  EXPECT_EQ(scratch.dp_.capacity(), 0u);
  EXPECT_EQ(scratch.wordWidths_.capacity(), 0u);
}
