// CSS initial-letter: a drop cap the book asks for in lines, on a leading span or on the paragraph's ::first-letter,
// laid out by the drop-cap machinery a left-floated large span already takes.
//
// The stub font: line height 24, ascender 18, a letter's ink 14 above the baseline and none below. A cap of n lines is
// 14 + (n - 1) * 24 tall above its baseline, so its scale is that over 14, and the lines beside it start after it.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/FlowContext.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Line {
  int y;
  int x;
  float mult;  // the block's font-size multiplier: a cap's scale
  std::string text;
};

constexpr int kPitch = 24;
constexpr int kInk = 14;  // the stub's ink above the baseline
// A paragraph long enough to wrap beside a cap of six lines at 480 px.
const std::string kProse =
    "he opening paragraph of the chapter runs on long enough to wrap beside a cap of several lines, line after line "
    "after line, until the cap is passed and the text takes the whole width of the column again for the rest of it, "
    "which goes on for a while longer so that every test has the lines it needs beside the letter and below it.";

struct InitialLetterFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("epub_il_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  struct Options {
    VerticalText verticalText = VerticalText::Off;
    std::string language = "en";
    bool embeddedStyle = true;
  };

  std::vector<Line> layout(const std::string& css, const std::string& body) { return layout(css, body, Options{}); }
  std::vector<Line> layout(const std::string& css, const std::string& body, const Options& options) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>" +
        body + "</body></html>\n";
    const auto epub = syntheticBookFromDocument(work, books++, doc, {{"style.css", "text/css", css}}, options.language);
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.extraParagraphSpacing = true;  // no reader's first-line indent
    params.paragraphAlignment = 1;        // left
    params.embeddedStyle = options.embeddedStyle;
    params.verticalText = options.verticalText;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<Line> lines;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) break;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        std::string text;
        for (uint16_t w = 0; w < block.wordCount(); ++w) text += (w > 0 ? " " : "") + std::string(block.wordText(w));
        lines.push_back({el->yPos + p * 1000, el->xPos, block.getRenderStyle().fontSizeMultiplier, text});
      }
    }
    return lines;
  }
};

std::string dump(const std::vector<Line>& lines) {
  std::string out;
  for (const Line& l : lines) {
    out += "y=" + std::to_string(l.y) + " x=" + std::to_string(l.x) + " m=" + std::to_string(l.mult) + ": " +
           l.text.substr(0, 40) + "\n";
  }
  return out;
}

// The cap's line, if a cap was placed: the one line set larger than the text.
const Line* capOf(const std::vector<Line>& lines) {
  for (const Line& l : lines) {
    if (l.mult > 1.5f) return &l;
  }
  return nullptr;
}

// How many text lines are moved right of the left edge by the cap.
int linesBeside(const std::vector<Line>& lines) {
  int n = 0;
  for (const Line& l : lines) n += l.mult <= 1.5f && l.x > 0 ? 1 : 0;
  return n;
}

}  // namespace

// The scale a cap of n lines takes, and the first text line's top in a paragraph that opens a chapter.
float scaleFor(const int lines) { return static_cast<float>(kInk + (lines - 1) * kPitch) / kInk; }
std::string paragraph(const std::string& open) { return "<p>" + open + kProse + "</p>"; }

TEST_F(InitialLetterFixture, AnInitialLetterSpanIsADropCapOfItsLines) {
  for (const int n : {2, 3, 4}) {
    SCOPED_TRACE(n);
    const auto lines =
        layout(".il { initial-letter: " + std::to_string(n) + " }", paragraph("<span class=\"il\">T</span>"));
    const Line* cap = capOf(lines);
    ASSERT_NE(nullptr, cap) << dump(lines);
    EXPECT_EQ("T", cap->text);
    EXPECT_NEAR(scaleFor(n), cap->mult, 0.01f) << dump(lines);
    EXPECT_EQ(n, linesBeside(lines)) << dump(lines);
    // Its ink runs from the first line's ink top (4 below the line's top) to the n-th line's baseline (18 below its
    // top): the cap's own ascender, 18 scaled, stands 4 scaled above its ink.
    const int inkTop = cap->y + static_cast<int>((18 - kInk) * cap->mult + 0.5f);
    EXPECT_EQ(4, inkTop) << dump(lines);
    EXPECT_NEAR(18 + (n - 1) * kPitch, inkTop + kInk * cap->mult, 1.0f) << dump(lines);
  }
}

