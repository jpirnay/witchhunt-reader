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
int runLength(const std::string& marker, const std::vector<std::string>& words, bool& done) {
  size_t pos = 0;
  int used = 0;
  for (const auto& word : words) {
    if (!LinkMarkerMatch::consumeToken(marker.c_str(), marker.size(), pos, word.c_str(), word.size())) break;
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
