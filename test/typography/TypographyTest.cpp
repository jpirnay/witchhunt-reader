// The typography profile a book's language picks (spec section 3), and the punctuation widths of each language's
// table (spec section 5.2, from JLREQ and CLREQ).
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <vector>

#include "Epub/CjkPunctuation.h"
#include "Epub/LanguageTag.h"
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

// The font takes a tag the layout must take too: a book tagged cn drew in the Simplified Chinese font with Latin rules.
TEST(TypographyProfile, ABookTaggedCnIsLaidOutAsChinese) {
  for (const char* tag : {"cn", "CN"}) {
    const TypographyProfile cn = TypographyProfile::forLanguage(tag);
    EXPECT_EQ(cn.punctuation, Table::SimplifiedChinese) << tag;
    EXPECT_EQ(cn.breaking, TypographyProfile::Breaking::BetweenCharacters) << tag;
    EXPECT_EQ(cn.justify, TypographyProfile::Justify::BetweenCharacters) << tag;
    EXPECT_TRUE(cn.wholeEmGrid) << tag;
    EXPECT_TRUE(cn.cjkLatinGap) << tag;
    EXPECT_TRUE(cn.verticalAllowed) << tag;
    EXPECT_TRUE(cn.cjkRepertoire) << tag;
  }
  EXPECT_FALSE(TypographyProfile::forLanguage("cnr").wholeEmGrid) << "Montenegrin";
}

TEST(TypographyProfile, TheOldDotNetTagsPickTheirScript) {
  EXPECT_EQ(TypographyProfile::forLanguage("zh-CHT").punctuation, Table::TraditionalChinese);
  EXPECT_EQ(TypographyProfile::forLanguage("zh_cht").punctuation, Table::TraditionalChinese);
  EXPECT_EQ(TypographyProfile::forLanguage("zh-CHS").punctuation, Table::SimplifiedChinese);
}

// SdCardFontSystem::setReaderLanguage maps these one to one, so this table is the font's as well as the layout's.
TEST(LanguageTag, EveryTagNamesOneCjkRegion) {
  using language_tag::CjkRegion;
  using language_tag::cjkRegionOf;
  for (const char* tag : {"ja", "ja-JP", "JA_jp"}) EXPECT_EQ(cjkRegionOf(tag), CjkRegion::Japanese) << tag;
  for (const char* tag : {"ko", "ko-KR", "KO_kr"}) EXPECT_EQ(cjkRegionOf(tag), CjkRegion::Korean) << tag;
  for (const char* tag : {"cn", "CN", "zh-CN", "zh_cn", "zh-SG", "zh-Hans", "zh-Hans-TW", "zh-CHS"}) {
    EXPECT_EQ(cjkRegionOf(tag), CjkRegion::SimplifiedChinese) << tag;
  }
  for (const char* tag : {"zh-TW", "zh_HK", "ZH-mo", "zh-Hant", "zh-Hant-CN", "zh-CHT", "lzh", "zh-yue-HK"}) {
    EXPECT_EQ(cjkRegionOf(tag), CjkRegion::TraditionalChinese) << tag;
  }
  for (const char* tag : {"zh", "ZH", "zh-yue"}) EXPECT_EQ(cjkRegionOf(tag), CjkRegion::ChineseAmbiguous) << tag;
  for (const char* tag : {"", "en", "en-US", "zhx", "cnr", "cn-x", "kok", "jv", "jav"}) {
    EXPECT_EQ(cjkRegionOf(tag), CjkRegion::None) << tag;
  }
}

TEST(LanguageTag, IndicScriptOfMapsEachLanguageToItsScript) {
  using language_tag::indicScriptOf;
  struct Row {
    std::vector<const char*> languages;
    const char* name;
    uint32_t probe;
  };
  const Row rows[] = {
      {{"hi", "mr", "ne", "sa", "kok", "mai", "bho", "awa", "mag", "raj", "doi", "brx", "new"}, "Devanagari", 0x0915},
      {{"bn", "as", "mni"}, "Bengali", 0x0995},
      {{"pa"}, "Gurmukhi", 0x0A15},
      {{"gu"}, "Gujarati", 0x0A95},
      {{"or"}, "Oriya", 0x0B15},
      {{"ta"}, "Tamil", 0x0B95},
      {{"te"}, "Telugu", 0x0C15},
      {{"kn"}, "Kannada", 0x0C95},
      {{"ml"}, "Malayalam", 0x0D15},
      {{"si"}, "Sinhala", 0x0D9A},
  };
  for (const Row& row : rows) {
    for (const char* tag : row.languages) {
      const indic::ScriptInfo* script = indicScriptOf(tag);
      ASSERT_NE(script, nullptr) << tag;
      EXPECT_STREQ(script->name, row.name) << tag;
      EXPECT_EQ(script->probe, row.probe) << tag;
    }
  }
  for (const char* tag : {"hi-IN", "HI_in"}) {
    ASSERT_NE(indicScriptOf(tag), nullptr) << tag;
    EXPECT_STREQ(indicScriptOf(tag)->name, "Devanagari") << tag;
  }
  for (const char* tag : {"en", "zh", "he", "ar", ""}) EXPECT_EQ(indicScriptOf(tag), nullptr) << tag;
}

