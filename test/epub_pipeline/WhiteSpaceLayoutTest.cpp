// CSS white-space: which spaces, tabs and line feeds a book's text keeps.
//
// pre, pre-wrap and break-spaces keep runs of spaces (a run is one token, as wide as its spaces) and line feeds;
// pre-line keeps line feeds only; normal and nowrap keep neither. <pre> is pre unless the book's CSS says otherwise.
// Every mode wraps at the column's edge: a 480 px screen has nowhere to put a line it does not wrap.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/FlowContext.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/TokenBoundary.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

// A run of kept spaces costs the text around it nothing: the kept bit is a spare bit of the boundary every token
// already has.
static_assert(sizeof(TokenBoundary::Boundary) == 4, "a token's boundary grew");

namespace {

struct Word {
  std::string text;
  int x;
  bool continues;
};
struct Line {
  int y;
  int x;  // a table cell's lines: -1 for the first cell, -2 for the second
  CssTextAlign alignment;
  std::vector<Word> words;
  uint16_t annotations;
};

constexpr uint8_t kJustified = 0;
constexpr uint8_t kLeft = 1;
constexpr uint8_t kBooksOwn = 4;
constexpr int kSpace = 6;  // the stub font's space width

struct WhiteSpaceFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("epub_ws_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  struct Options {
    int width = 480;
    bool extraParagraphSpacing = true;  // and so no reader's first-line indent
    bool hyphenation = false;
    uint8_t alignment = kLeft;
    bool embeddedStyle = true;
    VerticalText verticalText = VerticalText::Off;
    std::string language = "en";
  };

  std::vector<Line> layout(const std::string& css, const std::string& body, const Options& options) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>" +
        body + "</body></html>\n";
    const auto epub = syntheticBookFromDocument(work, books++, doc, {{"style.css", "text/css", css}}, options.language);
    Section::BuildParams params;
    params.viewportWidth = static_cast<uint16_t>(options.width);
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.extraParagraphSpacing = options.extraParagraphSpacing;
    params.hyphenationEnabled = options.hyphenation;
    params.paragraphAlignment = options.alignment;
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
      const auto add = [&](const TextBlock& block, const int y, const int x) {
        Line line{y + p * 1000, x, block.getRenderStyle().alignment, {}, block.annotationCount()};
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          line.words.push_back({block.wordText(w), block.wordXpos(w), block.wordContinues(w)});
        }
        lines.push_back(std::move(line));
      };
      for (const auto& el : page->elements) {
        if (el->getTag() == TAG_PageLine) add(*static_cast<const PageLine&>(*el).getBlock(), el->yPos, el->xPos);
        if (el->getTag() != TAG_PageTable) continue;
        int cell = 0;
        for (const TableRow& row : static_cast<const PageTableFragment&>(*el).getRows()) {
          for (const TableCell& c : row.cells) {
            --cell;
            for (const auto& block : c.lines) add(*block, el->yPos, cell);
          }
        }
      }
    }
    return lines;
  }
  std::vector<Line> layout(const std::string& css, const std::string& body) { return layout(css, body, Options{}); }

  static int widthOf(const std::string& text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    GfxRenderer renderer;
    return renderer.getTextWidth(0, text.c_str(), style);
  }
};

// The line as text: tokens in order, '_' for a word space, where a token does not continue the one before (but after a
// run of kept spaces, which is the space).
std::string text(const Line& line) {
  std::string out;
  for (size_t i = 0; i < line.words.size(); ++i) {
    if (i > 0 && !line.words[i].continues && line.words[i - 1].text[0] != ' ') out += '_';
    out += line.words[i].text;
  }
  return out;
}

// The line's tokens run together: CJK text has no word spaces to show.
std::string joined(const Line& line) {
  std::string out;
  for (const Word& w : line.words) out += w.text;
  return out;
}

std::string dump(const std::vector<Line>& lines) {
  std::string out;
  for (const Line& line : lines) {
    out += "y=" + std::to_string(line.y) + " x=" + std::to_string(line.x) + ":";
    for (const Word& w : line.words) out += " [" + w.text + "]@" + std::to_string(w.x) + (w.continues ? "c" : "");
    out += "\n";
  }
  return out;
}

}  // namespace

