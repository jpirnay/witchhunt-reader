// The typography profile a book's language picks (spec section 3), and the punctuation widths of each language's
// table (spec section 5.2, from JLREQ and CLREQ).
#include <gtest/gtest.h>

#include "Epub/CjkPunctuation.h"
#include "Epub/TypographyProfile.h"

namespace {

using Table = TypographyProfile::Punctuation;
using CjkPunctuation::Class;

// breaking and justify are not read by layout yet; cjkLatinGap is (since #575) and verticalAllowed is read by
// FlowContext::requestFor. The asserts are the language table's specification, so the next reader gets it right.
TEST(TypographyProfile, ChineseAndJapanesePickTheirTablesTheGridAndTheGap) {
  const TypographyProfile ja = TypographyProfile::forLanguage("ja");
  EXPECT_EQ(ja.punctuation, Table::Japanese);
  EXPECT_EQ(ja.breaking, TypographyProfile::Breaking::BetweenCharacters);
  EXPECT_EQ(ja.justify, TypographyProfile::Justify::BetweenCharacters);
  EXPECT_TRUE(ja.wholeEmGrid);
  EXPECT_TRUE(ja.cjkLatinGap);
  EXPECT_TRUE(ja.verticalAllowed);

  EXPECT_EQ(TypographyProfile::forLanguage("zh").punctuation, Table::SimplifiedChinese);
  EXPECT_EQ(TypographyProfile::forLanguage("zh-CN").punctuation, Table::SimplifiedChinese);
  EXPECT_TRUE(TypographyProfile::forLanguage("zh-CN").wholeEmGrid);
}

TEST(TypographyProfile, TraditionalChineseByScriptRegionOrClassicalChinese) {
  for (const char* tag : {"zh-Hant", "zh-TW", "zh_HK", "ZH-mo", "zh-Hant-CN", "lzh"}) {
    EXPECT_EQ(TypographyProfile::forLanguage(tag).punctuation, Table::TraditionalChinese) << tag;
  }
  // An explicit script subtag wins over the region.
  EXPECT_EQ(TypographyProfile::forLanguage("zh-Hans-TW").punctuation, Table::SimplifiedChinese);
}

TEST(TypographyProfile, KoreanKeepsWholeWordsWithoutGridOrGap) {
  const TypographyProfile ko = TypographyProfile::forLanguage("ko-KR");
  EXPECT_EQ(ko.breaking, TypographyProfile::Breaking::KoreanWords);
  EXPECT_EQ(ko.justify, TypographyProfile::Justify::WordSpaces);
  EXPECT_EQ(ko.punctuation, Table::SimplifiedChinese);
  EXPECT_FALSE(ko.wholeEmGrid);
  EXPECT_FALSE(ko.cjkLatinGap);
  EXPECT_TRUE(ko.verticalAllowed) << "KLREQ sets Korean vertically";
}

TEST(TypographyProfile, OtherLanguagesKeepLatinRulesAndTheirHyphenation) {
  const TypographyProfile en = TypographyProfile::forLanguage("en-US");
  EXPECT_EQ(en.breaking, TypographyProfile::Breaking::AtSpaces);
  EXPECT_EQ(en.punctuation, Table::SimplifiedChinese) << "CJK punctuation in any book";
  EXPECT_FALSE(en.wholeEmGrid);
  EXPECT_FALSE(en.cjkLatinGap);
  EXPECT_NE(en.hyphenator, nullptr);
  EXPECT_EQ(TypographyProfile::forLanguage("").hyphenator, nullptr);
  EXPECT_FALSE(TypographyProfile::forLanguage("zhx").wholeEmGrid) << "only the primary subtag zh is Chinese";
}

TEST(TypographyProfile, ChineseJapaneseAndKoreanAreWrittenWithTheCjkRepertoire) {
  for (const char* tag : {"zh", "zh-CN", "zh_TW", "zh-Hant-HK", "lzh", "ja", "ja-JP", "ko", "ko-KR", "KO_kr"}) {
    EXPECT_TRUE(TypographyProfile::forLanguage(tag).cjkRepertoire) << tag;
  }
  // Korean breaks at spaces as a Latin book does, and is still written with thousands of Hangul syllables.
  EXPECT_EQ(TypographyProfile::forLanguage("ko").breaking, TypographyProfile::Breaking::KoreanWords);
  for (const char* tag : {"", "en", "en-US", "de-DE", "vi", "zhx", "kok", "jv"}) {
    EXPECT_FALSE(TypographyProfile::forLanguage(tag).cjkRepertoire) << tag;
  }
}

TEST(CjkPunctuation, ClassesFollowJlreq) {
  EXPECT_EQ(CjkPunctuation::classOf(0x300C), Class::Opening);           // 「
  EXPECT_EQ(CjkPunctuation::classOf(0x300D), Class::Closing);           // 」
  EXPECT_EQ(CjkPunctuation::classOf(0xFF01), Class::Dividing);          // ！
  EXPECT_EQ(CjkPunctuation::classOf(0x30FB), Class::MiddleDot);         // ・
  EXPECT_EQ(CjkPunctuation::classOf(0x3002), Class::FullStop);          // 。
  EXPECT_EQ(CjkPunctuation::classOf(0x3001), Class::Comma);             // 、
  EXPECT_EQ(CjkPunctuation::classOf(0x3000), Class::IdeographicSpace);  // U+3000
  EXPECT_EQ(CjkPunctuation::classOf(0xFF02), Class::AmbiguousQuote);    // ＂
  EXPECT_EQ(CjkPunctuation::classOf(0x201C), Class::Opening);           // “
  EXPECT_EQ(CjkPunctuation::classOf(0x6F22), Class::None);              // 漢
  EXPECT_EQ(CjkPunctuation::classOf('A'), Class::None);
  EXPECT_TRUE(CjkPunctuation::isCurlyQuote(0x2019));
  EXPECT_FALSE(CjkPunctuation::isCurlyQuote(0x300D));
}

// Widths in eighths of the mark's advance: {forced, minimum}.
void expectWidth(const Table table, const Class mark, const Class neighbour, const bool edge, const int forced,
                 const int minimum) {
  const CjkPunctuation::Width w = CjkPunctuation::widthOf(table, mark, neighbour, edge);
  EXPECT_EQ(w.forced, forced);
  EXPECT_EQ(w.minimum, minimum);
}

TEST(CjkPunctuation, SimplifiedChineseHalvesMarksAtLineEdgesAndInRuns) {
  const Table sc = Table::SimplifiedChinese;
  expectWidth(sc, Class::Closing, Class::None, true, 4, 4);       // 」 ending a line
  expectWidth(sc, Class::Opening, Class::None, true, 4, 4);       // 「 starting a line
  expectWidth(sc, Class::Closing, Class::FullStop, false, 4, 4);  // 」 in 」。: all but the last of a run
  expectWidth(sc, Class::FullStop, Class::None, false, 8, 4);     // 。 before text: full, may halve
  expectWidth(sc, Class::Opening, Class::Opening, false, 4, 4);   // the second 「 of 「「
  expectWidth(sc, Class::Opening, Class::None, false, 8, 4);      // 「 after text
  expectWidth(sc, Class::Closing, Class::Opening, false, 6, 6);   // 」 in 」「: a quarter em each side
  expectWidth(sc, Class::Opening, Class::Closing, false, 6, 6);   // 「 in 」「
  expectWidth(sc, Class::IdeographicSpace, Class::None, true, 8, 8);
}

TEST(CjkPunctuation, TraditionalChineseKeepsCentredMarksFullWidth) {
  const Table tc = Table::TraditionalChinese;
  expectWidth(tc, Class::FullStop, Class::None, true, 8, 6);      // 。 at a line end stays full
  expectWidth(tc, Class::Closing, Class::None, true, 8, 4);       // a bracket may halve on demand
  expectWidth(tc, Class::Closing, Class::FullStop, false, 8, 4);  // no forced run rule
  expectWidth(tc, Class::Comma, Class::None, false, 8, 6);
}

TEST(CjkPunctuation, JapaneseKeepsTheFullStopAndLetsLineEndsChoose) {
  const Table ja = Table::Japanese;
  expectWidth(ja, Class::FullStop, Class::None, false, 8, 8);              // 。 keeps its blank mid-line, pushed in too
  expectWidth(ja, Class::FullStop, Class::IdeographicSpace, false, 8, 8);  // 。　 as well
  expectWidth(ja, Class::FullStop, Class::Closing, false, 4, 4);           // 。」: solid before the bracket
  expectWidth(ja, Class::FullStop, Class::None, true, 8, 4);               // half or full at a line end
  expectWidth(ja, Class::Comma, Class::None, true, 8, 4);
  expectWidth(ja, Class::Closing, Class::None, true, 8, 4);
  expectWidth(ja, Class::Opening, Class::None, true, 4, 4);       // a line start sets solid
  expectWidth(ja, Class::Opening, Class::FullStop, false, 4, 4);  // 。「: solid after 。
  expectWidth(ja, Class::Closing, Class::Opening, false, 6, 6);   // 」「
  expectWidth(ja, Class::Dividing, Class::None, false, 8, 8);     // ！？ keep their width (JLREQ 3.8.3)
  expectWidth(ja, Class::Dividing, Class::None, true, 8, 8);
  expectWidth(ja, Class::MiddleDot, Class::None, false, 8, 4);  // ・：； may halve
}

TEST(CjkPunctuation, NoMarkIsNarrowerMidLineThanAtALineEdge) {
  // The DP breaker stops widening a line at the first token whose pushed-in width overflows. That holds only if a
  // line's pushed-in width never drops when a token joins it: the token that was last goes from its line-edge minimum
  // to its mid-line one, which must be no smaller.
  for (const Table table : {Table::SimplifiedChinese, Table::TraditionalChinese, Table::Japanese}) {
    for (int m = 0; m <= static_cast<int>(Class::AmbiguousQuote); ++m) {
      const auto mark = static_cast<Class>(m);
      const CjkPunctuation::Width edge = CjkPunctuation::widthOf(table, mark, Class::None, true);
      EXPECT_LE(edge.minimum, edge.forced);
      for (int n = 0; n <= static_cast<int>(Class::AmbiguousQuote); ++n) {
        const CjkPunctuation::Width mid = CjkPunctuation::widthOf(table, mark, static_cast<Class>(n), false);
        EXPECT_GE(mid.minimum, edge.minimum) << static_cast<int>(table) << " " << m << " " << n;
        EXPECT_LE(mid.minimum, mid.forced);
      }
    }
  }
}

TEST(CjkPunctuation, TheCjkLatinGapSitsBetweenHanOrKanaAndLatinLettersOrDigits) {
  using CjkPunctuation::takesCjkLatinGap;
  EXPECT_TRUE(takesCjkLatinGap(0x6F22, 'A'));      // 漢A
  EXPECT_TRUE(takesCjkLatinGap('1', 0x5E74));      // 1年
  EXPECT_TRUE(takesCjkLatinGap(0x3044, 'i'));      // いi
  EXPECT_TRUE(takesCjkLatinGap(0x30FC, 'x'));      // ーx
  EXPECT_TRUE(takesCjkLatinGap(0x3005, 'e'));      // 々e
  EXPECT_TRUE(takesCjkLatinGap(0x00E9, 0x5B57));   // é字
  EXPECT_TRUE(takesCjkLatinGap(0x20B9F, 'Z'));     // 𠮟Z, Extension B
  EXPECT_FALSE(takesCjkLatinGap(0x3002, 'A'));     // 。A: punctuation
  EXPECT_FALSE(takesCjkLatinGap(0x30FB, 'A'));     // ・A
  EXPECT_FALSE(takesCjkLatinGap(0xAC00, 'A'));     // 가A: Hangul (KLREQ asks for nothing)
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0xFF21));  // 漢Ａ: a full-width letter sits on the grid
  EXPECT_FALSE(takesCjkLatinGap(0xFF71, 'A'));     // ｱA: half-width katakana is set like Latin
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x03B1));  // 漢α
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, '%'));
  EXPECT_FALSE(takesCjkLatinGap(0x00D7, 0x6F22));  // ×漢
  EXPECT_FALSE(takesCjkLatinGap('A', 'B'));
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x5B57));
}

