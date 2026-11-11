// InlineObjectLayoutTest.cpp
//
// Inline objects (ParsedText::addInlineObject, the tokens an inline image is laid out as), on the real ParsedText and
// TextBlock against the fixed-width renderer shim:
//
//   - an object takes its width in the line, breaks like a word and takes its share of a justified line
//   - glued to text it stays glued; two glued objects, or an object beside CJK, may break
//   - it is never hyphenated or split, and kerns with nothing
//   - its line hands it over in lineObjects(), with its id and x, and the line's TextBlock holds the text alone: no
//     U+FFFC to draw, read, select or look up, and a word glued to another only through the object
//   - in a right-to-left paragraph it is ordered as a neutral; the line's reading order closes up over it
//   - a ruby box on its line still names its base; bionic reading leaves it alone
//   - a paragraph laid out in passes keeps each object's id

#include <Utf8.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;
constexpr auto kRegular = EpdFontFamily::REGULAR;
constexpr int kGlyph = GfxRenderer::GLYPH_W;
constexpr int kSpace = GfxRenderer::SPACE_W;

BlockStyle aligned(const CssTextAlign alignment) {
  BlockStyle bs;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  bs.alignment = alignment;
  return bs;
}

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<bool> continues;
  std::vector<ParsedText::LineObject> objects;
  std::unique_ptr<TextBlock> block;
};

std::vector<Line> layout(ParsedText& text, const uint16_t width, const bool includeLastLine = true,
                         std::vector<std::vector<ParsedText::LineObject>>* hookObjects = nullptr) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  if (hookObjects != nullptr) {
    text.setBeforeLineHook([&text, hookObjects](uint8_t, bool) {
      const auto objects = text.lineObjects();
      hookObjects->emplace_back(objects.begin(), objects.end());
    });
  }
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line line;
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
          line.continues.push_back(block->wordContinues(i));
        }
        const auto objects = text.lineObjects();
        line.objects.assign(objects.begin(), objects.end());
        line.block = std::move(block);
        lines.push_back(std::move(line));
        return ParsedText::LineProcessResult::Accepted;
      },
      includeLastLine);
  return lines;
}

using Words = std::vector<std::string>;

}  // namespace

TEST(InlineObjects, AnObjectTakesItsWidthBetweenTheWords) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addInlineObject(7, 30, /*attachToPrevious=*/false, 0);
  text.addWord("bb", kRegular);
  const auto lines = layout(text, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ((Words{"aa", "bb"}), lines[0].words);
  EXPECT_EQ(0, lines[0].xpos[0]);
  EXPECT_EQ(2 * kGlyph + kSpace + 30 + kSpace, lines[0].xpos[1]);
  ASSERT_EQ(1u, lines[0].objects.size());
  EXPECT_EQ(7, lines[0].objects[0].id);
  EXPECT_EQ(2 * kGlyph + kSpace, lines[0].objects[0].x);
}

TEST(InlineObjects, TheLineTextHoldsNoObjectCharacter) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addInlineObject(1, 20, false, 0);
  text.addWord("bb", kRegular);
  const auto lines = layout(text, 400);
  ASSERT_EQ(1u, lines.size());
  std::string reading;
  lines[0].block->appendReadingText(reading);
  EXPECT_EQ("aa bb", reading);
  EXPECT_EQ(std::string::npos, reading.find("\xEF\xBF\xBC"));
}

TEST(InlineObjects, AnObjectWrapsLikeAWord) {
  // aa_bb_ is 50 px; the 40 px object does not fit a 70 px line after it and starts the next.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addWord("bb", kRegular);
  text.addInlineObject(3, 40, false, 0);
  text.addWord("cc", kRegular);
  const auto lines = layout(text, 70);
  ASSERT_EQ(2u, lines.size());
  EXPECT_EQ((Words{"aa", "bb"}), lines[0].words);
  EXPECT_TRUE(lines[0].objects.empty());
  EXPECT_EQ((Words{"cc"}), lines[1].words);
  ASSERT_EQ(1u, lines[1].objects.size());
  EXPECT_EQ(3, lines[1].objects[0].id);
  EXPECT_EQ(0, lines[1].objects[0].x);
  EXPECT_EQ(40 + kSpace, lines[1].xpos[0]);
}

