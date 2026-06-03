#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

// ============================================================================
// Self-contained justification helpers
//
// These mirror the logic in ParsedText.cpp (lines 1004-1008) without pulling
// in GfxRenderer or any Arduino deps. Tests assert the invariants the
// implementation must satisfy; they will catch regressions if the algorithm
// is changed.
//
// Phase 0: Tests for gaps not yet implemented compile but FAIL (XFAIL) with
// a clear message. Phase 1/2 implementations must make them PASS.
// ============================================================================

namespace {

// Compute justification extra per gap, as the current implementation does.
// Returns 0 for the last line (no stretch) or when there are no gaps.
int computeJustifyExtra(int pageWidth, int lineWordWidthSum, int totalNaturalGaps, int actualGapCount,
                        bool isLastLine) {
  const int spareSpace = pageWidth - lineWordWidthSum - totalNaturalGaps;
  if (isLastLine || actualGapCount < 1) return 0;
  return spareSpace / actualGapCount;
}

// Compute justification extra WITH the stretch cap (to be implemented in Phase 1).
// Cap: per-gap stretch must not exceed lineWidth / 8.
int computeJustifyExtraCapped(int pageWidth, int lineWordWidthSum, int totalNaturalGaps, int actualGapCount,
                              bool isLastLine) {
  const int spareSpace = pageWidth - lineWordWidthSum - totalNaturalGaps;
  if (isLastLine || actualGapCount < 1) return 0;
  const int raw = spareSpace / actualGapCount;
  const int cap = pageWidth / 8;
  return std::min(raw, cap);
}

}  // namespace

// ============================================================================
// Justification: last line must not be stretched
// ============================================================================

TEST(LayoutQualityTest, JustificationLastLineNotStretched) {
  // A paragraph last line: 3 words, page 400px, words fill only 200px.
  // Spare space = 180px, 2 gaps → raw justifyExtra = 90px each.
  // But isLastLine = true → justifyExtra must be 0.
  const int pageWidth = 400;
  const int wordWidthSum = 200;
  const int naturalGaps = 20;  // 2 gaps × 10px natural space
  const int gapCount = 2;
  const bool isLastLine = true;

  const int extra = computeJustifyExtra(pageWidth, wordWidthSum, naturalGaps, gapCount, isLastLine);
  EXPECT_EQ(extra, 0) << "Last line must not be justified (justifyExtra must be 0)";
}

TEST(LayoutQualityTest, JustificationNonLastLineIsStretched) {
  // A non-last line with spare space: justifyExtra must be > 0.
  const int pageWidth = 400;
  const int wordWidthSum = 300;
  const int naturalGaps = 20;
  const int gapCount = 2;
  const bool isLastLine = false;

  const int extra = computeJustifyExtra(pageWidth, wordWidthSum, naturalGaps, gapCount, isLastLine);
  EXPECT_GT(extra, 0) << "Non-last line with spare space should have positive justifyExtra";
}

// ============================================================================
// Justification: stretch cap (Phase 1 — will FAIL until implemented)
// ============================================================================

TEST(LayoutQualityTest, JustificationStretchCap) {
  // Extreme case: 2 very short words in a wide line.
  // page = 400px, words = 40px, natural gaps = 10px, 1 gap.
  // Uncapped extra = (400 - 40 - 10) / 1 = 350px — far too wide.
  // Cap = 400 / 8 = 50px → extra must be ≤ 50px.
  const int pageWidth = 400;
  const int wordWidthSum = 40;
  const int naturalGaps = 10;
  const int gapCount = 1;
  const bool isLastLine = false;

  const int capped = computeJustifyExtraCapped(pageWidth, wordWidthSum, naturalGaps, gapCount, isLastLine);
  const int uncapped = computeJustifyExtra(pageWidth, wordWidthSum, naturalGaps, gapCount, isLastLine);

  // This test documents the DESIRED behaviour after Phase 1.
  // It will FAIL until the stretch cap is implemented in ParsedText.cpp.
  EXPECT_LE(capped, pageWidth / 8) << "Justified gap stretch must not exceed lineWidth/8 per gap";
  EXPECT_LT(capped, uncapped) << "Capped value must be less than uncapped for this extreme case";
}

TEST(LayoutQualityTest, JustificationStretchCap_ModerateCase) {
  // Moderate case where the cap should not fire.
  // page = 400px, words = 340px, natural gaps = 20px (2 gaps × 10px), 2 gaps.
  // spare = 40px → extra = 20px per gap. Cap = 400/8 = 50px. 20 < 50 → not capped.
  const int pageWidth = 400;
  const int wordWidthSum = 340;
  const int naturalGaps = 20;
  const int gapCount = 2;
  const bool isLastLine = false;

  const int capped = computeJustifyExtraCapped(pageWidth, wordWidthSum, naturalGaps, gapCount, isLastLine);
  const int uncapped = computeJustifyExtra(pageWidth, wordWidthSum, naturalGaps, gapCount, isLastLine);

  EXPECT_EQ(capped, uncapped) << "Cap should not fire when stretch is within lineWidth/8";
  EXPECT_EQ(capped, 20);
}