TEST(LanguageTag, IsArabicScriptCoversEveryArabicScriptLanguage) {
  using language_tag::isArabicScript;
  for (const char* tag : {"ar",    "fa",    "prs",     "ur",      "ps",    "sd",      "ug",           "ckb",      "ks",
                          "pnb",   "azb",   "bal",     "skr",     "glk",   "mzn",     "lrc",          "bqi",      "ota",
                          "ar-EG", "AR_sa", "ku-Arab", "pa-Arab", "ur-PK", "sd-Arab", "ar-u-nu-latn", "ar-x-latn"}) {
    EXPECT_TRUE(isArabicScript(tag)) << tag;
  }
  for (const char* tag :
       {"ku", "sd-Deva", "ks-Deva", "pa-Latn", "pa", "en", "he", "hi", "", "ara", "ar-Latn", "ku-x-arab"}) {
    EXPECT_FALSE(isArabicScript(tag)) << tag;
  }
}

TEST(LanguageTag, IsHebrewScriptCoversHebrewYiddishLadinoAndAHebrScriptSubtag) {
  using language_tag::isHebrewScript;
  // Past a singleton the tag is an extension or private use, whose 4-letter subtags name no script.
  for (const char* tag :
       {"he", "iw", "yi", "lad", "jrb", "he-IL", "HE_il", "yi-Hebr", "ar-Hebr", "he-u-nu-latn", "he-x-latn"}) {
    EXPECT_TRUE(isHebrewScript(tag)) << tag;
  }
  for (const char* tag : {"lad-Latn", "yi-Latn", "en", "ar", "", "heb", "hi", "en-x-hebr"}) {
    EXPECT_FALSE(isHebrewScript(tag)) << tag;
  }
}

TEST(LanguageTag, IsMyanmarScriptCoversBurmeseItsNeighboursAndAMymrScriptSubtag) {
  using language_tag::isMyanmarScript;
  for (const char* tag : {"my",  "my-MM", "MY_mm",   "mya",     "bur",      "mnw",          "shn",      "ksw", "kjp",
                          "pwo", "blk",   "kht",     "aio",     "phk",      "pll",          "rbb",      "pce", "rki",
                          "tvn", "int",   "my-Mymr", "pi-Mymr", "kyu-Mymr", "my-u-nu-latn", "my-x-latn"}) {
    EXPECT_TRUE(isMyanmarScript(tag)) << tag;
  }
  // Pali is written in many scripts, Kayah in Kayah Li; a script subtag of another script says no.
  for (const char* tag :
       {"pi", "pi-Thai", "kyu", "my-Latn", "shn-Latn", "ksw-Thai", "en", "th", "", "myv", "mni", "en-x-mymr"}) {
    EXPECT_FALSE(isMyanmarScript(tag)) << tag;
  }
  // S'gaw Karen is Myanmar under every check, and nothing else.
  EXPECT_TRUE(isMyanmarScript("ksw"));
  EXPECT_EQ(language_tag::indicScriptOf("ksw"), nullptr);
  EXPECT_FALSE(language_tag::isArabicScript("ksw"));
  EXPECT_FALSE(language_tag::isHebrewScript("ksw"));
  EXPECT_EQ(language_tag::cjkRegionOf("ksw"), language_tag::CjkRegion::None);
}

TEST(LanguageTag, AForeignScriptSubtagIsNotIndic) {
  using language_tag::indicScriptOf;
  for (const char* tag : {"sa-Latn", "pa-Arab", "hi-Latn", "mni-Mtei", "hi-Latn-x-deva"})
    EXPECT_EQ(indicScriptOf(tag), nullptr) << tag;
  const struct {
    const char* tag;
    const char* name;
  } own[] = {{"hi-Deva", "Devanagari"},
             {"bn-Beng", "Bengali"},
             {"pa-Guru", "Gurmukhi"},
             // Past a singleton the tag is an extension or private use, whose 4-letter subtags name no script.
             {"bn-u-nu-latn", "Bengali"},
             {"hi-x-abcd", "Devanagari"}};
  for (const auto& row : own) {
    ASSERT_NE(indicScriptOf(row.tag), nullptr) << row.tag;
    EXPECT_STREQ(indicScriptOf(row.tag)->name, row.name) << row.tag;
  }
}

TEST(Typography, AnIndicLanguageSetsShapedScript) {
  for (const char* tag : {"hi", "doi", "mni", "si"}) {
    EXPECT_TRUE(TypographyProfile::forLanguage(tag).shapedScript) << tag;
  }
  for (const char* tag : {"sa-Latn", "en"}) EXPECT_FALSE(TypographyProfile::forLanguage(tag).shapedScript) << tag;
}

