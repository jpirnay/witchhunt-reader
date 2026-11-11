// Inline images: an image a line or two tall in a run of text is set in its line, as a word is (the parser's line
// images, ParsedText's inline objects). Through a real section build against the host renderer (line height 24,
// ascender 18), what has to hold:
//
//   1. A small image mid-sentence sits in the line, between its words, its bottom on the baseline, and the paragraph
//      stays one paragraph.
//   2. An image taller than the text lowers the line's text by what it rises above it, and the next line follows the
//      taller line; nothing overlaps the line above.
//   3. vertical-align middle, text-top, text-bottom, top and bottom place it against the text.
//   4. Larger images, images the book makes a block or floats, and the images of a paragraph that holds no text keep
//      today's block placement: centred, on a line of their own.
//   5. A line taller than the room left goes to the next page whole; the page's image list counts the image.
//   6. The line's text holds no U+FFFC: nothing to read, select or look up in place of the image.
//   7. Many images in one paragraph are all placed (the parser lays them out a split at a time).
//   8. In a right-to-left paragraph and in CJK text the image sits between its neighbours. Down a column it is an
//      upright character (a gaiji), one em across at most and centred on the column; a picture there and an image in
//      a ruby keep the block path.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int kViewportWidth = 480;
constexpr int kLineHeight = 24;  // the host renderer's
constexpr int kAscender = 18;

struct PlacedLine {
  int page = 0;
  int x = 0;
  int y = 0;
  std::vector<std::string> words;
  std::vector<int> xpos;
  std::vector<EpdFontFamily::Style> styles;
  std::string reading;
};

struct PlacedImage {
  int page = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool inLine = false;
  uint32_t linkHash = 0;
  bool turned = false;
};

struct Laid {
  std::vector<PlacedLine> lines;
  std::vector<PlacedImage> images;
  std::vector<bool> pageHasImages;
  std::vector<bool> pageImageOnly;
  std::vector<FootnoteEntry> links;  // every page's, in page order
  uint16_t pages = 0;
};

struct InlineImageFixture : testing::Test {
  fs::path work;
  int bookIndex = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / (std::string("epub_inline_img_") + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The book's images: icon 16x16, tall 40x40 (under two lines of 24), big 120x120 (a picture), wide 300x20.
  std::vector<SyntheticFile> images(const std::string& css) const {
    std::vector<SyntheticFile> files = {
        {"icon.png", "image/png", syntheticPngHeader(16, 16)},
        {"tall.png", "image/png", syntheticPngHeader(40, 40)},
        {"big.png", "image/png", syntheticPngHeader(120, 120)},
        {"wide.png", "image/png", syntheticPngHeader(300, 20)},
    };
    if (!css.empty()) files.push_back({"style.css", "text/css", css});
    return files;
  }

  Laid build(const std::string& body, const std::string& css = "", const uint16_t viewportHeight = 800,
             const std::string& language = "en", const VerticalText vertical = VerticalText::Off) {
    const std::string head = css.empty() ? "" : "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>";
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html "
        "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C</title>" +
        head + "</head><body>\n" + body + "\n</body></html>\n";
    auto epub = syntheticBookFromDocument(work, bookIndex++, doc, images(css), language);
    epub->loadImageManifest();
    Section::BuildParams params;
    params.viewportWidth = kViewportWidth;
    params.viewportHeight = viewportHeight;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    params.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
    params.verticalText = vertical;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    Laid laid;
    laid.pages = section.pageCount;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_NE(nullptr, page);
      if (!page) continue;
      laid.pageHasImages.push_back(page->hasImages());
      laid.pageImageOnly.push_back(page->isImageOnly());
      laid.links.insert(laid.links.end(), page->footnotes.begin(), page->footnotes.end());
      for (const auto& el : page->elements) {
        if (el->getTag() == TAG_PageImage) {
          const auto& pageImage = static_cast<const PageImage&>(*el);
          const auto& image = pageImage.getImageBlock();
          laid.images.push_back({p, el->xPos, el->yPos, image.getWidth(), image.getHeight(), pageImage.isInLine(),
                                 pageImage.linkHash(), image.isTurned()});
        } else if (el->getTag() == TAG_PageLine) {
          const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
          PlacedLine line{p, el->xPos, el->yPos, {}, {}, {}};
          for (uint16_t i = 0; i < block->wordCount(); ++i) {
            line.words.emplace_back(block->wordText(i));
            line.xpos.push_back(block->wordXpos(i));
            line.styles.push_back(block->wordStyle(i));
          }
          block->appendReadingText(line.reading);
          laid.lines.push_back(std::move(line));
        }
      }
    }
    return laid;
  }
};

