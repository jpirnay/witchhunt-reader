// Host tests for the plain-text fallback used when a dictionary definition's
// HTML cannot be laid out as styled pages.

#include <gtest/gtest.h>

#include <string>

#include "HtmlToPlainText.h"

namespace {

TEST(HtmlToPlainText, StripsTagsAndKeepsText) {
  EXPECT_EQ(htmlToPlainText("<b>quixotic</b> <i>adj.</i>"), "quixotic adj.");
  EXPECT_EQ(htmlToPlainText("plain"), "plain");
  EXPECT_EQ(htmlToPlainText(""), "");
  EXPECT_EQ(htmlToPlainText("<span class=\"x\">in span</span>"), "in span");
}

TEST(HtmlToPlainText, BlockElementsBecomeBreaks) {
  EXPECT_EQ(htmlToPlainText("a<br>b"), "a\nb");
  EXPECT_EQ(htmlToPlainText("<div>a</div><div>b</div>"), "a\nb");
  // A paragraph is worth a blank line; consecutive breaks do not stack up.
  EXPECT_EQ(htmlToPlainText("<p>a</p><p>b</p>"), "a\n\nb");
  EXPECT_EQ(htmlToPlainText("<h1>title</h1>body"), "title\n\nbody");
  EXPECT_EQ(htmlToPlainText("a<br><br><br>b"), "a\nb");
}

TEST(HtmlToPlainText, DecodesEntities) {
  EXPECT_EQ(htmlToPlainText("Tom &amp; Jerry"), "Tom & Jerry");
  EXPECT_EQ(htmlToPlainText("a&nbsp;b"),
            "a\xC2\xA0"
            "b");
  EXPECT_EQ(htmlToPlainText("&#65;&#66;"), "AB");
  EXPECT_EQ(htmlToPlainText("&#x2014;"), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  EXPECT_EQ(htmlToPlainText("&notanentity;"), "&notanentity;");
  EXPECT_EQ(htmlToPlainText("100% & up"), "100% & up");
}

TEST(HtmlToPlainText, TrimsSurroundingWhitespace) {
  EXPECT_EQ(htmlToPlainText("<p>only</p>"), "only");
  EXPECT_EQ(htmlToPlainText("text   "), "text");
  EXPECT_EQ(htmlToPlainText("a\tb"), "a b");
  // A leading break has nothing to separate, so it is dropped.
  EXPECT_EQ(htmlToPlainText("<br>a"), "a");
}

TEST(HtmlToPlainText, SurvivesMalformedMarkup) {
  // An unterminated tag is text, not a tag that eats the rest of the entry.
  EXPECT_EQ(htmlToPlainText("a <b"), "a <b");
  EXPECT_EQ(htmlToPlainText("x < y"), "x < y");
  EXPECT_EQ(htmlToPlainText("<!-- comment -->kept"), "kept");
}

}  // namespace
