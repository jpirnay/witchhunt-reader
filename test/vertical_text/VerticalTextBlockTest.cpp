// A TextBlock line of a vertical-rl page (spec 5): each word runs down its column, drawn in the turned frame. Against
// the fixed-width shim of test/word_size_layout: every codepoint 10 px, ascender 16.
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "AllocationCounter.h"
#include "Epub/TextEmphasis.h"
#include "Epub/TurnedFrame.h"
#include "Epub/blocks/TextBlock.h"

namespace {

constexpr int kFontId = 1;
using Kind = TextBlock::AnnotationKind;

// Portrait held, drawing in its turned frame as Page::render leaves the renderer.
void turnedPortrait(GfxRenderer& r) {
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
}

// A line of `words` at `xpos`, all in `style`, with the annotations given.
std::unique_ptr<TextBlock> line(std::vector<std::string> words, std::vector<int16_t> xpos,
                                const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                std::vector<TextBlock::Annotation> annotations = {}, const bool vertical = true) {
  const std::vector<EpdFontFamily::Style> styles(words.size(), style);
  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &styles;
  range.annotations = annotations.empty() ? nullptr : &annotations;
  range.first = 0;
  range.count = words.size();

  BlockStyle blockStyle;
  blockStyle.alignment = CssTextAlign::Left;
  blockStyle.textIndent = 0;
  blockStyle.textIndentDefined = true;
  std::unique_ptr<TextBlock> block(new TextBlock(range, xpos, blockStyle));
  block->setVertical(vertical);
  return block;
}

TextBlock::Annotation ruby(const uint16_t firstWord, const uint16_t wordCount, const int16_t x, const uint16_t width,
                           const std::string_view text) {
  TextBlock::Annotation a;
  a.firstWord = firstWord;
  a.wordCount = wordCount;
  a.x = x;
  a.width = width;
  a.kind = Kind::Ruby;
  a.text = text;
  return a;
}

TextBlock::Annotation sesame(const uint16_t firstWord, const uint16_t wordCount) {
  TextBlock::Annotation a;
  a.firstWord = firstWord;
  a.wordCount = wordCount;
  a.kind = Kind::Emphasis;
  a.mark = TextEmphasis::Sesame;
  return a;
}

}  // namespace

// em 10, ascender 16: baseline 100 + 16, axis 116 - 4 = 112, upright x = 479 - 112 - 5 = 362, y = pen + 9 - 16.
TEST(VerticalTextBlock, EveryWordRunsDownItsColumn) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block = line({"日", "本", "Latin"}, {0, 10, 20});
  EXPECT_TRUE(block->isVertical());
  block->render(r, kFontId, 50, 100);
  ASSERT_EQ(3u, r.drawCalls.size());
  EXPECT_EQ("日", r.drawCalls[0].text);
  EXPECT_EQ(GfxRenderer::Portrait, r.drawCalls[0].frame);
  EXPECT_EQ(362, r.drawCalls[0].x);
  EXPECT_EQ(43, r.drawCalls[0].y);
  EXPECT_EQ("本", r.drawCalls[1].text);
  EXPECT_EQ(GfxRenderer::Portrait, r.drawCalls[1].frame);
  EXPECT_EQ(362, r.drawCalls[1].x);
  EXPECT_EQ(53, r.drawCalls[1].y);
  EXPECT_EQ("Latin", r.drawCalls[2].text);
  EXPECT_EQ(GfxRenderer::LandscapeCounterClockwise, r.drawCalls[2].frame);
  EXPECT_EQ(70, r.drawCalls[2].x);
  EXPECT_EQ(100, r.drawCalls[2].y);
}

TEST(VerticalTextBlock, RubyAndMarksSitRightOfTheColumn) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto withRuby = line({"漢", "字"}, {0, 10}, EpdFontFamily::REGULAR, {ruby(0, 2, 0, 20, "かんじ")});
  withRuby->render(r, kFontId, 50, 100);
  int baseX = 0;
  int rubyX = 0;
  int rubyCount = 0;
  for (const auto& call : r.drawCalls) {
    EXPECT_EQ(GfxRenderer::Portrait, call.frame) << call.text;
    if (call.text == "漢") baseX = call.x;
    if (call.text == "か") rubyX = call.x;
    rubyCount += call.text != "漢" && call.text != "字";
  }
  EXPECT_EQ(3, rubyCount);
  EXPECT_EQ(354, baseX);
  EXPECT_EQ(371, rubyX);
  for (const auto& call : r.drawCalls) {
    if (call.text != "漢" && call.text != "字") EXPECT_GT(call.x, baseX) << call.text;
  }

  GfxRenderer m;
  turnedPortrait(m);
  const auto withMarks = line({"漢", "字"}, {0, 10}, EpdFontFamily::REGULAR, {sesame(0, 2)});
  withMarks->render(m, kFontId, 50, 100);
  int marks = 0;
  for (const auto& call : m.drawCalls) {
    EXPECT_EQ(GfxRenderer::Portrait, call.frame) << call.text;
    if (call.text == "漢" || call.text == "字") continue;
    ++marks;
    EXPECT_GT(call.x, 354) << call.text;
  }
  EXPECT_EQ(2, marks);
}

