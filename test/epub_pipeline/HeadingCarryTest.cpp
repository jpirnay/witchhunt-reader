// HeadingCarry: which spine files hold only their chapter's heading (or nothing) and are laid out with the next one.
// The scan of a file's body, calibre's split names, and the decision over a book's spine, its TOC and its files.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub.h"
#include "Epub/HeadingCarry.h"
#include "Epub/Section.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;
using HeadingCarry::Shape;

namespace {

std::string doc(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
         "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
         "<head><title>T</title><style>h1 { color: black; }</style></head>\n<body class=\"calibre\">\n" +
         body + "\n</body>\n</html>\n";
}

HeadingCarry::Scanner scanned(const std::string& xhtml, const size_t piece = 7) {
  HeadingCarry::Scanner scanner;
  for (size_t at = 0; at < xhtml.size(); at += piece) {
    const std::string part = xhtml.substr(at, piece);
    scanner.feed(reinterpret_cast<const uint8_t*>(part.data()), part.size());
  }
  return scanner;
}

Shape shapeOf(const std::string& body) { return scanned(doc(body)).finish(); }

}  // namespace

TEST(HeadingCarryScan, AHeadingAloneIsAHeading) {
  EXPECT_EQ(Shape::Heading, shapeOf("<h1>13</h1>"));
  EXPECT_EQ(Shape::Heading, shapeOf("<h2 class=\"x\">Chapter Thirteen</h2>"));
  // Two headings, a number and a title.
  EXPECT_EQ(Shape::Heading, shapeOf("<h1>13</h1>\n<h2>This Summer</h2>"));
}

TEST(HeadingCarryScan, CalibresSplitFileIsAHeading) {
  // What calibre leaves of a chapter cut after its heading: a printed-page marker inside the heading, and the empty
  // div that carried the page break.
  EXPECT_EQ(
      Shape::Heading,
      shapeOf("<div class=\"calibre1\">\n<h1 class=\"x03-chapter-number\" id=\"u1\"><span epub:type=\"pagebreak\" "
              "id=\"page_127\" role=\"doc-pagebreak\" title=\"127\" class=\"calibre6\"></span>13</h1>\n"
              "<div class=\"calibre7\" id=\"calibre_pb_0\"></div>\n</div>"));
}

TEST(HeadingCarryScan, NothingIsEmpty) {
  EXPECT_EQ(Shape::Empty, shapeOf(""));
  EXPECT_EQ(Shape::Empty, shapeOf("<div class=\"calibre7\" id=\"calibre_pb_0\"></div>\n  \n<p/>"));
  EXPECT_EQ(Shape::Empty, shapeOf("<h1></h1>"));
  EXPECT_EQ(Shape::Empty, scanned("<html><body/></html>").finish());
}

TEST(HeadingCarryScan, TextOutsideAHeadingIsNot) {
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><p>We get the</p>"));
  EXPECT_EQ(Shape::Other, shapeOf("<p>A dedication.</p>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1>loose"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><div>&#160;</div>"));  // an entity is text
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><![CDATA[text]]>"));
}

TEST(HeadingCarryScan, PicturesRulesLinksTablesAndMediaAreNot) {
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><img src=\"a.png\" alt=\"\"/>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><svg:svg xmlns:svg=\"http://www.w3.org/2000/svg\"></svg:svg>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><hr/>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1><a href=\"c.xhtml\">13</a></h1>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><table><tr><td></td></tr></table>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1><video src=\"v.mp4\"></video>"));
  // An anchor without an href is no link.
  EXPECT_EQ(Shape::Heading, shapeOf("<h1><a id=\"top\"></a>13</h1>"));
  EXPECT_EQ(Shape::Heading, shapeOf("<h1>Thirteen<br/>Again</h1>"));
}

TEST(HeadingCarryScan, HiddenTextAndMarkupDoNotCount) {
  EXPECT_EQ(Shape::Heading, shapeOf("<!-- a comment with <p>text</p> --><h1>13</h1><?pi data?>"));
  EXPECT_EQ(Shape::Heading, shapeOf("<h1>13</h1><script>var x = 1;</script><style>p { }</style>"));
  EXPECT_EQ(Shape::Heading, shapeOf("<h1 title=\"a > b\" data-x='<p>'>13</h1>"));
}

