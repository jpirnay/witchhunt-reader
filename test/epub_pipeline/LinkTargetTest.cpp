// An in-book link is relative to the document that holds it. Two chapters share a filename in two folders under
// OEBPS/ (a/ch.xhtml, b/ch.xhtml), as do two note files (a/notes.xhtml, b/notes.xhtml). The reader resolved a link's
// href without its document's folder: "../b/ch.xhtml" in OEBPS/a/ch.xhtml normalised to "b/ch.xhtml", which no spine
// item is, so the first item named ch.xhtml won and the link opened chapter a again (Murphy, 2026-11-15). With the OPF
// at the zip root the same link happened to name the right path. The links are taken from the built pages, as the
// reader takes them from the page it shows, and followed through Epub::resolveLinkToSpineIndex, as the reader follows
// a tapped or listed link. The note previews resolve the same hrefs; the NCX's are relative to the NCX.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/FootnotePreviews.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// Spine order: the first item named ch.xhtml and the first named notes.xhtml are the wrong ones for every link below.
constexpr int kChapterA = 0;
constexpr int kNotesB = 1;
constexpr int kChapterB = 2;
constexpr int kNotesA = 3;

std::string xhtml(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
         "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>C</title></head><body>\n" +
         body + "\n</body></html>\n";
}

std::string notes(const char* text) {
  return xhtml(std::string("<aside epub:type=\"footnote\" id=\"n1\"><p>") + text + "</p></aside>");
}

struct LinkTarget : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() / ("link_target_" + std::to_string(static_cast<long>(getpid())) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The OPF under OEBPS/. With `ncxOnly`, no nav: an NCX in OEBPS/toc/, its links relative to that folder.
  std::shared_ptr<Epub> book(const bool ncxOnly = false) const {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
            "</container>\n");
    zip.add("OEBPS/a/ch.xhtml", xhtml("<p id=\"top\">Chapter A. <a href=\"../b/ch.xhtml#bstart\">ToB</a> and a "
                                      "note<a epub:type=\"noteref\" href=\"notes.xhtml#n1\">1</a> and "
                                      "<a href=\"#top\">ToTop</a>.</p>"));
    zip.add("OEBPS/b/ch.xhtml", xhtml("<p id=\"bstart\">Chapter B. <a href=\"ch.xhtml#bstart\">ToSelf</a> and "
                                      "<a href=\"../a/ch.xhtml\">ToA</a>.</p>"));
    zip.add("OEBPS/a/notes.xhtml", notes("Note of folder a"));
    zip.add("OEBPS/b/notes.xhtml", notes("Note of folder b"));
    std::string manifest =
        "<item id=\"cha\" href=\"a/ch.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "<item id=\"nb\" href=\"b/notes.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "<item id=\"chb\" href=\"b/ch.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "<item id=\"na\" href=\"a/notes.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
    if (ncxOnly) {
      manifest += "<item id=\"ncx\" href=\"toc/toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
      zip.add("OEBPS/toc/toc.ncx",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" "
              "version=\"2005-1\">\n<navMap>\n"
              "<navPoint id=\"p1\"><navLabel><text>A</text></navLabel><content src=\"../a/ch.xhtml\"/></navPoint>\n"
              "<navPoint id=\"p2\"><navLabel><text>B</text></navLabel><content src=\"../b/ch.xhtml\"/></navPoint>\n"
              "</navMap>\n</ncx>\n");
    } else {
      manifest += "<item id=\"nav\" href=\"nav/nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/>\n";
      zip.add("OEBPS/nav/nav.xhtml", xhtml("<nav epub:type=\"toc\"><ol><li><a href=\"../a/ch.xhtml\">A</a></li>"
                                           "<li><a href=\"../b/ch.xhtml\">B</a></li></ol></nav>"));
    }
    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">link-target</dc:identifier>"
            "<dc:title>Link Target</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n" +
                manifest + "</manifest>\n<spine" + (ncxOnly ? " toc=\"ncx\"" : "") +
                ">\n<itemref idref=\"cha\"/><itemref idref=\"nb\"/><itemref idref=\"chb\"/><itemref idref=\"na\"/>"
                "\n</spine>\n</package>\n");
    const std::string path = (work / (ncxOnly ? "ncx.epub" : "nav.epub")).string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  // Every link the built pages of `spine` carry, as the reader lists them.
  static std::vector<std::string> pageLinks(const std::shared_ptr<Epub>& epub, const int spine) {
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, spine, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    std::vector<std::string> links;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const FootnoteEntry& link : page->footnotes) links.emplace_back(link.href);
    }
    return links;
  }
};

}  // namespace

TEST_F(LinkTarget, ALinkFollowsItsDocumentsFolder) {
  const auto epub = book();
  const auto fromA = pageLinks(epub, kChapterA);
  ASSERT_EQ(fromA, (std::vector<std::string>{"../b/ch.xhtml#bstart", "notes.xhtml#n1", "#top"}));
  EXPECT_EQ(epub->resolveLinkToSpineIndex(fromA[0], kChapterA), kChapterB);
  EXPECT_EQ(epub->resolveLinkToSpineIndex(fromA[1], kChapterA), kNotesA);
  EXPECT_EQ(epub->resolveLinkToSpineIndex(fromA[2], kChapterA), kChapterA);

  const auto fromB = pageLinks(epub, kChapterB);
  ASSERT_EQ(fromB, (std::vector<std::string>{"ch.xhtml#bstart", "../a/ch.xhtml"}));
  EXPECT_EQ(epub->resolveLinkToSpineIndex(fromB[0], kChapterB), kChapterB);
  EXPECT_EQ(epub->resolveLinkToSpineIndex(fromB[1], kChapterB), kChapterA);
}

TEST_F(LinkTarget, ANotePreviewIsTheNoteInTheLinksFolder) {
  const auto epub = book();
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterA));
  FootnotePreviews::Lookup lookup;
  ASSERT_TRUE(lookup.open(epub->getCachePath(), epub.get(), kChapterA));
  std::string preview;
  ASSERT_TRUE(lookup.find("notes.xhtml#n1", preview));
  EXPECT_NE(preview.find("folder a"), std::string::npos) << preview;
}

TEST_F(LinkTarget, TocLinksFollowTheirDocumentsFolder) {
  for (const bool ncxOnly : {false, true}) {
    SCOPED_TRACE(ncxOnly ? "ncx in OEBPS/toc/" : "nav in OEBPS/nav/");
    const auto epub = book(ncxOnly);
    ASSERT_EQ(epub->getTocItemsCount(), 2);
    EXPECT_EQ(epub->getTocItem(0).spineIndex, kChapterA);
    EXPECT_EQ(epub->getTocItem(1).spineIndex, kChapterB);
  }
}
