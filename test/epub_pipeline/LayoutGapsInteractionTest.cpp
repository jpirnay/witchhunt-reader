// Where the layout features of one round meet: CSS boxes, white-space, initial-letter drop caps, inline images and the
// reader's Paragraph Indent. Each has its own tests; these hold the places where two of them touch the same line or
// block. Through a real section build against the host renderer (line height 24, ascender 18).
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/ParagraphIndent.h"
#include "Epub/Section.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int kLineHeight = 24;  // the host renderer's
constexpr int kAscender = 18;    // its one em, as the paragraph indent reads it

struct Line {
  int page = 0;
  int x = 0;
  int y = 0;
  float mult = 1.0f;
  std::vector<std::string> words;
  std::vector<int> xpos;
};

struct Image {
  int page = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool inLine = false;
};

struct Box {
  int page = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct Laid {
  std::vector<Line> lines;
  std::vector<Image> images;
  std::vector<Box> boxes;
  std::vector<bool> pageHasImages;
};

struct Options {
  uint8_t indent = static_cast<uint8_t>(ParagraphIndent::FromBook);
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = true;
};

struct LayoutGapsFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("epub_gaps_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  Laid build(const std::string& css, const std::string& body, const Options& options = {}) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
        body + "\n</body></html>\n";
    const std::vector<SyntheticFile> files = {
        {"style.css", "text/css", css},
        {"icon.png", "image/png", syntheticPngHeader(16, 16)},
        {"tall.png", "image/png", syntheticPngHeader(40, 40)},
    };
    auto epub = syntheticBookFromDocument(work, books++, doc, files, "en");
    epub->loadImageManifest();
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = options.lineCompression;
    params.extraParagraphSpacing = options.extraParagraphSpacing;
    params.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
    params.paragraphIndent = options.indent;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    Laid laid;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_NE(nullptr, page);
      if (!page) continue;
      laid.pageHasImages.push_back(page->hasImages());
      for (const auto& el : page->elements) {
        switch (el->getTag()) {
          case TAG_PageLine: {
            const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
            Line line{p, el->xPos, el->yPos, block.getRenderStyle().fontSizeMultiplier, {}, {}};
            for (uint16_t w = 0; w < block.wordCount(); ++w) {
              line.words.emplace_back(block.wordText(w));
              line.xpos.push_back(block.wordXpos(w));
            }
            laid.lines.push_back(std::move(line));
            break;
          }
          case TAG_PageImage: {
            const auto& pageImage = static_cast<const PageImage&>(*el);
            const auto& image = pageImage.getImageBlock();
            laid.images.push_back({p, el->xPos, el->yPos, image.getWidth(), image.getHeight(), pageImage.isInLine()});
            break;
          }
          case TAG_PageBox: {
            const auto& box = static_cast<const PageBox&>(*el);
            laid.boxes.push_back({p, el->xPos, el->yPos, box.getWidth(), box.getHeight()});
            break;
          }
          default:
            break;
        }
      }
    }
    return laid;
  }
};

const Line* lineWith(const Laid& laid, const std::string& word) {
  for (const Line& line : laid.lines) {
    for (const std::string& w : line.words) {
      if (w == word) return &line;
    }
  }
  return nullptr;
}

int xOf(const Line& line, const std::string& word) {
  for (size_t i = 0; i < line.words.size(); ++i) {
    if (line.words[i] == word) return line.x + line.xpos[i];
  }
  return -1;
}

const std::string kProse =
    "he opening paragraph of the chapter runs on long enough to wrap beside a cap of several lines, line after line "
    "after line, until the cap is passed and the text takes the whole width of the column again for the rest of it.";

}  // namespace

// white-space x inline images: an image after a run of kept spaces sits where the next word would, with no word gap
// of its own (the run is the space), as a word after the run does.
TEST_F(LayoutGapsFixture, AnImageAfterKeptSpacesTakesNoGapOfItsOwn) {
  const Laid text = build("", "<pre>ab    cd</pre>");
  const Laid image = build("", "<pre>ab    <img src=\"icon.png\" alt=\"\"/>cd</pre>");
  const Line* textLine = lineWith(text, "cd");
  ASSERT_NE(nullptr, textLine);
  ASSERT_EQ(1u, image.images.size());
  EXPECT_TRUE(image.images[0].inLine);
  EXPECT_EQ(xOf(*textLine, "cd"), image.images[0].x) << "the image took a word gap after the kept spaces";
}

// Boxes x inline images: a box's frame starts below the glyphs of the line above, and an image that makes that line
// taller lowers its text, glyphs and all. Under a tight line spacing the glyphs reach past the line's advance.
TEST_F(LayoutGapsFixture, ABoxBelowALineAnImageLowersStartsBelowItsGlyphs) {
  Options tight;
  tight.lineCompression = 0.9f;
  tight.extraParagraphSpacing = false;  // nothing between the line and the frame but the frame's own rule
  const Laid laid =
      build("p, div { margin: 0; } .frame { border: 1px solid; }",
            "<p>Lamp <img src=\"tall.png\" alt=\"\"/> lit</p><div class=\"frame\">Boxed words</div>", tight);
  const Line* imageLine = lineWith(laid, "Lamp");
  ASSERT_NE(nullptr, imageLine);
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_TRUE(laid.images[0].inLine);
  ASSERT_EQ(1u, laid.boxes.size());
  // The text was lowered by the image (it rises 22 px above the 18 px ascender).
  EXPECT_GT(imageLine->y, laid.images[0].y);
  EXPECT_GE(laid.boxes[0].y, imageLine->y + kLineHeight) << "the frame starts inside the glyphs of the line above";
}

