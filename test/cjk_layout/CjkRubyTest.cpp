// Ruby on a TextBlock line (spec section 7): the boxes a line carries in its arena behind bit 1 of the flags byte, the
// band above the base they are drawn in, and the disk format. Against the fixed-width shim of test/word_size_layout:
// every codepoint 10 px, ascender 16.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/TypographyProfile.h"
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
  std::vector<int16_t> xpos;
  std::vector<RubyBox> ruby;
};

BlockStyle aligned(const CssTextAlign alignment, const int16_t indent = 0) {
  BlockStyle bs;
  bs.alignment = alignment;
  bs.textIndent = indent;
  bs.textIndentDefined = true;
  return bs;
}

Line lineOf(const TextBlock& block) {
  Line line;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    line.words.emplace_back(block.wordText(i));
    line.xpos.push_back(block.wordXpos(i));
  }
  for (uint16_t i = 0; i < block.annotationCount(); ++i) {
    const TextBlock::Annotation a = block.annotation(i);
    line.ruby.push_back({a.firstWord, a.wordCount, a.x, a.width, std::string(a.text)});
  }
  return line;
}

std::vector<Line> layout(ParsedText& text, const uint16_t width, const bool includeLastLine = true,
                         const bool preserveSource = false) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        lines.push_back(lineOf(*block));
        return ParsedText::LineProcessResult::Accepted;
      },
      includeLastLine, 0, 0, preserveSource);
  return lines;
}

// As layout(), but the first line that ends hyphenated is refused once, as the parser does at a page bottom: the
// line is laid out again whole and the rest broken again after it.
std::vector<Line> layoutRetryingOnce(ParsedText& text, const uint16_t width, bool& retried) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  retried = false;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry && !retried) {
          retried = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        lines.push_back(lineOf(*block));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
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

// {x, width} of each of a line's ruby boxes.
std::vector<std::pair<int, int>> boxesOf(const Line& line) {
  std::vector<std::pair<int, int>> result;
  for (const auto& box : line.ruby) result.emplace_back(box.x, box.w);
  return result;
}

using Words = std::vector<std::vector<std::string>>;
using Ruby = std::vector<std::tuple<size_t, size_t, std::string>>;
using Boxes = std::vector<std::pair<int, int>>;
using X = std::vector<int16_t>;

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

TEST(RubyGroups, ARubyOver255BytesIsCutAtACharacterBoundary) {
  // 100 x 3-byte kana = 300 bytes: 255 is a whole number of characters, 254 would not be if the cut
  // were mid-sequence. Appended text is cut the same way, against what the group already holds.
  std::string longRuby;
  for (int i = 0; i < 100; ++i) longRuby += "あ";
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("漢字", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 2, longRuby));
  text.addWord("猫", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(2, 1, std::string(254, 'a') + "あ"));  // 254 ASCII + a 3-byte character
  ASSERT_TRUE(text.appendRubyText("い"));                              // already at 254: nothing fits
  const auto lines = layout(text, 2000);
  ASSERT_EQ(lines.size(), 1u);
  const auto ruby = rubyOf(lines[0]);
  ASSERT_EQ(ruby.size(), 2u);
  EXPECT_EQ(std::get<2>(ruby[0]).size(), 255u);
  EXPECT_EQ(std::get<2>(ruby[0]), longRuby.substr(0, 255));
  EXPECT_EQ(std::get<2>(ruby[1]), std::string(254, 'a'));
}

