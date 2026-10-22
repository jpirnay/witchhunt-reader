// Bold and italic set on a block reach the blocks nested inside it.
//
//   <div style="font-style:italic"><p>Inner</p>tail</div>
//
// renders "Inner" and "tail" italic in every reader: font-style and font-weight are inherited. The
// parser kept a block's own CSS in one slot that every nested block overwrote and every block close
// cleared, so the <p> above came out upright, and so did "tail" after it. Only containers that are
// not BLOCK_TAGS (<section>, <article>, <ul>) pushed an entry onto the inline style stack, which
// applies AFTER the block slot: an italic <section> also overrode its own <p style="font-style:
// normal">. A <br> inside an italic <p> closed as a block too and cleared the paragraph's italic for
// the rest of it.
//
// What has to hold:
//   1. A block's bold/italic reaches the blocks and text nested inside it (div, li, blockquote, pre,
//      section), and the text after a nested block, up to the block's own end tag.
//   2. A nested block that sets font-style:normal / font-weight:normal overrides it, and the
//      parent's style returns once that child closes.
//   3. Nothing leaks past the container's end tag.
//   4. Headings stay bold, and take an enclosing block's italic too.
//   5. A <br> does not end the paragraph's own style.
//
// test_block_emphasis.epub sets every one of these, and its golden pins each word's style. What
// the book does not set is a <pre> styled by its own rule with an inline element inside it.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>

#include "EpdFontFamily.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr uint8_t kBold = EpdFontFamily::BOLD;
constexpr uint8_t kItalic = EpdFontFamily::ITALIC;
constexpr uint8_t kEmphasis = kBold | kItalic;

struct BlockEmphasisFixture : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_blockemph_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // Bold/italic bits of every word of the chapter, keyed by its text (each word is unique).
  std::map<std::string, uint8_t> styles(const std::string& css, const std::string& body) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
        body + "\n</body></html>\n";
    auto epub = syntheticBookFromDocument(work, books++, doc, {{"style.css", "text/css", css}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::map<std::string, uint8_t> out;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          out[block.wordText(w)] = static_cast<uint8_t>(block.wordStyle(w)) & kEmphasis;
        }
      }
    }
    return out;
  }
};

}  // namespace

TEST_F(BlockEmphasisFixture, BlockquoteAndPreReachTheirChildren) {
  const auto s = styles("blockquote { font-style: italic; } pre { font-weight: bold; }",
                        "<blockquote><p>Quoted</p></blockquote><pre><span>Code</span></pre>"
                        "<div style=\"font-weight:bold\"><pre>Nested</pre></div><p>Done</p>");
  EXPECT_EQ(s.at("Quoted"), kItalic);
  EXPECT_EQ(s.at("Code"), kBold);
  EXPECT_EQ(s.at("Nested"), kBold);
  EXPECT_EQ(s.at("Done"), 0);
}
