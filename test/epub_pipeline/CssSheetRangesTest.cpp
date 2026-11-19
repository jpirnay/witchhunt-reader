// A book's stylesheets each own one contiguous range of rule orders, and the CSS compile records each sheet's range and
// the sheets its @import rules name at the end of the rules cache (CSS_CACHE_VERSION 36). A chapter resolves against
// only the sheets its head links and what those import (Section::scopeStylesheets). Recording them changes no rule.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"
#include "Epub/HeadingCarry.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/css/CssParser.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// main.css is linked by every chapter and imports base.css. vertical.css is linked by none, as matcha-reader #453's
// vertical_image.css, whose body rule centred every chapter there.
constexpr const char* kMain =
    "@charset \"utf-8\";\n@import url(\"base.css\");\np { text-indent: 1em; }\n.note { font-style: italic; }\n"
    "h1, h2 { text-align: center; }\ndiv.poem > p { margin-left: 2em; }\n";
constexpr const char* kBase = "body { margin-left: 1em; }\n.note { font-weight: bold; }\n";
constexpr const char* kVertical = "body { text-align: center; }\n.vertical p { font-style: italic; }\n";

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint16_t u16At(const std::string& bytes, const size_t at) {
  uint16_t v = 0;
  if (at + 2 <= bytes.size()) std::memcpy(&v, bytes.data() + at, 2);
  return v;
}

// The rules cache without the sheet table it ends in: the header, the complex rules, the index and the rule records.
std::string withoutSheetTable(const std::string& cache) {
  if (cache.size() < 2) return {};
  const size_t tableBytes = u16At(cache, cache.size() - 2);
  if (tableBytes + 2 > cache.size()) return {};
  return cache.substr(0, cache.size() - 2 - tableBytes);
}

// Every rule record of the cache as {selector, order}, read where the format puts them (docs/file-formats.md), sorted.
std::vector<std::pair<std::string, uint16_t>> ruleOrders(const std::string& bytes) {
  std::vector<std::pair<std::string, uint16_t>> rules;
  if (bytes.size() < CssParser::CSS_CACHE_HEADER_BYTES) return rules;
  const size_t index = CssParser::CSS_CACHE_HEADER_BYTES + u16At(bytes, 12);
  for (uint16_t i = 0; i < u16At(bytes, 1); ++i) {
    uint32_t offset = 0;
    if (index + 8 * i + 8 > bytes.size()) break;
    std::memcpy(&offset, bytes.data() + index + 8 * i + 4, 4);
    const uint16_t length = u16At(bytes, offset);
    if (offset + 2 + length + 2 > bytes.size()) break;
    rules.emplace_back(bytes.substr(offset + 2, length), u16At(bytes, offset + 2 + length));
  }
  std::sort(rules.begin(), rules.end());
  return rules;
}

// The first line of a paragraph as the build lays it out: its alignment, and whether its first word is italic or bold.
struct FirstLine {
  CssTextAlign align = CssTextAlign::None;
  bool italic = false;
  bool bold = false;
};

// The first line of each paragraph of the chapter at `spine`, by its second word, built as the reader builds it under
// Justified, the default: a book's centre and right stand (BookAlignmentLayoutTest). A paragraph takes its own rules'
// text-align, not its body's.
std::map<std::string, FirstLine> firstLines(const std::shared_ptr<Epub>& epub, const int spine) {
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  params.embeddedStyle = true;
  params.paragraphAlignment = 0;
  GfxRenderer renderer;
  Section section(epub, spine, renderer);
  EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
  EXPECT_TRUE(section.loadSectionFile(params));
  std::map<std::string, FirstLine> lines;
  for (uint16_t p = 0; p < section.pageCount; ++p) {
    section.currentPage = p;
    const auto page = section.loadPageFromSectionFile();
    if (!page) continue;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
      if (block.wordCount() < 2 || lines.count(block.wordText(1)) != 0) continue;
      const uint8_t style = block.wordStyle(0);
      lines[block.wordText(1)] = {block.getRenderStyle().alignment, (style & EpdFontFamily::ITALIC) != 0,
                                  (style & EpdFontFamily::BOLD) != 0};
    }
  }
  return lines;
}

