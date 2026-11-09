// Matching a page word against an internal link's display text.
//
// Two behaviours carry the weight, and both come from how the parser stores the text:
//   - a noteref is normalised on the way in ("[12]" -> "12") while the page still paints the
//     brackets, so the page word has to be normalised by the same rule;
//   - every internal <a href> is collected, cross-references included, so the text can be several
//     words that together make one tappable run.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "components/LinkMarkerMatch.h"

namespace {

// Feed `words` in reading order and report how many of them the marker consumed, starting at the
// first one. 0 means the run did not start here; a positive count with `done` true is a complete
// match. Mirrors what the reader's scan does.
// `glued[i]`, when given, says page word i + 1 is glued to word i with no space (TextBlock::wordContinues).
int runLength(const std::string& marker, const std::vector<std::string>& words, bool& done,
              const std::vector<bool>& glued = {}) {
  size_t pos = 0;
  int used = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string& word = words[i];
    const bool nextGlued = i < glued.size() && glued[i];
    if (!LinkMarkerMatch::consumeToken(marker.c_str(), marker.size(), pos, word.c_str(), word.size(), nextGlued)) {
      break;
    }
    ++used;
    if (LinkMarkerMatch::complete(marker.c_str(), marker.size(), pos)) {
      done = true;
      return used;
    }
  }
  done = false;
  return used;
}

}  // namespace

TEST(LinkMarkerMatch, PlainNumberMatchesItself) {
  bool done = false;
  EXPECT_EQ(runLength("12", {"12"}, done), 1);
  EXPECT_TRUE(done);
}

// The regression the normalisation exists for: stored "12", painted "[12]".
TEST(LinkMarkerMatch, BracketedPageWordMatchesTheStrippedMarker) {
  for (const char* painted : {"[12]", "[12", "12]", " [12] "}) {
    bool done = false;
    EXPECT_EQ(runLength("12", {painted}, done), 1) << painted;
    EXPECT_TRUE(done) << painted;
  }
}

TEST(LinkMarkerMatch, DifferentNumberDoesNotMatch) {
  bool done = false;
  EXPECT_EQ(runLength("12", {"13"}, done), 0);
  EXPECT_FALSE(done);
  // A prefix is not a match either: "1" must not claim the marker "12".
  EXPECT_EQ(runLength("12", {"1"}, done), 0);
  EXPECT_FALSE(done);
}

// A cross-reference: several page words make one link.
TEST(LinkMarkerMatch, MultiWordLinkTextConsumesEveryWord) {
  bool done = false;
  EXPECT_EQ(runLength("turn to 256", {"turn", "to", "256", "and"}, done), 3);
  EXPECT_TRUE(done);
}

// A run that starts right and then diverges is not a match, and reports how far it got so the
// caller can abandon it rather than record half a link.
TEST(LinkMarkerMatch, PartialRunIsNotComplete) {
  bool done = false;
  EXPECT_EQ(runLength("turn to 256", {"turn", "away"}, done), 1);
  EXPECT_FALSE(done);
}

// Punctuation the layout glued to the marker comes off with the wrappers.
TEST(LinkMarkerMatch, WrappersComeOffBothEnds) {
  size_t start = 0;
  size_t end = 0;
  ASSERT_TRUE(LinkMarkerMatch::core("  [ 12 ]  ", 10, start, end));
  EXPECT_EQ(std::string("  [ 12 ]  ").substr(start, end - start), "12");
}

// A word with no interior is never a marker: it must not match the empty tail of one.
TEST(LinkMarkerMatch, EmptyCoreNeverMatches) {
  size_t start = 0;
  size_t end = 0;
  EXPECT_FALSE(LinkMarkerMatch::core("[]", 2, start, end));
  EXPECT_FALSE(LinkMarkerMatch::core("   ", 3, start, end));
  EXPECT_FALSE(LinkMarkerMatch::core("", 0, start, end));

  bool done = false;
  EXPECT_EQ(runLength("12", {"[]"}, done), 0);
  EXPECT_FALSE(done);
}

// An empty marker is never complete, so a link whose text was lost cannot claim the first word on
// the page.
TEST(LinkMarkerMatch, EmptyMarkerIsNeverComplete) {
  EXPECT_FALSE(LinkMarkerMatch::complete("", 0, 0));
  size_t pos = 0;
  EXPECT_FALSE(LinkMarkerMatch::consumeToken("", 0, pos, "1", 1));
  EXPECT_EQ(pos, 0u);
}

TEST(LinkMarkerMatch, NullInputsAreInert) {
  size_t pos = 0;
  EXPECT_FALSE(LinkMarkerMatch::consumeToken(nullptr, 0, pos, "1", 1));
  EXPECT_FALSE(LinkMarkerMatch::consumeToken("1", 1, pos, nullptr, 0));
  size_t start = 0;
  size_t end = 0;
  EXPECT_FALSE(LinkMarkerMatch::core(nullptr, 0, start, end));
}