TEST(RubyGroups, TheCapNeverCutsATwoOrFourByteSequence) {
  // 254 ASCII then a 2-byte (é) or 4-byte (U+1F600) character straddling or filling the limit.
  const std::string two = std::string(254, 'a') + "\xC3\xA9";                // 256 bytes: cut before the 2-byte char
  const std::string fourAt252 = std::string(252, 'a') + "\xF0\x9F\x98\x80";  // 256 bytes: cut before the 4-byte char
  const std::string fourFits = std::string(251, 'a') + "\xF0\x9F\x98\x80";   // 255 bytes: kept whole
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("一二三", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 1, two));
  ASSERT_TRUE(text.addRubyGroup(1, 1, fourAt252));
  ASSERT_TRUE(text.addRubyGroup(2, 1, fourFits));
  ASSERT_TRUE(text.appendRubyText("\xC3\xA9"));  // no room left in the last: nothing appended
  const auto lines = layout(text, 4000);
  ASSERT_EQ(lines.size(), 1u);
  const auto ruby = rubyOf(lines[0]);
  ASSERT_EQ(ruby.size(), 3u);
  EXPECT_EQ(std::get<2>(ruby[0]), std::string(254, 'a'));
  EXPECT_EQ(std::get<2>(ruby[1]), std::string(252, 'a'));
  EXPECT_EQ(std::get<2>(ruby[2]), fourFits);
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
  // Over 煙 at 10, overhanging と and を (RubyWidths).
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{8, 15}}));
}

// Ruby widths (spec section 7.2, A3 spec section 9): a ruby at half size, 5 px a character, is spread over a wider
// base; a longer one overhangs a kana or punctuation neighbour by up to half of it, pushes any other neighbour away
// and spreads its base 1:2:1.
TEST(RubyWidths, AShorterRubyIsSpreadOverItsBase) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("煙草", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(0, 2, "たばこ"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 10}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{0, 20}}));
}

TEST(RubyWidths, ALongerRubyOverhangsTheKanaBesideIt) {
  // 15 px of ruby over 10 px of base: 2 px over と, 3 px over を, and the base keeps its width.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と煙を", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "けむり"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 20}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{8, 15}}));
}

TEST(RubyWidths, ALongerRubyPushesHanNeighboursAndSpreadsItsBase) {
  // 30 px of ruby over 20 px of base between two kanji: the base widens to 30, 5 px more for each character, and
  // each character is drawn in the middle of its 15 px share (1:2:1).
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("漢字字漢", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 2, "ああああああ"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 12, 27, 40}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{10, 30}}));
}

TEST(RubyWidths, AdjacentGroupsShareNoSpace) {
  // Each 15 px ruby passes its 10 px base by 2 px on the left and 3 on the right. Over its kana neighbour it
  // overhangs; beside the other group it pushes: 煙 widens by 3 on its right, 草 by 2 on its left (drawn 2 px in).
  // The first ruby ends where the second starts.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("の煙草が", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "けむり"));
  ASSERT_TRUE(text.addRubyGroup(2, 1, "くさき"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 25, 35}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{8, 15}, {23, 15}}));
}

TEST(RubyWidths, AnOverhangOnOneSideOnlyKeepsTheRubyCentred) {
  // 30 px of ruby over 20 px of base passes it by 5 px each side: over と on the left, a push on the right (漢). The
  // ruby is not slid over the kana: the base widens on its right only, unspread, and stays under the ruby's centre.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と字字漢", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 2, "ああああああ"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 20, 35}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{5, 30}}));
}

TEST(RubyWidths, AKanaNeighbourTakesAtMostHalfOfItself) {
  // 25 px of ruby over 10 px of base: 7 px a side on the left, 8 on the right. と and を take 5 each (half of 10);
  // the rest, 2 and 3, push them away: 志 widens to 15 and is drawn 2 px in, under the ruby's centre.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と志を", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "こころざし"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 12, 25}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{5, 25}}));
}

TEST(RubyWidths, ANarrowedMarkLendsHalfOfItsNarrowedWidth) {
  // 」 before 。 is 5 px wide (half), so it takes 2 px of the ruby's 8 on the right; と takes 5 of the 7 on the left.
  // 煙 widens by 2 + 6 to 18 and is drawn 2 px in; the ruby spans 5..30, 2 px over 」 at 28.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と煙」。", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "こころざし"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0].words, (std::vector<std::string>{"と", "煙", "」", "。"}));
  EXPECT_EQ(lines[0].xpos, (X{0, 12, 28, 33}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{5, 25}}));
}

