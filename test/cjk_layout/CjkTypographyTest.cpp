// CJK punctuation widths, line-edge widths, flush justification and the whole-em grid (spec sections 5.1, 5.2,
// 5.5), through the real ParsedText and TextBlock against the fixed-width shim of test/word_size_layout: every
// codepoint 10 px (so 一 is a 10 px em and every CJK mark is full-width), a space 5 px. The shim draws a CJK
// opening bracket's ink on the right half of its advance, a closing bracket, 。 and 、 on the left half.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/TextEmphasis.h"
#include "Epub/TypographyProfile.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<bool> continues;
};

BlockStyle aligned(const CssTextAlign alignment, const int16_t indent = 0) {
  BlockStyle bs;
  bs.alignment = alignment;
  bs.textIndent = indent;
  bs.textIndentDefined = true;
  return bs;
}

std::vector<Line> layout(ParsedText& text, const uint16_t width, const int fontId = kFontId,
                         const int16_t blockStartY = 0, const int lineHeight = 0) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, fontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
          line.continues.push_back(block->wordContinues(i));
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true, blockStartY, lineHeight);
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

using Words = std::vector<std::vector<std::string>>;
using X = std::vector<int16_t>;

// The language's profile with the whole-em grid off, so a test line is as wide as it asks.
TypographyProfile withoutGrid(const char* language) {
  TypographyProfile profile = TypographyProfile::forLanguage(language);
  profile.wholeEmGrid = false;
  return profile;
}

// One paragraph of `text`; `greedy` selects the greedy breaker (hyphenation on), else the optimal one.
std::vector<Line> layoutText(const char* text, const TypographyProfile& profile, const CssTextAlign alignment,
                             const uint16_t width, const bool greedy = false, const int fontId = kFontId) {
  ParsedText paragraph(false, greedy, aligned(alignment));
  paragraph.setTypography(profile);
  paragraph.addWord(text, EpdFontFamily::REGULAR);
  return layout(paragraph, width, fontId);
}

}  // namespace

