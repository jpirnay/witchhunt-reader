// BidiUtilsTest — the witchhunt deviations from upstream's BidiUtils:
// applyBidiVisual() does not shape Arabic, the divine-name match is gone from
// the emission loop, and the shared workspace can be released.
//
// Inputs are codepoint arrays in LOGICAL order; expectations are codepoint
// arrays in VISUAL order (left to right, as drawn on screen).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BidiUtils.h"
#include "Utf8.h"

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

TEST(BidiUtils, ComputeVisualWordOrderReversesARightToLeftLine) {
  const std::vector<std::string> words{encode({0x05E9, 0x05DC, 0x05D5, 0x05DD}),
                                       encode({0x05E2, 0x05D5, 0x05DC, 0x05DD})};
  std::vector<uint16_t> order;
  ASSERT_TRUE(BidiUtils::computeVisualWordOrder(words, true, order));
  EXPECT_EQ(order, (std::vector<uint16_t>{1, 0}));
}

TEST(BidiUtils, ComputeVisualWordOrderKeepsAnEmbeddedLatinRun) {
  const std::vector<std::string> words{"abc", encode({0x05E9, 0x05DC, 0x05D5, 0x05DD}),
                                       encode({0x05E2, 0x05D5, 0x05DC, 0x05DD}), "def"};
  std::vector<uint16_t> order;
  ASSERT_TRUE(BidiUtils::computeVisualWordOrder(words, false, order));
  EXPECT_EQ(order, (std::vector<uint16_t>{0, 2, 1, 3}));
}