// Baseline 100 + 16 = 116, over edge 116 - (880 / 100 rounded: 9) = 107; the side line lies 2 px off it.
TEST(VerticalTextBlock, TheSideLineIsOnTheOverSide) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto underlined = line({"日本"}, {0}, EpdFontFamily::UNDERLINE);
  underlined->render(r, kFontId, 50, 100);
  ASSERT_EQ(1u, r.drawLineCalls.size());
  EXPECT_EQ(50, r.drawLineCalls[0].x1);
  EXPECT_EQ(105, r.drawLineCalls[0].y1);
  EXPECT_EQ(70, r.drawLineCalls[0].x2);
  EXPECT_EQ(105, r.drawLineCalls[0].y2);
  EXPECT_EQ(GfxRenderer::LandscapeCounterClockwise, r.drawLineCalls[0].frame);

  GfxRenderer s;
  turnedPortrait(s);
  const auto struck = line({"日本"}, {0}, EpdFontFamily::STRIKETHROUGH);
  struck->render(s, kFontId, 50, 100);
  ASSERT_EQ(1u, s.drawLineCalls.size());
  EXPECT_EQ(112, s.drawLineCalls[0].y1);
  EXPECT_EQ(50, s.drawLineCalls[0].x1);
  EXPECT_EQ(70, s.drawLineCalls[0].x2);
}

TEST(VerticalTextBlock, AWordBoxRunsDownTheColumn) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block = line({"Latin日"}, {0});
  EXPECT_EQ(60, block->wordBox(r, 0, kFontId, 50, 100).width);
}

TEST(VerticalTextBlock, DrawingAColumnAllocatesNothing) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block =
      line({"漢", "字", "日本"}, {0, 10, 20}, EpdFontFamily::REGULAR, {ruby(0, 2, 0, 20, "かんじ"), sesame(2, 1)});
  const auto underlined = line({"日本"}, {0}, EpdFontFamily::UNDERLINE);
  r.drawCalls.reserve(64);
  r.drawLineCalls.reserve(8);
  r.fillRectCalls.reserve(8);
  const long live = liveAllocations();
  const long all = allocations();
  block->render(r, kFontId, 50, 100);
  underlined->render(r, kFontId, 50, 200);
  EXPECT_EQ(all, allocations());
  EXPECT_EQ(live, liveAllocations());
}

TEST(VerticalTextBlock, AHorizontalLineDrawsAsBefore) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  const auto block = line({"日", "本", "Latin"}, {0, 10, 20}, EpdFontFamily::REGULAR, {}, false);
  EXPECT_FALSE(block->isVertical());
  block->render(r, kFontId, 50, 100);
  ASSERT_EQ(3u, r.drawCalls.size());
  const int expectedX[] = {50, 60, 70};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(GfxRenderer::Portrait, r.drawCalls[i].frame);
    EXPECT_EQ(expectedX[i], r.drawCalls[i].x);
    EXPECT_EQ(100, r.drawCalls[i].y);
  }
}

// Bit 2 of the flags byte (the third byte, after the u16 word count).
TEST(VerticalTextBlock, TheVerticalBitSurvivesTheCache) {
  for (const bool vertical : {true, false}) {
    const auto block = line({"日", "本"}, {0, 10}, EpdFontFamily::REGULAR, {}, vertical);
    FsFile file = HalFile::forReadWrite();
    ASSERT_TRUE(block->serialize(file));
    ASSERT_TRUE(file.seek(0));
    std::string bytes(file.size(), '\0');
    file.read(bytes.data(), bytes.size());
    EXPECT_EQ(vertical ? 4 : 0, bytes[2]);

    FsFile again = HalFile::forReadWrite();
    again.write(bytes.data(), bytes.size());
    ASSERT_TRUE(again.seek(0));
    const auto restored = TextBlock::deserialize(again);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(vertical, restored->isVertical());
    EXPECT_EQ(2, restored->wordCount());
  }
}

// The centred-marks bit is bit 3 of the flags byte. A line without it, as the code before wrote it, reads false.
TEST(VerticalTextBlock, TheCentredMarksBitSurvivesTheCache) {
  for (const bool centred : {true, false}) {
    const auto block = line({"日", "本"}, {0, 10});
    block->setCentredMarks(centred);
    FsFile file = HalFile::forReadWrite();
    ASSERT_TRUE(block->serialize(file));
    ASSERT_TRUE(file.seek(0));
    std::string bytes(file.size(), '\0');
    file.read(bytes.data(), bytes.size());
    EXPECT_EQ(centred ? 12 : 4, bytes[2]);

    FsFile again = HalFile::forReadWrite();
    again.write(bytes.data(), bytes.size());
    ASSERT_TRUE(again.seek(0));
    const auto restored = TextBlock::deserialize(again);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(centred, restored->centredMarks());
    EXPECT_TRUE(restored->isVertical());
  }
}

