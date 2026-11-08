// BidiUtilsTest — the witchhunt deviations from upstream's BidiUtils:
// applyBidiVisual() does not shape Arabic, the divine-name match is gone from
// the emission loop, and the shared workspace can be released.
//
// Inputs are codepoint arrays in LOGICAL order; expectations are codepoint
// arrays in VISUAL order (left to right, as drawn on screen).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BidiUtils.h"
#include "Utf8.h"

extern "C" {
#include "minibidi.h"
}

namespace {

using CP = std::vector<uint32_t>;

std::string encode(const CP& codepoints) {
  std::string utf8;
  for (const uint32_t cp : codepoints) utf8AppendCodepoint(cp, utf8);
  return utf8;
}

CP decode(const std::string& utf8) {
  CP codepoints;
  auto* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp) break;
    codepoints.push_back(cp);
  }
  return codepoints;
}

CP visual(const CP& logical, const int level = -1) {
  const std::string in = encode(logical);
  std::string out;
  if (!BidiUtils::applyBidiVisual(in.c_str(), out, level)) return decode(in);
  return decode(out);
}

}  // namespace

TEST(BidiUtils, ApplyBidiVisualDoesNotShapeArabic) {
  EXPECT_EQ(visual({0x0633, 0x0644, 0x0627, 0x0645}), (CP{0x0645, 0x0627, 0x0644, 0x0633}));
}

TEST(BidiUtils, ApplyBidiVisualLeavesLamAlefApart) { EXPECT_EQ(visual({0x0644, 0x0627}), (CP{0x0627, 0x0644})); }

TEST(BidiUtils, ApplyBidiVisualKeepsTheDivineName) {
  EXPECT_EQ(visual({0x05D9, 0x05D4, 0x05D5, 0x05D4}), (CP{0x05D4, 0x05D5, 0x05D4, 0x05D9}));
}

TEST(BidiUtils, ShapedVariantStillShapes) {
  const std::string in = encode({0x0644, 0x0627});
  std::string out;
  ASSERT_TRUE(BidiUtils::applyBidiVisualShaped(in.c_str(), out));
  EXPECT_EQ(decode(out), (CP{0xFEFB}));
}

TEST(BidiUtils, NiqqudFollowsItsBaseInLogicalOrder) {
  EXPECT_EQ(visual({0x05E9, 0x05B8, 0x05C1, 0x05DC, 0x05D5, 0x05B9, 0x05DD}),
            (CP{0x05DD, 0x05D5, 0x05B9, 0x05DC, 0x05E9, 0x05B8, 0x05C1}));
}

TEST(BidiUtils, BracketsMirrorInARightToLeftRun) {
  EXPECT_EQ(visual({0x0028, 0x05E9, 0x05DC, 0x05D5, 0x05DD, 0x0029}, 1),
            (CP{0x0028, 0x05DD, 0x05D5, 0x05DC, 0x05E9, 0x0029}));
}

TEST(BidiUtils, ATokenPastTheLineLimitIsLeftUnprocessed) {
  const std::string in = encode(CP(129, 0x05D0));
  std::string out = "x";
  EXPECT_FALSE(BidiUtils::applyBidiVisual(in.c_str(), out));
  EXPECT_EQ(out, "x");
}

TEST(BidiUtils, TheWorkspaceComesBackAfterRelease) {
  const CP logical{0x05E9, 0x05DC, 0x05D5, 0x05DD};
  const CP expected{0x05DD, 0x05D5, 0x05DC, 0x05E9};
  EXPECT_EQ(visual(logical), expected);
  BidiUtils::releaseWorkspace();
  EXPECT_EQ(visual(logical), expected);
  BidiUtils::releaseWorkspace();
  BidiUtils::releaseWorkspace();
  EXPECT_EQ(visual(logical), expected);
}

// The words as the line pass hands them: views of their text.
bool wordOrder(const std::vector<std::string>& texts, const bool paragraphIsRtl, std::vector<uint16_t>& order) {
  const std::vector<std::string_view> words(texts.begin(), texts.end());
  return BidiUtils::computeVisualWordOrder(words.data(), words.size(), paragraphIsRtl, order);
}

TEST(BidiUtils, ComputeVisualWordOrderReversesARightToLeftLine) {
  const std::vector<std::string> words{encode({0x05E9, 0x05DC, 0x05D5, 0x05DD}),
                                       encode({0x05E2, 0x05D5, 0x05DC, 0x05DD})};
  std::vector<uint16_t> order;
  ASSERT_TRUE(wordOrder(words, true, order));
  EXPECT_EQ(order, (std::vector<uint16_t>{1, 0}));
}