// ---- What each value keeps ----

TEST_F(WhiteSpaceFixture, PreKeepsRunsOfSpacesAndIndentation) {
  const auto lines = layout("", "<pre>a  b\n    x</pre>");
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "a  b") << dump(lines);
  EXPECT_EQ(lines[0].words[2].x, widthOf("a") + 2 * kSpace) << dump(lines);
  EXPECT_EQ(text(lines[1]), "    x") << dump(lines);
  EXPECT_EQ(lines[1].words[1].x, 4 * kSpace) << dump(lines);
}

// A single kept space is a run of one: it does not stretch, and it is as wide as a space.
TEST_F(WhiteSpaceFixture, ASingleKeptSpaceIsARunOfOne) {
  const auto lines = layout("", "<pre>a b</pre>");
  ASSERT_EQ(lines.size(), 1u) << dump(lines);
  ASSERT_EQ(lines[0].words.size(), 3u) << dump(lines);
  EXPECT_EQ(lines[0].words[1].text, " ");
  EXPECT_EQ(lines[0].words[2].x, widthOf("a") + kSpace) << dump(lines);
}

TEST_F(WhiteSpaceFixture, ATabRunsToTheNextTabStop) {
  const auto lines = layout("", "<pre>\tx\nab\ty\nabcd\tz\n<b>ab</b>\tq</pre>");
  ASSERT_EQ(lines.size(), 4u) << dump(lines);
  EXPECT_EQ(lines[0].words[1].x, 4 * kSpace) << dump(lines);
  EXPECT_EQ(lines[1].words[2].x, widthOf("ab") + 2 * kSpace) << dump(lines);
  EXPECT_EQ(lines[2].words[2].x, widthOf("abcd") + 4 * kSpace) << dump(lines);  // column 4: the next stop is 8
  // Columns count what an inline element holds too (and the run takes its style: a bold space is no wider here).
  EXPECT_EQ(lines[3].words[2].x, widthOf("ab", EpdFontFamily::BOLD) + 2 * kSpace) << dump(lines);
}

TEST_F(WhiteSpaceFixture, PreWrapAndBreakSpacesKeepWhatPreKeeps) {
  for (const char* value : {"pre-wrap", "break-spaces", "-moz-pre-wrap"}) {
    SCOPED_TRACE(value);
    const auto lines = layout(std::string("p { white-space: ") + value + " }", "<p>a  b\n\tc</p>");
    ASSERT_EQ(lines.size(), 2u) << dump(lines);
    EXPECT_EQ(text(lines[0]), "a  b");
    EXPECT_EQ(text(lines[1]), "    c");
  }
}

TEST_F(WhiteSpaceFixture, PreLineKeepsLineFeedsAndCollapsesSpaces) {
  const auto lines = layout("p { white-space: pre-line }", "<p>a   b\n   c  d</p>");
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "a_b");
  EXPECT_EQ(lines[0].words[1].x, widthOf("a") + kSpace);
  EXPECT_EQ(text(lines[1]), "c_d");
  EXPECT_EQ(lines[1].words[0].x, 0);
}

TEST_F(WhiteSpaceFixture, NormalAndNowrapCollapseSpacesAndLineFeeds) {
  for (const char* value : {"normal", "nowrap"}) {
    SCOPED_TRACE(value);
    const auto lines = layout(std::string("pre { white-space: ") + value + " }", "<pre>a   b\n  c</pre>");
    ASSERT_EQ(lines.size(), 1u) << dump(lines);
    EXPECT_EQ(text(lines[0]), "a_b_c");
  }
}

// nowrap would set the paragraph on one line past the screen's edge: it wraps where normal text does instead.
TEST_F(WhiteSpaceFixture, NowrapWrapsAtTheColumnEdge) {
  Options narrow;
  narrow.width = 100;
  const auto lines = layout("p { white-space: nowrap }", "<p>one two three four five six seven eight</p>", narrow);
  ASSERT_GT(lines.size(), 1u) << dump(lines);
  std::string all;
  for (const Line& line : lines) {
    all += (all.empty() ? "" : "_") + text(line);
    const Word& last = line.words.back();
    EXPECT_LE(last.x + widthOf(last.text), narrow.width) << dump(lines);
  }
  EXPECT_EQ(all, "one_two_three_four_five_six_seven_eight");
}

