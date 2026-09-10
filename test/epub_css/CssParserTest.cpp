#include <cstdio>
#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                         \
  do {                                                                          \
    if ((a) != (b)) {                                                           \
      fprintf(stderr, "  FAIL: %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
      testsFailed++;                                                            \
      return;                                                                   \
    }                                                                           \
  } while (0)

#define ASSERT_TRUE(cond)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      fprintf(stderr, "  FAIL: %s:%d: %s is false\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                              \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define PASS() testsPassed++

void testInlineLineThrough() {
  printf("testInlineLineThrough...\n");
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: line-through");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::LineThrough);
  PASS();
}

void testInlineUnderlineLineThrough() {
  printf("testInlineUnderlineLineThrough...\n");
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: underline line-through");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::UnderlineLineThrough);
  PASS();
}

void testInlineLineThroughUnderlineOrderInsensitive() {
  printf("testInlineLineThroughUnderlineOrderInsensitive...\n");
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: line-through underline");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::UnderlineLineThrough);
  PASS();
}

void testInlineTextDecorationNormalization() {
  printf("testInlineTextDecorationNormalization...\n");
  const CssStyle style = CssParser::parseInlineStyle("TEXT-DECORATION : LINE-THROUGH ;");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::LineThrough);
  PASS();
}

// `img { height: 100%; ...; height: auto; width: 100% }` — the later `auto` has to win. Dropping
// it as unparseable left the 100% standing, which sized the image box to the whole viewport while
// the decoder filled only the aspect-correct top of it; the rest replayed as a black band.
void testImageHeightAutoClearsEarlierLength() {
  printf("testImageHeightAutoClearsEarlierLength...\n");
  const CssStyle style = CssParser::parseInlineStyle("height: 100%; width: 100%; height: auto");
  ASSERT_TRUE(style.hasImageWidth());
  ASSERT_TRUE(!style.hasImageHeight());
  PASS();
}

// Order still matters the other way round: a length after the auto wins.
void testImageHeightLengthAfterAutoWins() {
  printf("testImageHeightLengthAfterAutoWins...\n");
  const CssStyle style = CssParser::parseInlineStyle("height: auto; height: 2em");
  ASSERT_TRUE(style.hasImageHeight());
  ASSERT_EQ(style.imageHeight.unit, CssUnit::Em);
  PASS();
}

// The auto marker must survive the cascade merge, so a more specific rule saying `width: auto`
// clears a length a less specific one set.
void testImageWidthAutoOverridesLowerPriorityRule() {
  printf("testImageWidthAutoOverridesLowerPriorityRule...\n");
  CssStyle resolved = CssParser::parseInlineStyle("width: 50%");
  resolved.applyOver(CssParser::parseInlineStyle("width: auto"));
  ASSERT_TRUE(!resolved.hasImageWidth());
  PASS();
}

// Unsupported keywords stay ignored — only auto and the CSS-wide keywords that resolve to it
// clear the property.
void testImageHeightUnknownKeywordIgnored() {
  printf("testImageHeightUnknownKeywordIgnored...\n");
  const CssStyle style = CssParser::parseInlineStyle("height: 100%; height: fit-content");
  ASSERT_TRUE(style.hasImageHeight());
  ASSERT_EQ(style.imageHeight.unit, CssUnit::Percent);
  PASS();
}

void testImageWidthImportant() {
  printf("testImageWidthImportant...\n");
  const CssStyle style = CssParser::parseInlineStyle("width: 50% !important");
  ASSERT_TRUE(style.hasImageWidth());
  ASSERT_EQ(style.imageWidth.unit, CssUnit::Percent);
  ASSERT_EQ(style.imageWidth.value, 50.0f);
  PASS();
}

// Regression: `!important` used to be stripped only by the dozen properties that called
// stripTrailingImportant themselves. Everything else compared the marker as part of the value
// and silently dropped the declaration. crosspoint-reader PR #3221.
void testImportantStrippedForEveryProperty() {
  printf("testImportantStrippedForEveryProperty...\n");

  const CssStyle align = CssParser::parseInlineStyle("text-align: center !important");
  ASSERT_TRUE(align.hasTextAlign());
  ASSERT_EQ(static_cast<int>(align.textAlign), static_cast<int>(CssTextAlign::Center));

  const CssStyle weight = CssParser::parseInlineStyle("font-weight: bold !important");
  ASSERT_TRUE(weight.hasFontWeight());
  ASSERT_EQ(static_cast<int>(weight.fontWeight), static_cast<int>(CssFontWeight::Bold));

  const CssStyle decoration = CssParser::parseInlineStyle("text-decoration: underline !important");
  ASSERT_TRUE(decoration.hasTextDecoration());
  ASSERT_EQ(static_cast<int>(decoration.textDecoration), static_cast<int>(CssTextDecoration::Underline));

  const CssStyle indent = CssParser::parseInlineStyle("text-indent: 2em !important");
  ASSERT_TRUE(indent.hasTextIndent());
  ASSERT_EQ(indent.textIndent.value, 2.0f);

  // Shorthand: the marker has to go before the value is split on whitespace, or the last
  // component parses as "0 !important" and the whole declaration is dropped.
  const CssStyle margin = CssParser::parseInlineStyle("margin: 0 !important");
  ASSERT_TRUE(margin.hasMarginTop());
  ASSERT_EQ(margin.marginTop.value, 0.0f);

  PASS();
}

int main() {
  printf("=== EPUB CSS Parser Tests ===\n\n");

  testInlineLineThrough();
  testInlineUnderlineLineThrough();
  testInlineLineThroughUnderlineOrderInsensitive();
  testInlineTextDecorationNormalization();
  testImageHeightAutoClearsEarlierLength();
  testImageHeightLengthAfterAutoWins();
  testImageWidthAutoOverridesLowerPriorityRule();
  testImageHeightUnknownKeywordIgnored();
  testImageWidthImportant();
  testImportantStrippedForEveryProperty();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