TEST(RubyWidths, ARubyOverANonCjkBaseKeepsTheBaseTogether) {
  // "tobacco" is not CJK: the 15 px the ruby is longer (7 left, 8 right, both beside kanji) widen the base as one
  // block, 煙 drawn 7 px in and 草 right after it, with no 1:2:1 gap between them.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("漢煙草漢", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 2, "tobacco"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 17, 27, 45}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{10, 35}}));
}

TEST(RubyWidths, RubyFollowsTheWordsSize) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と煙を", EpdFontFamily::REGULAR, false, false, 200);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "けむり"));
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (X{0, 20, 40}));
  EXPECT_EQ(boxesOf(lines[0]), (Boxes{{15, 30}}));
}

TEST(RubyWidths, TheRubyIsReadiedWithItsWords) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("と煙を", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(1, 1, "けむり"));
  GfxRenderer renderer;
  text.layoutAndExtractLines(renderer, kFontId, 400, [](std::unique_ptr<TextBlock>, bool, bool) {
    return ParsedText::LineProcessResult::Accepted;
  });
  ASSERT_FALSE(renderer.readyCalls.empty());
  EXPECT_EQ(renderer.readyCalls[0].text, "と煙を けむり-");
}

// A group at a line edge keeps its overhang inside the margins: what the edge cannot take is reserved in the line.
TEST(RubyEdges, AGroupStartingALineKeepsItsRubyInside) {
  for (const bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
    text.addWord("あああと煙を", EpdFontFamily::REGULAR);
    ASSERT_TRUE(text.addRubyGroup(4, 1, "けむり"));
    const auto lines = layout(text, 40);
    ASSERT_EQ(wordsOf(lines), (Words{{"あ", "あ", "あ", "と"}, {"煙", "を"}})) << hyphenation;
    // 煙 moves 2 px in, so its ruby's left overhang starts at the margin.
    EXPECT_EQ(lines[1].xpos, (X{2, 12})) << hyphenation;
    EXPECT_EQ(boxesOf(lines[1]), (Boxes{{0, 15}})) << hyphenation;
  }
}

TEST(RubyEdges, AGroupEndingALineKeepsItsRubyInside) {
  for (const bool hyphenation : {false, true}) {
    {
      // あああ煙 is 40 px, its ruby 3 px past 煙: 43 px do not fit in 42.
      ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
      text.addWord("あああ煙を", EpdFontFamily::REGULAR);
      ASSERT_TRUE(text.addRubyGroup(3, 1, "けむり"));
      EXPECT_EQ(wordsOf(layout(text, 42)), (Words{{"あ", "あ", "あ"}, {"煙", "を"}})) << hyphenation;
    }
    {
      ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
      text.addWord("あああ煙を", EpdFontFamily::REGULAR);
      ASSERT_TRUE(text.addRubyGroup(3, 1, "けむり"));
      const auto lines = layout(text, 43);
      ASSERT_EQ(wordsOf(lines), (Words{{"あ", "あ", "あ", "煙"}, {"を"}})) << hyphenation;
      EXPECT_EQ(boxesOf(lines[0]), (Boxes{{28, 15}})) << hyphenation;
    }
  }
}

TEST(RubyEdges, AJustifiedLineEndingInAGroupEndsFlushWithItsRuby) {
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  text.addWord("あああ煙をを", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(3, 1, "けむり"));
  const auto lines = layout(text, 47);
  ASSERT_GE(lines.size(), 1u);
  ASSERT_EQ(lines[0].words, (std::vector<std::string>{"あ", "あ", "あ", "煙"}));
  ASSERT_EQ(lines[0].ruby.size(), 1u);
  EXPECT_EQ(lines[0].ruby[0].x + lines[0].ruby[0].w, 47);
  // The whole ruby ends there, not only its base: 煙 sits 3 px in from the margin.
  EXPECT_EQ(lines[0].ruby[0].w, 15);
  EXPECT_EQ(lines[0].xpos, (X{0, 11, 22, 34}));
}