TEST(Typography, AnArabicScriptLanguageIsRtlAndShaped) {
  for (const char* tag : {"ur", "ps", "ckb", "fa", "ar", "sd", "ku-Arab"}) {
    const TypographyProfile p = TypographyProfile::forLanguage(tag);
    EXPECT_EQ(TypographyProfile::Direction::RightToLeft, p.direction) << tag;
    EXPECT_TRUE(p.shapedScript) << tag;
  }
  for (const char* tag : {"ku", "sd-Deva"}) {
    EXPECT_EQ(TypographyProfile::Direction::LeftToRight, TypographyProfile::forLanguage(tag).direction) << tag;
    EXPECT_FALSE(TypographyProfile::forLanguage(tag).shapedScript) << tag;
  }
}

// A Myanmar-script book is shaped, left to right, and breaks as a Latin book does: at spaces, with no hyphenation.
TEST(Typography, AMyanmarScriptLanguageIsShapedLeftToRight) {
  for (const char* tag : {"my", "mnw", "shn", "ksw", "bur", "pi-Mymr"}) {
    const TypographyProfile p = TypographyProfile::forLanguage(tag);
    EXPECT_TRUE(p.shapedScript) << tag;
    EXPECT_EQ(TypographyProfile::Direction::LeftToRight, p.direction) << tag;
    EXPECT_EQ(TypographyProfile::Breaking::AtSpaces, p.breaking) << tag;
    EXPECT_EQ(nullptr, p.hyphenator) << tag;
    EXPECT_FALSE(p.cjkRepertoire) << tag;
  }
  EXPECT_EQ(TypographyProfile::forLanguage("mnw").shapingLanguage, ot::languageTagsFor("mnw"));
  for (const char* tag : {"pi", "my-Latn"}) EXPECT_FALSE(TypographyProfile::forLanguage(tag).shapedScript) << tag;
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

TEST(TypographyProfile, HebrewArabicPersianUrduAndYiddishRunRightToLeft) {
  for (const char* tag : {"he", "he-IL", "HE", "iw", "yi", "ar", "ar-EG", "fa", "ur"}) {
    const TypographyProfile p = TypographyProfile::forLanguage(tag);
    EXPECT_EQ(TypographyProfile::Direction::RightToLeft, p.direction) << tag;
    EXPECT_EQ(nullptr, p.hyphenator) << tag;
    EXPECT_FALSE(p.verticalAllowed) << tag;
    EXPECT_FALSE(p.cjkRepertoire) << tag;
  }
  for (const char* tag : {"en", "de", "ja", "hy"}) {
    EXPECT_EQ(TypographyProfile::Direction::LeftToRight, TypographyProfile::forLanguage(tag).direction) << tag;
  }
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

TEST(TypographyProfile, TheIndicLanguagesAreShapedScripts) {
  for (const char* tag : {"hi", "hi-IN", "mr", "bn-BD", "ta", "ml",  "si",  "KOK", "ne_NP", "pa", "gu",
                          "or", "te",    "kn", "as",    "sa", "mai", "bho", "awa", "mag",   "raj"}) {
    EXPECT_TRUE(TypographyProfile::forLanguage(tag).shapedScript) << tag;
  }
  // Hebrew is not shaped; the Arabic script is (AnArabicScriptLanguageIsRtlAndShaped).
  for (const char* tag : {"", "en", "he", "ja", "hin", "kok1"}) {
    EXPECT_FALSE(TypographyProfile::forLanguage(tag).shapedScript) << tag;
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

TEST(TypographyProfile, OnlyChineseJapaneseAndKoreanMayBeSetVertically) {
  for (const char* tag : {"ja", "zh", "zh-Hant", "ko-KR"}) {
    EXPECT_TRUE(TypographyProfile::forLanguage(tag).verticalAllowed) << tag;
  }
  for (const char* tag : {"en", "he", ""}) {
    EXPECT_FALSE(TypographyProfile::forLanguage(tag).verticalAllowed) << tag;
  }
}

TEST(TypographyProfile, AShapedWordTriesTheBooksLanguageSystems) {
  // The engine's own table, by address: a plan is kept per language system array (ShapingFace::plan).
  EXPECT_EQ(TypographyProfile::forLanguage("mr").shapingLanguage, ot::languageTagsFor("mr"));
  EXPECT_EQ(TypographyProfile::forLanguage("hi-IN").shapingLanguage, ot::languageTagsFor("hi"));
  EXPECT_EQ(TypographyProfile::forLanguage("NE_np").shapingLanguage, ot::languageTagsFor("ne"));
  EXPECT_NE(TypographyProfile::forLanguage("mr").shapingLanguage, TypographyProfile::forLanguage("hi").shapingLanguage);
  // An unknown language or none: the script's default, the engine's empty list.
  EXPECT_EQ(TypographyProfile::forLanguage("").shapingLanguage, ot::languageTagsFor(""));
  // A profile the parser never set: Latin rules, and the script's default for a shaped word.
  EXPECT_EQ(TypographyProfile{}.shapingLanguage, nullptr);
}
}  // namespace