struct CssSheetRangesTest : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("css_sheet_ranges_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(work, ec);
  }

  // Two chapters that link main.css; the manifest lists main.css, base.css and vertical.css, in that order.
  std::shared_ptr<Epub> book() {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest =
        "<item id=\"main\" href=\"Styles/main.css\" media-type=\"text/css\"/>\n"
        "<item id=\"base\" href=\"Styles/base.css\" media-type=\"text/css\"/>\n"
        "<item id=\"vertical\" href=\"Styles/vertical.css\" media-type=\"text/css\"/>\n";
    std::string spine;
    for (int i = 0; i < 2; ++i) {
      const std::string id = "c" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"Text/" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>";
      zip.add("OEBPS/Text/" + id + ".xhtml",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
              "<title>T</title><link rel=\"stylesheet\" href=\"../Styles/main.css\" type=\"text/css\"/></head>"
              "<body><p class=\"note\">Chapter " +
                  std::to_string(i) + ".</p></body></html>\n");
    }
    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"uid\">urn:isbn:978-0-00-000000-3</dc:identifier><dc:title>Sheets</dc:title>"
            "<dc:language>en</dc:language></metadata><manifest>" +
                manifest + "</manifest><spine>" + spine + "</spine></package>\n");
    zip.add("OEBPS/Styles/main.css", kMain);
    zip.add("OEBPS/Styles/base.css", kBase);
    zip.add("OEBPS/Styles/vertical.css", kVertical);
    const std::string path = (work / "book.epub").string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    return epub;
  }

  // A book of `chapters` {file, head, body, TOC label} in spine order with a nav TOC, and `sheets` {file, css}, all in
  // the package's folder.
  struct File {
    std::string name;
    std::string head;
    std::string body;
    std::string tocLabel;
  };
  std::shared_ptr<Epub> bookOf(const std::vector<std::pair<std::string, std::string>>& sheets,
                               const std::vector<File>& chapters) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest =
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n";
    for (size_t i = 0; i < sheets.size(); ++i) {
      manifest +=
          "<item id=\"css" + std::to_string(i) + "\" href=\"" + sheets[i].first + "\" media-type=\"text/css\"/>\n";
      zip.add(sheets[i].first, sheets[i].second);
    }
    std::string spine;
    std::string toc;
    for (size_t i = 0; i < chapters.size(); ++i) {
      const File& c = chapters[i];
      manifest +=
          "<item id=\"c" + std::to_string(i) + "\" href=\"" + c.name + "\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"c" + std::to_string(i) + "\"/>";
      if (!c.tocLabel.empty()) toc += "<li><a href=\"" + c.name + "\">" + c.tocLabel + "</a></li>\n";
      zip.add(c.name,
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
              "<title>T</title>" +
                  c.head + "</head><body>" + c.body + "</body></html>\n");
    }
    zip.add("nav.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
            "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>N</title></head><body><nav epub:type=\"toc\">"
            "<ol>\n" +
                toc + "</ol></nav></body></html>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"uid\">sheets-of</dc:identifier><dc:title>Sheets</dc:title>"
            "<dc:language>en</dc:language></metadata><manifest>" +
                manifest + "</manifest><spine>" + spine + "</spine></package>\n");
    const std::string path = (work / "book-of.epub").string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }
};

std::string linkTo(const std::string& href) {
  return "<link rel=\"stylesheet\" type=\"text/css\" href=\"" + href + "\"/>";
}

}  // namespace

