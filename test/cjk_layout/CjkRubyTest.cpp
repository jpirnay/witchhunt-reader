// Ruby on a TextBlock line (spec section 7): the boxes a line carries in its arena behind bit 1 of the flags byte, the
// band above the base they are drawn in, and the disk format. Against the fixed-width shim of test/word_size_layout:
// every codepoint 10 px, ascender 16.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "GfxRenderer.h"

// Defined outside the anonymous namespace: CjkTypographyTest.cpp, where the global operator new counter lives, uses
// the same hand-built line.
std::unique_ptr<TextBlock> rubyLine(std::vector<TextBlock::Annotation> annotations,
                                    std::vector<std::string> words = {"漢", "字"}, float multiplier = 1.0f,
                                    std::vector<uint8_t> sizePct = {}) {
  std::vector<int16_t> xpos;
  int x = 0;
  for (const std::string& word : words) {
    xpos.push_back(static_cast<int16_t>(x));
    x += GfxRenderer::countCodepoints(word.c_str()) * GfxRenderer::GLYPH_W;
  }
  const std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
  std::vector<TextBlock::WordSize> sizes;
  for (const uint8_t pct : sizePct) sizes.push_back({pct, 0});

  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &styles;
  range.sizes = sizes.empty() ? nullptr : &sizes;
  range.annotations = &annotations;
  range.first = 0;
  range.count = words.size();

  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndent = 0;
  style.textIndentDefined = true;
  style.fontSizeMultiplier = multiplier;
  return std::unique_ptr<TextBlock>(new TextBlock(range, xpos, style));
}

namespace {

constexpr int kFontId = 1;
using Kind = TextBlock::AnnotationKind;

std::string serialized(const TextBlock& block) {
  FsFile file = HalFile::forReadWrite();
  EXPECT_TRUE(block.serialize(file));
  EXPECT_TRUE(file.seek(0));
  std::string bytes(file.size(), '\0');
  file.read(bytes.data(), bytes.size());
  return bytes;
}

std::unique_ptr<TextBlock> restore(const std::string& bytes) {
  FsFile file = HalFile::forReadWrite();
  file.write(bytes.data(), bytes.size());
  EXPECT_TRUE(file.seek(0));
  return TextBlock::deserialize(file);
}

// The draw calls at the ruby's scale 0.5 (the base words are drawn at 1.0).
std::vector<GfxRenderer::DrawCall> rubyCalls(const GfxRenderer& renderer) {
  std::vector<GfxRenderer::DrawCall> calls;
  for (const auto& call : renderer.drawCalls)
    if (call.scale == 0.5f) calls.push_back(call);
  return calls;
}

}  // namespace

TEST(RubyTextBlock, AnAnnotatedLineKeepsItsGroupsThroughSerialization) {
  auto line = rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"}});
  ASSERT_EQ(line->annotationCount(), 1);
  const std::string bytes = serialized(*line);
  EXPECT_EQ(bytes[2], 0x02);  // flags: annotations only
  const auto back = restore(bytes);
  ASSERT_NE(back, nullptr);
  const TextBlock::Annotation a = back->annotation(0);
  EXPECT_EQ(a.firstWord, 0);
  EXPECT_EQ(a.wordCount, 2);
  EXPECT_EQ(a.x, 0);
  EXPECT_EQ(a.width, 20);
  EXPECT_EQ(std::string(a.text), "かんじ");
  EXPECT_EQ(std::string(back->wordText(1)), "字");
}

TEST(RubyTextBlock, ALineWithoutAnnotationsIsTheBytesItWasBefore) {
  const std::vector<std::string> words = {"漢", "字"};
  const std::vector<int16_t> xpos = {0, 10};
  const std::vector<EpdFontFamily::Style> styles(2, EpdFontFamily::REGULAR);
  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &styles;
  range.count = 2;
  const std::vector<TextBlock::Annotation> none;
  BlockStyle style;

  const TextBlock noPointer(range, xpos, style);
  range.annotations = &none;
  const TextBlock emptyVector(range, xpos, style);
  const TextBlock vectorForm(words, xpos, styles, style);

  EXPECT_FALSE(noPointer.hasAnnotations());
  EXPECT_FALSE(emptyVector.hasAnnotations());
  const std::string a = serialized(noPointer);
  EXPECT_EQ(a, serialized(emptyVector));
  EXPECT_EQ(a, serialized(vectorForm));
  EXPECT_EQ(a[2], 0x00);
  // Header 5 bytes (no annotationBytes), textOff 4, xpos 4, styles 2, text 8, render style 9.
  EXPECT_EQ(a.size(), 5u + 4 + 4 + 2 + 8 + 9);
}