// initial-letter decides how tall the letter is: a font-size the book also gives it (here for the readers without
// initial-letter) changes nothing, nor does a float.
TEST_F(InitialLetterFixture, TheLinesDecideTheHeightNotTheFontSize) {
  for (const char* css : {".il { initial-letter: 3; font-size: 6em; float: left }",
                          ".il { initial-letter: 3; font-size: 0.5em }", ".il { -webkit-initial-letter: 3 }"}) {
    SCOPED_TRACE(css);
    const auto lines = layout(css, paragraph("<span class=\"il\">T</span>"));
    const Line* cap = capOf(lines);
    ASSERT_NE(nullptr, cap) << dump(lines);
    EXPECT_NEAR(scaleFor(3), cap->mult, 0.01f);
    EXPECT_EQ(3, linesBeside(lines)) << dump(lines);
  }
}

TEST_F(InitialLetterFixture, AFirstLetterRuleMakesTheCap) {
  const auto lines = layout("p.first::first-letter { initial-letter: 2 }",
                            "<p class=\"first\">\u201cT" + kProse + "</p><p>The next.</p>");
  const Line* cap = capOf(lines);
  ASSERT_NE(nullptr, cap) << dump(lines);
  EXPECT_EQ("\u201cT", cap->text);  // the quote before the letter goes with it
  EXPECT_NEAR(scaleFor(2), cap->mult, 0.01f);
  EXPECT_EQ(2, linesBeside(lines)) << dump(lines);
  EXPECT_EQ("he", lines[1].text.substr(0, 2)) << dump(lines);  // the rest of the word starts the first line
  EXPECT_EQ("The next.", lines.back().text) << dump(lines);    // the next paragraph has none
}

// A mark on the letter goes with it (text in decomposed form: e and a combining acute).
TEST_F(InitialLetterFixture, AFirstLetterKeepsItsMarks) {
  const auto lines = layout("p::first-letter { initial-letter: 2 }", "<p>e\u0301t\u00e9 " + kProse + "</p>");
  const Line* cap = capOf(lines);
  ASSERT_NE(nullptr, cap) << dump(lines);
  EXPECT_EQ("e\u0301", cap->text);
  EXPECT_EQ("t\u00e9", lines[1].text.substr(0, 3)) << dump(lines);
}

// A ::first-letter after the selectors an element's rules take: child, descendant, class and compound, matched where
// the paragraph stands; a paragraph the selector does not match gets no cap.
TEST_F(InitialLetterFixture, AFirstLetterRuleMatchesWhereTheParagraphStands) {
  const std::string css =
      "div.chapter > p.first::first-letter { initial-letter: 3 }\n"
      "section p::first-letter { initial-letter: 2 }\n"
      ".dropcap::first-letter { initial-letter: 4 }\n"
      "p.a.b::first-letter { initial-letter: 2 }\n";
  struct Case {
    std::string body;
    int lines;  // 0: no cap
  };
  const Case cases[] = {
      {"<div class=\"chapter\"><p class=\"first\">T" + kProse + "</p></div>", 3},
      {"<div class=\"other\"><p class=\"first\">T" + kProse + "</p></div>", 0},
      {"<div class=\"chapter\"><blockquote><p class=\"first\">T" + kProse + "</p></blockquote></div>", 0},
      {"<section><div><p>T" + kProse + "</p></div></section>", 2},
      {"<p class=\"dropcap\">T" + kProse + "</p>", 4},
      {"<p class=\"b a\">T" + kProse + "</p>", 2},
      {"<p class=\"a\">T" + kProse + "</p>", 0},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.body.substr(0, 60));
    const auto lines = layout(css, c.body);
    const Line* cap = capOf(lines);
    if (c.lines == 0) {
      EXPECT_EQ(nullptr, cap) << dump(lines);
      continue;
    }
    ASSERT_NE(nullptr, cap) << dump(lines);
    EXPECT_NEAR(scaleFor(c.lines), cap->mult, 0.01f);
    EXPECT_EQ(c.lines, linesBeside(lines)) << dump(lines);
  }
}