TEST(HeadingCarryScan, AHeadingLongerThanALineIsNot) {
  EXPECT_EQ(Shape::Heading, shapeOf("<h1>" + std::string(HeadingCarry::kMaxHeadingTextBytes, 'x') + "</h1>"));
  EXPECT_EQ(Shape::Other, shapeOf("<h1>" + std::string(HeadingCarry::kMaxHeadingTextBytes + 1, 'x') + "</h1>"));
}

TEST(HeadingCarryScan, ABodyThatDoesNotCloseIsNot) {
  EXPECT_EQ(Shape::Other, shapeOf("<div><h1>13</h1>"));                     // an element left open
  EXPECT_EQ(Shape::Other, shapeOf("<h1>13</h1></div>"));                    // an end tag with no start
  EXPECT_EQ(Shape::Other, scanned("<html><body><h1>13</h1>").finish());     // no </body>
  EXPECT_EQ(Shape::Other, scanned("<html><head></head></html>").finish());  // no body at all
  EXPECT_EQ(Shape::Other, shapeOf("<h1>1 < 2</h1>"));                       // not well-formed
  EXPECT_EQ(Shape::Other, scanned(doc("<h1>13</h1>") + std::string(HeadingCarry::kMaxFileBytes, ' ')).finish());
}

TEST(HeadingCarryScan, FindsTheBodysContent) {
  const std::string xhtml = doc("<h1>13</h1>");
  for (const size_t piece : {size_t{1}, size_t{5}, size_t{64}, xhtml.size()}) {
    const HeadingCarry::Scanner scanner = scanned(xhtml, piece);
    ASSERT_EQ(Shape::Heading, scanner.finish());
    EXPECT_EQ("\n<h1>13</h1>\n", xhtml.substr(scanner.bodyBegin(), scanner.bodyEnd() - scanner.bodyBegin()));
  }
}