// Boxes x inline images: a line image inside a box is framed with its line; a page with a box and no image is a text
// page to the gray pass (Black & White's and one-pass AA's decisions read hasImages).
TEST_F(LayoutGapsFixture, ALineImageInABoxIsFramedWithItsLine) {
  const Laid laid = build(".frame { border: 2px solid; padding: 6px; }",
                          "<div class=\"frame\"><p>Press the <img src=\"icon.png\" alt=\"key\"/> key.</p></div>");
  ASSERT_EQ(1u, laid.boxes.size());
  ASSERT_EQ(1u, laid.images.size());
  const Box& box = laid.boxes[0];
  const Image& image = laid.images[0];
  EXPECT_TRUE(image.inLine);
  EXPECT_GE(image.x, box.x + 2);
  EXPECT_LE(image.x + image.width, box.x + box.width - 2);
  EXPECT_GE(image.y, box.y + 2);
  EXPECT_LE(image.y + image.height, box.y + box.height - 2);
  EXPECT_TRUE(laid.pageHasImages[0]);

  const Laid textOnly =
      build(".frame { border: 2px solid; background: #eee; }", "<div class=\"frame\"><p>Text.</p></div>");
  ASSERT_EQ(1u, textOnly.boxes.size());
  EXPECT_FALSE(textOnly.pageHasImages[0]) << "a box made its page an image page";
}

// Boxes x white-space: code in a framed <pre> keeps its indentation, every line inside the frame's padding, and the
// frame holds all of them.
TEST_F(LayoutGapsFixture, KeptTextInABoxKeepsItsIndentationInsideTheFrame) {
  const Laid laid = build("pre { border: 1px solid; padding: 4px; }", "<pre>int f() {\n    return 1;\n}</pre>");
  ASSERT_EQ(1u, laid.boxes.size());
  ASSERT_EQ(3u, laid.lines.size());
  const Box& box = laid.boxes[0];
  const Line& inner = laid.lines[1];
  ASSERT_FALSE(inner.words.empty());
  EXPECT_EQ("    ", inner.words[0]) << "the kept indentation was lost";
  for (const Line& line : laid.lines) {
    EXPECT_GE(line.x, box.x + 1 + 4);
    EXPECT_GE(line.y, box.y + 1);
    EXPECT_LE(line.y + kLineHeight, box.y + box.height) << "a kept line ran out of its frame";
  }
  // Every line of the block starts at the same edge: a kept line feed takes no first-line indent.
  EXPECT_EQ(laid.lines[0].x, laid.lines[1].x);
  EXPECT_EQ(laid.lines[0].x, laid.lines[2].x);
}

// Boxes x Paragraph Indent: a fixed indent in a box is measured from the box's inner edge.
TEST_F(LayoutGapsFixture, AFixedIndentInABoxStartsAtItsInnerEdge) {
  Options twoEm;
  twoEm.indent = static_cast<uint8_t>(ParagraphIndent::TwoEm);
  const std::string longText =
      "the quick brown fox jumps over the lazy dog and keeps on running far past the hedge into the next field";
  const Laid laid = build(".frame { border: 2px solid; padding: 10px; }",
                          "<div class=\"frame\"><p>First " + longText + "</p></div>", twoEm);
  ASSERT_EQ(1u, laid.boxes.size());
  const Line* first = lineWith(laid, "First");
  ASSERT_NE(nullptr, first);
  const Line* second = nullptr;
  for (const Line& line : laid.lines) {
    if (line.y > first->y && (second == nullptr || line.y < second->y)) second = &line;
  }
  ASSERT_NE(nullptr, second);
  EXPECT_GE(second->x, laid.boxes[0].x + 2 + 10);
  EXPECT_EQ(second->x + 2 * kAscender, xOf(*first, "First"));
}

// initial-letter x Paragraph Indent: a cap's paragraph keeps its lines beside the cap under every setting (a block
// beside a float keeps Book's Style), and the next paragraph takes the setting.
TEST_F(LayoutGapsFixture, ACapsParagraphIsTheSameUnderEveryIndent) {
  const std::string css = ".cap { initial-letter: 3; } p { text-indent: 0; }";
  const std::string body = "<p><span class=\"cap\">T</span>" + kProse + "</p><p>Second paragraph here.</p>";
  std::vector<int> besideX;
  for (uint8_t setting = 0; setting <= static_cast<uint8_t>(ParagraphIndent::FromBook); ++setting) {
    Options options;
    options.indent = setting;
    options.extraParagraphSpacing = false;
    const Laid laid = build(css, body, options);
    const Line* cap = nullptr;
    for (const Line& line : laid.lines) {
      if (line.mult > 1.5f) cap = &line;
    }
    ASSERT_NE(nullptr, cap) << "no cap under indent setting " << int(setting);
    const Line* firstText = lineWith(laid, "he");
    ASSERT_NE(nullptr, firstText);
    besideX.push_back(xOf(*firstText, "he"));
    const Line* second = lineWith(laid, "Second");
    ASSERT_NE(nullptr, second);
    const int expected =
        setting < kParagraphIndentFixedSteps ? (static_cast<int>(setting) * 5 * kAscender + 5) / 10 : 0;
    EXPECT_EQ(expected, xOf(*second, "Second")) << "indent setting " << int(setting);
  }
  for (const int x : besideX) EXPECT_EQ(besideX.front(), x) << "the text beside the cap moved with the setting";
}
