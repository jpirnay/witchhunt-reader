// Emphasis dots (傍点, CSS text-emphasis) through ParsedText and TextBlock: the mark each token carries in its
// boundary, the per-line Emphasis records, and the marks drawn above each character. Against the fixed-width shim of
// test/word_size_layout: every codepoint 10 px, ascender 16, so a mark at scale 0.5 is 5 px wide and the band is 8.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/TextEmphasis.h"
#include "Epub/TokenBoundary.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;
using Kind = TextBlock::AnnotationKind;
using TextEmphasis::Dot;
using TextEmphasis::Sesame;

BlockStyle leftAligned() {
  BlockStyle bs;
  bs.alignment = CssTextAlign::Left;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  return bs;
}

struct Record {
  uint16_t first;
  uint16_t count;
  Kind kind;
  uint8_t mark;
  bool operator==(const Record& o) const {
    return first == o.first && count == o.count && kind == o.kind && mark == o.mark;
  }
};

struct Line {
  std::vector<std::string> words;
  std::vector<Record> records;
  int band = 0;
};

// What laying a block out and drawing each line at (0, 0) produced.
struct Result {
  std::vector<Line> lines;
  GfxRenderer renderer;

  // The draw calls of the marks (every call at scale 0.5 whose text is not a ruby kana).
  std::vector<GfxRenderer::DrawCall> markCalls(const std::string& glyph) const {
    std::vector<GfxRenderer::DrawCall> calls;
    for (const auto& call : renderer.drawCalls)
      if (call.scale == 0.5f && call.text == glyph) calls.push_back(call);
    return calls;
  }
};

void layout(ParsedText& text, const uint16_t width, Result& out, const int fontId = kFontId) {
  text.layoutAndExtractLines(
      out.renderer, fontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line line;
        for (uint16_t i = 0; i < block->wordCount(); ++i) line.words.emplace_back(block->wordText(i));
        for (uint16_t i = 0; i < block->annotationCount(); ++i) {
          const TextBlock::Annotation a = block->annotation(i);
          line.records.push_back({a.firstWord, a.wordCount, a.kind, a.mark});
        }
        line.band = block->annotationBand(out.renderer, fontId);
        block->render(out.renderer, fontId, 0, 0);
        out.lines.push_back(std::move(line));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
}

constexpr const char* kSesameGlyph = "\xEF\xB9\x85";  // U+FE45 ﹅

std::string utf8(const uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return s;
}

}  // namespace

TEST(CjkEmphasis, EachCharacterGetsAMarkCentredAboveIt) {
  ParsedText text(false, false, leftAligned());
  text.addWord("強調", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Sesame);
  Result r;
  layout(text, 400, r);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_EQ(r.lines[0].records, (std::vector<Record>{{0, 2, Kind::Emphasis, Sesame}}));
  const auto marks = r.markCalls(kSesameGlyph);
  ASSERT_EQ(marks.size(), 2u);
  EXPECT_EQ(marks[0].x, 2);
  EXPECT_EQ(marks[1].x, 12);
  EXPECT_EQ(marks[0].y, 0);
  EXPECT_EQ(marks[1].y, 0);
  int baseY = -1;
  for (const auto& call : r.renderer.drawCalls)
    if (call.scale == 1.0f && call.text == "強") baseY = call.y;
  EXPECT_EQ(baseY, 8);
}

TEST(CjkEmphasis, PunctuationAndSpacesTakeNoMark) {
  {
    ParsedText text(false, false, leftAligned());
    text.addWord("強調。", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Sesame);
    Result r;
    layout(text, 400, r);
    EXPECT_EQ(r.markCalls(kSesameGlyph).size(), 2u);
  }
  {
    ParsedText text(false, false, leftAligned());
    text.addWord("強\xE3\x80\x80調", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Sesame);  // U+3000 between
    Result r;
    layout(text, 400, r);
    EXPECT_EQ(r.markCalls(kSesameGlyph).size(), 2u);
  }
}

TEST(CjkEmphasis, ASpanKeepsItsMarksAcrossALineBreak) {
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四五", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Dot);
  Result r;
  layout(text, 30, r);
  ASSERT_EQ(r.lines.size(), 2u);
  EXPECT_EQ(r.lines[0].records, (std::vector<Record>{{0, 3, Kind::Emphasis, Dot}}));
  EXPECT_EQ(r.lines[1].records, (std::vector<Record>{{0, 2, Kind::Emphasis, Dot}}));
  EXPECT_EQ(r.lines[0].band, 8);
  EXPECT_EQ(r.lines[1].band, 8);
}

TEST(CjkEmphasis, RubyWinsOverEmphasisOnTheSameBase) {
  ParsedText text(false, false, leftAligned());
  text.addWord("強調", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Dot);
  ASSERT_TRUE(text.addRubyGroup(0, 1, "きょう"));
  Result r;
  layout(text, 400, r);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_EQ(r.lines[0].records, (std::vector<Record>{{0, 1, Kind::Ruby, 0}, {1, 1, Kind::Emphasis, Dot}}));
  // The dot is drawn over 調 only.
  EXPECT_EQ(r.markCalls(utf8(0x2022)).size(), 1u);
}