TEST(RubyTextBlock, CorruptAnnotationsAreRefused) {
  const std::string bytes = serialized(*rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"}}));
  ASSERT_NE(restore(bytes), nullptr);
  // Header 7 bytes, arena: textOff 4, xpos 4, styles 2, text 8; the region starts at 25: count (2), then the record.
  constexpr size_t kRecord = 27;

  std::string wordsPastTheLine = bytes;
  wordsPastTheLine[kRecord + 2] = 3;  // wordCount 3: firstWord 0 + 3 > 2 words
  EXPECT_EQ(restore(wordsPastTheLine), nullptr);

  std::string reservedFlag = bytes;
  reservedFlag[2] = static_cast<char>(reservedFlag[2] | 0x04);
  EXPECT_EQ(restore(reservedFlag), nullptr);

  std::string lenPastTheEnd = bytes;
  lenPastTheEnd[kRecord + 9] = static_cast<char>(200);
  EXPECT_EQ(restore(lenPastTheEnd), nullptr);
}

TEST(RubyTextBlock, RubySitsInABandOfHalfAnAscenderAboveItsBase) {
  const auto line = rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"}});
  GfxRenderer renderer;
  line->render(renderer, kFontId, 0, 100);
  const auto ruby = rubyCalls(renderer);
  ASSERT_EQ(ruby.size(), 3u);
  EXPECT_EQ(ruby[0].text, "か");
  EXPECT_EQ(ruby[1].text, "ん");
  EXPECT_EQ(ruby[2].text, "じ");
  // 15 px of ruby over the 20 px box, 5 px to spread 1:2:1.
  EXPECT_EQ(ruby[0].x, 0);
  EXPECT_EQ(ruby[1].x, 7);
  EXPECT_EQ(ruby[2].x, 14);
  for (const auto& call : ruby) EXPECT_EQ(call.y, 100);  // 108 - round(16 * 0.5)
  int bases = 0;
  for (const auto& call : renderer.drawCalls) {
    if (call.scale != 1.0f) continue;
    ++bases;
    EXPECT_EQ(call.y, 108);
  }
  EXPECT_EQ(bases, 2);
}

TEST(RubyTextBlock, ARubyThatIsNotCjkIsCentred) {
  const auto line = rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "abc"}});
  GfxRenderer renderer;
  line->render(renderer, kFontId, 0, 100);
  const auto ruby = rubyCalls(renderer);
  ASSERT_EQ(ruby.size(), 3u);
  EXPECT_EQ(ruby[0].x, 2);
  EXPECT_EQ(ruby[1].x, 7);
  EXPECT_EQ(ruby[2].x, 12);
}

TEST(RubyTextBlock, AKanaRubyOverALatinBaseIsCentred) {
  const auto line = rubyLine({{0, 1, 0, 20, Kind::Ruby, 0, "かな"}}, {"ab"});
  GfxRenderer renderer;
  line->render(renderer, kFontId, 0, 100);
  const auto ruby = rubyCalls(renderer);
  ASSERT_EQ(ruby.size(), 2u);
  EXPECT_EQ(ruby[0].x, 5);  // not 1:2:1's 2 and 12
  EXPECT_EQ(ruby[1].x, 10);
}

TEST(RubyTextBlock, TheBandFollowsTheBlocksSize) {
  GfxRenderer renderer;
  const TextBlock::Annotation ruby{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"};
  EXPECT_EQ(rubyLine({ruby})->annotationBand(renderer, kFontId), 8);
  EXPECT_EQ(rubyLine({ruby}, {"漢", "字"}, 2.0f)->annotationBand(renderer, kFontId), 16);
  EXPECT_EQ(rubyLine({})->annotationBand(renderer, kFontId), 0);
}

TEST(RubyTextBlock, TheBandIsTheSameBeforeAndAfterTheLineExists) {
  GfxRenderer renderer;
  const TextBlock::Annotation ruby{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"};
  for (const float multiplier : {1.0f, 1.25f}) {
    for (const std::vector<uint8_t>& sizes : {std::vector<uint8_t>{100, 100}, std::vector<uint8_t>{100, 150}}) {
      const auto line = rubyLine({ruby}, {"漢", "字"}, multiplier, sizes);
      EXPECT_EQ(line->annotationBand(renderer, kFontId),
                TextBlock::annotationBandFor(renderer, kFontId, multiplier, line->maxSizePct()))
          << multiplier << " " << static_cast<int>(sizes[1]);
    }
  }
}

TEST(RubyTextBlock, WordBoxSitsBelowTheBand) {
  const auto line = rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"}});
  GfxRenderer renderer;
  EXPECT_EQ(line->wordBox(renderer, 1, kFontId, 0, 100).y, 108);
}

TEST(RubyTextBlock, AnAnnotatedLineIsNotDrawnUnscaled) {
  EXPECT_FALSE(rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "かんじ"}})->drawsUnscaled());
  EXPECT_TRUE(rubyLine({})->drawsUnscaled());
}

