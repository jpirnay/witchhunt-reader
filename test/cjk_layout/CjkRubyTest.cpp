// Ruby on a TextBlock line (spec section 7): the boxes a line carries in its arena behind bit 1 of the flags byte, the
// band above the base they are drawn in, and the disk format. Against the fixed-width shim of test/word_size_layout:
// every codepoint 10 px, ascender 16.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Epub/blocks/TextBlock.h"
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
