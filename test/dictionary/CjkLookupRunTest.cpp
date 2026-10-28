#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "CjkLookupRun.h"

namespace {

using Script = CjkLookupRun::Script;

Script scriptOf(const std::string& s) { return CjkLookupRun::scriptOf(s.data(), s.size()); }

CjkLookupRun runOf(const std::vector<std::string>& words) {
  CjkLookupRun run;
  for (const auto& w : words) {
    if (!run.append(w.data(), w.size())) break;
  }
  return run;
}

}  // namespace

TEST(CjkLookupRun, TheFirstKeptCodepointDecidesTheScript) {
  EXPECT_EQ(Script::Unspaced, scriptOf("大学"));
  EXPECT_EQ(Script::Unspaced, scriptOf("「大"));  // the opening bracket is skipped
  EXPECT_EQ(Script::Unspaced, scriptOf("コーヒー"));
  EXPECT_EQ(Script::Hangul, scriptOf("학교에서,"));
  EXPECT_EQ(Script::Other, scriptOf("house"));
  EXPECT_EQ(Script::Other, scriptOf("A大"));
  EXPECT_EQ(Script::Other, scriptOf("。"));
  EXPECT_EQ(Script::Other, scriptOf(""));
}

TEST(CjkLookupRun, RunsFollowTheSpecRules) {
  const struct {
    std::vector<std::string> words;
    const char* run;
  } cases[] = {
      {{"大", "学", "生", "。", "他"}, "大学生"},  // stops at 。
      {{"大学。", "他"}, "大学"},                  // punctuation glued inside a word stops it too
      {{"「大", "学"}, "大学"},                    // only the first word's leading bracket is skipped
      {{"大", "「学"}, "大"},
      {{"時", "々"}, "時々"},
      {{"〆", "切"}, "〆切"},
      {{"コ", "ー", "ヒ", "ー"}, "コーヒー"},
      {{"大", "A"}, "大"},
      {{"大", "(学"}, "大"},       // a later word's leading ASCII punctuation stops the run
      {{"大", "", "学"}, "大学"},  // an empty word adds nothing and leaves the run open
      {{"大", "학"}, "大"},
      {{"大", "\u3000", "学"}, "大"},
      {{"１", "９", "世", "紀"}, "１９世紀"},  // fullwidth digits are not punctuation
      {{"학교에서,"}, "학교에서"},
      {{"학교", "에서"}, "학교"},  // Hangul: the token itself
  };
  for (const auto& c : cases) EXPECT_STREQ(runOf(c.words).text(), c.run) << c.run;
}

TEST(CjkLookupRun, TheRunStopsAtTwelveCodepoints) {
  const CjkLookupRun run = runOf(std::vector<std::string>(14, "中"));
  EXPECT_EQ(run.codepoints(), 12);
  EXPECT_EQ(run.bytes(), 36);
}

TEST(CjkLookupRun, AppendSaysWhenTheRunCloses) {
  CjkLookupRun run;
  EXPECT_TRUE(run.append("大", 3));
  EXPECT_FALSE(run.append("。", 3));
  CjkLookupRun hangul;
  EXPECT_FALSE(hangul.append("학교", 6));  // a Hangul run never takes a second word
}

TEST(CjkLookupRun, MatchLengthsMapBackToWords) {
  const CjkLookupRun run = runOf({"「大", "学", "生活"});
  EXPECT_STREQ(run.text(), "大学生活");
  EXPECT_EQ(run.wordsCovering(3), 1);
  EXPECT_EQ(run.wordsCovering(6), 2);
  EXPECT_EQ(run.wordsCovering(9), 3);
  EXPECT_EQ(run.wordsCovering(12), 3);
  EXPECT_EQ(run.wordsCovering(4), 0);  // not the end of a codepoint
  EXPECT_EQ(run.wordsCovering(0), 0);
}

TEST(CjkLookupRun, AnEmptyWordStillCountsAsAWord) {
  const CjkLookupRun run = runOf({"大", "", "学"});
  EXPECT_EQ(run.wordsCovering(3), 1);
  EXPECT_EQ(run.wordsCovering(6), 3);  // 学 is the page's third word
}

TEST(CjkLookupRun, ATruncatedSequenceClosesTheRun) {
  CjkLookupRun run;
  EXPECT_FALSE(run.append("大\xE5\xAD", 5));  // 学 cut off after two of its three bytes
  EXPECT_STREQ(run.text(), "大");
  EXPECT_EQ(run.bytes(), 3);
  EXPECT_EQ(Script::Other, CjkLookupRun::scriptOf("\xE5\xAD", 2));
}

TEST(CjkLookupRun, SupplementaryIdeographsCountAsOneCodepoint) {
  const CjkLookupRun run = runOf({"\U00020000", "\U00020001"});
  EXPECT_EQ(run.codepoints(), 2);
  EXPECT_EQ(run.bytes(), 8);
}
