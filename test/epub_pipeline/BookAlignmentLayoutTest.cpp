// How far the reader's Paragraph Alignment setting reaches into a book's own text-align.
//
// Project Hail Mary marks its section breaks with a centered em dash:
//
//   .transition1 { text-align: center; ... }
//   <div class="transition1">—</div>
//
// Under Justified, the default, the setting replaced every block's alignment, so the dash sat at the left margin.
//
// What has to hold:
//   1. The setting decides running text: a book's left and justify give way to it.
//   2. What the book centers, or sets to the end side, stays there.
//   3. The end side follows the text's direction: right is how Arabic and Hebrew text runs, so there it gives way.
//   4. Under the book's own alignment, all of it stands, as before.
//   5. With the book's styles off, none of it does.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

constexpr uint8_t kJustified = 0;
constexpr uint8_t kLeft = 1;
constexpr uint8_t kBooksOwn = 4;

struct BookAlignmentFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_bookalign_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string makeBook(const std::string& css, const std::string& body, const std::string& language) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">bookalign</dc:identifier>"
            "<dc:title>Book alignment</dc:title><dc:language>" +
                language +
                "</dc:language></metadata>\n<manifest>\n"
                "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n"
                "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("style.css", css);
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
                body + "\n</body></html>\n");
    const std::string path = (work / "bookalign.epub").string();
    zip.write(path);
    return path;
  }

  // How many rules the chapter draws, through the rule cache a real book goes through.
  int rulesDrawn(const std::string& css, const std::string& body, const bool embeddedStyle = true) {
    auto epub = std::make_shared<Epub>(makeBook(css, body, "en"), (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = embeddedStyle;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    int rules = 0;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) rules += el->getTag() == TAG_PageHR ? 1 : 0;
    }
    return rules;
  }

  // The alignment of the chapter's first text line.
  CssTextAlign firstLine(const std::string& css, const std::string& body, const uint8_t paragraphAlignment,
                         const bool embeddedStyle = true, const std::string& language = "en") {
    auto epub = std::make_shared<Epub>(makeBook(css, body, language), (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = embeddedStyle;
    params.paragraphAlignment = paragraphAlignment;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    section.currentPage = 0;
    const auto page = section.loadPageFromSectionFile();
    EXPECT_TRUE(page);
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
      if (block.wordCount() != 0) return block.getRenderStyle().alignment;
    }
    ADD_FAILURE() << "no text line";
    return CssTextAlign::None;
  }
};

}  // namespace

TEST_F(BookAlignmentFixture, TheBooksCenteredBlockStaysCenteredUnderJustified) {
  EXPECT_EQ(CssTextAlign::Center, firstLine(".brk { text-align: center; }", "<div class=\"brk\">—</div>", kJustified));
}

TEST_F(BookAlignmentFixture, TheBooksCenteredBlockStaysCenteredUnderLeft) {
  EXPECT_EQ(CssTextAlign::Center, firstLine(".brk { text-align: center; }", "<p class=\"brk\">Alpha</p>", kLeft));
}

TEST_F(BookAlignmentFixture, TheBooksRightAlignedBlockStaysThere) {
  EXPECT_EQ(CssTextAlign::Right, firstLine(".sig { text-align: right; }", "<p class=\"sig\">Alpha</p>", kJustified));
}

TEST_F(BookAlignmentFixture, RunningTextFollowsTheSetting) {
  EXPECT_EQ(CssTextAlign::Justify, firstLine("p { text-align: left; }", "<p>Alpha</p>", kJustified));
  EXPECT_EQ(CssTextAlign::Left, firstLine("p { text-align: justify; }", "<p>Alpha</p>", kLeft));
}

TEST_F(BookAlignmentFixture, RightIsRunningTextInARightToLeftBook) {
  EXPECT_EQ(CssTextAlign::Justify,
            firstLine("p { text-align: right; }", "<p>\xD7\x90\xD7\x91</p>", kJustified, true, "he"));
}

TEST_F(BookAlignmentFixture, LeftIsTheEndSideInARightToLeftBook) {
  EXPECT_EQ(CssTextAlign::Left,
            firstLine("p { text-align: left; }", "<p>\xD7\x90\xD7\x91</p>", kJustified, true, "he"));
}

TEST_F(BookAlignmentFixture, UnderTheBooksOwnAlignmentAllOfItStands) {
  EXPECT_EQ(CssTextAlign::Left, firstLine("p { text-align: left; }", "<p>Alpha</p>", kBooksOwn));
  EXPECT_EQ(CssTextAlign::Center, firstLine("p { text-align: center; }", "<p>Alpha</p>", kBooksOwn));
}

TEST_F(BookAlignmentFixture, WithTheBooksStylesOffNoneOfItStands) {
  EXPECT_EQ(CssTextAlign::Justify, firstLine(".brk { text-align: center; }", "<div class=\"brk\">—</div>", kJustified,
                                             /*embeddedStyle=*/false));
}

// Project Hail Mary's section break: the <hr> is structure, the dash is what shows.
TEST_F(BookAlignmentFixture, ARuleTheBookStripsOfItsBorderIsNotDrawn) {
  const char* css =
      ".transition { color: gray; display: block; height: 2px; margin: 0; border: currentColor none medium; }";
  EXPECT_EQ(0, rulesDrawn(css, "<p>Alpha</p><hr class=\"transition\"/><p>Bravo</p>"));
}

TEST_F(BookAlignmentFixture, APlainRuleIsDrawn) { EXPECT_EQ(1, rulesDrawn("", "<p>Alpha</p><hr/><p>Bravo</p>")); }

TEST_F(BookAlignmentFixture, ARuleThatALaterSelectorPaintsIsDrawn) {
  EXPECT_EQ(1, rulesDrawn("hr { border: 0; } hr.sep { height: 1px; background: #000; }",
                          "<p>Alpha</p><hr class=\"sep\"/><p>Bravo</p>"));
}

TEST_F(BookAlignmentFixture, WithTheBooksStylesOffTheRuleIsDrawn) {
  EXPECT_EQ(1, rulesDrawn("hr { border: none; }", "<p>Alpha</p><hr/><p>Bravo</p>", /*embeddedStyle=*/false));
}
