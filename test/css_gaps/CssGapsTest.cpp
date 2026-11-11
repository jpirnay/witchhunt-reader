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

// ============================================================================
// Boxes: borders and background shading (CssBox)
// ============================================================================

namespace {
CssBox box(const char* declarations) { return CssParser::parseInlineStyle(declarations).box; }
constexpr CssBox::Side kSides[] = {CssBox::Top, CssBox::Right, CssBox::Bottom, CssBox::Left};
// "w/style" per side, top first, as the page draws them: 0 for a side that draws nothing.
std::string drawn(const CssBox& b) {
  std::string out;
  for (const CssBox::Side side : kSides) {
    if (!out.empty()) out += ' ';
    out += std::to_string(b.drawnWidth(side));
    if (b.drawnWidth(side) > 0) out += "/" + std::to_string(static_cast<int>(b.style(side)));
  }
  return out;
}
constexpr int kSolid = static_cast<int>(CssBorderStyle::Solid);
}  // namespace

TEST(CssGapsBox, TheShorthandSetsEverySide) {
  EXPECT_EQ("1/2 1/2 1/2 1/2", drawn(box("border: 1px solid black")));
  EXPECT_EQ("2/3 2/3 2/3 2/3", drawn(box("border: double")));  // medium
  EXPECT_EQ("3/4 3/4 3/4 3/4", drawn(box("border: thick dotted #333")));
  EXPECT_EQ("1/5 1/5 1/5 1/5", drawn(box("border: dashed thin")));
  EXPECT_EQ("0 0 0 0", drawn(box("border: 2px")));  // no style: none
  EXPECT_EQ("0 0 0 0", drawn(box("border: none")));
  EXPECT_TRUE(box("border: none").hasStyle(CssBox::Top)) << "an explicit none must cancel a lower rule's line";
  EXPECT_FALSE(box("width: 50%").any());
}

TEST(CssGapsBox, GrooveRidgeInsetAndOutsetAreSolidAndHiddenIsNone) {
  for (const char* style : {"groove", "ridge", "inset", "outset"}) {
    EXPECT_EQ(CssBorderStyle::Solid, box((std::string("border: 1px ") + style).c_str()).style(CssBox::Top)) << style;
  }
  EXPECT_EQ("0 0 0 0", drawn(box("border: 1px hidden")));
}

TEST(CssGapsBox, OneSide) {
  EXPECT_EQ("0 0 0 3/2", drawn(box("border-left: 3px solid #ccc")));
  EXPECT_EQ("1/2 0 0 0", drawn(box("border-top: 1px solid")));
  EXPECT_EQ("0 0 2/5 0", drawn(box("border-bottom: 2px dashed")));
  EXPECT_EQ("0 1/4 0 0", drawn(box("border-right-style: dotted; border-right-width: 1px")));
  EXPECT_FALSE(box("border-left: 1px solid").hasStyle(CssBox::Top));
}

TEST(CssGapsBox, WidthsAndStylesTakeOneToFourValues) {
  EXPECT_EQ("1/2 2/2 3/2 2/2", drawn(box("border-style: solid; border-width: 1px 2px 3px")));
  EXPECT_EQ("1/2 2/2 1/2 2/2", drawn(box("border-style: solid; border-width: 1px 2px")));
  EXPECT_EQ("0 0 2/2 0", drawn(box("border-width: 0 0 2px; border-style: solid")));
  EXPECT_EQ("2/2 2/3 2/4 2/5", drawn(box("border-style: solid double dotted dashed")));
  EXPECT_EQ("2/2 0 2/2 0", drawn(box("border-style: solid none")));
  // A value that is no width (or style) makes the whole declaration invalid, as in CSS.
  EXPECT_FALSE(box("border-width: 1px fat").hasWidth(CssBox::Top));
  EXPECT_FALSE(box("border-style: solid wavy").hasStyle(CssBox::Top));
  EXPECT_FALSE(box("border-width: 1px 1px 1px 1px 1px").hasWidth(CssBox::Top));
}

