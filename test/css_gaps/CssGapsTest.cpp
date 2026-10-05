#include <gtest/gtest.h>

#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

// ============================================================================
// Gap 3: list-style-type / list-style: none
// ============================================================================

TEST(CssGapsListStyle, TypeNone) {
  const CssStyle style = CssParser::parseInlineStyle("list-style-type: none");
  ASSERT_TRUE(style.hasListStyleNone());
  ASSERT_TRUE(style.listStyleNone);
}

TEST(CssGapsListStyle, ShorthandNone) {
  const CssStyle style = CssParser::parseInlineStyle("list-style: none");
  ASSERT_TRUE(style.hasListStyleNone());
  ASSERT_TRUE(style.listStyleNone);
}

TEST(CssGapsListStyle, TypeDiscIsNotNone) {
  const CssStyle style = CssParser::parseInlineStyle("list-style-type: disc");
  // disc is the default — either not set or explicitly false
  ASSERT_FALSE(style.hasListStyleNone() && style.listStyleNone);
}

// ============================================================================
// Gap 4: page-break-before / page-break-after
// ============================================================================

TEST(CssGapsPageBreak, BeforeAlways) {
  const CssStyle style = CssParser::parseInlineStyle("page-break-before: always");
  ASSERT_TRUE(style.hasPageBreakBefore());
  ASSERT_TRUE(style.pageBreakBefore);
}

TEST(CssGapsPageBreak, AfterAlways) {
  const CssStyle style = CssParser::parseInlineStyle("page-break-after: always");
  ASSERT_TRUE(style.hasPageBreakAfter());
  ASSERT_TRUE(style.pageBreakAfter);
}

TEST(CssGapsPageBreak, Css3BreakBeforePage) {
  const CssStyle style = CssParser::parseInlineStyle("break-before: page");
  ASSERT_TRUE(style.hasPageBreakBefore());
  ASSERT_TRUE(style.pageBreakBefore);
}

TEST(CssGapsPageBreak, Css3BreakAfterPage) {
  const CssStyle style = CssParser::parseInlineStyle("break-after: page");
  ASSERT_TRUE(style.hasPageBreakAfter());
  ASSERT_TRUE(style.pageBreakAfter);
}

TEST(CssGapsPageBreak, BeforeAutoIsNotSet) {
  const CssStyle style = CssParser::parseInlineStyle("page-break-before: auto");
  // auto should not set the break flag
  ASSERT_FALSE(style.hasPageBreakBefore() && style.pageBreakBefore);
}

// ============================================================================
// Gap 2: line-height
// ============================================================================

TEST(CssGapsLineHeight, Unitless) {
  const CssStyle style = CssParser::parseInlineStyle("line-height: 1.5");
  ASSERT_TRUE(style.hasLineHeight());
  // 1.5 normalised around 1.5 base => 100%. Allow ±5%.
  ASSERT_NEAR(style.lineHeightMultiplier, 1.0f, 0.05f);
}

TEST(CssGapsLineHeight, Percent) {
  const CssStyle style = CssParser::parseInlineStyle("line-height: 150%");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_NEAR(style.lineHeightMultiplier, 1.0f, 0.05f);
}

TEST(CssGapsLineHeight, Em) {
  const CssStyle style = CssParser::parseInlineStyle("line-height: 1.5em");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_NEAR(style.lineHeightMultiplier, 1.0f, 0.05f);
}

TEST(CssGapsLineHeight, SmallerValue) {
  // line-height: 1.0 (compact) should give a multiplier < 1.0
  const CssStyle style = CssParser::parseInlineStyle("line-height: 1.0");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_LT(style.lineHeightMultiplier, 1.0f);
}

TEST(CssGapsLineHeight, LargerValue) {
  // line-height: 2.0 (spacious) should give a multiplier > 1.0
  const CssStyle style = CssParser::parseInlineStyle("line-height: 2.0");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_GT(style.lineHeightMultiplier, 1.0f);
}

TEST(CssGapsLineHeight, ClampMin) {
  // Very small value must be clamped to minimum (0.7)
  const CssStyle style = CssParser::parseInlineStyle("line-height: 0.1");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_GE(style.lineHeightMultiplier, 0.7f);
}

TEST(CssGapsLineHeight, ClampMax) {
  // Very large value must be clamped to maximum (2.0)
  const CssStyle style = CssParser::parseInlineStyle("line-height: 10.0");
  ASSERT_TRUE(style.hasLineHeight());
  ASSERT_LE(style.lineHeightMultiplier, 2.0f);
}

TEST(CssGapsLineHeight, NormalIsNotSet) {
  // 'normal' keyword should not set a line-height override
  const CssStyle style = CssParser::parseInlineStyle("line-height: normal");
  ASSERT_FALSE(style.hasLineHeight());
}

// ============================================================================
// Gap 1: font-size (heading scaling)
// ============================================================================