TEST(CjkPunctuationWidths, EachMarkIsATokenGluedWhereKinsokuHoldsIt) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"字", "」", "。", "次"}));
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, true, true, false}));
  // 」 before 。 is half-width (all but the last of a run), 。 before text keeps its width.
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 15, 25}));
}
TEST(CjkPunctuationWidths, ALatinWordKeepsItsCurlyQuotes) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE2\x80\x9CHello\xE2\x80\x9D", EpdFontFamily::REGULAR);  // “Hello”
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words.size(), 1u);
}
TEST(CjkPunctuationWidths, ACurlyQuotePairInCjkTextSetsAQuarterEmEachSide) {
  // ”“ in Chinese dialogue, at 160 % (16 px glyphs; the shim draws curly quotes full-width): ” before an opening
  // mark and “ after a closing one are 6/8 each, 12 px. The “ judges ” by its full width: narrowed to 12 px, ” is
  // below the 4/5 em a curly quote needs to count as CJK punctuation.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("\xE5\xA5\xBD\xE2\x80\x9D\xE2\x80\x9C\xE5\xAD\x97", EpdFontFamily::REGULAR, false, false,
               160);  // 好”“字
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  // “ keeps its ink (the right half): drawn 4 px left of its 12 px box at 28.
  EXPECT_EQ(lines[0].xpos, (X{0, 16, 24, 40}));
}
TEST(CjkPunctuationWidths, AMarkEndingALineIsHalfWidthSoTheLineHoldsIt) {
  // 字字字字。 is 45 px with the line-end 。 at half width: it fits a 45 px line. Measured at full width the line
  // would end before 字。.
  for (const bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
    text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
    EXPECT_EQ(wordsOf(layout(text, 45)), (Words{{"字", "字", "字", "字", "。"}, {"字", "字"}}))
        << (hyphenation ? "greedy breaker" : "DP breaker");
  }
}
TEST(CjkPunctuationWidths, AnOpeningBracketStartingALineIsHalfWidthAndDrawnByItsInk) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字字字字「字」", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 40);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "字", "字", "字"}, {"「", "字", "」"}}));
  // 「 keeps only its ink (the right half of its em): drawn 5 px left of the line start, so the ink starts there.
  EXPECT_EQ(lines[1].xpos, (X{-5, 5, 15}));
}
TEST(CjkPunctuationWidths, TraditionalChineseKeepsItsCentredMarksFullWidth) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("zh-TW"));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 400)[0].xpos, (X{0, 10, 20, 30}));
}
TEST(CjkPunctuationWidths, JapaneseLetsAFullStopEndingALineStayFull) {
  // JLREQ: 。 at a line end is half or full, and it stays full unless push-in needs it (PR A3b), so 字。 does not
  // fit after 字字字.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 45)), (Words{{"字", "字", "字"}, {"字", "。", "字", "字"}}));
}
TEST(CjkPunctuationWidths, APageBottomRetryKeepsTheMarksWidths) {
  // The retry re-lays a hyphenated line with its words whole: it merges tokens back and measures again, so the
  // line-edge widths must stay in step with the tokens. The last line's 」 is half-width before 。 as without a retry.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Justify));
  for (const char* word : {"가나", "다라마바사아", "자」。차"}) text.addWord(word, EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  std::vector<Line> lines;
  bool retried = false;
  text.layoutAndExtractLines(
      renderer, kFontId, 62,
      [&](std::unique_ptr<TextBlock> block, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry && !retried) {  // what the parser does at a page bottom
          retried = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  ASSERT_TRUE(retried);
  ASSERT_EQ(wordsOf(lines), (Words{{"가나"}, {"다라마바사아"}, {"자", "」", "。", "차"}}));
  EXPECT_EQ(lines[2].xpos, (X{0, 10, 15, 25}));
}

// Live allocations of this test binary, and every allocation it made, counted by the replacements of the global
// operator new and delete below.
namespace {
long gLiveAllocations = 0;
long gAllocations = 0;
}  // namespace
void* operator new(const std::size_t size) {
  if (void* p = std::malloc(size ? size : 1)) {
    ++gLiveAllocations;
    ++gAllocations;
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  if (p != nullptr) --gLiveAllocations;
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }

TEST(CjkPunctuationWidths, ReleasingTheLayoutScratchGivesBackTheMarksScratchToo) {
  // An incremental build gives the layout scratch back when it yields a slice: a laid-out block of text with marks
  // then holds nothing more than before its first word.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("zh"));
  const long before = gLiveAllocations;
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  layout(text, 400);
  text.releaseLayoutScratch();
  EXPECT_EQ(gLiveAllocations, before);
}
TEST(RubyGroups, ReleasingAnEmptyBlocksScratchGivesBackItsRuby) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  const long before = gLiveAllocations;
  text.addWord("漢字", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 2, "かんじ"));
  layout(text, 400);
  text.releaseLayoutScratch();
  EXPECT_EQ(gLiveAllocations, before);
}
// CjkRubyTest.cpp: the same hand-built line.
std::unique_ptr<TextBlock> rubyLine(std::vector<TextBlock::Annotation> annotations,
                                    std::vector<std::string> words = {"漢", "字"}, float multiplier = 1.0f,
                                    std::vector<uint8_t> sizePct = {});
TEST(RubyTextBlock, DrawingRubyAllocatesNothing) {
  const auto line = rubyLine({{0, 2, 0, 20, TextBlock::AnnotationKind::Ruby, 0, "かんじ"}});
  GfxRenderer renderer;
  renderer.drawCalls.reserve(16);
  const long before = gLiveAllocations;
  line->render(renderer, kFontId, 0, 100);
  EXPECT_EQ(gLiveAllocations, before);
  EXPECT_EQ(renderer.drawCalls.size(), 5u);
}
TEST(RubyTextBlock, DrawingMarksAllocatesNothing) {
  for (const int font : {kFontId, GfxRenderer::LATIN_FONT}) {  // glyph path, square path
    const auto line = rubyLine({{0, 2, 0, 0, TextBlock::AnnotationKind::Emphasis, TextEmphasis::Sesame, ""}});
    GfxRenderer renderer;
    renderer.drawCalls.reserve(16);
    renderer.fillRectCalls.reserve(16);
    const long before = gLiveAllocations;
    const long made = gAllocations;
    line->render(renderer, font, 0, 100);
    EXPECT_EQ(gLiveAllocations, before);
    EXPECT_EQ(gAllocations, made) << "an allocation freed before render() returns is still one per render";
    EXPECT_EQ(renderer.drawCalls.size() + renderer.fillRectCalls.size(), 4u);
  }
}
TEST(CjkJustification, AJustifiedCjkLineEndsFlush) {
  // 4 characters on a 47 px line: 7 px over 3 gaps, 2 px each and the last pixel to one of them.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  text.addWord("一二三四五六七八", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos.back() + 10, 47);
  EXPECT_EQ(lines[0].xpos, (X{0, 12, 24, 37}));
}
TEST(CjkJustification, LatinLinesKeepTheirRounding) {
  // aa bb cc on 81 px: 11 px over 2 word gaps, 5 each; the last pixel stays at the end as before.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  for (const char* word : {"aa", "bb", "cc", "dd"}) text.addWord(word, EpdFontFamily::REGULAR);
  const auto lines = layout(text, 81);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (X{0, 30, 60}));
}
TEST(CjkGrid, AJapaneseBookRoundsTheWidthDownToWholeEmsAndCentresIt) {
  // 47 px holds 4 ems of 10 px: the lines are 40 px wide, 3 px in from the left.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  EXPECT_EQ(wordsOf(lines), (Words{{"一", "二", "三", "四"}, {"五"}}));
  EXPECT_EQ(lines[0].xpos, (X{3, 13, 23, 33}));
  EXPECT_EQ(lines[1].xpos, (X{3}));
}
TEST(CjkGrid, AnIndentRoundsDownToWholeEmsAndNoIndentStaysNone) {
  for (const auto& [indent, firstX] : std::vector<std::pair<int16_t, int16_t>>{{15, 13}, {5, 13}, {0, 3}}) {
    ParsedText text(false, false, aligned(CssTextAlign::Left, indent));
    text.setTypography(TypographyProfile::forLanguage("zh"));
    text.addWord("一二三", EpdFontFamily::REGULAR);
    EXPECT_EQ(layout(text, 47)[0].xpos.front(), firstX) << "indent " << indent;
  }
}
TEST(CjkGrid, AFontWithoutCjkGlyphsGetsNoGridAndNoNarrowedMarks) {
  // A Latin reader font draws every CJK character as its replacement glyph, all at one advance. Measured by that
  // glyph, 一 would make a grid and the marks would pass as full-width; the font has no 一, so neither applies.
  ParsedText text(false, false, aligned(CssTextAlign::Left, 15));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("字」。次", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47, GfxRenderer::LATIN_FONT);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (X{15, 25, 35}));
  EXPECT_EQ(lines[1].xpos, (X{0}));
}
TEST(CjkGrid, OtherBooksHaveNoGrid) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 47)[0].xpos, (X{0, 10, 20, 30}));
}
TEST(CjkGrid, AHeadingsGridIsInItsOwnEms) {
  // A block at 1.2 times the body size has 12 px ems: 47 px holds 3 of them, 5 px in from the left.
  BlockStyle heading = aligned(CssTextAlign::Left);
  heading.fontSizeMultiplier = 1.2f;
  ParsedText text(false, false, heading);
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  EXPECT_EQ(wordsOf(lines), (Words{{"一", "二", "三"}, {"四"}}));
  EXPECT_EQ(lines[0].xpos, (X{5, 17, 29}));
}
TEST(CjkGrid, ABlockBesideAFloatKeepsItsOwnWidth) {
  BlockStyle beside = aligned(CssTextAlign::Left);
  beside.floatZoneCount = 1;
  beside.floatZones[0] = {0, 0, 0, false};  // a zone no line meets: the block is beside a float all the same
  ParsedText text(false, false, beside);
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五", EpdFontFamily::REGULAR);
  EXPECT_EQ(layout(text, 47)[0].xpos, (X{0, 10, 20, 30}));
}