TEST(CjkPunctuation, TheCjkLatinGapRangesEndWhereTheyShould) {
  using CjkPunctuation::takesCjkLatinGap;
  // Each pair is the last codepoint inside a range and the first outside it, against 'A'.
  EXPECT_FALSE(takesCjkLatinGap(0x3004, 'A'));
  EXPECT_TRUE(takesCjkLatinGap(0x3005, 'A'));
  EXPECT_TRUE(takesCjkLatinGap(0x3007, 'A'));
  EXPECT_FALSE(takesCjkLatinGap(0x3008, 'A'));
  EXPECT_TRUE(takesCjkLatinGap(0x3096, 'A'));
  EXPECT_FALSE(takesCjkLatinGap(0x3097, 'A'));
  EXPECT_TRUE(takesCjkLatinGap(0x30FA, 'A'));
  EXPECT_FALSE(takesCjkLatinGap(0x30FB, 'A'));
  EXPECT_TRUE(takesCjkLatinGap(0x9FFF, 'A'));
  EXPECT_FALSE(takesCjkLatinGap(0xA000, 'A'));
  EXPECT_TRUE(takesCjkLatinGap(0x2A6DF, 'A'));
  EXPECT_FALSE(takesCjkLatinGap(0x2A6E0, 'A'));
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x00BF));  // ¿
  EXPECT_TRUE(takesCjkLatinGap(0x6F22, 0x00C0));   // À
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x00D7));  // ×
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x00F7));  // ÷
  EXPECT_TRUE(takesCjkLatinGap(0x6F22, 0x024F));   // ɏ
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x0250));  // ɐ: IPA
  EXPECT_TRUE(takesCjkLatinGap(0x6F22, 0x1EFF));   // ỿ
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x1F00));  // Greek extended
  EXPECT_TRUE(takesCjkLatinGap(0x3083, 'A'));      // ゃ
  EXPECT_TRUE(takesCjkLatinGap(0x30C3, 'A'));      // ッ
  EXPECT_TRUE(takesCjkLatinGap(0x31F0, 'A'));      // ㇰ, katakana extension
  EXPECT_TRUE(takesCjkLatinGap('9', 0x30AB));      // 9カ
  EXPECT_TRUE(takesCjkLatinGap(0x3042, '0'));      // あ0
}

}  // namespace

namespace {
TEST(TypographyProfile, OnlyChineseJapaneseAndKoreanMayBeSetVertically) {
  for (const char* tag : {"ja", "zh", "zh-Hant", "ko-KR"}) {
    EXPECT_TRUE(TypographyProfile::forLanguage(tag).verticalAllowed) << tag;
  }
  for (const char* tag : {"en", "he", ""}) {
    EXPECT_FALSE(TypographyProfile::forLanguage(tag).verticalAllowed) << tag;
  }
}
}  // namespace