TEST(CssGapsBox, WidthUnits) {
  const auto top = [](const char* decl) { return static_cast<int>(box(decl).width(CssBox::Top)); };
  EXPECT_EQ(1, top("border-top-width: thin"));
  EXPECT_EQ(2, top("border-top-width: medium"));
  EXPECT_EQ(3, top("border-top-width: thick"));
  EXPECT_EQ(2, top("border-top-width: 2px"));
  EXPECT_EQ(1, top("border-top-width: 0.3px"));  // a hairline stays visible
  EXPECT_EQ(0, top("border-top-width: 0"));
  EXPECT_EQ(2, top("border-top-width: 1.5pt"));  // 2 px
  EXPECT_EQ(2, top("border-top-width: 0.1em"));  // 1.6 px at CSS's 16 px em
  EXPECT_EQ(3, top("border-top-width: 10px"));   // wider borders are drawn 3 px wide
  EXPECT_EQ(3, top("border-top-width: 0.5em"));  // 8 px
  EXPECT_FALSE(box("border-top-width: 10%").hasWidth(CssBox::Top));
  EXPECT_FALSE(box("border-top-width: inherit").hasWidth(CssBox::Top));
}

// A colour the paper shows as white draws no line; every other colour draws black.
TEST(CssGapsBox, PaperColouredBordersDrawNothing) {
  EXPECT_EQ("0 0 0 0", drawn(box("border: 1px solid white")));
  EXPECT_EQ("0 0 0 0", drawn(box("border: 1px solid transparent")));
  EXPECT_EQ("0 0 0 0", drawn(box("border: 1px solid #fff")));
  EXPECT_EQ("0 0 0 0", drawn(box("border: 1px solid rgba(0, 0, 0, 0)")));
  EXPECT_EQ("1/2 1/2 1/2 1/2", drawn(box("border: 1px solid #ccc")));
  EXPECT_EQ("1/2 1/2 1/2 1/2", drawn(box("border: 1px solid rgb(128 128 128 / 50%)")));
  EXPECT_EQ("1/2 1/2 1/2 1/2", drawn(box("border: 1px solid currentColor")));
  EXPECT_EQ("1/2 1/2 1/2 1/2", drawn(box("border: 1px solid darkslateblue")));
  // Colours alone are not kept: border-color draws black.
  EXPECT_FALSE(box("border-color: red").any());
}

TEST(CssGapsBox, BorderPropertiesThatAreNotALineAreIgnored) {
  for (const char* decl :
       {"border-radius: 4px", "border-collapse: collapse", "border-spacing: 2px", "border-image: url(x.png) 30 round",
        "border-block-start: 1px solid", "border-top-left-radius: 3px"}) {
    EXPECT_FALSE(box(decl).any()) << decl;
  }
  EXPECT_FALSE(box("border: inherit").any()) << "a border is not inherited, and the parent's is not known here";
  EXPECT_EQ("0 0 0 0", drawn(box("border: 1px solid; border: initial")));
}

TEST(CssGapsBox, BackgroundColoursShade) {
  const auto background = [](const char* decl) { return box(decl).background(); };
  using B = CssBox::Background;
  EXPECT_EQ(B::Shaded, background("background-color: #eee"));
  EXPECT_EQ(B::Shaded, background("background-color: #f5f5f5"));  // a code box
  EXPECT_EQ(B::Shaded, background("background: lightgray"));
  EXPECT_EQ(B::Shaded, background("background-color: rgb(240, 240, 240)"));
  EXPECT_EQ(B::Shaded, background("background-color: hsl(60, 100%, 90%)"));
  EXPECT_EQ(B::Shaded, background("background: #eee8d5 url(paper.png) no-repeat"));
  // Dark backgrounds draw the same light shade: the text stays black and must stay readable.
  EXPECT_EQ(B::Shaded, background("background-color: #000"));
  EXPECT_EQ(B::Shaded, background("background: navy"));
  // What leaves the paper white.
  EXPECT_EQ(B::None, background("background-color: white"));
  EXPECT_EQ(B::None, background("background-color: #fff"));
  EXPECT_EQ(B::None, background("background-color: #fafafa"));
  EXPECT_EQ(B::None, background("background-color: transparent"));
  EXPECT_EQ(B::None, background("background-color: rgba(0, 0, 0, 0)"));
  EXPECT_EQ(B::None, background("background-color: #00000000"));
  EXPECT_EQ(B::None, background("background-color: inherit"));
  EXPECT_EQ(B::None, background("background: none"));
  EXPECT_EQ(B::None,
            background("background: url(rabbit.png) no-repeat center"));  // the shorthand's colour: transparent
  EXPECT_EQ(B::None, background("background: linear-gradient(#000, #fff)"));
  // What cannot be read leaves it alone.
  EXPECT_EQ(B::Unset, background("background-color: var(--paper)"));
  EXPECT_EQ(B::Unset, background("background-image: url(x.png)"));
  EXPECT_EQ(B::Unset, background("color: #eee"));
}