TEST(CssGapsFontSize, Percent160) {
  const CssStyle style = CssParser::parseInlineStyle("font-size: 160%");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  ASSERT_NEAR(style.fontSizeMultiplier, 1.6f, 0.05f);
}

TEST(CssGapsFontSize, Em) {
  const CssStyle style = CssParser::parseInlineStyle("font-size: 1.4em");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  ASSERT_NEAR(style.fontSizeMultiplier, 1.4f, 0.05f);
}

TEST(CssGapsFontSize, Smaller) {
  const CssStyle style = CssParser::parseInlineStyle("font-size: 80%");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  ASSERT_NEAR(style.fontSizeMultiplier, 0.8f, 0.05f);
}

// ============================================================================
// font-variant: small-caps
// ============================================================================

TEST(CssGapsFontVariant, SmallCaps) {
  const CssStyle style = CssParser::parseInlineStyle("font-variant: small-caps");
  ASSERT_TRUE(style.hasSmallCaps());
  ASSERT_TRUE(style.smallCaps);
}

TEST(CssGapsFontVariant, CapsLonghand) {
  const CssStyle style = CssParser::parseInlineStyle("font-variant-caps: small-caps");
  ASSERT_TRUE(style.hasSmallCaps());
  ASSERT_TRUE(style.smallCaps);
}

TEST(CssGapsFontVariant, NormalCancels) {
  const CssStyle style = CssParser::parseInlineStyle("font-variant: normal");
  // "normal" is an explicit value so it can cancel inherited small-caps.
  ASSERT_TRUE(style.hasSmallCaps());
  ASSERT_FALSE(style.smallCaps);
}

TEST(CssGapsFontVariant, UnknownIsNotSet) {
  const CssStyle style = CssParser::parseInlineStyle("font-variant: oldstyle-nums");
  // Unrecognised value: leave the property undefined so inheritance is unaffected.
  ASSERT_FALSE(style.hasSmallCaps());
}

TEST(CssGapsFontVariant, CaseInsensitive) {
  const CssStyle style = CssParser::parseInlineStyle("FONT-VARIANT : SMALL-CAPS ;");
  ASSERT_TRUE(style.hasSmallCaps());
  ASSERT_TRUE(style.smallCaps);
}

// ============================================================================
// CSS ID selector support (#id, tag#id)
// ============================================================================

// Helper: load CSS rules from a string into a caller-supplied CssParser.
// CssParser is non-copyable/movable, so we populate in place.
static void loadCssFromString(CssParser& parser, const char* css) {
  HalFile f = HalFile::fromString(css);
  parser.loadFromStream(f);
}

TEST(CssGapsIdSelector, Basic) {
  CssParser parser("");
  loadCssFromString(parser, "#hero { font-weight: bold; }");
  const CssStyle style = parser.resolveStyle("p", "", "hero");
  ASSERT_TRUE(style.hasFontWeight());
  ASSERT_EQ(style.fontWeight, CssFontWeight::Bold);
}

TEST(CssGapsIdSelector, NotMatchedOnOtherElement) {
  CssParser parser("");
  loadCssFromString(parser, "#hero { font-weight: bold; }");
  // No id attr — should not pick up the #hero rule
  const CssStyle style = parser.resolveStyle("p", "", "");
  ASSERT_FALSE(style.hasFontWeight());
}

TEST(CssGapsIdSelector, TagIdWinsOverId) {
  // tag#id is more specific than #id — both applied, tag#id wins on conflict
  CssParser parser("");
  loadCssFromString(parser, "#intro { text-align: left; } p#intro { text-align: center; }");
  const CssStyle style = parser.resolveStyle("p", "", "intro");
  ASSERT_TRUE(style.hasTextAlign());
  ASSERT_EQ(style.textAlign, CssTextAlign::Center);
}

TEST(CssGapsIdSelector, CascadesOverClass) {
  // #id must override .class on the same property
  CssParser parser("");
  loadCssFromString(parser, ".note { text-align: left; } #special { text-align: right; }");
  const CssStyle style = parser.resolveStyle("p", "note", "special");
  ASSERT_TRUE(style.hasTextAlign());
  ASSERT_EQ(style.textAlign, CssTextAlign::Right);
}

TEST(CssGapsIdSelector, CascadesOverTag) {
  CssParser parser("");
  loadCssFromString(parser, "p { text-align: left; } #override { text-align: center; }");
  const CssStyle style = parser.resolveStyle("p", "", "override");
  ASSERT_TRUE(style.hasTextAlign());
  ASSERT_EQ(style.textAlign, CssTextAlign::Center);
}