TEST(InlineObjects, AnObjectTakesItsShareOfAJustifiedLine) {
  // aa [20] bb | cccccccc: the first line's two gaps share what its 70 px leave of 90.
  ParsedText text(false, false, aligned(CssTextAlign::Justify));
  text.addWord("aa", kRegular);
  text.addInlineObject(1, 20, false, 0);
  text.addWord("bb", kRegular);
  text.addWord("cccccccc", kRegular);
  const auto lines = layout(text, 90);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(1u, lines[0].objects.size());
  const int share = (90 - (2 * kGlyph + 20 + 2 * kGlyph + 2 * kSpace)) / 2;
  EXPECT_EQ(2 * kGlyph + kSpace + share, lines[0].objects[0].x);
  EXPECT_EQ(2 * kGlyph + 20 + 2 * (kSpace + share), lines[0].xpos[1]);
  EXPECT_EQ(90, lines[0].xpos[1] + 2 * kGlyph);  // flush right
}

TEST(InlineObjects, AnObjectGluedToTextStaysWithIt) {
  // aaaa a<obj>b: the glued run is 40 px and goes to the next line whole rather than breaking at the object.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aaaa", kRegular);
  text.addWord("a", kRegular);
  text.addInlineObject(1, 20, /*attachToPrevious=*/true, 0);
  text.addWord("b", kRegular, false, /*attachToPrevious=*/true);
  const auto lines = layout(text, 60);
  ASSERT_EQ(2u, lines.size());
  EXPECT_EQ((Words{"aaaa"}), lines[0].words);
  EXPECT_EQ((Words{"a", "b"}), lines[1].words);
  ASSERT_EQ(1u, lines[1].objects.size());
  EXPECT_EQ(kGlyph, lines[1].objects[0].x);
  EXPECT_EQ(kGlyph + 20, lines[1].xpos[1]);
  // b is glued to a through the object: one word to read, select and look up.
  EXPECT_TRUE(lines[1].continues[1]);
}

TEST(InlineObjects, AWordAfterASpacedObjectKeepsItsSpace) {
  // aa <obj>bb: bb is glued to the object, the object is not glued to aa, so aa and bb are two words.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addInlineObject(1, 20, /*attachToPrevious=*/false, 0);
  text.addWord("bb", kRegular, false, /*attachToPrevious=*/true);
  const auto lines = layout(text, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0].continues[1]);
  EXPECT_EQ(2 * kGlyph + kSpace + 20, lines[0].xpos[1]);
  std::string reading;
  lines[0].block->appendReadingText(reading);
  EXPECT_EQ("aa bb", reading);
}

TEST(InlineObjects, ALineMayEndBetweenTwoGluedObjects) {
  // Four glued 20 px objects in a 50 px line: two to a line, as a run of emoji images breaks.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  for (uint16_t id = 1; id <= 4; ++id) text.addInlineObject(id, 20, /*attachToPrevious=*/id > 1, 0);
  const auto lines = layout(text, 50);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(2u, lines[0].objects.size());
  ASSERT_EQ(2u, lines[1].objects.size());
  EXPECT_EQ(1, lines[0].objects[0].id);
  EXPECT_EQ(0, lines[0].objects[0].x);
  EXPECT_EQ(20, lines[0].objects[1].x);  // glued: no gap
  EXPECT_EQ(3, lines[1].objects[0].id);
  // A line of objects alone holds no word.
  EXPECT_TRUE(lines[0].words.empty());
  ASSERT_NE(nullptr, lines[0].block);
  EXPECT_TRUE(lines[0].block->valid());
}

TEST(InlineObjects, AnObjectBesideCjkBreaksAsACjkCharacterWould) {
  // 字字字<obj> glued: a 35 px line ends before the object, as it may between two CJK characters.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("字字字", kRegular);
  text.addInlineObject(1, 20, /*attachToPrevious=*/true, 0);
  const auto lines = layout(text, 35);
  ASSERT_EQ(2u, lines.size());
  EXPECT_EQ((Words{"字", "字", "字"}), lines[0].words);
  EXPECT_TRUE(lines[0].objects.empty());
  ASSERT_EQ(1u, lines[1].objects.size());
  EXPECT_EQ(0, lines[1].objects[0].x);
  EXPECT_TRUE(lines[1].words.empty());
}