// Each sheet's range, in manifest order, main.css's import of base.css, and every rule record's order inside its own
// sheet's range: main.css p 0, .note 1, h1 and h2 2 (div.poem > p, 3, is a complex rule); base.css body 4, .note 5;
// vertical.css body 6 (.vertical p, 7, complex).
TEST_F(CssSheetRangesTest, EachSheetOwnsTheOrdersOfItsRules) {
  const auto epub = book();
  CssParser parser(epub->getCachePath());  // a later open
  ASSERT_TRUE(parser.loadFromCache());     // the table comes with the index
  const auto ranges = parser.sheetRanges();
  ASSERT_EQ(ranges.size(), 3u);
  EXPECT_EQ(ranges[0].pathHash, CssParser::sheetPathHash("OEBPS/Styles/main.css"));
  EXPECT_EQ(ranges[0].firstOrder, 0);
  EXPECT_EQ(ranges[0].endOrder, 4);
  EXPECT_EQ(ranges[1].pathHash, CssParser::sheetPathHash("OEBPS/Styles/base.css"));
  EXPECT_EQ(ranges[1].firstOrder, 4);
  EXPECT_EQ(ranges[1].endOrder, 6);
  EXPECT_EQ(ranges[2].pathHash, CssParser::sheetPathHash("OEBPS/Styles/vertical.css"));
  EXPECT_EQ(ranges[2].firstOrder, 6);
  EXPECT_EQ(ranges[2].endOrder, 8);
  ASSERT_EQ(parser.sheetImports().size(), 1u);
  EXPECT_EQ(parser.sheetImports()[0].importer, 0);
  EXPECT_EQ(parser.sheetImports()[0].importedHash, CssParser::sheetPathHash("OEBPS/Styles/base.css"));

  const std::vector<std::pair<std::string, uint16_t>> expected = {{".note", 1}, {".note", 5}, {"body", 4}, {"body", 6},
                                                                  {"h1", 2},    {"h2", 2},    {"p", 0}};
  EXPECT_EQ(ruleOrders(readFile(fs::path(epub->getCachePath()) / "css_rules.cache")), expected);
}

// The rules compile as they did: up to the table, the cache holds the bytes a compile of the three sheets as one
// unnamed sheet writes, which naming the sheets did not change before the table either.
TEST_F(CssSheetRangesTest, TheRulesAreTheBytesASheetlessCompileWrites) {
  const auto epub = book();
  const std::string named = readFile(fs::path(epub->getCachePath()) / "css_rules.cache");

  const fs::path css = work / "all.css";
  std::ofstream(css, std::ios::binary) << kMain << kBase << kVertical;
  const fs::path oneDir = work / "one";
  fs::create_directories(oneDir);
  {
    CssParser parser(oneDir.string());
    ASSERT_TRUE(parser.beginCacheCompile());
    {
      FsFile file;
      ASSERT_TRUE(Storage.openFileForRead("CSS", css.string(), file));
      ASSERT_TRUE(parser.appendCompiledFromStream(file));
    }
    ASSERT_TRUE(parser.endCacheCompile());
  }
  const std::string unnamed = readFile(oneDir / "css_rules.cache");

  ASSERT_FALSE(withoutSheetTable(unnamed).empty());
  // Compared as bools: a failure prints the sizes, not two dumps of binary.
  EXPECT_TRUE(withoutSheetTable(named) == withoutSheetTable(unnamed))
      << named.size() << " bytes named, " << unnamed.size() << " unnamed";
  EXPECT_FALSE(named == unnamed) << "only the table tells the sheets apart";
}