TEST(RubyEdges, APageBottomRetryKeepsTheRubyInside) {
  // The retry re-lays a hyphenated line with its words whole: it merges tokens back and measures again, so the
  // ruby's reserves must stay in step with the tokens. 아あああ煙 is 55 px; with 煙's ruby 3 px past it the line
  // ends before 煙, and the next line starts at it.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Justify));
  for (const char* word : {"가나", "다라마바사아", "あああ煙を"}) text.addWord(word, EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(5, 1, "けむり"));
  bool retried = false;
  const auto lines = layoutRetryingOnce(text, 55, retried);
  ASSERT_TRUE(retried);
  ASSERT_EQ(wordsOf(lines), (Words{{"가나"}, {"다라마바사"}, {"아", "あ", "あ", "あ"}, {"煙", "を"}}));
  EXPECT_EQ(lines[3].xpos, (X{2, 12}));
  EXPECT_EQ(boxesOf(lines[3]), (Boxes{{0, 15}}));
}

TEST(RubyEdges, ARetriedLineStartingWithAGroupHoldsItsReserve) {
  // Line 2 is retried whole (the page-bottom breaker): 煙 is 13 px (3 px pushed on its right, before the space) and
  // holds its ruby's 2 px left overhang, so 煙 다라마바사아 is 2 + 13 + 5 + 60 = 80 px: it does not fit in 78.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Left));
  for (const char* word : {"ああああああと煙", "다라마바사아"}) text.addWord(word, EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(7, 1, "けむり"));
  bool retried = false;
  const auto lines = layoutRetryingOnce(text, 78, retried);
  ASSERT_TRUE(retried);
  ASSERT_EQ(wordsOf(lines), (Words{{"あ", "あ", "あ", "あ", "あ", "あ", "と"}, {"煙"}, {"다라마바사아"}}));
  EXPECT_EQ(lines[1].xpos, (X{2}));
  EXPECT_EQ(boxesOf(lines[1]), (Boxes{{0, 15}}));
}

TEST(RubyEdges, AWordHyphenatedAfterARetryStaysInsideToo) {
  // After the retried first line, the rest is broken again from there (the suffix breaker). 煙 starts line 4 holding
  // its ruby's 2 px left overhang, 13 px wide: the word split at that line's end has 58 - 2 - 13 - 5 = 38 px left,
  // not 40, so 다라마 ends it at 50 px rather than 다라마바 at 60.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Left));
  for (const char* word : {"가나", "다라마바사아", "ああああ煙", "다라마바사아"})
    text.addWord(word, EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(6, 1, "けむり"));
  bool retried = false;
  const auto lines = layoutRetryingOnce(text, 58, retried);
  ASSERT_TRUE(retried);
  ASSERT_EQ(wordsOf(lines),
            (Words{{"가나"}, {"다라마바사"}, {"아", "あ", "あ", "あ", "あ"}, {"煙", "다라마"}, {"바사아"}}));
  EXPECT_EQ(lines[3].xpos, (X{2, 20}));
  EXPECT_EQ(boxesOf(lines[3]), (Boxes{{0, 15}}));
}

TEST(RubyEdges, AWordHyphenatedAfterAGroupStartingALineStaysInside) {
  // 煙's ruby passes it by 2 px on the left, held inside the line where 煙 starts line 2, and 3 on the right, a push
  // (a space follows): 煙 is 13 px wide. The word split at that line's end has 78 - 2 - 13 - 5 = 58 px left, not 60:
  // "inter-" no longer fits.
  Hyphenator::setPreferredLanguage("en");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Left));
  text.addWord("ああああああと煙", EpdFontFamily::REGULAR);
  text.addWord("internationalization", EpdFontFamily::REGULAR);
  ASSERT_TRUE(text.addRubyGroup(7, 1, "けむり"));
  const auto lines = layout(text, 78);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[1].words, (std::vector<std::string>{"煙"}));
  EXPECT_EQ(lines[1].xpos, (X{2}));
  EXPECT_EQ(boxesOf(lines[1]), (Boxes{{0, 15}}));
  for (const Line& line : lines) {
    const int right = line.xpos.back() + GfxRenderer::countCodepoints(line.words.back().c_str()) * GfxRenderer::GLYPH_W;
    EXPECT_LE(right, 78) << line.words.back();
  }
}