TEST(RubyTextBlock, RubyIsPlacedByTheRawPrefixRoundedOnce) {
  // Block multiplier 0.3: the ruby draws at 0.15, 10 px glyphs advance 1.5 px. Rounding each character gave 2 px a
  // character (a 6 px ruby, the third character at 11); layout measures the whole ruby once: 4.5 -> 5 px, and the
  // characters stand at the rounded prefixes 0, 2, 3.
  const auto line = rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, "abc"}}, {"漢", "字"}, 0.3f);
  GfxRenderer renderer;
  line->render(renderer, kFontId, 0, 100);
  std::vector<int> xs;
  for (const auto& call : renderer.drawCalls)
    if (call.text.size() == 1) xs.push_back(call.x);
  ASSERT_EQ(xs.size(), 3u);
  EXPECT_EQ(xs[0], 7);  // (20 - 5) / 2
  EXPECT_EQ(xs[1], 9);
  EXPECT_EQ(xs[2], 10);
}

TEST(RubyTextBlock, TheBandRoundsUpForAnOddAscender) {
  // Ascender 16 at 1.0625 is 17: a band of 9, the ruby's own ascender round(17 * 0.5) = 9.
  GfxRenderer renderer;
  EXPECT_EQ(TextBlock::annotationBandFor(renderer, kFontId, 1.0625f, 100), 9);
  EXPECT_EQ(TextBlock::annotationBandFor(renderer, kFontId, 1.0f, 100), 8);
}

TEST(RubyTextBlock, TheRangeConstructorRefusesBadAnnotations) {
  EXPECT_FALSE(rubyLine({{0, 0, 0, 20, Kind::Ruby, 0, "かんじ"}})->valid());  // no words
  EXPECT_FALSE(rubyLine({{1, 2, 0, 20, Kind::Ruby, 0, "かんじ"}})->valid());  // past the line
  const std::string tooLong(256, 'a');
  EXPECT_FALSE(rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, tooLong}})->valid());
  EXPECT_TRUE(rubyLine({{0, 2, 0, 20, Kind::Ruby, 0, std::string(255, 'a')}})->valid());
}

// Ruby groups through ParsedText: the groups ride on the tokens' boundaries and come out as the line's annotations.
namespace {

struct RubyBox {
  size_t first;
  size_t count;
  int x;
  int w;
  std::string text;
};

struct Line {
  std::vector<std::string> words;
  std::vector<RubyBox> ruby;
};

BlockStyle aligned(const CssTextAlign alignment, const int16_t indent = 0) {
  BlockStyle bs;
  bs.alignment = alignment;
  bs.textIndent = indent;
  bs.textIndentDefined = true;
  return bs;
}

std::vector<Line> layout(ParsedText& text, const uint16_t width, const bool includeLastLine = true,
                         const bool preserveSource = false) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) line.words.emplace_back(block->wordText(i));
        for (uint16_t i = 0; i < block->annotationCount(); ++i) {
          const TextBlock::Annotation a = block->annotation(i);
          line.ruby.push_back({a.firstWord, a.wordCount, a.x, a.width, std::string(a.text)});
        }
        return ParsedText::LineProcessResult::Accepted;
      },
      includeLastLine, 0, 0, preserveSource);
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

// (first, count, text) of each of a line's ruby boxes.
std::vector<std::tuple<size_t, size_t, std::string>> rubyOf(const Line& line) {
  std::vector<std::tuple<size_t, size_t, std::string>> result;
  for (const auto& box : line.ruby) result.emplace_back(box.first, box.count, box.text);
  return result;
}

using Words = std::vector<std::vector<std::string>>;
using Ruby = std::vector<std::tuple<size_t, size_t, std::string>>;

}  // namespace