TEST(CssGapsIdSelector, CaseSensitive) {
  // Id (and class) names are case-sensitive in XHTML, as every other reader matches them. They
  // used to be folded to lowercase, which let one rule reach a differently-cased name: The
  // Anarchy's paragraph rule `.Dial` styled its `class="dial"` speaker names. No book in a
  // 109-book survey needed the folding.
  CssParser parser("");
  loadCssFromString(parser, "#MyID { font-style: italic; }");
  const CssStyle exact = parser.resolveStyle("span", "", "MyID");
  ASSERT_TRUE(exact.hasFontStyle());
  ASSERT_EQ(exact.fontStyle, CssFontStyle::Italic);
  ASSERT_FALSE(parser.resolveStyle("span", "", "myid").hasFontStyle());
}

TEST(CssGapsIdSelector, DoesNotAffectUnrelatedElement) {
  CssParser parser("");
  loadCssFromString(parser, "#toc { font-weight: bold; }");
  // Element without the matching id should not get the rule
  const CssStyle style = parser.resolveStyle("div", "", "chapter");
  ASSERT_FALSE(style.hasFontWeight());
}

TEST(CssGapsIdSelector, Grouped) {
  // Grouped selector: #a, #b { } should store two rules
  CssParser parser("");
  loadCssFromString(parser, "#alpha, #beta { font-style: italic; }");
  const CssStyle styleA = parser.resolveStyle("p", "", "alpha");
  const CssStyle styleB = parser.resolveStyle("p", "", "beta");
  ASSERT_TRUE(styleA.hasFontStyle());
  ASSERT_EQ(styleA.fontStyle, CssFontStyle::Italic);
  ASSERT_TRUE(styleB.hasFontStyle());
  ASSERT_EQ(styleB.fontStyle, CssFontStyle::Italic);
}

// ============================================================================
// StackBuffer overflow handling (selector / declaration truncation)
//
// The streaming parser buffers each selector group and each declaration in a
// fixed 1024-byte StackBuffer. Before the overflow flag was added, content past
// the cap was silently dropped: a truncated selector could be parsed as a bogus
// rule, and a truncated declaration could be parsed as garbage. These tests pin
// the recovery behaviour — oversized tokens are skipped, and valid rules before
// and after them still parse.
// ============================================================================

TEST(CssGapsStackBufferOverflow, OverflowedSelectorGroupDropsUsablePrefix) {
  CssParser parser("");
  // A comma group whose first member (#keep) is short and usable, followed by a
  // giant filler selector that overflows the 1024-byte StackBuffer. The whole rule
  // must be dropped: without the overflow guard the truncated buffer still begins
  // with "#keep, ..." and #keep would be stored as a bogus rule.
  std::string css = "#keep, #";
  css.append(2000, 'x');
  css += " { font-weight: bold; }";
  loadCssFromString(parser, css.c_str());

  const CssStyle style = parser.resolveStyle("p", "", "keep");
  ASSERT_FALSE(style.hasFontWeight());  // giant rule dropped in full — #keep not stored
}

TEST(CssGapsStackBufferOverflow, ParserRecoversAfterOverflowedSelector) {
  CssParser parser("");
  // The dropped giant rule must not derail parsing: valid rules before and after
  // it still resolve.
  std::string css = "p { text-align: center; } #";
  css.append(2000, 'x');
  css += " { font-weight: bold; } #ok { font-style: italic; }";
  loadCssFromString(parser, css.c_str());

  const CssStyle before = parser.resolveStyle("p", "", "");
  ASSERT_TRUE(before.hasTextAlign());
  ASSERT_EQ(before.textAlign, CssTextAlign::Center);

  const CssStyle after = parser.resolveStyle("span", "", "ok");
  ASSERT_TRUE(after.hasFontStyle());
  ASSERT_EQ(after.fontStyle, CssFontStyle::Italic);
}

TEST(CssGapsStackBufferOverflow, OverflowedDeclarationNotParsedAsProperty) {
  CssParser parser("");
  // A "text-align:" declaration padded past the buffer. Without the overflow guard
  // the truncated "text-align:xxxx..." is parsed, interpretAlignment() falls back to
  // Left, and hasTextAlign() becomes true. With the guard the declaration is dropped.
  std::string css = "p { text-align:";
  css.append(2000, 'x');
  css += "; }";
  loadCssFromString(parser, css.c_str());

  const CssStyle style = parser.resolveStyle("p", "", "");
  ASSERT_FALSE(style.hasTextAlign());  // truncated declaration dropped, not parsed as Left
}

TEST(CssGapsStackBufferOverflow, ValidDeclarationSurvivesAfterOverflowedOne) {
  CssParser parser("");
  // The oversized declaration is dropped when ';' flushes it; the following valid
  // declaration in the same block must still apply.
  std::string css = "p { color:";
  css.append(2000, 'z');  // overflow the declaration StackBuffer
  css += "; text-align: right; }";
  loadCssFromString(parser, css.c_str());

  const CssStyle style = parser.resolveStyle("p", "", "");
  ASSERT_TRUE(style.hasTextAlign());
  ASSERT_EQ(style.textAlign, CssTextAlign::Right);
}