const std::string kObject = "\xEF\xBF\xBC";

}  // namespace

TEST_F(InlineImageFixture, ASmallImageSitsInItsLineOnTheBaseline) {
  const Laid laid = build("<p>Press the <img src=\"icon.png\" alt=\"key\"/> key to go on.</p>");
  ASSERT_EQ(1u, laid.lines.size());
  ASSERT_EQ(1u, laid.images.size());
  const PlacedLine& line = laid.lines[0];
  const PlacedImage& image = laid.images[0];
  EXPECT_EQ((std::vector<std::string>{"Press", "the", "key", "to", "go", "on."}), line.words);
  // Between "the" and "key", a space from each.
  EXPECT_GT(image.x, line.x + line.xpos[1]);
  EXPECT_EQ(line.x + line.xpos[2], image.x + 16 + 6);
  // Its bottom on the baseline: the line's text is not lowered, as the image is lower than the ascender.
  EXPECT_EQ(line.y + kAscender, image.y + image.height);
  EXPECT_EQ("Press the key to go on.", line.reading);
  EXPECT_EQ(std::string::npos, line.reading.find(kObject));
  EXPECT_TRUE(laid.pageHasImages[0]);
}

TEST_F(InlineImageFixture, ATallerImageLowersItsLineAndTheNextFollows) {
  // 40 px against an 18 px ascender: the text moves down 22 px; the next line starts below the image's line.
  const std::string words = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen ";
  const Laid laid = build("<p>" + words + "<img src=\"tall.png\" alt=\"\"/> " + words + words + "</p>");
  ASSERT_EQ(1u, laid.images.size());
  const PlacedImage& image = laid.images[0];
  const PlacedLine* imageLine = nullptr;
  const PlacedLine* next = nullptr;
  for (size_t i = 0; i < laid.lines.size(); ++i) {
    if (laid.lines[i].y <= image.y + image.height && laid.lines[i].y + kLineHeight >= image.y + image.height) {
      imageLine = &laid.lines[i];
      if (i + 1 < laid.lines.size()) next = &laid.lines[i + 1];
      break;
    }
  }
  ASSERT_NE(nullptr, imageLine);
  ASSERT_NE(nullptr, next);
  EXPECT_EQ(imageLine->y + kAscender, image.y + image.height) << "bottom on the baseline";
  EXPECT_EQ(imageLine->y - (40 - kAscender), image.y) << "the line's top is the image's top";
  EXPECT_EQ(imageLine->y + kLineHeight, next->y);
  // The line above ends where the image's line begins: no overlap.
  const PlacedLine& above = *(imageLine - 1);
  EXPECT_LE(above.y + kLineHeight, image.y);
}

TEST_F(InlineImageFixture, VerticalAlignPlacesTheImageAgainstTheText) {
  const auto placed = [&](const std::string& align) {
    const Laid laid = build("<p>aa <img src=\"icon.png\" alt=\"\" style=\"vertical-align: " + align + "\"/> bb</p>");
    EXPECT_EQ(1u, laid.images.size());
    EXPECT_EQ(1u, laid.lines.size());
    if (laid.images.empty() || laid.lines.empty()) return 0;
    return laid.images[0].y - laid.lines[0].y;  // the image's top from the text's
  };
  EXPECT_EQ(kAscender - 16, placed("baseline"));
  EXPECT_EQ(kAscender - kAscender / 3 - 8, placed("middle"));
  EXPECT_EQ(0, placed("text-top"));
  EXPECT_EQ(0, placed("top"));
  EXPECT_EQ(kLineHeight - 16, placed("bottom"));
  EXPECT_EQ(kLineHeight - 16, placed("text-bottom"));
  // A length raises it from the baseline (lowers it when negative), in ems of the text (the host's ascender, 18) or
  // percent of the line height; raised above the text, the image lowers the text.
  EXPECT_EQ(kAscender - 16 + 9, placed("-0.5em"));
  EXPECT_EQ(-7, placed("8px")) << "9 px up: 7 above the text's top, which moves down by that";
  EXPECT_EQ(kAscender - 16 + 6, placed("-25%"));
  EXPECT_EQ(kAscender - 16 - 5, placed("0.5ex"));
}