TEST(CjkEmphasis, AFaceWithoutTheMarkDrawsADot) {
  ParsedText text(false, false, leftAligned());
  text.addWord("強調", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Sesame);
  Result r;
  layout(text, 400, r, GfxRenderer::LATIN_FONT);
  EXPECT_TRUE(r.markCalls(kSesameGlyph).empty());
  ASSERT_EQ(r.renderer.fillRectCalls.size(), 2u);
  for (const auto& rect : r.renderer.fillRectCalls) {
    EXPECT_EQ(rect.w, 2);  // max(2, 8 / 4)
    EXPECT_EQ(rect.h, 2);
  }
}

TEST(CjkEmphasis, ABionicHalfKeepsItsMark) {
  ParsedText text(false, false, leftAligned(), /*bionicReadingEnabled=*/true);
  text.addWord("emphasis", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Dot);
  Result r;
  layout(text, 400, r);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_EQ(r.lines[0].words, (std::vector<std::string>{"emph", "asis"}));
  EXPECT_EQ(r.lines[0].records, (std::vector<Record>{{0, 2, Kind::Emphasis, Dot}}));
}

TEST(CjkEmphasis, ASpanOfOnlyPunctuationGetsNoRecordAndNoBand) {
  for (const char* span : {"……", "「」"}) {
    ParsedText text(false, false, leftAligned());
    text.addWord(span, EpdFontFamily::REGULAR, false, false, 100, 0, 0, Dot);
    Result r;
    layout(text, 400, r);
    ASSERT_EQ(r.lines.size(), 1u);
    EXPECT_TRUE(r.lines[0].records.empty()) << span;
    EXPECT_EQ(r.lines[0].band, 0) << span;
  }
}

TEST(CjkEmphasis, ASpanWithOneMarkableCharacterKeepsItsRecord) {
  ParsedText text(false, false, leftAligned());
  text.addWord("「強」", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Dot);
  Result r;
  layout(text, 400, r);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_EQ(r.lines[0].records.size(), 1u);
}

TEST(CjkEmphasis, AHyphenationRemainderKeepsItsMark) {
  Hyphenator::setPreferredLanguage("en");
  ParsedText text(false, /*hyphenationEnabled=*/true, leftAligned());
  text.addWord("internationalization", EpdFontFamily::REGULAR, false, false, 100, 0, 0, Dot);
  Result r;
  layout(text, 80, r);
  ASSERT_GE(r.lines.size(), 2u);
  for (const Line& line : r.lines) {
    ASSERT_EQ(line.records.size(), 1u);
    EXPECT_EQ(line.records[0].first, 0);
    EXPECT_EQ(line.records[0].count, line.words.size());
    EXPECT_EQ(line.records[0].mark, Dot);
  }
}

TEST(TextEmphasisMarks, LettersAndDigitsTakeAMarkAsciiPunctuationDoesNot) {
  for (const char c : std::string("aZ09mQ")) EXPECT_TRUE(TextEmphasis::takesMark(static_cast<uint8_t>(c))) << c;
  for (const char c : std::string(" .,;:!?-\"'()[]/\\_@#$%&*+=<>~^`{|}")) {
    EXPECT_FALSE(TextEmphasis::takesMark(static_cast<uint8_t>(c))) << c;
  }
}

TEST(TextEmphasisMarks, NoBreakSpaceAndGeneralPunctuationTakeNone) {
  EXPECT_FALSE(TextEmphasis::takesMark(0xA0));
  EXPECT_FALSE(TextEmphasis::takesMark(0x3000));
  for (uint32_t cp = 0x2010; cp <= 0x205E; ++cp) EXPECT_FALSE(TextEmphasis::takesMark(cp)) << std::hex << cp;
}

TEST(TextEmphasisMarks, LatinOnePunctuationTakesNone) {
  for (const uint32_t cp : {0xA1u, 0xA7u, 0xABu, 0xB6u, 0xB7u, 0xBBu, 0xBFu}) {
    EXPECT_FALSE(TextEmphasis::takesMark(cp)) << std::hex << cp;
  }
  EXPECT_TRUE(TextEmphasis::takesMark(0xE9));  // é
}

TEST(TextEmphasisMarks, KanaAndKanjiTakeAMarkCjkPunctuationDoesNot) {
  for (const uint32_t cp : {0x3042u, 0x30A2u, 0x5F37u, 0x8ABFu})
    EXPECT_TRUE(TextEmphasis::takesMark(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x3002u, 0x3001u, 0x300Cu, 0x300Du})
    EXPECT_FALSE(TextEmphasis::takesMark(cp)) << std::hex << cp;
}