// An element inside a <pre> may set white-space of its own: it is inherited like any other.
TEST_F(WhiteSpaceFixture, ANormalSpanInsidePreCollapses) {
  const auto lines = layout(".n { white-space: normal }", "<pre>a  <span class=\"n\">b   c</span>  d</pre>");
  ASSERT_EQ(lines.size(), 1u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "a  b_c  d");
}

// ---- <pre>'s own style ----

TEST_F(WhiteSpaceFixture, TheBooksCssDecidesWhatAPreKeeps) {
  const auto normal = layout("pre { white-space: normal }", "<pre>a  b\nc</pre>");
  ASSERT_EQ(normal.size(), 1u) << dump(normal);
  EXPECT_EQ(text(normal[0]), "a_b_c");
  const auto preLine = layout("pre { white-space: pre-line }", "<pre>a  b\nc</pre>");
  ASSERT_EQ(preLine.size(), 2u) << dump(preLine);
  EXPECT_EQ(text(preLine[0]), "a_b");
  // inherit leaves the element to HTML's own style, as an unset value does.
  const auto inherit = layout("pre { white-space: inherit }", "<pre>a  b</pre>");
  ASSERT_EQ(inherit.size(), 1u) << dump(inherit);
  EXPECT_EQ(text(inherit[0]), "a  b");
}

TEST_F(WhiteSpaceFixture, APreKeepsItsSpacesWithTheBooksStyleOff) {
  Options noCss;
  noCss.embeddedStyle = false;
  const auto lines = layout("pre { white-space: normal }", "<pre>a  b\n  c</pre>", noCss);
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "a  b");
  EXPECT_EQ(text(lines[1]), "  c");
}

// ---- Line feeds ----

// A line feed in a <pre> breaks the line; it does not start a block. The lines of a code block whose <pre> has margins
// stood one margin apart (40 px between 24 px lines here), and with the reader's paragraph indent on, every line was
// indented.
TEST_F(WhiteSpaceFixture, PreLineFeedsDoNotRepeatTheBlocksSpacing) {
  const auto lines = layout("pre { margin: 40px 0 40px 30px; }", "<p>Before</p><pre>a\nb\nc</pre><p>After</p>");
  ASSERT_EQ(lines.size(), 5u) << dump(lines);
  EXPECT_EQ(lines[1].y - lines[0].y, 24 + 12 + 40) << dump(lines);  // a line, the paragraph spacing, the top margin
  EXPECT_EQ(lines[2].y - lines[1].y, 24) << dump(lines);
  EXPECT_EQ(lines[3].y - lines[2].y, 24) << dump(lines);
  EXPECT_EQ(lines[4].y - lines[3].y, 24 + 40 + 12) << dump(lines);  // the bottom margin, once
  for (size_t i = 1; i <= 3; ++i) EXPECT_EQ(lines[i].x, 30) << dump(lines);
}

TEST_F(WhiteSpaceFixture, PreLinesTakeNoFirstLineIndentButTheBooks) {
  Options noSpacing;
  noSpacing.extraParagraphSpacing = false;  // the reader's paragraph indent is on
  const auto lines =
      layout("pre.ind { text-indent: 30px; }", "<pre>a\nb</pre><pre class=\"ind\">c\nd</pre><p>e</p>", noSpacing);
  ASSERT_EQ(lines.size(), 5u) << dump(lines);
  EXPECT_EQ(lines[0].words[0].x, 0) << dump(lines);  // the text's own spaces are its indent
  EXPECT_EQ(lines[1].words[0].x, 0) << dump(lines);
  EXPECT_EQ(lines[2].words[0].x, 30) << dump(lines);  // the book's indent, on its block's first line
  EXPECT_EQ(lines[3].words[0].x, 0) << dump(lines);
  EXPECT_GT(lines[4].words[0].x, 0) << dump(lines);  // and the reader's on the paragraph after it
}

