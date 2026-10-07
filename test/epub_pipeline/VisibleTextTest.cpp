// The one definition of "visible text" the layout parser and the KOReader XPath mappers share.
// A page's content offset is a count of these bytes, so the rule has to be the same on both
// sides down to the byte, and it has to be stated once.
#include <gtest/gtest.h>

#include "Epub/VisibleText.h"

TEST(VisibleText, CountsEveryNonWhitespaceByte) {
  EXPECT_EQ(VisibleText::visibleBytes("ab cd", 5), 4u);
  EXPECT_EQ(VisibleText::visibleBytes(" \t\r\n", 4), 0u);
  EXPECT_EQ(VisibleText::visibleBytes("", 0), 0u);
}

TEST(VisibleText, MultiByteSequencesCountPerByte) {
  // U+2019 is three bytes; none of them is whitespace.
  EXPECT_EQ(VisibleText::visibleBytes("\xE2\x80\x99", 3), 3u);
  // U+00A0 (no-break space) is not ASCII whitespace: two visible bytes, on both sides.
  EXPECT_EQ(VisibleText::visibleBytes("\xC2\xA0", 2), 2u);
}

TEST(VisibleText, HeadScriptAndStyleAreNotVisible) {
  EXPECT_TRUE(VisibleText::isNonVisibleTag("head"));
  EXPECT_TRUE(VisibleText::isNonVisibleTag("script"));
  EXPECT_TRUE(VisibleText::isNonVisibleTag("STYLE"));
  EXPECT_FALSE(VisibleText::isNonVisibleTag("title"));
  EXPECT_FALSE(VisibleText::isNonVisibleTag("p"));
}
