// Ours (the Myanmar design's PR 3, docs/superpowers/specs/2026-11-09-myanmar-design.md): a Burmese line ends between
// syllables inside a phrase, without a hyphen, whatever the hyphenation setting. The phrase stays one token; the greedy
// breaker cuts it at the widest prefix that fits, before a syllable's start that myanmar::breakAllowed allows. Laid
// out through the real ParsedText against the fixed-width stub of this directory (every code point 10 px, a space
// 5 px, no face: the words are measured as their code points), so a prefix's width is its code point count.
//
// The words of the kinzi, stack, asat, digit and mixed cases follow Somnius/CrossInk-Indic's Burmese torture lines
// (test/indic/texts/burmese-torture.txt at 6c545b38, by Lef, MIT).
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;
constexpr int kGlyph = GfxRenderer::GLYPH_W;
constexpr int kSpace = GfxRenderer::SPACE_W;

using Lines = std::vector<std::vector<std::string>>;

BlockStyle leftAligned() {
  BlockStyle bs;
  bs.alignment = CssTextAlign::Left;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  return bs;
}

Lines layOut(ParsedText& text, const int width) {
  GfxRenderer renderer;
  Lines lines;
  text.layoutAndExtractLines(
      renderer, kFontId, static_cast<uint16_t>(width),
      [&](std::unique_ptr<TextBlock> line, bool, bool) {
        std::vector<std::string>& words = lines.emplace_back();
        for (uint16_t i = 0; i < line->wordCount(); ++i) words.emplace_back(line->wordText(i));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

Lines layOutWords(const std::vector<std::string>& words, const int width, const bool hyphenation = false) {
  ParsedText text(false, hyphenation, leftAligned());
  for (const std::string& word : words) text.addWord(word, EpdFontFamily::REGULAR);
  return layOut(text, width);
}

// What of `phrase` ends the first line when a one-letter word and a space come before it and `room` code points are
// left after them: empty when the phrase moves down whole.
std::string firstLinePart(const std::string& phrase, const int room, const bool hyphenation = false) {
  const Lines lines = layOutWords({"\xE1\x80\x80", phrase}, kGlyph + kSpace + room * kGlyph, hyphenation);  // ka
  if (lines.empty() || lines[0].size() < 2) return "";
  return lines[0][1];
}

class MyanmarLineBreak : public testing::Test {
 protected:
  // No language's patterns: every break below is Myanmar's own.
  ScopedLanguageHyphenator noPatterns{nullptr};
};

}  // namespace

// With hyphenation off (the default), a Myanmar paragraph still takes the breaker that splits a word at a line end:
// the line is filled to the syllable, where a Hangul word in the same place moves down whole.
TEST_F(MyanmarLineBreak, HyphenationOffCutsAPhraseBetweenSyllables) {
  // ka ka, a space and six syllables kaa: 20 + 5 + 120 px; 100 px leave 75 px after the first word.
  const Lines lines = layOutWords({"ကက", "ကာကာကာကာကာကာ"}, 100);
  EXPECT_EQ(lines, (Lines{{"ကက", "ကာကာကာ"}, {"ကာကာကာ"}}));
  EXPECT_EQ(layOutWords({"가나", "다라마바사아자차카타"}, 100), (Lines{{"가나"}, {"다라마바사아자차카타"}}));
}

TEST_F(MyanmarLineBreak, HyphenationOnCutsTheSameWithoutAHyphen) {
  EXPECT_EQ(layOutWords({"ကက", "ကာကာကာကာကာကာ"}, 100, true), (Lines{{"ကက", "ကာကာကာ"}, {"ကာကာကာ"}}));
}

// Kinzi (nga, asat, stacker) stays with the syllable before it and the consonant it sits on with it: mingalaba is cut
// before la and pa only.
TEST_F(MyanmarLineBreak, KinziStaysWithItsSyllable) {
  const std::string mingalaba = "မင်္ဂလာပါ";
  EXPECT_EQ("", firstLinePart(mingalaba, 2));  // not before the killed nga
  EXPECT_EQ("", firstLinePart(mingalaba, 4));  // not after the stacker
  EXPECT_EQ("မင်္ဂ", firstLinePart(mingalaba, 5));
  EXPECT_EQ("မင်္ဂ", firstLinePart(mingalaba, 6));  // not before a vowel sign
  EXPECT_EQ("မင်္ဂလာ", firstLinePart(mingalaba, 7));
  EXPECT_EQ("", firstLinePart("သင်္ဘော", 6));  // kinzi binds thinbaw's two syllables: it moves down whole
}

// A stack (C, stacker, C) is never cut: neither before its first consonant nor after the stacker.
TEST_F(MyanmarLineBreak, AStackIsNeverCut) {
  const std::string kambaGyi = "ကမ္ဘာကြီး";
  EXPECT_EQ("", firstLinePart(kambaGyi, 1));
  EXPECT_EQ("", firstLinePart(kambaGyi, 3));
  EXPECT_EQ("ကမ္ဘာ", firstLinePart(kambaGyi, 5));
  EXPECT_EQ(layOutWords({"က", "ပစ္စည်း"}, 80), (Lines{{"က"}, {"ပစ္စည်း"}}));  // bound by its stack, it moves whole
}

// A consonant that asat kills ends the syllable before it: kyundaw is cut before ta only.
TEST_F(MyanmarLineBreak, AKilledConsonantStaysWithItsSyllable) {
  const std::string kyundaw = "ကျွန်တော်";
  EXPECT_EQ("", firstLinePart(kyundaw, 2));
  EXPECT_EQ("", firstLinePart(kyundaw, 4));  // not between na and its asat
  EXPECT_EQ("ကျွန်", firstLinePart(kyundaw, 5));
}

// The dot below before asat (the canonical order, as in kant) and after it: the killed na stays with ka either way,
// and the next syllable may start the line.
TEST_F(MyanmarLineBreak, TheDotBelowInEitherOrderKeepsTheKilledConsonant) {
  const std::string canonical = "\xE1\x80\x80\xE1\x80\x94\xE1\x80\xB7\xE1\x80\xBA\xE1\x80\x9E\xE1\x80\x8A\xE1\x80\xBA";
  const std::string asatFirst = "\xE1\x80\x80\xE1\x80\x94\xE1\x80\xBA\xE1\x80\xB7\xE1\x80\x9E\xE1\x80\x8A\xE1\x80\xBA";
  for (const std::string& phrase : {canonical, asatFirst}) {
    EXPECT_EQ("", firstLinePart(phrase, 1)) << phrase;
    EXPECT_EQ("", firstLinePart(phrase, 3)) << phrase;  // not before the dot below or asat, nor before na
    EXPECT_EQ(phrase.substr(0, 12), firstLinePart(phrase, 4)) << phrase;
  }
}

// The section marks end their phrases: a line never starts with one, and may end after one.
TEST_F(MyanmarLineBreak, ASectionMarkNeverStartsALine) {
  const std::string phrase = "ပါ၊ကာ။ပါ";
  EXPECT_EQ("", firstLinePart(phrase, 2));
  EXPECT_EQ("ပါ၊", firstLinePart(phrase, 3));
  EXPECT_EQ("ပါ၊", firstLinePart(phrase, 5));
  EXPECT_EQ("ပါ၊ကာ။", firstLinePart(phrase, 6));
}

// A number is never cut; a syllable after it may start the line, as may one after a letter.
TEST_F(MyanmarLineBreak, ANumberIsNeverCut) {
  const std::string year = "၂၀၂၆ခုနှစ်";
  EXPECT_EQ("", firstLinePart(year, 3));
  EXPECT_EQ("၂၀၂၆", firstLinePart(year, 4));
  EXPECT_EQ("၂၀၂၆ခု", firstLinePart(year, 7));  // nhit: na with its medial ha, sa and asat
}

// The breaks are Myanmar's alone: none where Latin meets Myanmar text, none inside the Latin with hyphenation off.
TEST_F(MyanmarLineBreak, LatinInsideAPhraseIsNotCut) {
  const std::string rangoon = "Rangoonရန်ကုန်";
  EXPECT_EQ("", firstLinePart(rangoon, 3));
  EXPECT_EQ("", firstLinePart(rangoon, 9));  // not before ra
  EXPECT_EQ("Rangoonရန်", firstLinePart(rangoon, 10));
}

// A Latin word in a Myanmar paragraph takes no pattern break with hyphenation off, and its hyphen with it on.
TEST_F(MyanmarLineBreak, ALatinWordTakesPatternBreaksOnlyWithHyphenationOn) {
  const ScopedLanguageHyphenator english(Hyphenator::forLanguage("en"));
  ASSERT_NE(nullptr, Hyphenator::languageHyphenator());
  const std::vector<std::string> words = {"မြန်မာ", "international", "ကာ"};
  // 6 + 1 + 13 code points: "international" does not fit after the phrase, but its first syllables do.
  const Lines off = layOutWords(words, 150);
  ASSERT_GE(off.size(), 2u);
  EXPECT_EQ(off[0], (std::vector<std::string>{"မြန်မာ"}));
  EXPECT_EQ("international", off[1][0]);
  const Lines on = layOutWords(words, 150, true);
  ASSERT_GE(on.size(), 2u);
  ASSERT_EQ(2u, on[0].size());
  EXPECT_EQ('-', on[0][1].back());
  EXPECT_EQ(0u, std::string("international").find(on[0][1].substr(0, on[0][1].size() - 1)));
}

// A Latin word wider than the line is still split, as the paragraph breaker of a Latin paragraph splits it.
TEST_F(MyanmarLineBreak, ALatinWordWiderThanTheLineIsStillSplit) {
  const Lines lines = layOutWords({"ကာ", "abcdefghijklmnop"}, 100);
  ASSERT_GE(lines.size(), 3u);
  EXPECT_EQ(lines[0], (std::vector<std::string>{"ကာ"}));
  EXPECT_EQ('-', lines[1].back().back());
}

// The candidates are measured in order until one is too wide: the widest that fits is the one before it. A phrase
// twice as long, cut at the same place, costs the breaker no more measurements.
TEST_F(MyanmarLineBreak, TheCandidateSearchStopsAtTheFirstPrefixThatIsTooWide) {
  static int advances = 0;
  GfxRenderer::onTextAdvance = [] { ++advances; };
  const std::string filler(75, 'a');  // 750 px of the line's 800, and its space: 45 px left, two syllables
  const auto measured = [&](const int syllables) {
    std::string phrase;
    for (int s = 0; s < syllables; ++s) phrase += "ကာ";
    advances = 0;
    const Lines lines = layOutWords({filler, phrase}, 800);
    EXPECT_EQ(2u, lines.size()) << syllables;
    if (!lines.empty()) {
      EXPECT_EQ(lines[0], (std::vector<std::string>{filler, "ကာကာ"})) << syllables;
    }
    return advances;
  };
  const int twenty = measured(20);
  const int forty = measured(40);
  GfxRenderer::onTextAdvance = nullptr;
  EXPECT_EQ(twenty, forty);
}

// A cut between Myanmar syllables is how a Burmese line ends, not a hyphenation: the parser's page-bottom retry, which
// would set the phrase whole on the next page and leave this page's last line a phrase short, is never asked for it.
TEST_F(MyanmarLineBreak, APageBottomKeepsTheCut) {
  ParsedText text(false, /*hyphenationEnabled=*/true, leftAligned());
  for (const char* word : {"ကက", "ကာကာကာကာကာကာ"}) text.addWord(word, EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  Lines lines;
  bool asked = false;
  text.layoutAndExtractLines(
      renderer, kFontId, 100,
      [&](std::unique_ptr<TextBlock> line, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry) {  // what the parser does at a page bottom
          asked = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        std::vector<std::string>& words = lines.emplace_back();
        for (uint16_t i = 0; i < line->wordCount(); i++) words.emplace_back(line->wordText(i));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  EXPECT_FALSE(asked);
  EXPECT_EQ(lines, (Lines{{"ကက", "ကာကာကာ"}, {"ကာကာကာ"}}));
}