TEST_F(InlineImageFixture, AVerticalAlignLengthLeavesTextAsItWas) {
  // On a <sup> a length changes nothing, as before it was read: the marker stays raised.
  const Laid laid = build("<p>claim<sup style=\"vertical-align: 0.3em\">1</sup> more</p>");
  ASSERT_EQ(1u, laid.lines.size());
  ASSERT_EQ(3u, laid.lines[0].words.size());
  EXPECT_EQ("1", laid.lines[0].words[1]);
  EXPECT_NE(0, laid.lines[0].styles[1] & EpdFontFamily::SUP);
}

TEST_F(InlineImageFixture, AnImageInsideSupIsRaisedWithIt) {
  // A footnote marker drawn as an image, inside <sup>: raised as SUP text is, by two fifths of the ascender.
  const Laid laid = build("<p>A claim<sup><img src=\"icon.png\" alt=\"1\"/></sup> and more.</p>");
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.lines.size());
  EXPECT_EQ(laid.lines[0].y + kAscender - kAscender * 2 / 5, laid.images[0].y + laid.images[0].height);
}

TEST_F(InlineImageFixture, ABigImageKeepsTheBlockPath) {
  const Laid laid = build("<p>Before <img src=\"big.png\" alt=\"\"/> after.</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ((kViewportWidth - 120) / 2, laid.images[0].x);
  ASSERT_EQ(2u, laid.lines.size()) << "the paragraph splits around a block image";
  EXPECT_LE(laid.lines[0].y + kLineHeight, laid.images[0].y);
  EXPECT_GE(laid.lines[1].y, laid.images[0].y + 120);
}

TEST_F(InlineImageFixture, AnImageOnlyParagraphStaysACentredBlock) {
  const Laid laid = build("<p>Text above.</p><p><img src=\"icon.png\" alt=\"\"/></p><p>Text below.</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ((kViewportWidth - 16) / 2, laid.images[0].x);
  ASSERT_EQ(2u, laid.lines.size());
  EXPECT_LE(laid.lines[0].y + kLineHeight, laid.images[0].y);
  EXPECT_GE(laid.lines[1].y, laid.images[0].y + 16);
}

TEST_F(InlineImageFixture, ARowOfImagesWithoutTextStaysBlocks) {
  // Ornaments in a paragraph of their own: one under another, centred, as before.
  const Laid laid = build("<p><img src=\"icon.png\" alt=\"\"/><img src=\"icon.png\" alt=\"\"/></p><p>Text.</p>");
  ASSERT_EQ(2u, laid.images.size());
  EXPECT_EQ(laid.images[0].x, laid.images[1].x);
  EXPECT_EQ(laid.images[0].y + 16, laid.images[1].y);
}

TEST_F(InlineImageFixture, AnImageTheBookMakesABlockStaysOne) {
  const Laid laid =
      build("<p>Before <img class=\"b\" src=\"icon.png\" alt=\"\"/> after.</p>", "img.b { display: block; }");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ((kViewportWidth - 16) / 2, laid.images[0].x);
  EXPECT_EQ(2u, laid.lines.size());
}

TEST_F(InlineImageFixture, AFloatedImageIsNotSetInTheLine) {
  const Laid laid = build("<p>Before <img src=\"icon.png\" alt=\"\" style=\"float: left\"/> after.</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ((kViewportWidth - 16) / 2, laid.images[0].x);
}

TEST_F(InlineImageFixture, AWideShortImageTakesALineOfItsOwnInTheParagraph) {
  // 300 px of a 480 px column: it wraps like a long word, still inline.
  const Laid laid =
      build("<p>words words words words words words words words <img src=\"wide.png\" alt=\"\"/> words words.</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ(300, laid.images[0].width);
  EXPECT_GE(laid.lines.size(), 2u);
  EXPECT_NE((kViewportWidth - 300) / 2, laid.images[0].x) << "not the centred block";
}

TEST_F(InlineImageFixture, ALineWithAnImageGoesToTheNextPageWhole) {
  // Room for three lines of 24 on a page of 80: the fourth line, with a 40 px image, moves to page 2 with its image.
  const Laid laid = build(
      "<p>alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon "
      "phi chi psi omega alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma "
      "<img src=\"tall.png\" alt=\"\"/> tau upsilon phi chi psi omega</p>",
      "", 80);
  ASSERT_EQ(1u, laid.images.size());
  const PlacedImage& image = laid.images[0];
  for (const PlacedLine& line : laid.lines) {
    if (line.page != image.page) continue;
    // Every line on the image's page clears it, above or below.
    EXPECT_TRUE(line.y + kLineHeight <= image.y || line.y >= image.y + image.height ||
                line.y + kAscender == image.y + image.height)
        << "line at " << line.y << " overlaps the image at " << image.y;
  }
  EXPECT_LE(image.y + image.height, 80);
  EXPECT_TRUE(laid.pageHasImages[image.page]);
}

TEST_F(InlineImageFixture, ManyImagesInOneParagraphAreAllPlaced) {
  std::string body = "<p>";
  for (int i = 0; i < 40; ++i) body += "w" + std::to_string(i) + " <img src=\"icon.png\" alt=\"\"/> ";
  body += "end.</p>";
  const Laid laid = build(body);
  EXPECT_EQ(40u, laid.images.size());
  for (const PlacedLine& line : laid.lines) EXPECT_EQ(std::string::npos, line.reading.find(kObject));
}

TEST_F(InlineImageFixture, APageHoldsAtMostSixteenLineImages) {
  // Forty icons fit two pages by height; the cap moves the line that would bring a page past sixteen to the next.
  std::string body = "<p>";
  for (int i = 0; i < 40; ++i) body += "w" + std::to_string(i) + " <img src=\"icon.png\" alt=\"\"/> ";
  body += "end.</p>";
  const Laid laid = build(body);
  ASSERT_EQ(40u, laid.images.size());
  std::vector<int> perPage(laid.pages, 0);
  for (const PlacedImage& image : laid.images) perPage[image.page]++;
  for (uint16_t p = 0; p < laid.pages; ++p) EXPECT_LE(perPage[p], 16) << "page " << p;
  EXPECT_GE(laid.pages, 3u);
}

TEST_F(InlineImageFixture, ARightToLeftParagraphSetsTheImageBetweenItsWords) {
  const std::string shalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
  const std::string olam = "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";
  const Laid laid =
      build("<p dir=\"rtl\">" + shalom + " <img src=\"icon.png\" alt=\"\"/> " + olam + "</p>", "", 800, "he");
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.lines.size());
  const PlacedLine& line = laid.lines[0];
  ASSERT_EQ(2u, line.words.size());
  // Visual order: עולם (left), the image, שלום (right).
  EXPECT_LT(line.x + line.xpos[0], laid.images[0].x);
  EXPECT_LT(laid.images[0].x + 16, line.x + line.xpos[1]);
  EXPECT_EQ(std::string::npos, line.reading.find(kObject));
}

TEST_F(InlineImageFixture, CjkTextSetsTheImageAmongItsCharacters) {
  const Laid laid = build("<p>漢字<img src=\"icon.png\" alt=\"\"/>漢字</p>", "", 800, "ja");
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.lines.size());
  const PlacedLine& line = laid.lines[0];
  ASSERT_EQ(4u, line.words.size());
  EXPECT_LT(line.x + line.xpos[1], laid.images[0].x);
  EXPECT_LE(laid.images[0].x + 16, line.x + line.xpos[2]);
}

TEST_F(InlineImageFixture, AnImageInAColumnIsAnUprightCharacter) {
  // A gaiji down a vertical column: a cell of its own between the characters, centred across the column on its axis
  // (the host font's em is 16, its baseline 18 below the column's top: the axis is 6 above it), drawn upright.
  const Laid laid = build("<p>漢字<img class=\"gaiji\" src=\"icon.png\" alt=\"\"/>漢字</p>",
                          "html { writing-mode: vertical-rl; }", 800, "ja", VerticalText::Automatic);
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.lines.size()) << "the column goes on past the image";
  const PlacedLine& line = laid.lines[0];
  const PlacedImage& image = laid.images[0];
  ASSERT_EQ(4u, line.words.size());
  EXPECT_TRUE(image.inLine);
  EXPECT_TRUE(image.turned);
  EXPECT_EQ(line.x + line.xpos[1] + 16, image.x) << "right after the second character";
  EXPECT_EQ(image.x + 16, line.x + line.xpos[2]) << "the third character right after it";
  EXPECT_EQ(line.y + kAscender - 6 - 8, image.y) << "centred on the column's axis";
  EXPECT_EQ(std::string::npos, line.reading.find(kObject));
}

TEST_F(InlineImageFixture, AWideImageInAColumnIsShrunkToTheEm) {
  // 40 px across the column, under two of its 24 px pitches: set in the column, at the em's 16 px.
  const Laid laid = build("<p>漢字<img src=\"tall.png\" alt=\"\"/>漢字</p>", "html { writing-mode: vertical-rl; }", 800,
                          "ja", VerticalText::Automatic);
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_TRUE(laid.images[0].inLine);
  EXPECT_EQ(16, laid.images[0].width);
  EXPECT_EQ(16, laid.images[0].height);
}

TEST_F(InlineImageFixture, APictureInAColumnKeepsTheBlockPath) {
  const Laid laid = build("<p>漢字<img src=\"big.png\" alt=\"\"/>漢字</p>", "html { writing-mode: vertical-rl; }", 800,
                          "ja", VerticalText::Automatic);
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_FALSE(laid.images[0].inLine);
  EXPECT_GE(laid.lines.size(), 2u) << "the paragraph splits around the picture, as before";
}

TEST_F(InlineImageFixture, AGaijiBesideARubyInAColumnKeepsTheRubyOverItsBase) {
  const std::string body = "<p>前<ruby>漢<rt>かん</rt></ruby><img src=\"icon.png\" alt=\"\"/>字後</p>";
  const std::string css = "html { writing-mode: vertical-rl; }";
  GfxRenderer renderer;
  const std::string doc =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C</title>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
      body + "\n</body></html>\n";
  auto epub = syntheticBookFromDocument(work, bookIndex++, doc, images(css), "ja");
  epub->loadImageManifest();
  Section::BuildParams params;
  params.viewportWidth = kViewportWidth;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  params.embeddedStyle = true;
  params.verticalText = VerticalText::Automatic;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(params));
  section.currentPage = 0;
  const auto page = section.loadPageFromSectionFile();
  ASSERT_NE(nullptr, page);
  ASSERT_TRUE(page->isVertical());
  const TextBlock* line = nullptr;
  const PageImage* image = nullptr;
  for (const auto& el : page->elements) {
    if (el->getTag() == TAG_PageLine) line = static_cast<const PageLine&>(*el).getBlock();
    if (el->getTag() == TAG_PageImage) image = static_cast<const PageImage*>(el.get());
  }
  ASSERT_NE(nullptr, line);
  ASSERT_NE(nullptr, image);
  EXPECT_TRUE(image->isInLine());
  ASSERT_EQ(4, line->wordCount());  // 前 漢 字 後
  ASSERT_EQ(1, line->annotationCount());
  const TextBlock::Annotation ruby = line->annotation(0);
  EXPECT_EQ(1, ruby.firstWord) << "over 漢";
  EXPECT_EQ(1, ruby.wordCount);
  EXPECT_EQ("かん", std::string(ruby.text));
  EXPECT_EQ(line->wordXpos(1) + 16, image->xPos) << "the gaiji right after its neighbour";
}