TEST(RubyGroups, AGroupNeverBreaksInside) {
  for (const bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
    text.addWord("は吾輩", EpdFontFamily::REGULAR);
    ASSERT_TRUE(text.addRubyGroup(1, 2, "わがはい"));
    const auto lines = layout(text, 25);
    EXPECT_EQ(wordsOf(lines), (Words{{"は"}, {"吾", "輩"}})) << hyphenation;
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(rubyOf(lines[0]), Ruby{});
    EXPECT_EQ(rubyOf(lines[1]), (Ruby{{0, 2, "わがはい"}}));
  }
}

TEST(RubyGroups, AGroupRefusesWhatCannotBeOne) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("一二三", EpdFontFamily::REGULAR);
  EXPECT_FALSE(text.addRubyGroup(0, 0, "x"));
  EXPECT_FALSE(text.addRubyGroup(2, 2, "x"));
  EXPECT_TRUE(text.addRubyGroup(1, 2, "x"));
  EXPECT_FALSE(text.addRubyGroup(0, 2, "y"));  // token 1 has a role
  EXPECT_TRUE(text.addRubyGroup(0, 1, "z"));   // a group before it keeps the texts in token order
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(rubyOf(lines[0]), (Ruby{{0, 1, "z"}, {1, 2, "x"}}));
}

TEST(RubyGroups, AnRtWithoutANewBaseAppendsToTheLastGroup) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  EXPECT_FALSE(text.appendRubyText("じ"));
  text.addWord("漢", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 1, "かん"));
  EXPECT_TRUE(text.appendRubyText("じ"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(rubyOf(lines[0]), (Ruby{{0, 1, "かんじ"}}));
}

TEST(RubyGroups, ConsumedLinesTakeTheirRubyWithThem) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("吾輩は猫", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 2, "わがはい"));
  ASSERT_TRUE(text.addRubyGroup(3, 1, "ねこ"));
  const auto first = layout(text, 30, /*includeLastLine=*/false);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].words, (std::vector<std::string>{"吾", "輩", "は"}));
  EXPECT_EQ(rubyOf(first[0]), (Ruby{{0, 2, "わがはい"}}));
  const auto second = layout(text, 30, /*includeLastLine=*/true);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].words, (std::vector<std::string>{"猫"}));
  EXPECT_EQ(rubyOf(second[0]), (Ruby{{0, 1, "ねこ"}}));
}

TEST(RubyGroups, PreserveSourceLaysTheSameGroupsOutTwice) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("吾輩は猫", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 2, "わがはい"));
  ASSERT_TRUE(text.addRubyGroup(3, 1, "ねこ"));
  for (int pass = 0; pass < 2; ++pass) {
    const auto lines = layout(text, 30, /*includeLastLine=*/true, /*preserveSource=*/true);
    ASSERT_EQ(lines.size(), 2u) << pass;
    EXPECT_EQ(wordsOf(lines), (Words{{"吾", "輩", "は"}, {"猫"}})) << pass;
    EXPECT_EQ(rubyOf(lines[0]), (Ruby{{0, 2, "わがはい"}})) << pass;
    EXPECT_EQ(rubyOf(lines[1]), (Ruby{{0, 1, "ねこ"}})) << pass;
  }
}

TEST(RubyGroups, ALatinBaseSplitByBionicReadingStaysOneGroup) {
  ParsedText text(false, false, aligned(CssTextAlign::Left), /*bionicReadingEnabled=*/true);
  text.addWord("Tokyo", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 1, "とうきょう"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"Tok", "yo"}));
  EXPECT_EQ(rubyOf(lines[0]), (Ruby{{0, 2, "とうきょう"}}));
}

TEST(RubyGroups, AGroupBaseIsNeverHyphenated) {
  Hyphenator::setPreferredLanguage("en");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Left));
  text.addWord("internationalization", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 1, "x"));
  const auto lines = layout(text, 60);
  EXPECT_EQ(wordsOf(lines), (Words{{"internationalization"}}));
}

TEST(RubyGroups, AGroupInTheMiddleOfAWordKeepsItsRuby) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と煙を", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "けむり"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(rubyOf(lines[0]), (Ruby{{1, 1, "けむり"}}));
  ASSERT_EQ(lines[0].ruby.size(), 1u);
  EXPECT_EQ(lines[0].ruby[0].x, 10);
  EXPECT_EQ(lines[0].ruby[0].w, 10);
}
