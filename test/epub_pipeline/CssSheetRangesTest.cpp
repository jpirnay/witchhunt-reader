// A book's stylesheets each own one contiguous range of rule orders, and the CSS compile records each sheet's range and
// the sheets its @import rules name at the end of the rules cache (CSS_CACHE_VERSION 36): what a chapter that resolves
// against only the sheets it links will read (docs/future_work/memory-and-parsing.md). Recording them changes no rule.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"
#include "Epub/css/CssParser.h"
#include "HalStorage.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// main.css is linked by every chapter and imports base.css. vertical.css is linked by none, and its body rule centres
// every chapter while every stylesheet applies to every chapter (matcha-reader #453's vertical_image.css).
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
};

}  // namespace

// Each sheet's range, in manifest order, main.css's import of base.css, and every rule record's order inside its own
// sheet's range: main.css p 0, .note 1, h1 and h2 2 (div.poem > p, 3, is a complex rule); base.css body 4, .note 5;
// vertical.css body 6 (.vertical p, 7, complex).
TEST_F(CssSheetRangesTest, EachSheetOwnsTheOrdersOfItsRules) {
  const auto epub = book();
  CssParser parser(epub->getCachePath());  // a later open
  ASSERT_TRUE(parser.loadSheetTable());
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