TEST(InlineObjects, CjkAfterAnObjectBreaksAsCjkDoes) {
  // <obj>字字字 glued: the line may end after the object.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addInlineObject(1, 20, false, 0);
  text.addWord("字字字", kRegular, false, /*attachToPrevious=*/true);
  const auto lines = layout(text, 35);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(1u, lines[0].objects.size());
  EXPECT_EQ((Words{"字"}), lines[0].words);
  EXPECT_EQ(20, lines[0].xpos[0]);
  EXPECT_EQ((Words{"字", "字"}), lines[1].words);
}

TEST(InlineObjects, AnObjectIsNeverHyphenatedOrSplit) {
  // Wider than the line: it stands alone on its line, whole, with no hyphen beside it.
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addInlineObject(1, 120, false, 0);
  text.addWord("bb", kRegular);
  const auto lines = layout(text, 100);
  ASSERT_EQ(3u, lines.size());
  EXPECT_EQ((Words{"aa"}), lines[0].words);
  EXPECT_TRUE(lines[1].words.empty());
  ASSERT_EQ(1u, lines[1].objects.size());
  EXPECT_EQ(0, lines[1].objects[0].x);
  EXPECT_EQ((Words{"bb"}), lines[2].words);
}

TEST(InlineObjects, AnObjectKernsWithNothing) {
  // Every pair with a space kerns -1 px, but one with nothing on its other side (an object's).
  GfxRenderer::spaceKernFP = [](int, const uint32_t left, const uint32_t right, EpdFontFamily::Style) {
    return left != 0 && right != 0 ? int32_t{-16} : int32_t{0};
  };
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addInlineObject(1, 20, false, 0);
  text.addWord("bb", kRegular);
  const auto lines = layout(text, 400);
  GfxRenderer::spaceKernFP = nullptr;
  ASSERT_EQ(1u, lines.size());
  // Each space beside the object kerns on its word's side only.
  ASSERT_EQ(1u, lines[0].objects.size());
  EXPECT_EQ(2 * kGlyph + kSpace - 1, lines[0].objects[0].x);
  EXPECT_EQ(2 * kGlyph + kSpace - 1 + 20 + kSpace - 1, lines[0].xpos[1]);
}

TEST(InlineObjects, TheBeforeLineHookSeesTheLinesObjects) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aaaa", kRegular);
  text.addInlineObject(5, 30, false, 0);
  text.addWord("bbbbbb", kRegular);
  std::vector<std::vector<ParsedText::LineObject>> seen;
  const auto lines = layout(text, 80, true, &seen);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(2u, seen.size());
  ASSERT_EQ(1u, seen[0].size());
  EXPECT_EQ(5, seen[0][0].id);
  EXPECT_TRUE(seen[1].empty());
}

TEST(InlineObjects, ALaterPassKeepsTheObjectsIds) {
  // A long block laid out in parts: the kept last line's object is handed over by the next pass, with its id.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aaaa", kRegular);
  text.addInlineObject(1, 20, false, 0);
  text.addWord("bbbb", kRegular);
  text.addInlineObject(2, 20, false, 0);
  auto first = layout(text, 70, /*includeLastLine=*/false);
  ASSERT_EQ(1u, first.size());
  ASSERT_EQ(1u, first[0].objects.size());
  EXPECT_EQ(1, first[0].objects[0].id);
  text.addWord("cc", kRegular);
  auto rest = layout(text, 70);
  ASSERT_EQ(2u, rest.size());
  ASSERT_EQ(1u, rest[0].objects.size());
  EXPECT_EQ(2, rest[0].objects[0].id);
  EXPECT_EQ(4 * kGlyph + kSpace, rest[0].objects[0].x);
  EXPECT_TRUE(rest[1].objects.empty());
}