// The optimal line breaker narrows only the paragraph's first line by its indent; every later line plans at the full
// width (the extraction indents the first line alone). Tokens of 20 px with 5 px gaps: 4 fit 100 px, 3 fit 80.
namespace {
size_t lineSize(const std::vector<Line>& lines, const size_t i) { return lines.at(i).words.size(); }
}  // namespace

TEST(OptimalBreakerIndent, OnlyTheFirstLineGivesUpTheIndent) {
  ParsedText text(false, false, aligned(CssTextAlign::Left, 20));
  for (int i = 0; i < 12; ++i) text.addWord("aa", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 100);
  ASSERT_GE(lines.size(), 3u);
  EXPECT_EQ(lineSize(lines, 0), 3u);  // 100 - 20 = 80 px
  EXPECT_EQ(lineSize(lines, 1), 4u);  // the full 100 px
  EXPECT_EQ(lineSize(lines, 2), 4u);
}
TEST(OptimalBreakerIndent, ACjkParagraphKeepsTheWholeEmGridAfterTheFirstLine) {
  // 47 px holds 4 ems; the 10 px indent is one em, so the first line takes 3 characters and the rest 4.
  ParsedText text(false, false, aligned(CssTextAlign::Left, 10));
  text.setTypography(TypographyProfile::forLanguage("ja"));
  text.addWord("一二三四五六七八九十一二", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 47);
  ASSERT_GE(lines.size(), 3u);
  EXPECT_EQ(lineSize(lines, 0), 3u);
  EXPECT_EQ(lineSize(lines, 1), 4u);
  EXPECT_EQ(lineSize(lines, 2), 4u);
}
TEST(OptimalBreakerIndent, BesideAFloatTheFirstLineTakesBothAndLaterLinesOnlyTheFloat) {
  // A 20 px float over the first two 10 px lines. Line 0: 100 - 20 (float) - 20 (indent) = 60 px, 2 tokens.
  // Line 1: 80 px, 3 tokens. Line 2 on: 100 px, 4 tokens.
  BlockStyle beside = aligned(CssTextAlign::Left, 20);
  beside.floatZoneCount = 1;
  beside.floatZones[0] = {0, 15, 20, false};
  ParsedText text(false, false, beside);
  for (int i = 0; i < 12; ++i) text.addWord("aa", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 100, kFontId, /*blockStartY=*/0, /*lineHeight=*/10);
  ASSERT_GE(lines.size(), 4u);
  EXPECT_EQ(lineSize(lines, 0), 2u);
  EXPECT_EQ(lineSize(lines, 1), 3u);
  EXPECT_EQ(lineSize(lines, 2), 4u);
}

