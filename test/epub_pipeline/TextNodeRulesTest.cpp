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
  for (int n = 240; n <= 272; ++n) {
    // "\xE2\x80\x94" is U+2014; the literal ends before "Dword" so D is not read as a hex digit.
    const std::string body = "<p>" + std::string(n, 'a') + "\xE2\x80\x94" + "Dword</p>";
    const std::string point = kP + "p[1]/text()[1]." + std::to_string(n + 1);
    EXPECT_EQ(push(body, static_cast<uint32_t>(n + 3)), point) << "n=" << n;
    EXPECT_EQ(pull(body, point), static_cast<uint32_t>(n + 3)) << "n=" << n;
  }
}
}  // namespace