TEST(HeadingCarrySplitNames, CalibresSiblingsMatch) {
  EXPECT_TRUE(HeadingCarry::splitSiblings("OEBPS/Text/21_Chapter_13_split_000.xhtml",
                                          "OEBPS/Text/21_Chapter_13_split_001.xhtml"));
  EXPECT_TRUE(HeadingCarry::splitSiblings("index_split_009.html", "index_split_010.html"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("index_split_000.html", "index_split_002.html"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("a_split_000.html", "b_split_001.html"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("a_split_000.html", "a_split_001.xhtml"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("a_split_001.html", "a_split_000.html"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("chapter1.xhtml", "chapter2.xhtml"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("x_split_.xhtml", "x_split_1.xhtml"));
  EXPECT_FALSE(HeadingCarry::splitSiblings("dir_split_000.d/a.xhtml", "dir_split_000.d/b.xhtml"));
}

namespace {

struct Item {
  std::string name;
  std::string body;
  std::string tocLabel;  // empty: no TOC entry
};

class HeadingCarryBookTest : public testing::Test {
 protected:
  void SetUp() override {
    work = fs::temp_directory_path() / ("heading_carry_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // A STORED book of `items` in spine order, with a nav TOC of the items that have a label.
  std::shared_ptr<Epub> book(const std::vector<Item>& items) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    std::string manifest =
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
        "properties=\"nav\"/>\n";
    std::string spine;
    std::string toc;
    for (size_t i = 0; i < items.size(); ++i) {
      const std::string id = "s" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"" + items[i].name + "\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>\n";
      if (!items[i].tocLabel.empty()) {
        toc += "<li><a href=\"" + items[i].name + "\">" + items[i].tocLabel + "</a></li>\n";
      }
      zip.add(items[i].name, doc(items[i].body));
    }
    zip.add("nav.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
            "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>N</title></head><body><nav epub:type=\"toc\">"
            "<ol>\n" +
                toc + "</ol></nav></body></html>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">carry</dc:identifier><dc:title>Carry</dc:title><dc:language>en</dc:language>"
            "</metadata>\n<manifest>\n" +
                manifest + "</manifest>\n<spine>\n" + spine + "</spine>\n</package>\n");
    const std::string path = (work / ("book" + std::to_string(books++) + ".epub")).string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  static std::string text(const std::string& word) {
    return "<p>" + word + " opens the chapter.</p><p>" + word + " goes on for a second paragraph.</p>";
  }

  fs::path work;
  int books = 0;
};

const char* const kMarker = "<div class=\"calibre7\" id=\"calibre_pb_0\"></div>";

}  // namespace

TEST_F(HeadingCarryBookTest, DecidesEveryShapeOfTheSpine) {
  const auto epub = book({
      {"c00.xhtml", "<h1>One</h1>" + text("ONE"), "One"},                   // 0 a whole chapter
      {"c01_split_000.xhtml", std::string("<h1>2</h1>") + kMarker, "Two"},  // 1 carried: TOC and split names
      {"c01_split_001.xhtml", text("TWO"), ""},                             // 2
      {"c03.xhtml", "<h1>Three</h1>", "Three"},                             // 3 carried: TOC
      {"c04.xhtml", text("THREE"), ""},                                     // 4
      {"c05_split_000.xhtml", "<h1>4</h1>", ""},                            // 5 carried: split names alone
      {"c05_split_001.xhtml", text("FOUR"), ""},                            // 6
      {"c07_split_000.xhtml", "<h1>Part Two</h1>", "Part Two"},             // 7 kept: the next file is a heading
      {"c07_split_001.xhtml", "<h1>5</h1>", ""},                            // 8 carried: the last of the two
      {"c07_split_002.xhtml", text("FIVE"), ""},                            // 9
      {"c10.xhtml", std::string(kMarker), "Ten"},                           // 10 carried: empty
      {"c11.xhtml", text("TEN"), ""},                                       // 11
      {"c12.xhtml", "<h1>Twelve</h1>", "Twelve"},                           // 12 kept: the next has an entry
      {"c13.xhtml", text("TWELVE"), "Twelve text"},                         // 13
      {"c14.xhtml", "<h1>Last</h1>", "Last"},                               // 14 kept: the last spine item
  });
  ASSERT_EQ(15, epub->getSpineItemsCount());
  const bool expected[] = {false, true,  false, true,  false, true,  false, false,
                           true,  false, true,  false, false, false, false};
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    EXPECT_EQ(expected[spine], epub->isCarriedSpine(spine)) << "spine " << spine;
  }
  EXPECT_FALSE(epub->isCarriedSpine(-1));
  EXPECT_FALSE(epub->isCarriedSpine(15));
}

TEST_F(HeadingCarryBookTest, ANextFileThatIsLargeIsNotScanned) {
  const auto epub = book({
      {"c00.xhtml", "<h1>1</h1>", "One"},
      {"c01.xhtml", "<h1>Not read</h1>" + std::string(HeadingCarry::kMaxFileBytes, ' '), ""},
  });
  EXPECT_TRUE(epub->isCarriedSpine(0));
}

TEST_F(HeadingCarryBookTest, ReadsTheBankedCopyAndSkipsAStaleOne) {
  const auto epub = book({{"c00.xhtml", "<h1>1</h1>", "One"}, {"c01.xhtml", text("ONE"), ""}});
  const std::string banked = Section::sectionHtmlCachePath(epub->getCachePath(), 0);
  fs::create_directories(fs::path(banked).parent_path());
  {
    // A copy of another size is stale (an interrupted bank): the entry is read instead.
    std::ofstream(banked, std::ios::binary) << doc("<p>stale</p>") << "padding";
  }
  EXPECT_TRUE(epub->isCarriedSpine(0));
  // A whole copy is read in place of the entry: here one that says otherwise, to show which was read.
  const auto again = std::make_shared<Epub>(epub->getPath(), (work / "cache").string());
  ASSERT_TRUE(again->load(true));
  const std::string otherBody = doc("<p>x</p>");
  const auto entryBytes = again->getSpineItem(0).zipUncompressedSize;
  std::string bytes = otherBody + std::string(entryBytes > otherBody.size() ? entryBytes - otherBody.size() : 0, ' ');
  ASSERT_EQ(entryBytes, bytes.size());
  std::ofstream(banked, std::ios::binary | std::ios::trunc) << bytes;
  EXPECT_FALSE(again->isCarriedSpine(0));
}

TEST_F(HeadingCarryBookTest, AFileThatCannotBeReadIsAskedAgain) {
  const auto epub = book({{"c00.xhtml", "<h1>1</h1>", "One"}, {"c01.xhtml", text("ONE"), ""}});
  {
    // No block for the inflate: not carried for now, and not remembered as such.
    const AllocationRefusal refusal(512);
    EXPECT_FALSE(epub->isCarriedSpine(0));
  }
  EXPECT_TRUE(epub->isCarriedSpine(0));
}

TEST_F(HeadingCarryBookTest, TheNearMissesAreNotCarried) {
  auto epub =
      std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_heading_near_miss.epub", (work / "cache").string());
  ASSERT_TRUE(epub->load(true));
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    EXPECT_FALSE(epub->isCarriedSpine(spine)) << "spine " << spine;
  }
}