TEST(CjkLatinGap, AQuarterEmBetweenHanOrKanaAndLatinLettersOrDigits) {
  // 10 px ems: (2 × 10 + 4) / 8 = 3 px on each side of A and 1.
  EXPECT_EQ(layoutText("字A字1か", withoutGrid("ja"), CssTextAlign::Left, 400)[0].xpos, (X{0, 13, 26, 39, 52}));
  EXPECT_EQ(layoutText("字A", withoutGrid("zh"), CssTextAlign::Left, 400)[0].xpos, (X{0, 13}));
}
TEST(CjkLatinGap, NoGapAtASpaceAMarkAFullWidthLetterOrInOtherBooks) {
  const auto xpos = [](const TypographyProfile& profile, const std::vector<const char*>& words) {
    ParsedText text(false, false, aligned(CssTextAlign::Left));
    text.setTypography(profile);
    for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
    return layout(text, 400)[0].xpos;
  };
  EXPECT_EQ(xpos(withoutGrid("ja"), {"字", "A字"}), (X{0, 15, 28}));           // the source's space; the gap after A
  EXPECT_EQ(xpos(withoutGrid("ja"), {"字「A」字"}), (X{0, 10, 20, 30, 40}));   // next to brackets
  EXPECT_EQ(xpos(withoutGrid("ja"), {"字ＡＢ字"}), (X{0, 10, 20, 30}));        // full-width letters
  EXPECT_EQ(xpos(withoutGrid("ja"), {"ｱA"}), (X{0, 10}));                      // half-width katakana
  EXPECT_EQ(xpos(TypographyProfile::forLanguage("ko"), {"漢A"}), (X{0, 10}));  // Korean books (KLREQ)
  EXPECT_EQ(xpos(TypographyProfile{}, {"字A字"}), (X{0, 10, 20}));             // other books (spec 5.6)
}
TEST(CjkLatinGap, ALineBreakAtTheGapDropsIt) {
  // 字字字A is 43 px with its gap and fills 43 px; the last 字 would need 13 px more and starts the next line at 0.
  // The other way round, A starts the next line at 0.
  for (const bool greedy : {false, true}) {
    const auto after = layoutText("字字字A字", withoutGrid("ja"), CssTextAlign::Left, 43, greedy);
    ASSERT_EQ(wordsOf(after), (Words{{"字", "字", "字", "A"}, {"字"}})) << (greedy ? "greedy" : "DP");
    EXPECT_EQ(after[0].xpos, (X{0, 10, 20, 33}));
    EXPECT_EQ(after[1].xpos, (X{0}));
    const auto before = layoutText("字字字字A", withoutGrid("ja"), CssTextAlign::Left, 40, greedy);
    ASSERT_EQ(wordsOf(before), (Words{{"字", "字", "字", "字"}, {"A"}})) << (greedy ? "greedy" : "DP");
    EXPECT_EQ(before[1].xpos, (X{0}));
  }
}
TEST(CjkLatinGap, TheGapIsAQuarterEmOfTheCjkSide) {
  // 字 at 160 % is 16 px: the gaps on both of its sides are (2 × 16 + 4) / 8 = 4 px, though the letters are at 100 %.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(withoutGrid("ja"));
  text.addWord("A", EpdFontFamily::REGULAR);
  text.addWord("字", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true, 160);
  text.addWord("A", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
  EXPECT_EQ(layout(text, 400)[0].xpos, (X{0, 14, 34}));
}
TEST(CjkLatinGap, TheGapIsTheCjkSidesAlsoWhenTheLatinSideIsLarger) {
  // A at 160 % is 16 px beside 字 at 100 %: the gap is (2 × 10 + 4) / 8 = 3 px, not the 4 px of the larger side.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(withoutGrid("ja"));
  text.addWord("字", EpdFontFamily::REGULAR);
  text.addWord("A", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true, 160);
  text.addWord("字", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
  EXPECT_EQ(layout(text, 400)[0].xpos, (X{0, 13, 32}));
}
TEST(CjkLatinGap, AFontWithoutCjkGlyphsGetsNoGap) {
  // A Latin reader font has no 一 of its own: no em, so no gap, as no grid and no narrowed mark (A3a).
  EXPECT_EQ(layoutText("字A字", TypographyProfile::forLanguage("ja"), CssTextAlign::Left, 400, false,
                       GfxRenderer::LATIN_FONT)[0]
                .xpos,
            (X{0, 10, 20}));
}
TEST(CjkLatinGap, TheGapStretchesWithJustification) {
  // 字A字字 is 46 px; the 4 px left on 50 go 1, 1 and 2 to its three gaps (the two CJK-Latin gaps among them), flush.
  const auto lines = layoutText("字A字字字字", withoutGrid("ja"), CssTextAlign::Justify, 50);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "A", "字", "字"}, {"字", "字"}}));
  EXPECT_EQ(lines[0].xpos, (X{0, 14, 28, 40}));
}
