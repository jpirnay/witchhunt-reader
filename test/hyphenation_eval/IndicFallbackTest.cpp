// The Hyphenator's fallback breaks (a word the language patterns could not split) fall only between the syllables of
// an Indic or Myanmar word, never inside one. With no language patterns installed every break is a fallback break.
#include <ArabicScripts.h>
#include <IndicScripts.h>
#include <MyanmarScript.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/Hyphenator.h"
#include "lib/Epub/Epub/hyphenation/LiangHyphenation.h"

namespace {

std::vector<uint32_t> codepointsOf(const std::string& word) {
  std::vector<uint32_t> cps;
  const auto* p = reinterpret_cast<const unsigned char*>(word.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) cps.push_back(cp);
  return cps;
}

// The code points on either side of the byte offset `offset` of `word`.
std::pair<uint32_t, uint32_t> neighbours(const std::string& word, const size_t offset) {
  return {codepointsOf(word.substr(0, offset)).back(), codepointsOf(word.substr(offset)).front()};
}

}  // namespace

TEST(IndicFallback, ABreakNeverFallsInsideASyllable) {
  Hyphenator::setLanguageHyphenator(nullptr);
  for (const std::string& word : {std::string("प्रार्थनासंस्कृतम्कृष्णज्ञानम्"), std::string("শ্রীঈশ্বরচন্দ্রকর্তৃক")}) {
    const auto breaks = Hyphenator::breakOffsets(word, true);
    EXPECT_GE(breaks.size(), 3u) << word;
    for (const auto& b : breaks) {
      const auto [before, after] = neighbours(word, b.byteOffset);
      EXPECT_TRUE(indic::syllableBreakAllowed(before, after)) << word << " at byte " << b.byteOffset;
    }
  }
}

// Pins that a word outside the Indic blocks keeps a break at every index the prefix and suffix minimums leave.
TEST(IndicFallback, ALatinWordsFallbackBreaksAreUnchanged) {
  Hyphenator::setLanguageHyphenator(nullptr);
  const std::string word = "Donaudampfschiffahrt";
  const auto breaks = Hyphenator::breakOffsets(word, true);
  const size_t first = LiangWordConfig::kDefaultMinPrefix;
  const size_t last = word.size() - LiangWordConfig::kDefaultMinSuffix;
  ASSERT_EQ(last - first + 1, breaks.size());
  for (size_t i = 0; i < breaks.size(); ++i) EXPECT_EQ(first + i, breaks[i].byteOffset);
}

// An Arabic forced split falls only between base clusters: never before a mark, never after a ZWJ.
TEST(IndicFallback, AnArabicForcedSplitFallsBetweenBaseClusters) {
  Hyphenator::setLanguageHyphenator(nullptr);
  const std::string zwj = "\xE2\x80\x8D";
  const std::string word = "\u0643\u064E\u062A\u064E\u0627\u0628\u064F\u0646\u0640" + zwj + "\u0645\u0652\u0644" + zwj +
                           "\u0643\u062A\u0627\u0628\u0627\u0646\u0651\u0650";
  const auto breaks = Hyphenator::breakOffsets(word, true);
  EXPECT_GE(breaks.size(), 3u);
  for (const auto& b : breaks) {
    const auto [before, after] = neighbours(word, b.byteOffset);
    EXPECT_TRUE(arabic::isClusterBreak(before, after)) << "at byte " << b.byteOffset;
    EXPECT_FALSE(arabic::isMark(after)) << "before a mark at byte " << b.byteOffset;
    EXPECT_NE(arabic::ZWJ, before) << "after a ZWJ at byte " << b.byteOffset;
  }
}

// A Latin or emoji word with a ZWJ or ZWNJ keeps a break at every index, as without the joiner.
TEST(IndicFallback, AJoinerOutsideArabicBlocksNoBreak) {
  Hyphenator::setLanguageHyphenator(nullptr);
  const std::string zwj = "\xE2\x80\x8D";
  const std::string zwnj = "\xE2\x80\x8C";
  const std::string man = "\xF0\x9F\x91\xA8";
  const std::string woman = "\xF0\x9F\x91\xA9";
  for (const std::string& word : {std::string("Donau") + zwnj + "dampf" + zwj + "schifffahrt",
                                  std::string("Donau") + man + zwj + woman + "dampfschifffahrt"}) {
    const size_t cpCount = codepointsOf(word).size();
    const auto breaks = Hyphenator::breakOffsets(word, true);
    EXPECT_EQ(cpCount - LiangWordConfig::kDefaultMinSuffix - LiangWordConfig::kDefaultMinPrefix + 1, breaks.size());
  }
}

// An Arabic letter's ZWNJ and ZWJ still bind: no break before the joiner, none after the ZWJ.
TEST(IndicFallback, AnArabicJoinerBlocksItsBreaks) {
  Hyphenator::setLanguageHyphenator(nullptr);
  const std::string zwj = "\xE2\x80\x8D";
  const std::string zwnj = "\xE2\x80\x8C";
  const std::string word =
      "\u0643\u062A\u0627\u0628" + zwnj + "\u0643\u062A\u0627\u0628" + zwj + "\u0643\u062A\u0627\u0628\u0627\u0646";
  for (const auto& b : Hyphenator::breakOffsets(word, true)) {
    const auto [before, after] = neighbours(word, b.byteOffset);
    EXPECT_NE(arabic::ZWNJ, after);
    EXPECT_NE(arabic::ZWJ, after);
    EXPECT_NE(arabic::ZWJ, before);
  }
}