// CSS indents a block's first line only, not a line after a forced break: any element that keeps its line feeds, not
// only <pre>, indents its first line alone, under the book's indent and under the reader's.
TEST_F(WhiteSpaceFixture, AKeptLineFeedIsNoNewFirstLine) {
  for (const char* value : {"pre-line", "pre-wrap", "break-spaces"}) {
    SCOPED_TRACE(value);
    const auto lines =
        layout(std::string("p.v { text-indent: 30px; white-space: ") + value + " }", "<p class=\"v\">a\nb\nc</p>");
    ASSERT_EQ(lines.size(), 3u) << dump(lines);
    EXPECT_EQ(lines[0].words[0].x, 30) << dump(lines);
    EXPECT_EQ(lines[1].words[0].x, 0) << dump(lines);
    EXPECT_EQ(lines[2].words[0].x, 0) << dump(lines);
  }
  Options noSpacing;
  noSpacing.extraParagraphSpacing = false;  // the reader's paragraph indent is on
  const auto lines = layout("p.v { white-space: pre-line }", "<p class=\"v\">a\nb</p>", noSpacing);
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_GT(lines[0].words[0].x, 0) << dump(lines);  // pre-line collapses its spaces: its first line is prose's
  EXPECT_EQ(lines[1].words[0].x, 0) << dump(lines);
}

TEST_F(WhiteSpaceFixture, AnyKeptLineFeedPartsLinesWithoutParagraphSpacing) {
  const auto lines =
      layout("p.v { white-space: pre-line; margin: 20px 0 }", "<p class=\"v\">a\nb\n\nc</p><p>After</p>");
  ASSERT_EQ(lines.size(), 5u) << dump(lines);
  EXPECT_EQ(text(lines[2]), " ") << dump(lines);  // the blank line stays a line
  for (size_t i = 1; i <= 3; ++i) EXPECT_EQ(lines[i].y - lines[i - 1].y, 24) << dump(lines);
  EXPECT_EQ(lines[4].y - lines[3].y, 24 + 20 + 12) << dump(lines);
}

// A table cell is laid out as one paragraph: a kept line feed there is a word space, and nothing reaches the page
// outside the table (a <pre> line feed in a cell used to lay out an empty line above it).
TEST_F(WhiteSpaceFixture, AKeptLineFeedInATableCellIsAWordSpace) {
  const auto lines = layout("", "<table><tr><td><pre>a  b\nc</pre></td><td>x</td></tr></table><p>After</p>");
  ASSERT_EQ(lines.size(), 3u) << dump(lines);
  EXPECT_EQ(lines[0].x, -1);
  EXPECT_EQ(text(lines[0]), "a  b_c") << dump(lines);
  EXPECT_EQ(text(lines[2]), "After") << dump(lines);
}

// ---- Wrapping ----

// pre does not wrap in CSS; here it wraps as pre-wrap does: after a run of spaces, which hangs at the line's end, so
// the next line starts with its text.
TEST_F(WhiteSpaceFixture, ALongPreLineWrapsAfterItsSpaces) {
  Options narrow;
  narrow.width = 120;
  const auto lines = layout("", "<pre>aaaa  bbbb  cccc  dddd  eeee</pre>", narrow);
  ASSERT_GT(lines.size(), 1u) << dump(lines);
  std::string all;
  for (const Line& line : lines) {
    all += text(line);
    EXPECT_NE(line.words.front().text[0], ' ') << dump(lines);
    size_t lastText = line.words.size() - 1;
    if (line.words[lastText].text[0] == ' ') --lastText;
    EXPECT_LE(line.words[lastText].x + widthOf(line.words[lastText].text), narrow.width) << dump(lines);
  }
  EXPECT_EQ(all, "aaaa  bbbb  cccc  dddd  eeee") << dump(lines);
}

// A run of spaces wider than the room left takes no room at the line's end: it does not send the word before it away.
TEST_F(WhiteSpaceFixture, ARunOfSpacesHangsAtTheLineEnd) {
  Options narrow;
  narrow.width = widthOf("aaaa") + kSpace;  // the word and one space; the run is four
  const auto lines = layout("", "<pre>aaaa    bbbb</pre>", narrow);
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "aaaa    ");
  EXPECT_EQ(text(lines[1]), "bbbb");
}