// A failed token leaves markerPos where it was, so the caller can restart the run at the next
// page word without carrying state from the abandoned one.
TEST(LinkMarkerMatch, AFailedTokenDoesNotAdvance) {
  const std::string marker = "turn to 256";
  size_t pos = 0;
  ASSERT_TRUE(LinkMarkerMatch::consumeToken(marker.c_str(), marker.size(), pos, "turn", 4));
  const size_t afterFirst = pos;
  EXPECT_FALSE(LinkMarkerMatch::consumeToken(marker.c_str(), marker.size(), pos, "away", 4));
  EXPECT_EQ(pos, afterFirst);
}

// Multi-word text whose own tokens carry brackets still matches word for word.
TEST(LinkMarkerMatch, BracketsInsideAMultiWordMarker) {
  bool done = false;
  EXPECT_EQ(runLength("see [4] below", {"see", "[4]", "below"}, done), 3);
  EXPECT_TRUE(done);
}

// A paragraph that needs bidi keeps its controls in the word text (an inserted isolate pair around a dir="ltr" span, a
// literal mark): the marker is what is between them. Written as UTF-8 escapes: LRI U+2066 = E2 81 A6, PDI U+2069 =
// E2 81 A9, RLM U+200F = E2 80 8F, LRM U+200E = E2 80 8E, ALM U+061C = D8 9C.
TEST(LinkMarkerMatch, ABidiIsolatePairAroundTheNumberIsSkipped) {
  bool done = false;
  EXPECT_EQ(runLength("1",
                      {"\xE2\x81\xA6"
                       "1"
                       "\xE2\x81\xA9"},
                      done),
            1);
  EXPECT_TRUE(done);
}

TEST(LinkMarkerMatch, ATrailingMarkIsSkipped) {
  bool done = false;
  EXPECT_EQ(runLength("12", {"12\xE2\x80\x8F"}, done), 1);
  EXPECT_TRUE(done);
  done = false;
  EXPECT_EQ(runLength("12",
                      {"\xD8\x9C"
                       "[12]\xE2\x80\x8E"},
                      done),
            1);
  EXPECT_TRUE(done);
}

TEST(LinkMarkerMatch, AControlOnTheMarkerSideIsSkippedToo) {
  bool done = false;
  EXPECT_EQ(runLength("1\xE2\x80\x8F", {"1"}, done), 1);
  EXPECT_TRUE(done);
}

TEST(LinkMarkerMatch, AWordOfControlsOnlyDoesNotMatch) {
  bool done = false;
  EXPECT_EQ(runLength("1", {"\xE2\x81\xA6\xE2\x81\xA9"}, done), 0);
  EXPECT_FALSE(done);
  EXPECT_EQ(runLength("1", {"\xE2\x80\x8F"}, done), 0);
}

TEST(LinkMarkerMatch, AControlInsideTheNumberOrOutsideTheSetStillBlocks) {
  bool done = false;
  EXPECT_EQ(runLength("12",
                      {"1\xE2\x80\x8F"
                       "2"},
                      done),
            0);
  EXPECT_EQ(runLength("1",
                      {"\xE2\x81\xA5"
                       "1"},
                      done),
            0);  // U+2065 is not a control
}

// CJK text is stored one page word per character, while a link's text is one token between spaces: the words
// consume the token a piece at a time (#663, docs/contributing/touch-architecture.md: such a link could not be tapped).
TEST(LinkMarkerMatch, ACjkLinkSpreadOverOneWordPerCharacterMatches) {
  bool done = false;
  EXPECT_EQ(runLength("第一章", {"第", "一", "章", "を"}, done), 3);
  EXPECT_TRUE(done);
  EXPECT_EQ(runLength("注1", {"注", "1"}, done), 2);  // a Latin digit after a Han character
  EXPECT_TRUE(done);
  EXPECT_EQ(runLength("see 第一章", {"see", "第", "一", "章"}, done), 4);
  EXPECT_TRUE(done);
}

TEST(LinkMarkerMatch, ACjkRunThatDivergesIsNotComplete) {
  bool done = false;
  EXPECT_EQ(runLength("第一章", {"第", "二", "章"}, done), 1);
  EXPECT_FALSE(done);
  EXPECT_EQ(runLength("第一章", {"第", "一"}, done), 2);  // the page ends first
  EXPECT_FALSE(done);
}

TEST(LinkMarkerMatch, APrefixMatchesOnlyWhereTheParserCouldHaveSplitTheToken) {
  bool done = false;
  // Two separate Latin words never make one token: "1" and "2" are not the link "12".
  EXPECT_EQ(runLength("12", {"1", "2"}, done), 0);
  EXPECT_FALSE(done);
  // Glued with no space (a style that changes mid-word), they do.
  EXPECT_EQ(runLength("12", {"1", "2"}, done, {true}), 2);
  EXPECT_TRUE(done);
  // Hangul is kept whole between spaces: a Korean word is never split per syllable.
  EXPECT_EQ(runLength("가나", {"가", "나"}, done), 0);
  EXPECT_FALSE(done);
  // Never inside a code point: two bytes of 第 are not a prefix of "第一".
  EXPECT_EQ(runLength("第一", {"\xE7\xAC"}, done), 0);
  EXPECT_FALSE(done);
}
