// A line may end after a dash, and a dash takes no space it did not have in the source.
//
// Before: where an inline tag met a dash ("gone—<i>Umbriel</i>") the parser dropped the glue so the line could break
// there, and the word after the tag got a real, stretchable word gap the book never had. A dash inside one word
// ("gone—Umbriel") had no break at all unless hyphenation was on, and hyphenation is off by default on the X3 and X4.
// Rule ported from crosspoint-reader PR #3883 (Phạm Bình An), by way of CrossInk 59b53529 (Julia Nguyen).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Word {
  std::string text;
  int x;
  bool continues;
};
using Line = std::vector<Word>;

struct DashBreakFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_dash_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::vector<Line> layout(const std::string& body, const int width,
                           const CssTextAlign align = CssTextAlign::Left) const {
    const auto epub = syntheticBook(work, 0, body);
    Section::BuildParams params;
    params.viewportWidth = static_cast<uint16_t>(width);
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.extraParagraphSpacing = true;  // no first-line indent: the widths below are the text's own
    params.paragraphAlignment = static_cast<uint8_t>(align);
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<Line> lines;
    section.currentPage = 0;
    const auto page = section.loadPageFromSectionFile();
    if (!page) return lines;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
      Line line;
      for (uint16_t w = 0; w < block.wordCount(); ++w) {
        line.push_back({block.wordText(w), block.wordXpos(w), block.wordContinues(w)});
      }
      if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
  }

  static int widthOf(const std::string& text) {
    GfxRenderer renderer;
    return renderer.getTextWidth(0, text.c_str());
  }

  static std::string joined(const Line& line) {
    std::string out;
    for (size_t i = 0; i < line.size(); ++i) {
      if (i > 0 && !line[i].continues) out += ' ';
      out += line[i].text;
    }
    return out;
  }
  static std::vector<std::string> joined(const std::vector<Line>& lines) {
    std::vector<std::string> out;
    for (const Line& line : lines) out.push_back(joined(line));
    return out;
  }
};

using Strings = std::vector<std::string>;

// Wide enough for `first` alone, too narrow for what follows it.
int widthFor(const std::string& first) { return DashBreakFixture::widthOf(first) + 4; }

}  // namespace

TEST_F(DashBreakFixture, ADashBeforeAnInlineTagGetsNoSpace) {
  const auto lines = layout("<p>gone—<i>Umbriel</i> was</p>", 480);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(joined(lines[0]), "gone—Umbriel was");
  ASSERT_GE(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][1].x - lines[0][0].x, widthOf("gone—"));
}

TEST_F(DashBreakFixture, ADashClosingAnInlineTagGetsNoSpace) {
  const auto lines = layout("<p><i>gone—</i>Umbriel was</p>", 480);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(joined(lines[0]), "gone—Umbriel was");
}

// Justification hands out spare width at word gaps: this seam is not one, on a line that stretches.
TEST_F(DashBreakFixture, JustificationDoesNotOpenTheSeamAfterADash) {
  const auto lines =
      layout("<p>aaaa gone—<i>Umbriel</i> bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn</p>", 200,
             CssTextAlign::Justify);
  ASSERT_GE(lines.size(), 2u);
  const Line& line = lines[0];
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    if (line[i].text != "gone—") continue;
    EXPECT_EQ(line[i + 1].x - line[i].x, widthOf("gone—"));
    return;
  }
  FAIL() << "the seam is not on the first line: " << joined(line);
}

TEST_F(DashBreakFixture, ALineMayEndAtADashBeforeAnInlineTag) {
  const auto lines = layout("<p>aaaaaa—<i>bbbbbb</i></p>", widthFor("aaaaaa—"));
  EXPECT_EQ(joined(lines), (Strings{"aaaaaa—", "bbbbbb"}));
}

TEST_F(DashBreakFixture, ALineMayEndAtADashInsideAWord) {
  const auto lines = layout("<p>aaaaaa—bbbbbb</p>", widthFor("aaaaaa—"));
  EXPECT_EQ(joined(lines), (Strings{"aaaaaa—", "bbbbbb"}));
}

TEST_F(DashBreakFixture, ALineMayEndAtAHyphenInsideAWord) {
  const auto lines = layout("<p>aaaaaa-bbbbbb</p>", widthFor("aaaaaa-"));
  EXPECT_EQ(joined(lines), (Strings{"aaaaaa-", "bbbbbb"}));
}

TEST_F(DashBreakFixture, AWordThatFitsStaysTogetherAcrossItsDash) {
  const auto lines = layout("<p>aaaaaa—bbbbbb cccc</p>", 480);
  EXPECT_EQ(joined(lines), (Strings{"aaaaaa—bbbbbb cccc"}));
}

// The word goes to the next line whole: these are not break opportunities.
TEST_F(DashBreakFixture, ANonBreakingHyphenIsNoBreak) {
  const auto lines = layout("<p>xxxxxxxx aaaa‑bbbb</p>", widthFor("xxxxxxxx aaaa"));
  EXPECT_EQ(joined(lines), (Strings{"xxxxxxxx", "aaaa‑bbbb"}));
}

TEST_F(DashBreakFixture, AMinusSignIsNoBreak) {
  const auto lines = layout("<p>xxxxxxxx aaaa−bbbb</p>", widthFor("xxxxxxxx aaaa"));
  EXPECT_EQ(joined(lines), (Strings{"xxxxxxxx", "aaaa−bbbb"}));
}

TEST_F(DashBreakFixture, AHyphenBeforeADigitIsNoBreak) {
  const auto lines = layout("<p>xxxxxxxx aaaa-5555</p>", widthFor("xxxxxxxx aaaa"));
  EXPECT_EQ(joined(lines), (Strings{"xxxxxxxx", "aaaa-5555"}));
}

TEST_F(DashBreakFixture, ARunOfDashesStaysTogether) {
  const auto lines = layout("<p>aaaaaa——bbbbbb</p>", widthFor("aaaaaa—") + 2);
  EXPECT_NE(joined(lines)[0], "aaaaaa—");
}

TEST_F(DashBreakFixture, ADashOpeningAWordStaysOnIt) {
  const auto lines = layout("<p>xxxxxxxx —aaaa</p>", widthFor("xxxxxxxx —"));
  EXPECT_EQ(joined(lines), (Strings{"xxxxxxxx", "—aaaa"}));
}

TEST_F(DashBreakFixture, ClosingPunctuationStaysWithTheDashBeforeIt) {
  const auto lines = layout("<p>xxxxxxxx aaaa—”</p>", widthFor("xxxxxxxx aaaa—"));
  EXPECT_EQ(joined(lines), (Strings{"xxxxxxxx", "aaaa—”"}));
}
