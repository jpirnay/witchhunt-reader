// Ported from crosspoint-reader PR #2892 (Uri Tauber), commit 25cf99b2, verbatim; the last two tests are ours.
#include <gtest/gtest.h>

#include <iomanip>

#include "lib/Epub/Epub/TokenBoundary.h"

TEST(TokenBoundary, AllowsBreakFollowsTheTwoFlags) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/false, /*noSpaceBefore=*/false));  // ordinary word gap
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/false, /*noSpaceBefore=*/true));   // CJK zero-width gap
  EXPECT_FALSE(TokenBoundary::allowsBreak(/*continues=*/true, /*noSpaceBefore=*/false));  // attached text
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/true, /*noSpaceBefore=*/true));    // after a visible hyphen
}

TEST(TokenBoundary, VisibleExplicitHyphensAllowBreaks) {
  constexpr uint32_t breakableCodepoints[] = {
      '-',    0x058A, 0x2010, 0x2012, 0x2013, 0x2014, 0x2015, 0x2043, 0x207B, 0x208B,
      0x2212, 0x2E17, 0x2E3A, 0x2E3B, 0xFE58, 0xFE63, 0xFF0D, 0x005F, 0x2026,
  };
  for (const uint32_t cp : breakableCodepoints) {
    EXPECT_TRUE(TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) << "codepoint U+" << std::hex << cp;
  }
}

TEST(TokenBoundary, ConditionalAndNonBreakingHyphensKeepTheirSemantics) {
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x00AD));  // soft hyphen
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x2011));  // non-breaking hyphen
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen('.'));
}

// Ours: the per-token Boundary ParsedText stores, and the predicates over it.
TEST(TokenBoundary, ADefaultBoundaryIsAnOrdinarySpace) {
  constexpr TokenBoundary::Boundary b;
  EXPECT_TRUE(TokenBoundary::hasSpace(b));
  EXPECT_TRUE(TokenBoundary::allowsBreak(b));
}

TEST(TokenBoundary, BoundaryKindsMatchTheTwoFlagPredicates) {
  constexpr TokenBoundary::Boundary glued{/*continues=*/true, /*noSpaceBefore=*/false};
  constexpr TokenBoundary::Boundary cjkGap{/*continues=*/false, /*noSpaceBefore=*/true};
  EXPECT_FALSE(TokenBoundary::hasSpace(glued));
  EXPECT_FALSE(TokenBoundary::allowsBreak(glued));
  EXPECT_FALSE(TokenBoundary::hasSpace(cjkGap));
  EXPECT_TRUE(TokenBoundary::allowsBreak(cjkGap));
}

TEST(TokenBoundaryRuby, ARoleChangesNeitherBreakNorSpace) {
  for (const TokenBoundary::Boundary b :
       {TokenBoundary::Boundary{}, TokenBoundary::Boundary{false, true}, TokenBoundary::Boundary{true, false}}) {
    for (const TokenBoundary::RubyRole role : {TokenBoundary::RubyRole::Leader, TokenBoundary::RubyRole::Follower}) {
      const TokenBoundary::Boundary withRole = TokenBoundary::withRubyRole(b, role);
      EXPECT_EQ(TokenBoundary::allowsBreak(b), TokenBoundary::allowsBreak(withRole));
      EXPECT_EQ(TokenBoundary::hasSpace(b), TokenBoundary::hasSpace(withRole));
      EXPECT_EQ(TokenBoundary::rubyRole(withRole), role);
    }
    EXPECT_EQ(TokenBoundary::rubyRole(b), TokenBoundary::RubyRole::None);
  }
}