// Push-in takes nothing a ruby needs: a mark between two groups that overhang it keeps their overhangs.
TEST(RubyPushIn, AMarkUnderTwoOverhangsGivesNothing) {
  // Each 20 px ruby passes its 10 px base by 5 px a side. Between the groups both overhang 、 by 5 px, half of its
  // 10 px; elsewhere they push: the first 字 widens to 15 and is drawn 5 px in (the line start), the second to 15
  // (字 follows). 字、字字字 is 60 px, 70 with the last 字. 、 could give its 5 px mid-line blank and make that 65,
  // but the two overhangs fill it: pushed in, the rubies would lie 5 px over each other. So the line breaks before
  // the last 字, and the boxes meet at 20 (Simplified Chinese table, every book's).
  for (const bool hyphenation : {false, true}) {
    ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
    text.addWord("字、字字字字", EpdFontFamily::REGULAR);
    ASSERT_TRUE(text.addRubyGroup(0, 1, "かかかか"));
    ASSERT_TRUE(text.addRubyGroup(2, 1, "かかかか"));
    const auto lines = layout(text, 65);
    ASSERT_EQ(wordsOf(lines), (Words{{"字", "、", "字", "字", "字"}, {"字"}})) << hyphenation;
    EXPECT_EQ(lines[0].xpos, (X{5, 15, 25, 40, 50})) << hyphenation;
    EXPECT_EQ(boxesOf(lines[0]), (Boxes{{0, 20}, {20, 20}})) << hyphenation;
  }
}
// A mark inside a group gives nothing, mid-line or at the line end: the group's ruby box spans its base at the widths
// the line holds without push-in. Each ruby here is か, 5 px over a 20 px base, so no base widens.
TEST(RubyPushIn, AMarkInsideAGroupGivesNothing) {
  for (const bool hyphenation : {false, true}) {
    {
      // Simplified Chinese 字、字字字。 fits 50 px only with 、 at half (CjkPushIn.AMidLineMarkGivesWay...). In the
      // group over 字、, 、 keeps its 10 px, and the line breaks before 字。.
      ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
      text.addWord("字、字字字。字字", EpdFontFamily::REGULAR);
      ASSERT_TRUE(text.addRubyGroup(0, 2, "か"));
      const auto lines = layout(text, 50);
      ASSERT_EQ(wordsOf(lines), (Words{{"字", "、", "字", "字"}, {"字", "。", "字", "字"}})) << hyphenation;
      EXPECT_EQ(lines[0].xpos, (X{0, 10, 20, 30})) << hyphenation;
      EXPECT_EQ(boxesOf(lines[0]), (Boxes{{0, 20}})) << hyphenation;
    }
    {
      // Japanese 字字字字。 fits 45 px only with the line-end 。 at half (CjkPushIn.ALineEndMarkHalves...). In the
      // group over 字。 it stays full, and the line breaks before the group.
      TypographyProfile japanese = TypographyProfile::forLanguage("ja");
      japanese.wholeEmGrid = false;  // a 45 px line, not the 40 of whole ems
      ParsedText text(false, hyphenation, aligned(CssTextAlign::Left));
      text.setTypography(japanese);
      text.addWord("字字字字。字字", EpdFontFamily::REGULAR);
      ASSERT_TRUE(text.addRubyGroup(3, 2, "か"));
      const auto lines = layout(text, 45);
      ASSERT_EQ(wordsOf(lines), (Words{{"字", "字", "字"}, {"字", "。", "字", "字"}})) << hyphenation;
      EXPECT_EQ(boxesOf(lines[1]), (Boxes{{0, 20}})) << hyphenation;
    }
  }
}