TEST_F(InlineImageFixture, AnImageInARubyKeepsTheBlockPath) {
  const Laid laid = build("<p>前<ruby>漢<img src=\"icon.png\" alt=\"\"/><rt>かん</rt></ruby>後</p>", "", 800, "ja");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ((kViewportWidth - 16) / 2, laid.images[0].x);
}

TEST_F(InlineImageFixture, AGlyphImageInsideAWordKeepsTheWordWhole) {
  // Æ drawn as an image: "gir" is glued to it, and the reading text joins nothing across the gap but the word.
  const Laid laid = build("<p>The god <img src=\"icon.png\" alt=\"AE\"/>gir sails.</p>");
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.lines.size());
  const PlacedLine& line = laid.lines[0];
  EXPECT_EQ((std::vector<std::string>{"The", "god", "gir", "sails."}), line.words);
  EXPECT_EQ(line.x + line.xpos[2], laid.images[0].x + 16) << "glued: no space after the image";
  EXPECT_EQ("The god gir sails.", line.reading);
}

TEST_F(InlineImageFixture, ThePageImageSaysItIsInALine) {
  const Laid laid = build("<p>An <img src=\"icon.png\" alt=\"\"/> icon.</p><p><img src=\"big.png\" alt=\"\"/></p>");
  ASSERT_EQ(2u, laid.images.size());
  EXPECT_TRUE(laid.images[0].inLine) << "kept through the section file";
  EXPECT_FALSE(laid.images[1].inLine);
}

