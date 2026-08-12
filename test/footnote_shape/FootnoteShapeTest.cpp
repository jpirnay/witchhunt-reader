#include <Epub/FootnoteShape.h>
#include <gtest/gtest.h>

#include <cstring>

namespace {

bool marker(const char* text) { return FootnoteShape::isMarkerText(text, strlen(text)); }

}  // namespace

// The shapes real reference links use. FootnotePreviews::gather collects these, so the
// sawFootnote_ latch that triggers the gather must accept them too.
TEST(FootnoteShape, AcceptsLatinMarkerText) {
  EXPECT_TRUE(marker("1"));
  EXPECT_TRUE(marker("12"));
  EXPECT_TRUE(marker("[3]"));
  EXPECT_TRUE(marker("(4)"));
  EXPECT_TRUE(marker("*"));
  EXPECT_TRUE(marker("**"));
  EXPECT_TRUE(marker("†"));  // dagger
  EXPECT_TRUE(marker("‡"));  // double dagger
  EXPECT_TRUE(marker("§"));  // section sign
  EXPECT_TRUE(marker("¶"));  // pilcrow
}

// Superscripts are the commonest marker in print and are not English-specific. ¹ ² ³ sit in
// Latin-1, the rest in the U+2070 block — a split that is easy to half-implement.
TEST(FootnoteShape, AcceptsSuperscriptAndSubscriptDigits) {
  EXPECT_TRUE(marker("¹"));
  EXPECT_TRUE(marker("²"));
  EXPECT_TRUE(marker("³"));
  EXPECT_TRUE(marker("⁴"));
  EXPECT_TRUE(marker("⁹"));
  EXPECT_TRUE(marker("⁰"));
  EXPECT_TRUE(marker("¹²"));
  EXPECT_TRUE(marker("⁽¹⁾"));
  EXPECT_TRUE(marker("₁"));
}

// A book is not obliged to number its notes with Latin digits.
TEST(FootnoteShape, AcceptsNonLatinDigits) {
  EXPECT_TRUE(marker("٣"));    // Arabic-Indic three
  EXPECT_TRUE(marker("۴"));    // Extended Arabic-Indic four (Persian, Urdu)
  EXPECT_TRUE(marker("३"));    // Devanagari three
  EXPECT_TRUE(marker("৩"));    // Bengali three
  EXPECT_TRUE(marker("๓"));    // Thai three
  EXPECT_TRUE(marker("៣"));    // Khmer three
  EXPECT_TRUE(marker("༣"));    // Tibetan three
  EXPECT_TRUE(marker("１"));   // fullwidth one
  EXPECT_TRUE(marker("[٣]"));  // wrapped
}

TEST(FootnoteShape, AcceptsEnclosedNumbersHanNumeralsAndCjkForms) {
  EXPECT_TRUE(marker("①"));
  EXPECT_TRUE(marker("⑳"));
  EXPECT_TRUE(marker("⑴"));
  EXPECT_TRUE(marker("⓪"));
  EXPECT_TRUE(marker("❶"));
  EXPECT_TRUE(marker("一"));
  EXPECT_TRUE(marker("十二"));
  EXPECT_TRUE(marker("【二】"));
  EXPECT_TRUE(marker("（1）"));
  EXPECT_TRUE(marker("※"));   // Japanese/Korean reference mark
  EXPECT_TRUE(marker("＊"));  // fullwidth asterisk
}

// French typography puts an espace insécable before the marker; CJK markup uses the
// ideographic space. Neither is an allowed marker codepoint, so both must be trimmed.
TEST(FootnoteShape, TrimsUnicodeAndAsciiSpace) {
  EXPECT_TRUE(marker("  7  "));
  EXPECT_TRUE(marker("\n      1\n    "));  // pretty-printed XHTML indentation
  EXPECT_TRUE(marker(" ¹"));               // no-break space
  EXPECT_TRUE(marker(" 1 "));              // narrow no-break space
  EXPECT_TRUE(marker(" 1"));               // thin space
  EXPECT_TRUE(marker("　1"));              // ideographic space
}

// Regression: the exact link text of a Calibre-generated HTML table of contents.
// Treating these as footnotes made book open run the whole-book two-pass gather
// (~2.8 s behind a "Gathering footnotes" popup) and write an empty cache.
TEST(FootnoteShape, RejectsTableOfContentsEntries) {
  EXPECT_FALSE(marker("Title Page"));
  EXPECT_FALSE(marker("Copyright"));
  EXPECT_FALSE(marker("Dedication"));
  EXPECT_FALSE(marker("Chapter One"));
  EXPECT_FALSE(marker("Chapter Two"));
  EXPECT_FALSE(marker("About the Author"));
}

// Widening the digit set must not widen it to whole words in those scripts.
TEST(FootnoteShape, RejectsNonLatinTableOfContentsEntries) {
  EXPECT_FALSE(marker("Глава Один"));   // Russian "Chapter One"
  EXPECT_FALSE(marker("第一章"));       // Chinese "Chapter One"
  EXPECT_FALSE(marker("الفصل الأول"));  // Arabic "Chapter One"
  EXPECT_FALSE(marker("अध्याय"));        // Hindi "chapter"
  EXPECT_FALSE(marker("บทที่ ๓"));        // Thai "chapter 3"
  EXPECT_FALSE(marker("目次"));         // Japanese "table of contents"
}

TEST(FootnoteShape, RejectsOtherProseLinks) {
  EXPECT_FALSE(marker(""));
  EXPECT_FALSE(marker("   "));
  EXPECT_FALSE(marker("see chapter 2"));
  EXPECT_FALSE(marker("12345"));  // 5 codepoints: past the marker ceiling
  EXPECT_FALSE(marker("back"));
  EXPECT_FALSE(marker("1 2"));    // interior space is not trimmed away
  EXPECT_FALSE(marker("١٢٣٤٥"));  // 5 Arabic-Indic digits: same ceiling
}

// A truncated multi-byte sequence must reject, not resynchronise into a false positive.
TEST(FootnoteShape, RejectsMalformedUtf8) {
  EXPECT_FALSE(FootnoteShape::isMarkerText("\xE2\x80", 2));
  EXPECT_FALSE(FootnoteShape::isMarkerText("\xFF", 1));
}

TEST(FootnoteShape, DetectsNoterefTagging) {
  EXPECT_TRUE(FootnoteShape::isNoterefTagged("noteref", nullptr));
  EXPECT_TRUE(FootnoteShape::isNoterefTagged("backlink noteref", nullptr));
  EXPECT_TRUE(FootnoteShape::isNoterefTagged(nullptr, "doc-noteref"));
  EXPECT_FALSE(FootnoteShape::isNoterefTagged(nullptr, nullptr));
  EXPECT_FALSE(FootnoteShape::isNoterefTagged("noterefs", "doc-noterefs"));
  EXPECT_FALSE(FootnoteShape::isNoterefTagged("footnote", "doc-biblioref"));
}

TEST(FootnoteShape, MatchesAttributeTokensExactly) {
  EXPECT_TRUE(FootnoteShape::hasAttributeToken("a b c", "b"));
  EXPECT_TRUE(FootnoteShape::hasAttributeToken("  a\tb\nc  ", "c"));
  EXPECT_FALSE(FootnoteShape::hasAttributeToken("abc", "b"));
  EXPECT_FALSE(FootnoteShape::hasAttributeToken("", "b"));
  EXPECT_FALSE(FootnoteShape::hasAttributeToken(nullptr, "b"));
}
