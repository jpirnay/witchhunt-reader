// The paragraph's word arrays grow to the parser's split capacity and no further.
//
// The parser splits a block on the token past its threshold (96, or the last split's kept line plus 48), so the
// block never holds more tokens than that, but for the few a ruby group may hold the split up. Doubling took the
// arrays of a span-per-word paragraph to 256 entries for at most 175 tokens: 33 B an entry on the device, at the
// build's heap peak (docs/future_work/writing-modes-handover.md, item 1).
#include <gtest/gtest.h>

#include <string>

#include "Epub/ParsedText.h"

namespace {

ParsedText paragraph() { return ParsedText(false, false, BlockStyle{}, false); }

void addWords(ParsedText& text, const int count) {
  for (int i = 0; i < count; i++) text.addWord("w", EpdFontFamily::REGULAR);
}

TEST(WordCapacity, TheArraysStopAtTheSplitCapacity) {
  ParsedText text = paragraph();
  text.setSplitCapacity(97);
  addWords(text, 97);
  EXPECT_EQ(text.wordCapacity(), 97u);
}

TEST(WordCapacity, WithoutACapTheArraysDouble) {
  ParsedText text = paragraph();
  addWords(text, 97);
  EXPECT_EQ(text.wordCapacity(), 128u);
}

TEST(WordCapacity, AGroupThatHoldsTheSplitUpGrowsTheArraysBySixteens) {
  ParsedText text = paragraph();
  text.setSplitCapacity(97);
  addWords(text, 98);
  EXPECT_EQ(text.wordCapacity(), 112u);
  addWords(text, 15);  // 113
  EXPECT_EQ(text.wordCapacity(), 128u);
}

TEST(WordCapacity, ASplitThatKeepsALongLineRaisesTheCapacityToItsThreshold) {
  // A kept line of 97 tokens: the next split waits for 145, and the arrays grow to exactly 146.
  ParsedText text = paragraph();
  text.setSplitCapacity(97);
  addWords(text, 97);
  text.setSplitCapacity(146);
  addWords(text, 49);
  EXPECT_EQ(text.wordCapacity(), 146u);
}

TEST(WordCapacity, ACapBelowTheCapacityChangesNothingUntilTheArraysAreFull) {
  // A later split keeps a short line: the threshold falls back to 96 while the arrays hold 146. They keep what they
  // have, and grow by 16s only once a block fills them past the cap.
  ParsedText text = paragraph();
  text.setSplitCapacity(146);
  addWords(text, 146);
  ASSERT_EQ(text.wordCapacity(), 146u);
  text.setSplitCapacity(97);
  addWords(text, 1);
  EXPECT_EQ(text.wordCapacity(), 160u);
}

}  // namespace