TEST_F(InlineImageFixture, APageEndingOnALineOfImagesIsNoPicturePage) {
  // The paragraph's last line holds images alone, and goes to a page of its own: that page is not centred as a picture.
  std::string body = "<p>alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi";
  for (int i = 0; i < 20; ++i) body += "<img src=\"icon.png\" alt=\"\"/>";
  body += "</p>";
  const Laid laid = build(body, "", 50);
  ASSERT_GE(laid.pages, 2u);
  bool imagesAlone = false;
  for (uint16_t p = 0; p < laid.pages; ++p) {
    bool text = false;
    for (const PlacedLine& line : laid.lines) text = text || line.page == p;
    if (!text && laid.pageHasImages[p]) {
      imagesAlone = true;
      EXPECT_FALSE(laid.pageImageOnly[p]) << "page " << p;
    }
  }
  EXPECT_TRUE(imagesAlone) << "no page holds the images alone: the test does not test";
}

TEST_F(InlineImageFixture, AnImageInsideALinkIsTheLinksTarget) {
  // A link around an image alone is kept without text; the image names it.
  const Laid laid = build(
      "<p>See the note <a href=\"#n1\"><img src=\"icon.png\" alt=\"note\"/></a> here.</p>"
      "<p id=\"n1\">The note.</p>");
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.links.size());
  EXPECT_STREQ("#n1", laid.links[0].href);
  EXPECT_STREQ("", laid.links[0].number) << "no text of its own: listed under the Link label";
  EXPECT_TRUE(laid.images[0].inLine);
  EXPECT_EQ(linkHrefHash("#n1"), laid.images[0].linkHash);
}