TEST(BidiUtils, ComputeVisualWordOrderKeepsAnEmbeddedLatinRun) {
  const std::vector<std::string> words{"abc", encode({0x05E9, 0x05DC, 0x05D5, 0x05DD}),
                                       encode({0x05E2, 0x05D5, 0x05DC, 0x05DD}), "def"};
  std::vector<uint16_t> order;
  ASSERT_TRUE(wordOrder(words, false, order));
  EXPECT_EQ(order, (std::vector<uint16_t>{0, 2, 1, 3}));
}

TEST(BidiUtils, ComputeVisualWordOrderSkipsTheTransparentMarks) {
  // A point (U+05B8) and a fatha (U+064E) inside the words: the same order as without them, and a view's bytes
  // past its end are never read (the texts carry no NUL of their own).
  const std::vector<std::string> words{"abc", encode({0x05E9, 0x05B8, 0x05DC, 0x05D5, 0x05DD}),
                                       encode({0x0639, 0x064E, 0x0631, 0x0628}), "def"};
  std::vector<uint16_t> order;
  ASSERT_TRUE(wordOrder(words, false, order));
  EXPECT_EQ(order, (std::vector<uint16_t>{0, 2, 1, 3}));
  // A word that is marks alone counts as no character: the line is its other words.
  const std::vector<std::string> marksOnly{encode({0x05B8}), encode({0x05E9, 0x05DC}), encode({0x05E2, 0x05DC})};
  ASSERT_TRUE(wordOrder(marksOnly, true, order));
  EXPECT_EQ(order, (std::vector<uint16_t>{2, 1, 0}));
}

TEST(BidiUtils, L4MirrorsInLogicalOrderBeforeTheLineIsReordered) {
  // "(1999)," in a right-to-left paragraph
  EXPECT_EQ(visual({'(', '1', '9', '9', '9', ')', ','}, 1), (CP{',', '(', '1', '9', '9', '9', ')'}));
  EXPECT_EQ(visual({'1', '9', '9', '9', ')'}, 1), (CP{'(', '1', '9', '9', '9'}));
  EXPECT_EQ(visual({'(', 0x05E9, 0x05DC, 0x05D5, 0x05DD, ')', ' ', 'a', 'b', 'c'}, 1),
            (CP{'a', 'b', 'c', ' ', '(', 0x05DD, 0x05D5, 0x05DC, 0x05E9, ')'}));
}

TEST(BidiUtils, QamatsQatanStaysAfterItsBase) {
  EXPECT_EQ(visual({0x05DB, 0x05BC, 0x05C7, 0x05DC}, 1), (CP{0x05DC, 0x05DB, 0x05BC, 0x05C7}));
}

TEST(BidiUtils, AMalformedByteDoesNotTruncateTheToken) {
  const std::string in = std::string(encode({0x05E9})) + "\xFF" + encode({0x05DC});
  std::string out;
  ASSERT_TRUE(BidiUtils::applyBidiVisual(in.c_str(), out, 1));
  EXPECT_EQ(decode(out), (CP{0x05DC, 0xFFFD, 0x05E9}));
}

// ---- witchhunt deviations 11 and 12: the line pass's entry points ----

namespace {

struct Ordered {
  bool ok;
  std::vector<uint16_t> visual;
  CP drawn;
};

Ordered order(const CP& logical, const int level) {
  Ordered r{false, std::vector<uint16_t>(logical.size(), 0xBEEF), CP(logical.size(), 0xBEEF)};
  r.ok = BidiUtils::orderLine(logical.data(), logical.size(), level, r.visual.data(), r.drawn.data());
  return r;
}

using Idx = std::vector<uint16_t>;

}  // namespace

TEST(BidiUtils, OrderLineReversesARightToLeftRun) {
  const auto r = order({0x05E9, 0x05DC, 0x05D5, 0x05DD}, 1);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{3, 2, 1, 0}));
  EXPECT_EQ(r.drawn, (CP{0x05DD, 0x05D5, 0x05DC, 0x05E9}));
}

TEST(BidiUtils, OrderLineKeepsALeftToRightLine) {
  const auto r = order({'a', 'b', 'c'}, 0);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{0, 1, 2}));
}

TEST(BidiUtils, OrderLineMirrorsBracketsInARightToLeftRun) {
  const auto r = order({0x0028, 0x05D0, 0x0029}, 1);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{2, 1, 0}));
  EXPECT_EQ(r.drawn, (CP{0x0028, 0x05D0, 0x0029}));
}

