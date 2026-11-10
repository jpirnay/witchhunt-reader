// CSS white-space: which spaces, tabs and line feeds a book's text keeps.
#include <gtest/gtest.h>
#include <unistd.h>

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
struct Line {
  int y;
  int x;
  std::vector<Word> words;
};

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
    bool extraParagraphSpacing = true;
    bool hyphenation = false;
    uint8_t alignment = 1;  // left
    bool embeddedStyle = true;
  };

  std::vector<Line> layout(const std::string& css, const std::string& body, const Options& options) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>" +
        body + "</body></html>\n";
    const auto epub = syntheticBookFromDocument(work, books++, doc, {{"style.css", "text/css", css}});
    Section::BuildParams params;
    params.viewportWidth = static_cast<uint16_t>(options.width);
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.extraParagraphSpacing = options.extraParagraphSpacing;
    params.hyphenationEnabled = options.hyphenation;
    params.paragraphAlignment = options.alignment;
    params.embeddedStyle = options.embeddedStyle;
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
        Line line{el->yPos + p * 1000, el->xPos, {}};
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          line.words.push_back({block.wordText(w), block.wordXpos(w), block.wordContinues(w)});
        }
        lines.push_back(std::move(line));
      }
    }
    return lines;
  }
  std::vector<Line> layout(const std::string& css, const std::string& body) { return layout(css, body, Options{}); }
};

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

TEST_F(WhiteSpaceFixture, PreLinesAfterALineFeedTakeNoFirstLineIndent) {
  Options noSpacing;
  noSpacing.extraParagraphSpacing = false;  // the reader's paragraph indent is on
  const auto lines =
      layout("pre.ind { text-indent: 30px; }", "<pre>a\nb</pre><pre class=\"ind\">c\nd</pre>", noSpacing);
  ASSERT_EQ(lines.size(), 4u) << dump(lines);
  EXPECT_EQ(lines[1].words[0].x, 0) << dump(lines);
  EXPECT_EQ(lines[2].words[0].x, 30) << dump(lines);  // the book's indent, on its block's first line
  EXPECT_EQ(lines[3].words[0].x, 0) << dump(lines);
}