// The first letter of a wrapper's first line is in its first paragraph, as CSS has it.
TEST_F(InitialLetterFixture, AWrappersFirstLetterIsItsFirstParagraphs) {
  const auto lines = layout(".chapter::first-letter { initial-letter: 3 }",
                            "<div class=\"chapter\">" + paragraph("<b>T</b>") + paragraph("A") + "</div>");
  const Line* cap = capOf(lines);
  ASSERT_NE(nullptr, cap) << dump(lines);
  EXPECT_EQ("T", cap->text);
  EXPECT_EQ(3, linesBeside(lines)) << dump(lines);
}

// Books guard initial-letter with @supports, over a fallback of their own: the guarded rule now applies.
TEST_F(InitialLetterFixture, AnSupportsGuardedRuleMakesTheCap) {
  const std::string css =
      ".dc { float: left; font-size: 2.2em; }\n"
      "@supports (initial-letter: 4) or (-webkit-initial-letter: 4) {\n"
      "  .dc { float: none; font-size: 1em; initial-letter: 4; -webkit-initial-letter: 4; }\n"
      "}\n";
  const auto lines = layout(css, paragraph("<span class=\"dc\">T</span>"));
  const Line* cap = capOf(lines);
  ASSERT_NE(nullptr, cap) << dump(lines);
  EXPECT_NEAR(scaleFor(4), cap->mult, 0.01f);
  EXPECT_EQ(4, linesBeside(lines)) << dump(lines);
}

// A raised cap (sink 1) stands above the first line, which goes down to make room for it, and the text beside it is
// that one line. The letter's baseline is the first line's.
TEST_F(InitialLetterFixture, ARaisedCapStandsAboveItsFirstLine) {
  const std::string body = "<p>Before</p>" + paragraph("<span class=\"il\">T</span>");
  const auto dropped = layout("p { margin: 0 } .il { initial-letter: 3 }", body);
  const auto raised = layout("p { margin: 0 } .il { initial-letter: 3 1 }", body);
  ASSERT_GE(dropped.size(), 3u);
  ASSERT_GE(raised.size(), 3u);
  EXPECT_EQ(raised[2].y, dropped[2].y + 2 * kPitch) << dump(raised);  // [0] Before, [1] the cap, [2] the first line
  EXPECT_EQ(1, linesBeside(raised)) << dump(raised);
  const Line* cap = capOf(raised);
  ASSERT_NE(nullptr, cap);
  EXPECT_NEAR(scaleFor(3), cap->mult, 0.01f);
  const float inkBottom = cap->y + 18 * cap->mult;
  EXPECT_NEAR(raised[2].y + 18, inkBottom, 1.0f) << dump(raised);
}

// A sunken cap (sink 3 for a cap of 2) sits a line lower, and the text beside it is three lines.
TEST_F(InitialLetterFixture, ASunkenCapSitsLower) {
  const auto lines = layout(".il { initial-letter: 2 3 }", paragraph("<span class=\"il\">T</span>"));
  const Line* cap = capOf(lines);
  ASSERT_NE(nullptr, cap) << dump(lines);
  EXPECT_NEAR(scaleFor(2), cap->mult, 0.01f);
  EXPECT_EQ(3, linesBeside(lines)) << dump(lines);
  EXPECT_NEAR(18 + 2 * kPitch, cap->y + 18 * cap->mult, 1.0f) << dump(lines);  // on the third line's baseline
}