// matcha-reader #453: a sheet that no chapter links (its vertical_image.css) centred every chapter while every
// stylesheet applied to every chapter. A chapter resolves against the main.css it links and the base.css main.css
// imports: its note is italic (main.css) and bold (base.css), and not centred.
TEST_F(CssSheetRangesTest, ASheetNoChapterLinksStylesNoChapter) {
  const auto epub = bookOf({{"main.css", "@import \"base.css\";\n.note { font-style: italic; }\n"},
                            {"base.css", ".note { font-weight: bold; }\n"},
                            {"vertical.css", "p { text-align: center; }\n"}},
                           {{"c00.xhtml", linkTo("main.css"), "<p class=\"note\">NOTE text.</p>", "One"}});
  const auto lines = firstLines(epub, 0);
  ASSERT_EQ(lines.count("text."), 1u);
  EXPECT_EQ(lines.at("text.").align, CssTextAlign::Justify);
  EXPECT_TRUE(lines.at("text.").italic);
  EXPECT_TRUE(lines.at("text.").bold) << "base.css, through main.css's @import";
}

// test/epubs/test_css_scoping.epub (make_test_css_scoping.py): each chapter resolves against the sheets it links and
// what those import; one that links none, or a sheet the book does not have, against every sheet, as every chapter
// did. Outside a build the book resolves against every sheet: the scope ends with its build.
TEST_F(CssSheetRangesTest, EachChapterOfTheScopingBookResolvesAgainstItsOwnSheets) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_css_scoping.epub", (work / "cache").string());
  ASSERT_TRUE(epub->load(true));
  ASSERT_EQ(epub->getSpineItemsCount(), 5);
  struct Expected {
    CssTextAlign body;
    CssTextAlign part;
    bool importedItalic;
  };
  constexpr Expected kEverySheet{CssTextAlign::Center, CssTextAlign::Center, true};
  const Expected expected[] = {
      {CssTextAlign::Justify, CssTextAlign::Right, true},  // main.css, and base.css through its @import
      kEverySheet,                                         // main.css and centre.css
      kEverySheet,                                         // no link
      kEverySheet,                                         // main.css and a sheet the book does not have
      {CssTextAlign::Center, CssTextAlign::Center, false}  // centre.css alone
  };
  for (int spine = 0; spine < 5; ++spine) {
    SCOPED_TRACE("chapter " + std::to_string(spine + 1));
    const auto lines = firstLines(epub, spine);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines.at("BODY").align, expected[spine].body);
    EXPECT_FALSE(lines.at("BODY").italic);
    EXPECT_EQ(lines.at("PART").align, expected[spine].part);
    EXPECT_EQ(lines.at("IMPORTED").align, expected[spine].body);
    EXPECT_EQ(lines.at("IMPORTED").italic, expected[spine].importedItalic);
  }
  EXPECT_EQ(epub->getCssParser()->resolveStyle("p", "").textAlign, CssTextAlign::Center);
}

// A heading file laid out with the chapter after it (HeadingCarry) is part of its build: the chapter resolves against
// the sheets either links. head.css sets the heading, which only the heading file links; other.css, which neither
// links, does not centre them.
TEST_F(CssSheetRangesTest, ACarriedHeadingFileBringsItsSheets) {
  const auto epub = bookOf({{"main.css", "p { font-style: italic; }\n"},
                            {"head.css", "h1 { text-align: right; }\n"},
                            {"other.css", "p { text-align: center; }\n"}},
                           {{"c00.xhtml", linkTo("head.css"), "<h1>HEADING One</h1>", "One"},
                            {"c01.xhtml", linkTo("main.css"),
                             "<p>CHAPTER opens the chapter.</p><p>CHAPTER goes on for a second paragraph.</p>", ""}});
  ASSERT_TRUE(epub->isCarriedSpine(0));
  const auto lines = firstLines(epub, 1);
  ASSERT_EQ(lines.count("One"), 1u) << "the heading is laid out in the chapter's build";
  EXPECT_EQ(lines.at("One").align, CssTextAlign::Right);
  ASSERT_EQ(lines.count("opens"), 1u);
  EXPECT_EQ(lines.at("opens").align, CssTextAlign::Justify);
  EXPECT_TRUE(lines.at("opens").italic);
}