TEST(BidiUtils, OrderLinePutsMarksAfterTheirBase) {
  const auto r = order({0x05E9, 0x05B8, 0x05C1, 0x05DC, 0x05D5, 0x05B9, 0x05DD}, 1);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{6, 4, 5, 3, 0, 1, 2}));
  EXPECT_EQ(r.drawn, (CP{0x05DD, 0x05D5, 0x05B9, 0x05DC, 0x05E9, 0x05B8, 0x05C1}));
}

TEST(BidiUtils, OrderLineMatchesApplyBidiVisual) {
  const CP lines[] = {
      {'H', 'e', ' ', 's', 'a', 'i', 'd', ' ', 0x05E9, 0x05DC, 0x05D5, 0x05DD, '.'},
      {0x05E9, 0x05DC, 0x05D5, 0x05DD, ' ', '(', 'w', 'o', 'r', 'l', 'd', ')', ' ', '1', '9', '9', '8', '.'},
      {0x05E9, 0x05B8, 0x05C1, 0x05DC, 0x05D5, 0x05B9, 0x05DD, ' ', 0x05E2, 0x05D5, 0x05B9, 0x05DC, 0x05B8, 0x05DD},
      {'a', 'b', 'c', '(', 0x05E9, 0x05DC, 0x05D5, 0x05DD, ')', 'd', 'e', 'f'},
  };
  for (const CP& line : lines) {
    for (int level = 0; level <= 1; level++) {
      const std::string in = encode(line);
      std::string expected;
      ASSERT_TRUE(BidiUtils::applyBidiVisual(in.c_str(), expected, level));
      const auto r = order(line, level);
      ASSERT_TRUE(r.ok);
      EXPECT_EQ(encode(r.drawn), expected) << "level " << level << " line " << in;
    }
  }
}

TEST(BidiUtils, OrderLineRefusesPastTheLimit) {
  const auto r = order(CP(129, 0x05D0), 1);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.visual[0], 0xBEEF);
  EXPECT_EQ(r.drawn[0], 0xBEEFu);
}

TEST(BidiUtils, ABoundWorkspaceIsUsedAndNoHeapOneMade) {
  BidiUtils::releaseWorkspace();
  alignas(4) static uint8_t buffer[BidiUtils::kWorkspaceBytes];
  BidiUtils::bindWorkspace(buffer);
  EXPECT_TRUE(order({0x05E9, 0x05DC, 0x05D5, 0x05DD}, 1).ok);
  EXPECT_FALSE(BidiUtils::heapWorkspaceHeld());

  BidiUtils::bindWorkspace(nullptr);
  EXPECT_TRUE(order({0x05E9, 0x05DC, 0x05D5, 0x05DD}, 1).ok);
  EXPECT_TRUE(BidiUtils::heapWorkspaceHeld());

  BidiUtils::releaseWorkspace();
  EXPECT_FALSE(BidiUtils::heapWorkspaceHeld());
}

TEST(BidiUtils, HasRtlClassFindsRightToLeftLettersAndControls) {
  const std::string yes[] = {std::string("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"),
                             std::string("\xE2\x80\xAE") + "abc",
                             std::string("\xE2\x81\xA7") + "abc",
                             std::string("\xE2\x81\xA8") + "abc",
                             std::string("\xE2\x80\x8F"),
                             std::string("\xD8\x9C"),
                             std::string("\xEF\xAC\xA0")};
  for (const std::string& s : yes) EXPECT_TRUE(BidiUtils::hasRtlClass(s.c_str(), s.size())) << s;

  const std::string no[] = {std::string("abc"),
                            std::string("\xE6\x97\xA5\xE6\x9C\xAC"),
                            std::string("\xE2\x81\xA6") + "abc",
                            std::string("\xE2\x80\x8E") + "abc",
                            std::string("\xE2\x80\x9C") + "q" + "\xE2\x80\x9D",
                            std::string("\xEF\xBC\x8C")};
  for (const std::string& s : no) EXPECT_FALSE(BidiUtils::hasRtlClass(s.c_str(), s.size())) << s;
}

TEST(BidiUtils, StrongLevelOfLettersAndNeutrals) {
  EXPECT_EQ(BidiUtils::strongLevel('a'), 0);
  EXPECT_EQ(BidiUtils::strongLevel(0x05D0), 1);
  EXPECT_EQ(BidiUtils::strongLevel(0x0627), 1);
  EXPECT_EQ(BidiUtils::strongLevel(0x200E), 0);
  EXPECT_EQ(BidiUtils::strongLevel(0x200F), 1);
  EXPECT_EQ(BidiUtils::strongLevel(0x4E00), 0);
  EXPECT_EQ(BidiUtils::strongLevel('1'), -1);
  EXPECT_EQ(BidiUtils::strongLevel(' '), -1);
}