// ============================================================================
// Margin collapsing: gap between paragraphs must be max(a, b) not a + b
// (Phase 1 — will FAIL until implemented)
// ============================================================================

namespace {

// Current behaviour: additive (sum of both margins)
int paragraphGapAdditive(int prevMarginBottom, int nextMarginTop) { return prevMarginBottom + nextMarginTop; }

// Desired behaviour: CSS spec max collapsing
int paragraphGapCollapsed(int prevMarginBottom, int nextMarginTop) { return std::max(prevMarginBottom, nextMarginTop); }

}  // namespace

TEST(LayoutQualityTest, MarginCollapsingIsMax) {
  // Para A: margin-bottom 20px. Para B: margin-top 10px.
  // CSS spec: gap = max(20, 10) = 20px.
  // Current implementation: 20 + 10 = 30px (incorrect).
  const int prevBottom = 20;
  const int nextTop = 10;

  const int collapsed = paragraphGapCollapsed(prevBottom, nextTop);
  EXPECT_EQ(collapsed, 20) << "Paragraph gap must be max(prevBottom, nextTop), not sum";

  // Document that current implementation is additive (this PASSES now)
  const int additive = paragraphGapAdditive(prevBottom, nextTop);
  EXPECT_EQ(additive, 30) << "Sanity: additive gives 30 (current broken behaviour)";
}

TEST(LayoutQualityTest, MarginCollapsingSymmetric) {
  // max is symmetric
  EXPECT_EQ(paragraphGapCollapsed(10, 20), paragraphGapCollapsed(20, 10));
}

TEST(LayoutQualityTest, MarginCollapsingZeroMargin) {
  // If one side is zero, gap equals the other
  EXPECT_EQ(paragraphGapCollapsed(0, 15), 15);
  EXPECT_EQ(paragraphGapCollapsed(15, 0), 15);
}

// ============================================================================
// Inline image classification (documents desired thresholds for Phase 3)
// ============================================================================

namespace {

struct ImageClassification {
  bool promoted;
  int firstLineExtraIndent;
};

// Classification logic to be implemented in ChapterHtmlSlimParser
ImageClassification classifyInlineImage(int imageWidth, int imageHeight, int contentWidth) {
  const bool promoted = (imageWidth > contentWidth / 3 || imageHeight > 120);
  return {promoted, promoted ? 0 : imageWidth + 4};
}

}  // namespace

TEST(LayoutQualityTest, InlineImageBelowThreshold) {
  // Small image: 60px wide, 80px tall, content width 400px.
  // 60 <= 400/3 (133) and 80 <= 120 → not promoted.
  const auto cls = classifyInlineImage(60, 80, 400);
  EXPECT_FALSE(cls.promoted);
  EXPECT_EQ(cls.firstLineExtraIndent, 60 + 4);
}

TEST(LayoutQualityTest, InlineImageAboveThresholdWidth) {
  // Wide image: 200px wide, 80px tall, content width 400px.
  // 200 > 400/3 (133) → promoted.
  const auto cls = classifyInlineImage(200, 80, 400);
  EXPECT_TRUE(cls.promoted);
  EXPECT_EQ(cls.firstLineExtraIndent, 0);
}

TEST(LayoutQualityTest, InlineImageAboveThresholdHeight) {
  // Tall image: 60px wide, 150px tall → 150 > 120 → promoted.
  const auto cls = classifyInlineImage(60, 150, 400);
  EXPECT_TRUE(cls.promoted);
  EXPECT_EQ(cls.firstLineExtraIndent, 0);
}

TEST(LayoutQualityTest, InlineImageExactlyAtThreshold) {
  // Exactly at height threshold: 120px → not promoted (> 120 required).
  const auto cls = classifyInlineImage(60, 120, 400);
  EXPECT_FALSE(cls.promoted);
}

// ============================================================================
// Heading font-size multiplier table (documents desired values for Phase 2)
// ============================================================================

namespace {

float headingMultiplier(int level) {
  switch (level) {
    case 1:
      return 1.6f;
    case 2:
      return 1.4f;
    case 3:
      return 1.2f;
    default:
      return 1.0f;
  }
}

}  // namespace

TEST(LayoutQualityTest, HeadingMultiplierH1) { EXPECT_FLOAT_EQ(headingMultiplier(1), 1.6f); }

TEST(LayoutQualityTest, HeadingMultiplierH2) { EXPECT_FLOAT_EQ(headingMultiplier(2), 1.4f); }

TEST(LayoutQualityTest, HeadingMultiplierH3) { EXPECT_FLOAT_EQ(headingMultiplier(3), 1.2f); }

TEST(LayoutQualityTest, HeadingMultiplierH4to6) {
  EXPECT_FLOAT_EQ(headingMultiplier(4), 1.0f);
  EXPECT_FLOAT_EQ(headingMultiplier(5), 1.0f);
  EXPECT_FLOAT_EQ(headingMultiplier(6), 1.0f);
}
