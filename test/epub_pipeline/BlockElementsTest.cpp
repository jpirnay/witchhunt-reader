// HTML's block elements, as a book without any stylesheet has them.
//
// The parser knows blocks by name. <dl>, <dt>, <dd>, <figure>, <figcaption>, <center>, <address>, <header>, <footer>,
// <nav> and the rest were on no list, so they were laid out as inline text: nothing ended a word at </dt>, and
// <dt>Term</dt><dd>Meaning</dd> came out as the one word "TermMeaning"; <center>* * *</center> joined the paragraph
// before it.
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

struct Line {
  std::string text;
  int x;  // the first word's left edge
};

struct BlockElementsFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_blocks_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::vector<Line> layout(const std::string& css, const std::string& body,
                           const CssTextAlign align = CssTextAlign::Left) {
    const auto epub = syntheticBookFromDocument(
        work, 0,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
            body + "\n</body></html>\n",
        {{"style.css", "text/css", "p { margin: 0; text-indent: 0; }\n" + css}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
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
      if (block.wordCount() == 0) continue;
      Line line{{}, el->xPos + block.wordXpos(0)};
      for (uint16_t w = 0; w < block.wordCount(); ++w) {
        if (w > 0) line.text += ' ';
        line.text += block.wordText(w);
      }
      lines.push_back(line);
    }
    return lines;
  }

  static std::vector<std::string> texts(const std::vector<Line>& lines) {
    std::vector<std::string> out;
    for (const Line& line : lines) out.push_back(line.text);
    return out;
  }
};

using Strings = std::vector<std::string>;

}  // namespace

TEST_F(BlockElementsFixture, ADefinitionListIsAParagraphPerTermAndDefinition) {
  const auto lines = layout("", "<dl><dt>Term</dt><dd>Meaning</dd><dt>Other</dt><dd>Second</dd></dl>");
  EXPECT_EQ(texts(lines), (Strings{"Term", "Meaning", "Other", "Second"}));
}

TEST_F(BlockElementsFixture, ADefinitionListDoesNotJoinTheTextAroundIt) {
  const auto lines = layout("", "<p>Before</p><dl><dt>Term</dt><dd>Meaning</dd></dl><p>After</p>");
  EXPECT_EQ(texts(lines), (Strings{"Before", "Term", "Meaning", "After"}));
}

// A definition sits indented under its term in every browser, with or without a stylesheet.
TEST_F(BlockElementsFixture, ADefinitionIsIndentedUnderItsTerm) {
  const auto lines = layout("", "<dl><dt>Term</dt><dd>Meaning</dd></dl>");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_GT(lines[1].x, lines[0].x);
}

TEST_F(BlockElementsFixture, TheIndentReachesAParagraphInsideADefinition) {
  const auto lines = layout("", "<dl><dt>Term</dt><dd><p>Meaning</p></dd></dl>");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_GT(lines[1].x, lines[0].x);
}

TEST_F(BlockElementsFixture, TheBooksOwnMarginReplacesTheDefinitionIndent) {
  const auto lines = layout("dd { margin-left: 0; }\n", "<dl><dt>Term</dt><dd>Meaning</dd></dl>");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].x, lines[0].x);
}

TEST_F(BlockElementsFixture, AFigureCaptionIsABlockOfItsOwn) {
  const auto lines = layout("", "<p>Before</p><figure><figcaption>Caption</figcaption></figure><p>After</p>");
  EXPECT_EQ(texts(lines), (Strings{"Before", "Caption", "After"}));
}

// Centred as a rule from the book would be: under Book's Style.
TEST_F(BlockElementsFixture, CenterIsACenteredBlock) {
  const auto lines = layout("", "<p>One</p><center>* * *</center><p>Two</p>", CssTextAlign::None);
  ASSERT_EQ(texts(lines), (Strings{"One", "* * *", "Two"}));
  EXPECT_GT(lines[1].x, 100) << "centred on a 480 px page";
  EXPECT_EQ(lines[0].x, lines[2].x);
}

TEST_F(BlockElementsFixture, ParagraphsInsideCenterAreCentered) {
  const auto lines = layout("", "<center><p>Inside</p></center><p>Outside</p>", CssTextAlign::None);
  ASSERT_EQ(texts(lines), (Strings{"Inside", "Outside"}));
  EXPECT_GT(lines[0].x, 100);
  EXPECT_LT(lines[1].x, 100);
}

TEST_F(BlockElementsFixture, TheReadersAlignmentOverridesCenterAsItDoesTheBooksRules) {
  const auto lines = layout("", "<p>One</p><center>* * *</center>", CssTextAlign::Left);
  ASSERT_EQ(texts(lines), (Strings{"One", "* * *"}));
  EXPECT_LT(lines[1].x, 100) << "not centred on a 480 px page";
}

TEST_F(BlockElementsFixture, TheBooksOwnRuleBeatsTheDefaultOfCenter) {
  const auto lines = layout("center { text-align: left; }\n", "<center>Text</center>", CssTextAlign::None);
  ASSERT_EQ(texts(lines), (Strings{"Text"}));
  EXPECT_LT(lines[0].x, 100);
}

class BlockElementTagFixture : public BlockElementsFixture, public testing::WithParamInterface<const char*> {};

TEST_P(BlockElementTagFixture, StartsAndEndsABlock) {
  const std::string tag = GetParam();
  const auto lines = layout("", "<p>Before</p><" + tag + ">Inside</" + tag + "><p>After</p>");
  EXPECT_EQ(texts(lines), (Strings{"Before", "Inside", "After"}));
}

INSTANTIATE_TEST_SUITE_P(HtmlBlocks, BlockElementTagFixture,
                         testing::Values("address", "details", "fieldset", "figure", "footer", "form", "header",
                                         "hgroup", "legend", "nav", "summary"));