// Deviation 12: a character X9 removed takes the level of the one before it, so it never splits a run in L2.
TEST(BidiUtils, RemovedControlsDoNotSplitARun) {
  const auto r = order({0x202B, 0x05E9, 0x05DC, 0x05D5, 0x05DD, 0x202C, 0x003A, 0x0020, 0x0033, 0x0020, 0x0070, 0x006F,
                        0x0073, 0x0074, 0x0073},
                       0);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{0, 8, 7, 6, 5, 4, 3, 2, 1, 9, 10, 11, 12, 13, 14}));
}

TEST(BidiUtils, AnEmbeddingInsideARightToLeftLine) {
  const auto r = order({0x05E9, 0x05DC, 0x05D5, 0x05DD, 0x0020, 0x202A, 0x0061, 0x0062, 0x0063, 0x202C, 0x0020, 0x05E9,
                        0x05DC, 0x05D5, 0x05DD},
                       1);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{14, 13, 12, 11, 10, 6, 7, 8, 9, 5, 4, 3, 2, 1, 0}));
}

TEST(BidiUtils, AnOverrideStillReversesItsText) {
  const auto r = order({0x0061, 0x0062, 0x0020, 0x202E, 0x0064, 0x0065, 0x0066, 0x202C, 0x0020, 0x0067, 0x0068}, 0);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{0, 1, 2, 3, 7, 6, 5, 4, 8, 9, 10}));
}

// UAX #9 N0 sets the two brackets of a pair and nothing else; the neutrals inside are N1's and N2's.
TEST(BidiUtils, ABracketPairLeavesTheSpaceInsideToN1) {
  // alef mem resh, " (hello world) ", bet in a right-to-left paragraph: the space between the two Latin words has L on
  // both sides, so it is left to right and "hello world" keeps its order inside the mirrored brackets.
  const CP in{0x05D0, 0x05DE, 0x05E8, ' ', '(', 'h', 'e', 'l', 'l',   'o',
              ' ',    'w',    'o',    'r', 'l', 'd', ')', ' ', 0x05D1};
  EXPECT_EQ(visual(in, 1), (CP{0x05D1, ' ', '(', 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', ')', ' ',
                               0x05E8, 0x05DE, 0x05D0}));
}

TEST(BidiUtils, ABracketPairOfOppositeTextInContextTakesTheContext) {
  // UAX #9 N0 c: the pair holds only text opposite to the embedding direction, and the strong text before it is that
  // direction too ("book(s)" after the L of "book"), so both brackets are L and are not mirrored.
  const CP in{0x05D0, ' ', 'b', 'o', 'o', 'k', '(', 's', ')', ' ', 0x05D1};
  EXPECT_EQ(visual(in, 1), (CP{0x05D1, ' ', 'b', 'o', 'o', 'k', '(', 's', ')', ' ', 0x05D0}));
}

TEST(BidiUtils, AnNsmAfterAChangedBracketTakesItsType) {
  // UAX #9 N0: a character whose original class was NSM, directly after a bracket N0 changed, takes the bracket's new
  // type. The closing bracket of "book(s)" becomes L, so the accent after it is L, level 2, and stays in the Latin run.
  const CP in{0x05D0, ' ', 'b', 'o', 'o', 'k', '(', 's', ')', 0x0301, ' ', 0x05D1};
  const auto r = order(in, 1);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.visual, (Idx{11, 10, 2, 3, 4, 5, 6, 7, 8, 9, 1, 0}));
}

TEST(BidiUtils, ArabicSupplementAndExtendedAreStrongRtl) {
  EXPECT_EQ(bidi_class(0x0750), AL);  // Arabic Supplement
  EXPECT_EQ(bidi_class(0x08A0), AL);  // Extended-A
  EXPECT_EQ(bidi_class(0x088F), AL);  // letter, added in Unicode 17
  EXPECT_EQ(bidi_class(0x08CA), NSM);
  EXPECT_EQ(bidi_class(0x08F0), NSM);
  EXPECT_EQ(bidi_class(0x0897), NSM);
  EXPECT_EQ(bidi_class(0x0890), AN);  // number sign
  EXPECT_EQ(bidi_class(0x08E2), AN);  // end of ayah
  EXPECT_EQ(bidi_class(0xFDF2), AL);  // ligature allah
  EXPECT_EQ(bidi_class(0xFDFA), AL);
}