// A rules cache of this version always ends in its sheet table, written last, so one that does not is damaged (a write
// cut off, a bad card): the open deletes it and compiles the stylesheets again, rather than resolve every chapter
// against every sheet for as long as the file lasts.
TEST_F(CssSheetRangesTest, ARulesCacheWithoutAValidSheetTableIsCompiledAgain) {
  const auto epub = book();
  const fs::path cacheFile = fs::path(epub->getCachePath()) / "css_rules.cache";
  const std::string compiled = readFile(cacheFile);
  ASSERT_GT(compiled.size(), 2u);
  const size_t table = u16At(compiled, compiled.size() - 2) + 2u;
  ASSERT_LT(table, compiled.size());
  for (const bool cut : {true, false}) {
    SCOPED_TRACE(cut ? "the table cut off" : "a table that does not add up");
    std::string damaged = compiled;
    if (cut) {
      damaged.resize(compiled.size() - table);
    } else {
      damaged[damaged.size() - 2] = static_cast<char>(damaged[damaged.size() - 2] ^ 0x10);
    }
    std::ofstream(cacheFile, std::ios::binary | std::ios::trunc) << damaged;
    auto reopened = std::make_shared<Epub>(epub->getPath(), (work / "cache").string());
    ASSERT_TRUE(reopened->load(true));
    EXPECT_TRUE(readFile(cacheFile) == compiled) << "compiled again";
    EXPECT_EQ(firstLines(reopened, 0).at("0.").align, CssTextAlign::Justify);
  }
}

// --- The head, read by the heading carry's scanner (HeadingCarry::Scanner's link hook, HeadingCarry::scanHead) ---

namespace {

// A chapter whose head holds `head`, after an optional prolog, with `body`.
std::string chapterWith(const std::string& head, const std::string& body = "<p>Text.</p>",
                        const std::string& prolog = "") {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + prolog +
         "<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:h=\"http://www.w3.org/1999/xhtml\">\n"
         "<head><title>T</title>" +
         head + "</head>\n<body>" + body + "</body>\n</html>\n";
}

// What a chapter in the package's folder holding `xhtml` resolves against, its head read as Section::scopeStylesheets
// reads it; -1 when the file could not be written or read.
int scopeOfHead(CssParser& css, const fs::path& file, const std::string& xhtml) {
  std::ofstream(file, std::ios::binary) << xhtml;
  CssParser::SheetLinks links{&css, ""};
  char values[256];
  const HeadingCarry::LinkHook hook{&CssParser::addLink, &links, values, sizeof(values)};
  HeadingCarry::Scanner scan;
  scan.setLinkHook(&hook);
  FsFile in;
  if (!Storage.openFileForRead("TST", file.string(), in)) return -1;
  if (!HeadingCarry::scanHead(in, scan)) links.unknown = true;
  return css.scopeToSheets(links);
}

}  // namespace