// An end-aligned line ends at its text, its trailing spaces past the edge.
TEST_F(WhiteSpaceFixture, AnEndAlignedLineEndsAtItsText) {
  Options book;
  book.alignment = kBooksOwn;
  const auto lines = layout("p { white-space: pre-wrap; text-align: right }", "<p>abc   </p><p>  def</p>", book);
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_EQ(lines[0].words[0].x + widthOf("abc"), 480) << dump(lines);
  EXPECT_EQ(lines[1].words[1].x + widthOf("def"), 480) << dump(lines);  // leading spaces take their room
}

// A token wider than the line is cut at the line's edge, as CSS's overflow-wrap cuts: no hyphen, and no syllable
// break that would leave a line half empty.
TEST_F(WhiteSpaceFixture, AnOverWideTokenIsCutAtTheEdgeWithoutAHyphen) {
  Options narrow;
  narrow.width = 120;
  const std::string token = "VeryLongIdentifierNameThatGoesOnAndOn()";
  const auto lines = layout("", "<pre>" + token + "</pre>", narrow);
  ASSERT_GT(lines.size(), 1u) << dump(lines);
  std::string all;
  for (size_t i = 0; i < lines.size(); ++i) {
    ASSERT_EQ(lines[i].words.size(), 1u) << dump(lines);
    const std::string& part = lines[i].words[0].text;
    all += part;
    EXPECT_LE(widthOf(part), narrow.width) << dump(lines);
    // Every line but the last is as full as a cut lets it be: one more character would not fit.
    if (i + 1 < lines.size()) {
      EXPECT_GT(widthOf(part + lines[i + 1].words[0].text.substr(0, 1)), narrow.width) << dump(lines);
    }
  }
  EXPECT_EQ(all, token) << dump(lines);
}

TEST_F(WhiteSpaceFixture, KeptTextIsNotHyphenated) {
  Options hyphenated;
  hyphenated.width = 190;
  hyphenated.hyphenation = true;
  const auto lines =
      layout("p.k { white-space: pre-wrap }",
             "<p class=\"k\">aaaa  uncharacteristically</p><p>aaaa uncharacteristically</p>", hyphenated);
  ASSERT_EQ(lines.size(), 4u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "aaaa  ");
  EXPECT_EQ(text(lines[1]), "uncharacteristically");
  // The same words outside it are hyphenated, so the test can tell.
  EXPECT_EQ(lines[2].words.back().text.back(), '-') << dump(lines);
}

// A long line of runs splits into several layout passes (the block's word cap) and keeps every word.
TEST_F(WhiteSpaceFixture, AVeryLongKeptLineKeepsEveryWord) {
  std::string longLine;
  for (int k = 0; k < 120; ++k) longLine += "w  ";
  Options narrow;
  narrow.width = 100;
  const auto lines = layout("", "<pre>" + longLine + "</pre>", narrow);
  size_t words = 0;
  for (const Line& line : lines) {
    for (const Word& w : line.words) words += w.text == "w" ? 1 : 0;
  }
  EXPECT_EQ(words, 120u) << dump(lines);
}

// ---- Alignment ----

TEST_F(WhiteSpaceFixture, KeptTextRunsFromItsStartSideUnderJustified) {
  Options justified;
  justified.alignment = kJustified;
  justified.width = 200;
  const auto lines = layout("", "<pre>aa bb  cc dd ee ff gg hh ii jj kk ll mm nn</pre>", justified);
  ASSERT_EQ(lines.size(), 2u) << dump(lines);
  EXPECT_EQ(lines[0].alignment, CssTextAlign::Left);
  EXPECT_EQ(lines[0].words[2].x, widthOf("aa") + kSpace) << dump(lines);  // nothing stretched
  const auto rtl = layout("", "<pre dir=\"rtl\">שלום</pre>", justified);
  ASSERT_EQ(rtl.size(), 1u) << dump(rtl);
  EXPECT_EQ(rtl[0].alignment, CssTextAlign::Right);
}