// Measured on the host before the bit: sizeof(TextBlock) == 88 (the firmware's 32-bit pin is 52).
TEST(VerticalTextBlock, TheCentredMarksBitDoesNotGrowTheBlock) { EXPECT_EQ(88u, sizeof(TextBlock)); }

namespace {
TextBlock::Annotation combine(const uint16_t word, const uint16_t width) {
  TextBlock::Annotation a;
  a.firstWord = word;
  a.wordCount = 1;
  a.width = width;
  a.kind = Kind::Combine;
  return a;
}
}  // namespace

// Word 1 "13" is combined: one em in the column (10), drawn as one scaled upright block in the reader's frame.
TEST(VerticalTextBlock, ACombinedWordDrawsAsOneUprightBlock) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block = line({"日", "13", "本"}, {0, 10, 20}, EpdFontFamily::REGULAR, {combine(1, 10)});
  EXPECT_EQ(10, block->wordBox(r, 1, kFontId, 50, 100).width);
  EXPECT_TRUE(block->isCombined(1));
  EXPECT_FALSE(block->isCombined(0));
  block->render(r, kFontId, 50, 100);
  ASSERT_EQ(3u, r.drawCalls.size());
  EXPECT_EQ("13", r.drawCalls[1].text);
  EXPECT_EQ(GfxRenderer::Portrait, r.drawCalls[1].frame);
  EXPECT_EQ(0.5f, r.drawCalls[1].scale);
}

TEST(VerticalTextBlock, ACombineRecordReservesNoBand) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block = line({"日", "13"}, {0, 10}, EpdFontFamily::REGULAR, {combine(1, 10)});
  EXPECT_EQ(0, block->annotationBand(r, kFontId));
}

TEST(VerticalTextBlock, ARubyLineStillReservesItsBand) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block = line({"漢", "13"}, {0, 10}, EpdFontFamily::REGULAR, {ruby(0, 1, 0, 10, "かん"), combine(1, 10)});
  EXPECT_EQ(8, block->annotationBand(r, kFontId));
}

// Sesame over words 0-2, "13" (word 1) combined, ruby on word 0. Em 10, the mark at half size: advance 5. The pen runs
// down the column from y = 50: the cells are [50, 60), [60, 70), [70, 80). A mark is centred in its cell, so its pen
// starts at cell + (10 - 5) / 2 = 52, 62, 72, and the shim's upright draw of a 0.5 run lands 4 above it: y = 48,
// 58, 68. Word 1 gets ONE mark (y = 58, the middle of its cell), not one per digit. All sit in the ruby's band, x =
// 371.
TEST(VerticalTextBlock, ACombinedWordTakesOneCentredEmphasisMark) {
  GfxRenderer r;
  turnedPortrait(r);
  const auto block = line({"日", "13", "本"}, {0, 10, 20}, EpdFontFamily::REGULAR,
                          {sesame(0, 3), ruby(0, 1, 0, 10, "かん"), combine(1, 10)});
  EXPECT_EQ(8, block->annotationBand(r, kFontId));
  block->render(r, kFontId, 50, 100);
  std::vector<int> markY;
  for (const auto& call : r.drawCalls) {
    if (call.text != "﹅") continue;  // U+FE45, the sesame dot
    EXPECT_EQ(371, call.x);
    markY.push_back(call.y);
  }
  EXPECT_EQ((std::vector<int>{48, 58, 68}), markY);
}

TEST(VerticalTextBlock, TheCombineRecordSurvivesTheCache) {
  const auto block = line({"日", "13"}, {0, 10}, EpdFontFamily::REGULAR, {combine(1, 10)});
  FsFile file = HalFile::forReadWrite();
  ASSERT_TRUE(block->serialize(file));
  ASSERT_TRUE(file.seek(0));
  std::string bytes(file.size(), '\0');
  file.read(bytes.data(), bytes.size());
  FsFile again = HalFile::forReadWrite();
  again.write(bytes.data(), bytes.size());
  ASSERT_TRUE(again.seek(0));
  const auto restored = TextBlock::deserialize(again);
  ASSERT_NE(nullptr, restored);
  EXPECT_TRUE(restored->isCombined(1));
  EXPECT_FALSE(restored->isCombined(0));
  GfxRenderer r;
  turnedPortrait(r);
  EXPECT_EQ(0, restored->annotationBand(r, kFontId));
}