// At most six lines, and fewer where the cap would take more than half the column.
TEST_F(InitialLetterFixture, TheLinesAreClampedToWhatTheColumnHolds) {
  const auto tall = layout(".il { initial-letter: 12 }", paragraph("<span class=\"il\">T</span>"));
  const Line* cap = capOf(tall);
  ASSERT_NE(nullptr, cap) << dump(tall);
  EXPECT_NEAR(scaleFor(6), cap->mult, 0.01f);
  EXPECT_EQ(6, linesBeside(tall)) << dump(tall);
  const auto wide = layout(".il { initial-letter: 6 }", paragraph("<span class=\"il\">WWWW</span>"));
  cap = capOf(wide);
  ASSERT_NE(nullptr, cap) << dump(wide);
  EXPECT_LT(cap->mult, scaleFor(6));
  EXPECT_LE(wide[1].x, 240) << dump(wide);  // the text beside it keeps half the column
  EXPECT_EQ(static_cast<int>((cap->mult * kInk - kInk) / kPitch + 1.5f), linesBeside(wide)) << dump(wide);
}

TEST_F(InitialLetterFixture, NormalOrOneLineIsNoCap) {
  for (const char* css : {".il { initial-letter: normal }", ".il { initial-letter: 1 }"}) {
    SCOPED_TRACE(css);
    const auto lines = layout(css, paragraph("<span class=\"il\">T</span>"));
    EXPECT_EQ(nullptr, capOf(lines)) << dump(lines);
    EXPECT_EQ(0, linesBeside(lines)) << dump(lines);
  }
  // A cap the book sizes by its font-size and a float stays one: normal only says it is no initial letter.
  const auto sized =
      layout(".il { float: left; font-size: 3em; initial-letter: normal }", paragraph("<span class=\"il\">T</span>"));
  EXPECT_NE(nullptr, capOf(sized)) << dump(sized);
}

// Only the first letter of a paragraph: a span further in, a list item's (after its marker), or one inside a table
// keeps its place in the text.
TEST_F(InitialLetterFixture, OnlyAParagraphsFirstLetter) {
  const auto inner = layout(".il { initial-letter: 3 }", "<p>Some <span class=\"il\">T</span>" + kProse + "</p>");
  EXPECT_EQ(nullptr, capOf(inner)) << dump(inner);
  const auto item = layout("li::first-letter { initial-letter: 3 }", "<ul><li>The item</li></ul>");
  EXPECT_EQ(nullptr, capOf(item)) << dump(item);
}

// Right to left and down a column the machinery places no cap: the letter stays in its line, and nothing breaks.
TEST_F(InitialLetterFixture, RightToLeftAndVerticalKeepTheLetterInItsLine) {
  const auto rtl = layout(".il { initial-letter: 3 } p::first-letter { initial-letter: 3 }",
                          "<p dir=\"rtl\"><span class=\"il\">ש</span>לום עולם</p><p dir=\"rtl\">שלום</p>");
  EXPECT_EQ(nullptr, capOf(rtl)) << dump(rtl);
  ASSERT_EQ(2u, rtl.size()) << dump(rtl);
  Options vertical;
  vertical.verticalText = VerticalText::On;
  vertical.language = "ja";
  const auto column = layout(".il { initial-letter: 3 } p::first-letter { initial-letter: 3 }",
                             "<p><span class=\"il\">吾</span>輩は猫である。</p><p>名前はまだ無い。</p>", vertical);
  EXPECT_EQ(nullptr, capOf(column)) << dump(column);
  ASSERT_EQ(2u, column.size()) << dump(column);
}

// With the book's styles off there is no initial-letter to read.
TEST_F(InitialLetterFixture, NoCapWithTheBooksStyleOff) {
  Options noCss;
  noCss.embeddedStyle = false;
  const auto lines = layout("p::first-letter { initial-letter: 3 }", paragraph("T"), noCss);
  EXPECT_EQ(nullptr, capOf(lines)) << dump(lines);
}