TEST_F(WhiteSpaceFixture, TheBooksCenteredPreStaysCentered) {
  Options justified;
  justified.alignment = kJustified;
  const auto lines = layout("pre { text-align: center }", "<pre>a  b</pre>", justified);
  ASSERT_EQ(lines.size(), 1u) << dump(lines);
  EXPECT_EQ(lines[0].alignment, CssTextAlign::Center);
  // An alignment the <pre> takes from a wrapper stands too.
  const auto wrapped = layout("div { text-align: center }", "<div><pre>a  b</pre></div>", justified);
  ASSERT_EQ(wrapped.size(), 1u) << dump(wrapped);
  EXPECT_EQ(wrapped[0].alignment, CssTextAlign::Center);
}

// Inline code that keeps its spaces, in a justified paragraph: its spaces keep their width while the paragraph's
// word spaces stretch.
TEST_F(WhiteSpaceFixture, KeptSpacesDoNotStretchInAJustifiedParagraph) {
  Options justified;
  justified.alignment = kJustified;
  justified.width = 200;
  const auto lines = layout("code { white-space: pre-wrap }",
                            "<p>one two <code>a  b c</code> three four five six seven eight nine ten</p>", justified);
  ASSERT_GT(lines.size(), 1u) << dump(lines);
  const std::vector<Word>& w = lines[0].words;
  ASSERT_EQ(text(lines[0]).substr(0, 15), "one_two_a  b c_") << dump(lines);
  EXPECT_GT(w[1].x - w[0].x, widthOf("one") + kSpace) << dump(lines);  // a word space, stretched
  EXPECT_EQ(w[4].x - w[2].x, widthOf("a") + 2 * kSpace) << dump(lines);
  EXPECT_EQ(w[6].x - w[4].x, widthOf("b") + kSpace) << dump(lines);
}

// ---- Scripts and layouts ----

// Right to left, the line's start is its right: the indent stands there, the run between the words between them.
TEST_F(WhiteSpaceFixture, RightToLeftKeepsItsIndentAtTheRight) {
  const auto lines = layout("", "<pre dir=\"rtl\">  שלום   עולם</pre>");
  ASSERT_EQ(lines.size(), 1u) << dump(lines);
  const std::vector<Word>& w = lines[0].words;
  ASSERT_EQ(w.size(), 4u) << dump(lines);
  EXPECT_EQ(w[3].text, "  ");
  EXPECT_EQ(w[3].x + 2 * kSpace, 480) << dump(lines);
  EXPECT_EQ(w[1].text, "   ");
  EXPECT_EQ(w[2].x - w[1].x, 3 * kSpace) << dump(lines);
}

TEST_F(WhiteSpaceFixture, CjkTextKeepsItsSpacesAndItsLineFeeds) {
  const auto pre = layout("", "<pre>中文  中文</pre>", Options{});
  ASSERT_EQ(pre.size(), 1u) << dump(pre);
  EXPECT_EQ(joined(pre[0]), "中文  中文");
  EXPECT_EQ(pre[0].words[3].x - pre[0].words[2].x, 2 * kSpace) << dump(pre);
  // A line feed between unspaced CJK text vanishes when it is collapsed; a kept one breaks the line.
  const auto collapsed = layout("", "<p>中文\n中文</p>");
  ASSERT_EQ(collapsed.size(), 1u) << dump(collapsed);
  const auto kept = layout("p { white-space: pre-line }", "<p>中文\n中文</p>");
  ASSERT_EQ(kept.size(), 2u) << dump(kept);
}

TEST_F(WhiteSpaceFixture, VerticalTextKeepsItsSpaces) {
  Options vertical;
  vertical.verticalText = VerticalText::On;
  vertical.language = "ja";
  const auto lines = layout("", "<pre>漢字  漢字</pre>", vertical);
  ASSERT_EQ(lines.size(), 1u) << dump(lines);
  EXPECT_EQ(joined(lines[0]), "漢字  漢字");
  EXPECT_EQ(lines[0].words[3].x - lines[0].words[2].x, 2 * kSpace) << dump(lines);
}

TEST_F(WhiteSpaceFixture, RubyStaysOverItsBaseInKeptText) {
  const auto lines = layout("", "<pre><ruby>漢<rt>かん</rt></ruby>  字</pre>");
  ASSERT_EQ(lines.size(), 1u) << dump(lines);
  EXPECT_EQ(text(lines[0]), "漢  字");
  EXPECT_EQ(lines[0].annotations, 1u);
}
