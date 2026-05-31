#include <cstdio>
#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

// Minimal test harness matching the style of CssParserTest.cpp
static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                              \
  do {                                                                               \
    if ((a) != (b)) {                                                                \
      fprintf(stderr, "  FAIL: %s:%d  %s == %s  (got %d, expected %d)\n",           \
              __FILE__, __LINE__, #a, #b, (int)(a), (int)(b));                      \
      testsFailed++;                                                                 \
      return;                                                                        \
    }                                                                                \
  } while (0)

#define ASSERT_TRUE(cond)                                                             \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      fprintf(stderr, "  FAIL: %s:%d  %s is false\n", __FILE__, __LINE__, #cond);   \
      testsFailed++;                                                                  \
      return;                                                                         \
    }                                                                                 \
  } while (0)

#define ASSERT_FALSE(cond)  ASSERT_TRUE(!(cond))

static bool assertFloatNear(float got, float expected, float tol,
                             const char* expr, const char* expStr,
                             const char* file, int line) {
  if (got < expected - tol || got > expected + tol) {
    fprintf(stderr, "  FAIL: %s:%d  %s ≈ %s  (got %.4f, expected %.4f ±%.4f)\n",
            file, line, expr, expStr, got, expected, tol);
    testsFailed++;
    return false;
  }
  return true;
}
#define ASSERT_FLOAT_NEAR(a, b, tol) \
  do { if (!assertFloatNear((float)(a), (float)(b), (float)(tol), #a, #b, __FILE__, __LINE__)) return; } while(0)

#define PASS() do { testsPassed++; } while (0)

// ============================================================================
// Gap 3: list-style-type / list-style: none
// ============================================================================

void testListStyleTypeNone() {
  printf("testListStyleTypeNone...\n");
  const CssStyle style = CssParser::parseInlineStyle("list-style-type: none");
  ASSERT_TRUE(style.hasListStyleNone());
  ASSERT_TRUE(style.listStyleNone);
  PASS();
}

void testListStyleShorthandNone() {
  printf("testListStyleShorthandNone...\n");
  const CssStyle style = CssParser::parseInlineStyle("list-style: none");
  ASSERT_TRUE(style.hasListStyleNone());
  ASSERT_TRUE(style.listStyleNone);
  PASS();
}

void testListStyleTypeDisc_notNone() {
  printf("testListStyleTypeDisc_notNone...\n");
  const CssStyle style = CssParser::parseInlineStyle("list-style-type: disc");
  // disc is the default — either not set or explicitly false
  ASSERT_FALSE(style.hasListStyleNone() && style.listStyleNone);
  PASS();
}

// ============================================================================
// Gap 4: page-break-before / page-break-after
// ============================================================================

void testPageBreakBeforeAlways() {
  printf("testPageBreakBeforeAlways...\n");
  const CssStyle style = CssParser::parseInlineStyle("page-break-before: always");
  ASSERT_TRUE(style.hasPageBreakBefore());
  ASSERT_TRUE(style.pageBreakBefore);
  PASS();
}

void testPageBreakAfterAlways() {
  printf("testPageBreakAfterAlways...\n");
  const CssStyle style = CssParser::parseInlineStyle("page-break-after: always");
  ASSERT_TRUE(style.hasPageBreakAfter());
  ASSERT_TRUE(style.pageBreakAfter);
  PASS();
}

void testBreakBeforePage_css3() {
  printf("testBreakBeforePage_css3...\n");
  const CssStyle style = CssParser::parseInlineStyle("break-before: page");
  ASSERT_TRUE(style.hasPageBreakBefore());
  ASSERT_TRUE(style.pageBreakBefore);
  PASS();
}

void testBreakAfterPage_css3() {
  printf("testBreakAfterPage_css3...\n");
  const CssStyle style = CssParser::parseInlineStyle("break-after: page");
  ASSERT_TRUE(style.hasPageBreakAfter());
  ASSERT_TRUE(style.pageBreakAfter);
  PASS();
}

void testPageBreakBeforeAuto_notSet() {
  printf("testPageBreakBeforeAuto_notSet...\n");
  const CssStyle style = CssParser::parseInlineStyle("page-break-before: auto");
  // auto should not set the break flag
  ASSERT_FALSE(style.hasPageBreakBefore() && style.pageBreakBefore);
  PASS();
}

// ============================================================================
// Gap 2: line-height
// ============================================================================

void testLineHeightUnitless() {
  printf("testLineHeightUnitless...\n");
  const CssStyle style = CssParser::parseInlineStyle("line-height: 1.5");
  ASSERT_TRUE(style.hasLineHeight());
  // 1.5 normalised around 1.5 base => 100%. Allow ±5%.
  ASSERT_FLOAT_NEAR(style.lineHeightMultiplier, 1.0f, 0.05f);
  PASS();
}

void testLineHeightPercent() {
  printf("testLineHeightPercent...\n");
  const CssStyle style = CssParser::parseInlineStyle("line-height: 150%");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_FLOAT_NEAR(style.lineHeightMultiplier, 1.0f, 0.05f);
  PASS();
}

void testLineHeightEm() {
  printf("testLineHeightEm...\n");
  const CssStyle style = CssParser::parseInlineStyle("line-height: 1.5em");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_FLOAT_NEAR(style.lineHeightMultiplier, 1.0f, 0.05f);
  PASS();
}

void testLineHeightSmallerValue() {
  printf("testLineHeightSmallerValue...\n");
  // line-height: 1.0 (compact) should give a multiplier < 1.0
  const CssStyle style = CssParser::parseInlineStyle("line-height: 1.0");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_TRUE(style.lineHeightMultiplier < 1.0f);
  PASS();
}

void testLineHeightLargerValue() {
  printf("testLineHeightLargerValue...\n");
  // line-height: 2.0 (spacious) should give a multiplier > 1.0
  const CssStyle style = CssParser::parseInlineStyle("line-height: 2.0");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_TRUE(style.lineHeightMultiplier > 1.0f);
  PASS();
}

void testLineHeightClampMin() {
  printf("testLineHeightClampMin...\n");
  // Very small value must be clamped to minimum (0.7)
  const CssStyle style = CssParser::parseInlineStyle("line-height: 0.1");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_TRUE(style.lineHeightMultiplier >= 0.7f);
  PASS();
}

void testLineHeightClampMax() {
  printf("testLineHeightClampMax...\n");
  // Very large value must be clamped to maximum (2.0)
  const CssStyle style = CssParser::parseInlineStyle("line-height: 10.0");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_TRUE(style.lineHeightMultiplier <= 2.0f);
  PASS();
}

void testLineHeightNormal_notSet() {
  printf("testLineHeightNormal_notSet...\n");
  // 'normal' keyword should not set a line-height override
  const CssStyle style = CssParser::parseInlineStyle("line-height: normal");
  ASSERT_FALSE(style.hasLineHeight());
  PASS();
}

// ============================================================================
// Gap 1: font-size (heading scaling)
// ============================================================================

void testFontSizePercent160() {
  printf("testFontSizePercent160...\n");
  const CssStyle style = CssParser::parseInlineStyle("font-size: 160%");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  ASSERT_FLOAT_NEAR(style.fontSizeMultiplier, 1.6f, 0.05f);
  PASS();
}

void testFontSizeEm() {
  printf("testFontSizeEm...\n");
  const CssStyle style = CssParser::parseInlineStyle("font-size: 1.4em");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  ASSERT_FLOAT_NEAR(style.fontSizeMultiplier, 1.4f, 0.05f);
  PASS();
}

void testFontSizeSmaller() {
  printf("testFontSizeSmaller...\n");
  const CssStyle style = CssParser::parseInlineStyle("font-size: 80%");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  ASSERT_FLOAT_NEAR(style.fontSizeMultiplier, 0.8f, 0.05f);
  PASS();
}

// ============================================================================
// main
// ============================================================================

int main() {
  printf("=== CSS Gaps Tests ===\n\n");

  printf("--- Gap 3: list-style-type: none ---\n");
  testListStyleTypeNone();
  testListStyleShorthandNone();
  testListStyleTypeDisc_notNone();

  printf("\n--- Gap 4: page-break ---\n");
  testPageBreakBeforeAlways();
  testPageBreakAfterAlways();
  testBreakBeforePage_css3();
  testBreakAfterPage_css3();
  testPageBreakBeforeAuto_notSet();

  printf("\n--- Gap 2: line-height ---\n");
  testLineHeightUnitless();
  testLineHeightPercent();
  testLineHeightEm();
  testLineHeightSmallerValue();
  testLineHeightLargerValue();
  testLineHeightClampMin();
  testLineHeightClampMax();
  testLineHeightNormal_notSet();

  printf("\n--- Gap 1: font-size multiplier ---\n");
  testFontSizePercent160();
  testFontSizeEm();
  testFontSizeSmaller();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