// The head is read as markup: a link in a comment, in a CDATA section or in the body names no sheet; quotes of either
// kind, upper case, attributes in any order, a '>' in a quoted value and a prefixed name read as a browser reads them,
// and only a link's rel and href are kept. A rel or href not quoted, or longer than the 256 B the scan keeps, cannot be
// read, nor can a head past 8 KB: every sheet applies. A file that ends in its head was read whole. Not read: an
// <?xml-stylesheet?> instruction (docs/future_work/memory-and-parsing.md lists what the scan does not see).
TEST_F(CssSheetRangesTest, TheHeadIsReadAsMarkup) {
  const auto epub = bookOf({{"a.css", "@import \"base.css\";\n.a { font-weight: bold; }\n"},
                            {"base.css", ".b { font-style: italic; }\n"},
                            {"c.css", "p { text-align: center; }\n"},
                            {"d.css", "@import \"nowhere.css\";\n.d { font-weight: bold; }\n"}},
                           {{"c00.xhtml", "", "<p>Text.</p>", "One"}});
  CssParser& css = *epub->getCssParser();
  ASSERT_TRUE(css.loadFromCache());
  ASSERT_EQ(css.sheetRanges().size(), 4u);
  const std::string a = linkTo("a.css");
  const std::string c = linkTo("c.css");
  const struct {
    const char* what;
    std::string xhtml;
    int inScope;
  } cases[] = {
      {"a link", chapterWith(a), 2},
      {"a link in a comment", chapterWith(a + "<!-- " + c + " -->"), 2},
      {"a link in CDATA", chapterWith(a + "<script><![CDATA[ s = '" + c + "'; ]]></script>"), 2},
      {"a link in the body", chapterWith(a, "<p>Text.</p>" + c), 2},
      {"single quotes, upper case, no slash", chapterWith("<LINK HREF='c.css' REL='StyleSheet'>"), 1},
      {"a '>' in a quoted value", chapterWith("<meta name=\"x\" content=\"a > b\"/>" + c), 1},
      {"a prefixed name", chapterWith("<h:link rel=\"stylesheet\" href=\"c.css\"/>"), 1},
      {"a long title", chapterWith("<link rel=\"stylesheet\" title=\"" + std::string(300, 't') + "\" href=\"c.css\"/>"),
       1},
      {"an unquoted value", chapterWith(a + "<link rel=stylesheet href=\"c.css\"/>"), 0},
      {"an href past 256 B", chapterWith(a + linkTo(std::string(300, 'x') + ".css")), 0},
      {"a head past 8 KB", chapterWith(a + "<meta name=\"x\" content=\"" + std::string(9000, 'x') + "\"/>"), 0},
      {"a file that ends in its head", "<html><head>" + a, 2},
      {"an xml-stylesheet instruction",
       chapterWith(a, "<p>Text.</p>", "<?xml-stylesheet href=\"c.css\" type=\"text/css\"?>\n"), 2},
  };
  for (const auto& test : cases) {
    EXPECT_EQ(scopeOfHead(css, work / "head.xhtml", test.xhtml), test.inScope) << test.what;
  }
}

// The link hook changes nothing the heading carry decides: the same shape and body bounds with it as without, for
// heads with links in them and the bodies the carry looks at.
TEST_F(CssSheetRangesTest, TheLinkHookChangesNoCarryVerdict) {
  const std::string heads[] = {
      "", linkTo("a.css") + "<!-- " + linkTo("b.css") + " -->",
      "<LINK HREF='c.css' REL='StyleSheet'><style>h1 { color: black; }</style>",
      "<link rel=stylesheet href=x.css><h:link rel=\"stylesheet\" href=\"" + std::string(300, 'x') + "\"/>"};
  const std::string bodies[] = {"<h1>One</h1>", "<h1>One</h1><p>Text.</p>", "", "<a href=\"x.xhtml\">x</a>",
                                "<div><h2 class=\"t\">Two</h2></div>"};
  for (const std::string& head : heads) {
    for (const std::string& body : bodies) {
      const std::string xhtml = chapterWith(head, body);
      HeadingCarry::Scanner plain;
      HeadingCarry::Scanner hooked;
      CssParser::SheetLinks links;
      char values[64];
      const HeadingCarry::LinkHook hook{[](void*, std::string_view, std::string_view, bool) {}, &links, values,
                                        sizeof(values)};
      hooked.setLinkHook(&hook);
      for (size_t at = 0; at < xhtml.size(); at += 7) {
        const std::string piece = xhtml.substr(at, 7);
        plain.feed(reinterpret_cast<const uint8_t*>(piece.data()), piece.size());
        hooked.feed(reinterpret_cast<const uint8_t*>(piece.data()), piece.size());
      }
      EXPECT_EQ(plain.finish(), hooked.finish()) << xhtml;
      EXPECT_EQ(plain.bodyBegin(), hooked.bodyBegin()) << xhtml;
      EXPECT_EQ(plain.bodyEnd(), hooked.bodyEnd()) << xhtml;
    }
  }
}