TEST(InlineObjects, ARightToLeftLineOrdersTheObjectAsANeutral) {
  // שלום <obj> עולם in a right-to-left paragraph: the object sits between the two words, the first word rightmost.
  BlockStyle style = aligned(CssTextAlign::Right);
  style.direction = CssDirection::Rtl;
  ParsedText text(false, false, style);
  const std::string shalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
  const std::string olam = "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";
  text.addWord(shalom, kRegular);
  text.addInlineObject(9, 30, false, 0);
  text.addWord(olam, kRegular);
  const auto lines = layout(text, 200);
  ASSERT_EQ(1u, lines.size());
  const Line& line = lines[0];
  ASSERT_EQ(2u, line.words.size());
  ASSERT_EQ(1u, line.objects.size());
  // Visual order left to right: עולם, the object, שלום; right-aligned in 200 px.
  const int content = 4 * kGlyph + kSpace + 30 + kSpace + 4 * kGlyph;
  EXPECT_EQ(olam, line.words[0]);
  EXPECT_EQ(shalom, line.words[1]);
  EXPECT_EQ(200 - content, line.xpos[0]);
  EXPECT_EQ(200 - content + 4 * kGlyph + kSpace, line.objects[0].x);
  EXPECT_EQ(200 - content + 4 * kGlyph + kSpace + 30 + kSpace, line.xpos[1]);
  // Read in its order: שלום first.
  std::string reading;
  line.block->appendReadingText(reading);
  EXPECT_EQ(shalom + " " + olam, reading);
}

TEST(InlineObjects, ARightToLeftLineHandsEachObjectItsOwnId) {
  // Two objects in a right-to-left line are stored in visual order, the reverse of their token order.
  BlockStyle style = aligned(CssTextAlign::Right);
  style.direction = CssDirection::Rtl;
  ParsedText text(false, false, style);
  text.addWord("\xD7\xA9\xD7\x9C", kRegular);
  text.addInlineObject(1, 20, false, 0);
  text.addWord("\xD7\xA2\xD7\x95", kRegular);
  text.addInlineObject(2, 30, false, 0);
  const auto lines = layout(text, 200);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].objects.size());
  // Leftmost first: the second object (30 px) at the line's start.
  EXPECT_EQ(2, lines[0].objects[0].id);
  EXPECT_EQ(1, lines[0].objects[1].id);
  EXPECT_LT(lines[0].objects[0].x, lines[0].objects[1].x);
}

TEST(InlineObjects, ARubyBoxOnTheLineStillNamesItsBase) {
  // <obj>字字 with ruby over 字字: the box covers the two kept words, the object before them gone from the line.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addInlineObject(1, 20, false, 0);
  text.addWord("字字", kRegular);
  ASSERT_TRUE(text.addRubyGroup(1, 2, "かな"));
  const auto lines = layout(text, 200);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ((Words{"字", "字"}), lines[0].words);
  const TextBlock& block = *lines[0].block;
  ASSERT_EQ(1u, block.annotationCount());
  const TextBlock::Annotation box = block.annotation(0);
  EXPECT_EQ(0, box.firstWord);
  EXPECT_EQ(2, box.wordCount);
  EXPECT_EQ("かな", std::string(box.text));
}

TEST(InlineObjects, BionicReadingLeavesTheObjectAlone) {
  ParsedText text(false, false, aligned(CssTextAlign::Left), /*bionicReadingEnabled=*/true);
  text.addWord("reading", kRegular);
  text.addInlineObject(1, 20, false, 0);
  const auto lines = layout(text, 400);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0].objects.size());
  for (const std::string& word : lines[0].words) EXPECT_EQ(std::string::npos, word.find("\xEF\xBF\xBC"));
}

TEST(InlineObjects, LayoutAsksNoFontForTheObjectCharacter) {
  GfxRenderer renderer;
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addInlineObject(1, 20, false, 0);
  renderer.readyCalls.clear();
  text.layoutAndExtractLines(
      renderer, kFontId, 400,
      [](std::unique_ptr<TextBlock>, bool, bool) { return ParsedText::LineProcessResult::Accepted; }, true);
  ASSERT_FALSE(renderer.readyCalls.empty());
  for (const auto& call : renderer.readyCalls) EXPECT_EQ(std::string::npos, call.text.find("\xEF\xBF\xBC"));
}

TEST(InlineObjects, ABlockWithoutObjectsKeepsNoObjectLine) {
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.addWord("aa", kRegular);
  text.addWord("bb", kRegular);
  const auto lines = layout(text, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_TRUE(text.lineObjects().empty());
}
