// The text() numbering and .M offsets we push and pull follow crengine's DOM, not our SAX stream.
// Rules R1-R5 and the crengine functions they come from are in ChapterXPathForwardMapper.cpp's
// header comment. Offsets below are visible bytes before the target word (Task 1's rule: every
// non-whitespace byte counts; NBSP from &nbsp; is two).
#include <KOReaderSync/ChapterXPathIndexer.h>
#include <KOReaderSync/ProgressMapper.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "SyntheticBook.h"

namespace {
namespace fs = std::filesystem;

struct TextNodeRules : testing::Test {
  fs::path work;
  int bookCount = 0;
  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("text_node_rules_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string push(const std::string& body, const uint32_t offset) {
    return ChapterXPathIndexer::findXPathForVisibleOffset(syntheticBook(work, bookCount++, body), 0, offset);
  }
  // The visible offset the pull lands on for a KOReader text point, or UINT32_MAX when inexact.
  uint32_t pull(const std::string& body, const std::string& xpath) {
    const auto pos =
        ProgressMapper::toCrossPoint(syntheticBook(work, bookCount++, body), KOReaderPosition{xpath, 0.5f});
    return pos.hasVisibleTextOffset ? pos.visibleTextOffset : UINT32_MAX;
  }
};

const std::string kP = "/body/DocFragment[1]/body/";

// R1: the leading whitespace of a block is not a text node.
TEST_F(TextNodeRules, LeadingWhitespaceOfABlockIsNotANode) {
  EXPECT_EQ(push("<p>\n  <em>x</em> rest of it</p>", 1), kP + "p[1]/text()[1].1");
}

// R2: a whitespace run is one codepoint.
TEST_F(TextNodeRules, HardWrappedParagraphCountsCollapsedCodepoints) {
  EXPECT_EQ(push("<p>Hello\n      world\n      again</p>", 10), kP + "p[1]/text()[1].12");
}

// R2 across SAX chunks: a run longer than any parser buffer is still one codepoint.
TEST_F(TextNodeRules, AWhitespaceRunSplitAcrossChunksIsStillOneCodepoint) {
  const std::string run(3000, ' ');
  EXPECT_EQ(push("<p>Hello" + run + "world</p>", 5), kP + "p[1]/text()[1].6");
  EXPECT_EQ(push("<p>" + run + "<em>x</em> rest</p>", 1), kP + "p[1]/text()[1].1");
}

// R5: pre keeps everything.
TEST_F(TextNodeRules, PreKeepsRawWhitespaceAndItsLeadingRun) {
  EXPECT_EQ(push("<pre>Hello\n      world</pre>", 5), kP + "pre[1]/text()[1].12");
  EXPECT_EQ(push("<pre>\n  <b>x</b> y</pre>", 1), kP + "pre[1]/text()[2].1");
}

// R3: whitespace next to a block sibling is gone in mixed content; between inlines it stays.
TEST_F(TextNodeRules, MixedContentDropsWhitespaceNextToBlocks) {
  EXPECT_EQ(push("<div>\n<p>a</p>\n<span>s</span>\nloose text</div>", 2), kP + "div[1]/text()[1].1");
  EXPECT_EQ(push("<div>\n<span>s</span>\nloose <em>e</em> text\n<p>later</p></div>", 7), kP + "div[1]/text()[2].1");
  // The space between the spans has inline neighbours on both sides: it stays as text()[1].
  EXPECT_EQ(push("<div><p>c</p><span>a</span> <span>b</span>tail</div>", 3), kP + "div[1]/text()[2].0");
}

// table, thead, tbody, tfoot and tr keep no text in crengine: stray text there names the element,
// because a text point would be a null XPointer (R4). A cell keeps its text point.
TEST_F(TextNodeRules, StrayTableTextPushesAsTheElementPath) {
  EXPECT_EQ(push("<table>stray<tr><td>x</td></tr></table>", 0), kP + "table[1]");
  EXPECT_EQ(push("<table><tr><td>cell text</td></tr></table>", 0), kP + "table[1]/tr[1]/td[1]/text()[1].0");
}

// Text directly inside an inline element is a text point inside it. Offset 5 is the 6th visible
// byte, the "w" of "words" (whitespace is not counted); in the span's text node "first " is 6
// codepoints, so the point is .6.
TEST_F(TextNodeRules, TextInsideASpanPushesAsATextPointInsideTheSpan) {
  const std::string body = "<p><span>first words and more words</span></p>";
  EXPECT_EQ(push(body, 5), kP + "p[1]/span[1]/text()[1].6");
  EXPECT_EQ(pull(body, kP + "p[1]/span[1]/text()[1].6"), 5u);
}

// NBSP is a character, not whitespace, on both sides.
TEST_F(TextNodeRules, NbspIsACharacter) { EXPECT_EQ(push("<p>a&nbsp;&nbsp;b</p>", 5), kP + "p[1]/text()[1].3"); }

// The pull resolves the same numbering, so a KOReader text point lands where crengine meant.
TEST_F(TextNodeRules, ThePullUsesTheSameRules) {
  EXPECT_EQ(pull("<p>\n  <em>x</em> rest of it</p>", kP + "p[1]/text()[1].1"), 1u);
  EXPECT_EQ(pull("<p>Hello\n      world\n      again</p>", kP + "p[1]/text()[1].12"), 10u);
  EXPECT_EQ(pull("<pre>Hello\n      world</pre>", kP + "pre[1]/text()[1].12"), 5u);
  EXPECT_EQ(pull("<div>\n<p>a</p>\n<span>s</span>\nloose text</div>", kP + "div[1]/text()[1].1"), 2u);
}

// A 3-byte codepoint (U+2014) split by the SAX buffer's flush still names, and resolves to, the
// word after it. Sweeps the filler so the split lands wherever the parser's flush falls.
TEST_F(TextNodeRules, ACodepointSplitAtTheChunkBoundaryStillLandsExactly) {
  // The range brackets the SAX parser's 256-byte content flush; widen it if that buffer changes.
  for (int n = 240; n <= 272; ++n) {
    // "\xE2\x80\x94" is U+2014; the literal ends before "Dword" so D is not read as a hex digit.
    const std::string body = "<p>" + std::string(n, 'a') + "\xE2\x80\x94" + "Dword</p>";
    const std::string point = kP + "p[1]/text()[1]." + std::to_string(n + 1);
    EXPECT_EQ(push(body, static_cast<uint32_t>(n + 3)), point) << "n=" << n;
    EXPECT_EQ(pull(body, point), static_cast<uint32_t>(n + 3)) << "n=" << n;
  }
}

// A page that starts at the chapter's total (a last page holding only an image or spacing) has no
// text at or after its start: the push names the end of the last text node, which pulls back to
// the total. "last words" is 9 visible bytes and 10 collapsed codepoints.
TEST_F(TextNodeRules, AnOffsetAtTheChapterEndNamesTheEndOfTheLastText) {
  EXPECT_EQ(push("<p>last words</p>", 9), kP + "p[1]/text()[1].10");
  EXPECT_EQ(pull("<p>last words</p>", kP + "p[1]/text()[1].10"), 9u);
}

// A bare HTML <br> is not XML; the layout parser repairs it, and the mappers must parse the same
// tree: <br> is an empty element, so "two" is the paragraph's second text node.
TEST_F(TextNodeRules, ABareBrParsesLikeTheLayoutParser) {
  EXPECT_EQ(push("<p>one<br>two</p>", 3), kP + "p[1]/text()[2].0");
  EXPECT_EQ(pull("<p>one<br>two</p>", kP + "p[1]/text()[2].0"), 3u);
}

// Calibre output from some Kindle/MOBI sources keeps every paragraph in <div><span>, with only an
// empty <p id="filepos..."> per chapter (crosspoint-reader issue #3665). A push that counts only
// text inside <p>/<li> names that empty p[1], and KOReader opens the chapter's first page. Ours
// names whatever element holds the text; these pin that shape.
//
// Ported from crosspoint-reader PR #3923 ("fix(KOSync): resolve upload XPaths for text outside
// <p>/<li>", Brendan LeFebvre / @brendanlefebvre). The fixture markup and the text points are
// theirs; the offsets are recomputed for our rule, which counts no whitespace:
//   "3" 0 | "She drove" 1-8 | "back to D.C." 9-18 | U+201C 19-21, "Who?" 22-25, U+201D 26-28 | 29
const std::string kKindleDivSpan =
    "\n<p id=\"filepos1\" class=\"calibre1\"></p>\n"
    "<div class=\"calibre8\"><span class=\"calibre9\">3</span></div><div class=\"calibre10\"> </div>\n"
    "<div class=\"calibre12\"><span class=\"calibre6\"><span class=\"bold\">She drove</span> back to "
    "D.C.</span></div>\n"
    "<div class=\"calibre19\"><span class=\"calibre6\">\xE2\x80\x9CWho?\xE2\x80\x9D</span></div>\n";

// div[2] holds only a space, which is not a text node (R1), so "She drove" is in div[3]. The
// space before "back" follows an inline sibling and stays (R3), so "to" is codepoint 6.
TEST_F(TextNodeRules, KindleDivSpanTextPushesAsTextPointsInsideTheDivs) {
  EXPECT_EQ(push(kKindleDivSpan, 0), kP + "div[1]/span[1]/text()[1].0");
  EXPECT_EQ(push(kKindleDivSpan, 1), kP + "div[3]/span[1]/span[1]/text()[1].0");
  EXPECT_EQ(push(kKindleDivSpan, 13), kP + "div[3]/span[1]/text()[1].6");
  EXPECT_EQ(push(kKindleDivSpan, 19), kP + "div[4]/span[1]/text()[1].0");
}

TEST_F(TextNodeRules, KindleDivSpanTextPointsPullBackToTheSameText) {
  EXPECT_EQ(pull(kKindleDivSpan, kP + "div[1]/span[1]/text()[1].0"), 0u);
  EXPECT_EQ(pull(kKindleDivSpan, kP + "div[3]/span[1]/span[1]/text()[1].0"), 1u);
  EXPECT_EQ(pull(kKindleDivSpan, kP + "div[3]/span[1]/text()[1].6"), 13u);
  EXPECT_EQ(pull(kKindleDivSpan, kP + "div[4]/span[1]/text()[1].0"), 19u);
}

// The chapter's total names the end of the last text, not the trailing newline after it.
TEST_F(TextNodeRules, KindleDivSpanEndNamesTheEndOfTheLastText) {
  EXPECT_EQ(push(kKindleDivSpan, 29), kP + "div[4]/span[1]/text()[1].6");
}

// Text with no paragraph around it at all: straight in a div, or in the body. syntheticBook writes
// "<body>\n", so the body's text node is "\nHello world": the newline is not whitespace-only, so it
// stays, as one space (R2), and "w" is codepoint 7 (upstream's fixture had no newline: .6).
TEST_F(TextNodeRules, TextOutsideAnyParagraphIsATextPoint) {
  EXPECT_EQ(push("<div>not a paragraph or list item</div>", 0), kP + "div[1]/text()[1].0");
  EXPECT_EQ(push("Hello world", 5), kP + "text()[1].7");
  EXPECT_EQ(pull("Hello world", kP + "text()[1].7"), 5u);
}
}  // namespace
