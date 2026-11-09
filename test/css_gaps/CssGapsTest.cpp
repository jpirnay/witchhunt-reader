#include <gtest/gtest.h>

#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

// ============================================================================
// Gap 3: list-style-type / list-style: none
// ============================================================================

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
// Gap 2: line-height
// ============================================================================

// line-height is a multiple of the element's font size, as CSS has it: unitless and em as given, rem as em, % over 100,
// px and pt at CSS's 16 px medium. normal and the keywords leave it unset (the font's own line height).
TEST(CssGapsLineHeight, IsAMultipleOfTheFontSize) {
  const auto em = [](const char* decl) {
    const CssStyle s = CssParser::parseInlineStyle(decl);
    return s.hasLineHeight() ? s.lineHeightEm : -1.0f;
  };
  EXPECT_FLOAT_EQ(1.5f, em("line-height: 1.5"));
  EXPECT_FLOAT_EQ(1.75f, em("line-height: 175%"));
  EXPECT_FLOAT_EQ(1.6f, em("line-height: 1.6em"));
  EXPECT_FLOAT_EQ(1.25f, em("line-height: 1.25rem"));
  EXPECT_FLOAT_EQ(1.5f, em("line-height: 24px"));
  EXPECT_FLOAT_EQ(2.0f, em("line-height: 24pt"));
  EXPECT_FLOAT_EQ(1.75f, em("LINE-HEIGHT : 1.75 !important"));
}

TEST(CssGapsLineHeight, IsClamped) {
  const auto em = [](const char* decl) { return CssParser::parseInlineStyle(decl).lineHeightEm; };
  EXPECT_FLOAT_EQ(0.5f, em("line-height: 0.1"));
  EXPECT_FLOAT_EQ(4.0f, em("line-height: 9"));
}

TEST(CssGapsLineHeight, KeywordsAndNonsenseLeaveItUnset) {
  for (const char* decl : {"line-height: normal", "line-height: inherit", "line-height: initial", "line-height: unset",
                           "line-height: tall", "line-height: 0", "line-height: -1.5", "line-height: 1.5vh"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(decl).hasLineHeight()) << decl;
  }
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

// ============================================================================
// CSS ID selector support (#id, tag#id)
// ============================================================================

// Helper: load CSS rules from a string into a caller-supplied CssParser.
// CssParser is non-copyable/movable, so we populate in place.
static void loadCssFromString(CssParser& parser, const char* css) {
  HalFile f = HalFile::fromString(css);
  parser.loadFromStream(f);
}

TEST(CssGapsIdSelector, NotMatchedOnOtherElement) {
  CssParser parser("");
  loadCssFromString(parser, "#hero { font-weight: bold; }");
  // No id attr — should not pick up the #hero rule
  const CssStyle style = parser.resolveStyle("p", "", "");
  ASSERT_FALSE(style.hasFontWeight());
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

// ============================================================================
// The rule an <hr> draws: border none hides it, unless something else paints it
// ============================================================================

namespace {
bool hides(const char* declarations) { return CssParser::parseInlineStyle(declarations).hidesRule(); }
}  // namespace

TEST(CssGapsRule, NoBorderHidesTheRule) {
  EXPECT_TRUE(hides("border: currentColor none medium"));  // Project Hail Mary's section breaks
  EXPECT_TRUE(hides("border: none"));
  EXPECT_TRUE(hides("border: 0"));
  EXPECT_TRUE(hides("border: 0px"));
  EXPECT_TRUE(hides("border-style: none"));
  EXPECT_TRUE(hides("border-style: hidden"));
  EXPECT_TRUE(hides("border-width: 0"));
  EXPECT_TRUE(hides("border: none; background: none"));
  EXPECT_TRUE(hides("border: none; background-color: transparent"));
}

TEST(CssGapsRule, ARuleTheBookLeftAloneStays) {
  EXPECT_FALSE(hides(""));
  EXPECT_FALSE(hides("width: 50%"));
  EXPECT_FALSE(hides("border: inherit"));
}

TEST(CssGapsRule, AVisibleBorderKeepsTheRule) {
  EXPECT_FALSE(hides("border: 1px solid black"));
  EXPECT_FALSE(hides("border-style: dashed"));
  EXPECT_FALSE(hides("border-width: 0.5px"));
  EXPECT_FALSE(hides("border-width: 0 0 1px"));
  EXPECT_FALSE(hides("border-style: none none solid"));
}

TEST(CssGapsRule, SomethingElsePaintingKeepsTheRule) {
  EXPECT_FALSE(hides("border: 0; height: 1px; background: #333"));
  EXPECT_FALSE(hides("border: none; background-color: gray"));
  EXPECT_FALSE(hides("border: none; background-image: url(line.png)"));
  EXPECT_FALSE(hides("border: none; border-top: 1px solid black"));
  EXPECT_FALSE(hides("border: none; border-bottom-style: solid"));
}

TEST(CssGapsRule, ABorderlessSideAddsNothing) {
  EXPECT_TRUE(hides("border: none; border-top: none"));
  EXPECT_TRUE(hides("border: none; border-bottom-width: 0"));
}

// A reset that takes the border away does not hide what a later rule paints, in either order.
TEST(CssGapsRule, TheTwoFactsCascadeOnTheirOwn) {
  CssStyle resetThenPaint = CssParser::parseInlineStyle("border: 0");
  resetThenPaint.applyOver(CssParser::parseInlineStyle("background: #000"));
  EXPECT_FALSE(resetThenPaint.hidesRule());
  CssStyle paintThenReset = CssParser::parseInlineStyle("background: #000");
  paintThenReset.applyOver(CssParser::parseInlineStyle("border: 0"));
  EXPECT_FALSE(paintThenReset.hidesRule());
}