TEST_F(InlineImageFixture, ALinkWithTextAndAnImageKeepsItsText) {
  const Laid laid =
      build("<p>Read <a href=\"#n2\"><img src=\"icon.png\" alt=\"\"/> more</a> now.</p><p id=\"n2\">x</p>");
  ASSERT_EQ(1u, laid.images.size());
  ASSERT_EQ(1u, laid.links.size());
  EXPECT_STREQ("more", laid.links[0].number);
  EXPECT_EQ(linkHrefHash("#n2"), laid.images[0].linkHash);
}

TEST_F(InlineImageFixture, ABlockImageInsideALinkIsTheLinksTarget) {
  // A picture alone inside a link: placed as a block as before, and listed and named as an inline image's link is.
  const Laid laid = build("<p><a href=\"#n4\"><img src=\"big.png\" alt=\"\"/></a></p><p id=\"n4\">x</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_FALSE(laid.images[0].inLine);
  EXPECT_EQ((kViewportWidth - 120) / 2, laid.images[0].x);
  ASSERT_EQ(1u, laid.links.size());
  EXPECT_STREQ("#n4", laid.links[0].href);
  EXPECT_STREQ("", laid.links[0].number);
  EXPECT_EQ(linkHrefHash("#n4"), laid.images[0].linkHash);
}

TEST_F(InlineImageFixture, AFloatedImageInsideALinkIsTheLinksTarget) {
  const Laid laid = build(
      "<div style=\"float: left; width: 30%\"><a href=\"#n5\"><img src=\"big.png\" alt=\"\"/></a></div>"
      "<p>Text beside the floated picture, long enough to wrap beside it.</p><p id=\"n5\">x</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ(0, laid.images[0].x) << "floated left, as before";
  ASSERT_EQ(1u, laid.links.size());
  EXPECT_STREQ("#n5", laid.links[0].href);
  EXPECT_EQ(linkHrefHash("#n5"), laid.images[0].linkHash);
}

TEST_F(InlineImageFixture, AnImageOutsideALinkNamesNone) {
  const Laid laid = build("<p>Plain <img src=\"icon.png\" alt=\"\"/> image.</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_EQ(0u, laid.images[0].linkHash);
  EXPECT_TRUE(laid.links.empty());
}

TEST_F(InlineImageFixture, ALinkedImageAloneInItsParagraphKeepsTheLink) {
  // The image-only paragraph places it as a block, and the link and its image's name go with it.
  const Laid laid = build("<p><a href=\"#n3\"><img src=\"icon.png\" alt=\"\"/></a></p><p id=\"n3\">x</p>");
  ASSERT_EQ(1u, laid.images.size());
  EXPECT_FALSE(laid.images[0].inLine);
  EXPECT_EQ((kViewportWidth - 16) / 2, laid.images[0].x);
  ASSERT_EQ(1u, laid.links.size());
  EXPECT_STREQ("#n3", laid.links[0].href);
  EXPECT_EQ(linkHrefHash("#n3"), laid.images[0].linkHash);
}