namespace {

// The breaks of a Myanmar phrase the fallback cuts, as the code points before each: every one a syllable's start by
// myanmar::breakAllowed (reading past the dot below), and none drawing a hyphen.
std::vector<size_t> myanmarBreakIndexes(const std::string& word) {
  const std::vector<uint32_t> cps = codepointsOf(word);
  std::vector<size_t> indexes;
  for (const auto& b : Hyphenator::breakOffsets(word, true)) {
    EXPECT_FALSE(b.requiresInsertedHyphen) << word << " at byte " << b.byteOffset;
    const size_t idx = codepointsOf(word.substr(0, b.byteOffset)).size();
    size_t next = idx + 1;
    while (next < cps.size() && cps[next] == myanmar::DOT_BELOW) ++next;
    EXPECT_TRUE(myanmar::breakAllowed(cps[idx - 1], cps[idx], next < cps.size() ? cps[next] : 0))
        << word << " at code point " << idx;
    indexes.push_back(idx);
  }
  return indexes;
}

}  // namespace

// A Myanmar phrase wider than the line is cut between syllables only: never after the stacker, never before a killed
// final consonant or kinzi (the dot below skipped), never inside a number, and never with a hyphen.
TEST(IndicFallback, AMyanmarPhraseIsCutBetweenSyllablesWithoutAHyphen) {
  Hyphenator::setLanguageHyphenator(nullptr);
  // မင်္ဂလာပါ: kinzi and the stacked ga stay with ma, so the cuts are before la and pa.
  EXPECT_EQ(myanmarBreakIndexes("မင်္ဂလာပါ"), (std::vector<size_t>{5, 7}));
  // ကျောင်းသား: the killed nga stays in ကျောင်း; the one cut is before sa.
  EXPECT_EQ(myanmarBreakIndexes("ကျောင်းသား"), (std::vector<size_t>{7}));
  // ကြောင့်မြန်မာ: the nga with the dot below before asat stays; then မြန် | မာ.
  EXPECT_EQ(myanmarBreakIndexes("ကြောင့်မြန်မာ"), (std::vector<size_t>{7, 11}));
  // ၁၂၃၄ကျပ်: never inside the number; the cut is before ka.
  EXPECT_EQ(myanmarBreakIndexes("၁၂၃၄ကျပ်"), (std::vector<size_t>{4}));
  // A stack: ကမ္ဘာ is not cut at all, its first consonant held by the stacker after it.
  EXPECT_TRUE(myanmarBreakIndexes("ကမ္ဘာ").empty());
  // A ZWJ binds the consonant after it.
  EXPECT_EQ(myanmarBreakIndexes("ကာ‍ကာကာ"), (std::vector<size_t>{5}));
}

// Beside a Myanmar letter no hyphen is drawn; inside the Latin part of the same word it still is.
TEST(IndicFallback, AHyphenIsDrawnOnlyAwayFromMyanmar) {
  Hyphenator::setLanguageHyphenator(nullptr);
  const std::string word = "Rangoonရန်ကုန်မြို့";
  const size_t latin = std::string("Rangoon").size();
  size_t myanmarBreaks = 0;
  for (const auto& b : Hyphenator::breakOffsets(word, true)) {
    if (b.byteOffset < latin) {
      EXPECT_TRUE(b.requiresInsertedHyphen) << "at byte " << b.byteOffset;
    } else {
      EXPECT_FALSE(b.requiresInsertedHyphen) << "at byte " << b.byteOffset;
      ++myanmarBreaks;
    }
  }
  EXPECT_EQ(myanmarBreaks, 3u);  // before ra, ka (ကုန်) and ma (မြို့)
}

// The Myanmar design's PR 3: the line-end breaks of a Myanmar phrase are the fallback's cuts, offered whatever the
// language's patterns, none with a hyphen, and only inside a Myanmar run.
TEST(IndicFallback, AMyanmarPhraseEndsALineWhereTheFallbackWouldCutIt) {
  const auto offsets = [](const std::vector<Hyphenator::BreakInfo>& breaks) {
    std::vector<size_t> out;
    for (const auto& b : breaks) out.push_back(b.byteOffset);
    return out;
  };
  for (const std::string word : {"မင်္ဂလာပါ", "ကျောင်းသား", "ကြောင့်မြန်မာ", "၁၂၃၄ကျပ်", "ကမ္ဘာ", "ကာ‍ကာကာ"}) {
    Hyphenator::setLanguageHyphenator(nullptr);
    const std::vector<size_t> fallback = offsets(Hyphenator::breakOffsets(word, true));
    Hyphenator::setLanguageHyphenator(Hyphenator::forLanguage("en"));
    const std::vector<Hyphenator::BreakInfo> lineEnds = Hyphenator::myanmarLineEndBreaks(word);
    for (const auto& b : lineEnds) EXPECT_FALSE(b.requiresInsertedHyphen) << word;
    EXPECT_EQ(fallback, offsets(lineEnds)) << word;
  }
  Hyphenator::setLanguageHyphenator(nullptr);
  // Not where Latin meets Myanmar, nor before a bracket: Rangoon|ရန်|ကုန် is cut before ka only, (ကာကာ) after its
  // first syllable only; and a Latin word has none.
  EXPECT_EQ(offsets(Hyphenator::myanmarLineEndBreaks("Rangoonရန်ကုန်")), (std::vector<size_t>{16}));
  EXPECT_EQ(offsets(Hyphenator::myanmarLineEndBreaks("(ကာကာ)")), (std::vector<size_t>{7}));
  EXPECT_TRUE(Hyphenator::myanmarLineEndBreaks("Rangoon").empty());
}
