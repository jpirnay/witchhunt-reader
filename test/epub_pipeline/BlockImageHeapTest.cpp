// A block image the heap cannot place is dropped, and the chapter around it comes out as it would with the image.
// The <img> branch returned without counting the element as open when the placement ran out of memory, and its
// endElement still took the depth down: every bold, italic and alignment scope was one level off for the rest of the
// chapter, and the section was cached that way. BLOCK_IMAGE_FAULT_INJECTION refuses the n-th block image its
// ImageBlock (ChapterHtmlSlimParser::placeImageWithBlockSpacing). From crosspoint-reader PR #4472 (Justin Mitchell).
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "EpdFontFamily.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr uint8_t kEmphasis = EpdFontFamily::BOLD | EpdFontFamily::ITALIC;

// A picture (far taller than two lines, so a block image) inside a bold run, inside an italic block. The scopes open
// around the image are the ones a lost depth level closes early: the image's own end tag ended the bold run, and each
// end tag after it ended the scope of the element one level up, the <div>'s italic at the <p>'s end.
const char* const kBody =
    "<div style=\"font-style:italic\">\n"
    "<p><b>boldbefore <img src=\"pic.png\" alt=\"pic\"/> boldafter</b> italicafter</p>\n"
    "<p>italicnext <b>bolditalicnext</b></p>\n"
    "</div>\n"
    "<p>plainlast <b>boldlast</b></p>\n";

struct Built {
  std::map<std::string, uint8_t> styles;  // bold/italic bits of each word (every word is unique)
  int images = 0;
};

// Restores "never refuse" however the test ends.
struct BlockImagesLeft {
  explicit BlockImagesLeft(const size_t n) { blockImagesLeft() = n; }
  ~BlockImagesLeft() { blockImagesLeft() = SIZE_MAX; }
  BlockImagesLeft(const BlockImagesLeft&) = delete;
  BlockImagesLeft& operator=(const BlockImagesLeft&) = delete;
};

struct BlockImageHeap : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("block_image_heap_" + std::to_string(static_cast<long>(getpid())) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  Built build(const size_t imagesAllowed) {
    auto epub = syntheticBook(work, books++, kBody, {{"pic.png", "image/png", syntheticPngHeader(300, 200)}});
    epub->loadImageManifest();
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    {
      const BlockImagesLeft guard(imagesAllowed);
      EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    }
    EXPECT_TRUE(section.loadSectionFile(params));
    Built out;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_NE(nullptr, page);
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() == TAG_PageImage) ++out.images;
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          out.styles[block.wordText(w)] = static_cast<uint8_t>(block.wordStyle(w)) & kEmphasis;
        }
      }
    }
    return out;
  }
};

void expectStyles(const Built& built) {
  constexpr uint8_t kBold = EpdFontFamily::BOLD;
  constexpr uint8_t kItalic = EpdFontFamily::ITALIC;
  EXPECT_EQ(built.styles.at("boldbefore"), kBold | kItalic);
  EXPECT_EQ(built.styles.at("boldafter"), kBold | kItalic);
  EXPECT_EQ(built.styles.at("italicafter"), kItalic);
  EXPECT_EQ(built.styles.at("italicnext"), kItalic);
  EXPECT_EQ(built.styles.at("bolditalicnext"), kBold | kItalic);
  EXPECT_EQ(built.styles.at("plainlast"), 0);
  EXPECT_EQ(built.styles.at("boldlast"), kBold);
}

}  // namespace

TEST_F(BlockImageHeap, ThePlacedImageLeavesEveryStyleScopeAsTheBookSetsIt) {
  const Built built = build(SIZE_MAX);
  EXPECT_EQ(built.images, 1);
  expectStyles(built);
}

TEST_F(BlockImageHeap, AnImageTheHeapCannotPlaceLeavesTheStylesAroundItAlone) {
  const Built built = build(0);
  EXPECT_EQ(built.images, 0) << "the refused image is dropped";
  expectStyles(built);
}
