// Each word a ParsedText holds knows where it begins in the chapter's visible text, and every
// line it hands out reports its first word's offset. That is what lets a page know where it
// starts (Section's content-offset LUT) without the layout engine keeping any text around.
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "Epub/ParsedText.h"
#include "GfxRenderer.h"

namespace {

// Ten words, offsets 0, 10, 20, ... so a line's first-word offset identifies the word.
ParsedText tenWords() {
  ParsedText text(/*extraParagraphSpacing=*/false);
  for (int i = 0; i < 10; ++i) {
    text.addWord("word" + std::to_string(i), EpdFontFamily::REGULAR, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT,
                 static_cast<uint32_t>(i * 10));
  }
  return text;
}

std::vector<uint32_t> lineOffsets(ParsedText& text, const int width, const bool includeLastLine = true) {
  GfxRenderer renderer;
  std::vector<uint32_t> offsets;
  text.layoutAndExtractLines(
      renderer, 0, static_cast<uint16_t>(width),
      [&](std::unique_ptr<TextBlock>, bool, bool) {
        offsets.push_back(text.lastLineVisibleOffset());
        return ParsedText::LineProcessResult::Accepted;
      },
      includeLastLine);
  return offsets;
}

}  // namespace

TEST(ParsedTextOffset, TheFirstWordOfTheBlockIsWhereTheBlockStarts) {
  ParsedText text = tenWords();
  EXPECT_EQ(text.firstWordVisibleOffset(), std::optional<uint32_t>(0));
}

TEST(ParsedTextOffset, AnEmptyBlockHasNoOffset) {
  ParsedText text(false);
  EXPECT_EQ(text.firstWordVisibleOffset(), std::nullopt);
}

TEST(ParsedTextOffset, EveryLineReportsItsFirstWord) {
  ParsedText text = tenWords();
  // Narrow enough that the stub renderer breaks the ten words over several lines.
  const auto offsets = lineOffsets(text, 120);
  ASSERT_GT(offsets.size(), 1u);
  EXPECT_EQ(offsets.front(), 0u);
  for (size_t i = 1; i < offsets.size(); ++i) {
    EXPECT_GT(offsets[i], offsets[i - 1]);
    EXPECT_EQ(offsets[i] % 10, 0u) << "a line began at an offset no word has";
  }
}

TEST(ParsedTextOffset, ConsumedWordsTakeTheirOffsetsWithThem) {
  ParsedText whole = tenWords();
  const auto allLines = lineOffsets(whole, 120);
  ASSERT_GT(allLines.size(), 2u);

  // Withholding the last line (a continuation flush) leaves its words in the block; the block
  // must then start exactly where the whole-paragraph layout started that last line.
  ParsedText text = tenWords();
  const auto emitted = lineOffsets(text, 120, /*includeLastLine=*/false);
  ASSERT_EQ(emitted.size() + 1, allLines.size());
  EXPECT_EQ(text.firstWordVisibleOffset(), std::optional<uint32_t>(allLines.back()));
}

TEST(ParsedTextOffset, ResetForgetsTheOffsets) {
  ParsedText text = tenWords();
  text.reset(BlockStyle());
  EXPECT_EQ(text.firstWordVisibleOffset(), std::nullopt);
}