TEST(CssGapsBox, AShadeOrABorderIsDrawn) {
  EXPECT_TRUE(box("background: #eee").drawn());
  EXPECT_TRUE(box("border-left: 1px solid").drawn());
  EXPECT_FALSE(box("border: none; background: transparent").drawn());
  EXPECT_FALSE(box("border-width: 2px").drawn());
}

// Each side's width and style cascade as longhands of their own, by specificity and then source order, whatever order
// the rules are merged in.
TEST(CssGapsBox, SidesCascadeAsLonghands) {
  CssParser parser("");
  loadCssFromString(parser,
                    "div.box { border: 1px solid; background: #eee; }\n"
                    "div.box.wide { border-width: 3px; }\n"
                    "div.box.open { border-top: none; }\n"
                    "div.box.plain { background: transparent; }\n"
                    "div.box.dashed { border-style: dashed; }\n");
  EXPECT_EQ("1/2 1/2 1/2 1/2", drawn(parser.resolveStyle("div", "box").box));
  EXPECT_EQ("3/2 3/2 3/2 3/2", drawn(parser.resolveStyle("div", "wide box").box));
  EXPECT_EQ("0 1/2 1/2 1/2", drawn(parser.resolveStyle("div", "open box").box));
  EXPECT_EQ("3/5 3/5 3/5 3/5", drawn(parser.resolveStyle("div", "dashed wide box").box));
  EXPECT_TRUE(parser.resolveStyle("div", "box").box.shaded());
  EXPECT_FALSE(parser.resolveStyle("div", "plain box").box.shaded());
  // A less specific rule later in the sheet does not take a side back.
  CssParser later("");
  loadCssFromString(later, "p.note { border-left: 2px solid; }\np { border: none; }\n");
  EXPECT_EQ("0 0 0 2/2", drawn(later.resolveStyle("p", "note").box));
}

// An inline style= goes over the sheet, side by side.
TEST(CssGapsBox, InlineStyleGoesOverTheSheet) {
  CssStyle style = CssParser::parseInlineStyle("border: 1px solid; background: #eee");
  style.applyOver(CssParser::parseInlineStyle("border-bottom: 2px double; background-color: white"));
  EXPECT_EQ("1/2 1/2 2/3 1/2", drawn(style.box));
  EXPECT_FALSE(style.box.shaded());
}

// A vertical-rl element's physical sides are its logical ones turned: its right border is where its lines start.
TEST(CssGapsBox, AVerticalElementsSidesTurnWithIt) {
  CssStyle rl = CssParser::parseInlineStyle("border-right: 1px solid; border-bottom: 2px dashed");
  rl.resolveLogical(CssWritingMode::VerticalRl, CssDirection::Unset);
  EXPECT_EQ("1/2 2/5 0 0", drawn(rl.box));  // block-start, inline-end, block-end, inline-start
  CssStyle lr = CssParser::parseInlineStyle("border-left: 1px solid; border-top: 3px dotted");
  lr.resolveLogical(CssWritingMode::VerticalLr, CssDirection::Unset);
  EXPECT_EQ("1/2 0 0 3/4", drawn(lr.box));
  CssStyle horizontal = CssParser::parseInlineStyle("border-left: 1px solid");
  horizontal.resolveLogical(CssWritingMode::HorizontalTb, CssDirection::Rtl);
  EXPECT_EQ("0 0 0 1/2", drawn(horizontal.box)) << "borders are physical in a horizontal element, rtl or not";
}

// The <hr> facts travel in the box and are unchanged by it.
TEST(CssGapsBox, TheRuleFactsAreKeptBesideTheSides) {
  const CssStyle style = CssParser::parseInlineStyle("border: none; background: #000");
  EXPECT_FALSE(style.hidesRule());
  EXPECT_TRUE(style.box.shaded());
  EXPECT_EQ(CssStyle::kRuleBorderNone | CssStyle::kRulePainted, style.box.ruleBorder());
  EXPECT_EQ(kSolid, static_cast<int>(CssParser::parseInlineStyle("border: 1px solid").box.style(CssBox::Left)));
}
