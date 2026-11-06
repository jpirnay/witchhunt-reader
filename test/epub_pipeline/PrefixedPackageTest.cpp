// A book whose XML names every element with a prefix of its own choosing.
//
// A prefix is only a local name for a namespace: <ns0:package xmlns:ns0="http://www.idpf.org/2007/opf"> and <package
// xmlns="http://www.idpf.org/2007/opf"> mean the same thing. Python's ElementTree writes them like this, so a tool
// that rewrites a book may too. The parsers compared literal names and took "package" and "opf:package" only: such a
// book had no spine at all. CrossInk 8bd0001a (Julia Nguyen) fixed the OPF with Expat's namespace mode; ours is on
// yxml, which has none, so the prefixes are read from the xmlns attributes.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "Epub.h"
#include "Epub/BookMetadataCache.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

constexpr char kContainer[] =
    "<?xml version=\"1.0\"?>\n<ns0:container xmlns:ns0=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
    "version=\"1.0\"><ns0:rootfiles><ns0:rootfile full-path=\"content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></ns0:rootfiles></ns0:container>\n";

constexpr char kPlainContainer[] =
    "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
    "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n";

constexpr char kNcx[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ns2:ncx xmlns:ns2=\"http://www.daisy.org/z3986/2005/ncx/\" "
    "version=\"2005-1\"><ns2:navMap><ns2:navPoint id=\"p1\" playOrder=\"1\"><ns2:navLabel><ns2:text>Chapter One"
    "</ns2:text></ns2:navLabel><ns2:content src=\"chapter.xhtml\"/></ns2:navPoint></ns2:navMap></ns2:ncx>\n";

constexpr char kChapter[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C</title>"
    "</head><body><p>Text</p></body></html>\n";

// `metadataOpen` carries the DC declaration where the book under test puts it.
std::string prefixedOpf(const std::string& packageAttrs, const std::string& metadataAttrs,
                        const std::string& extraMetadata = "") {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ns0:package xmlns:ns0=\"http://www.idpf.org/2007/opf\" " +
         packageAttrs + " version=\"2.0\" unique-identifier=\"id\"><ns0:metadata " + metadataAttrs + ">" +
         extraMetadata +
         "<ns1:identifier id=\"id\" ns0:scheme=\"ISBN\">9780000000000</ns1:identifier>"
         "<ns1:title>Prefixed</ns1:title>"
         "<ns1:creator ns0:role=\"aut\" ns0:file-as=\"Doe, Jane\">Jane Doe</ns1:creator>"
         "<ns1:language>de</ns1:language></ns0:metadata><ns0:manifest>"
         "<ns0:item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
         "<ns0:item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/></ns0:manifest>"
         "<ns0:spine toc=\"ncx\"><ns0:itemref idref=\"c\"/></ns0:spine></ns0:package>\n";
}

constexpr char kDc[] = "xmlns:ns1=\"http://purl.org/dc/elements/1.1/\"";

struct PrefixedPackageFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_prefixed_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::shared_ptr<Epub> book(const std::string& container, const std::string& opf) const {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml", container);
    zip.add("content.opf", opf);
    zip.add("toc.ncx", kNcx);
    zip.add("chapter.xhtml", kChapter);
    const std::string path = (work / "book.epub").string();
    zip.write(path);
    return std::make_shared<Epub>(path, (work / "cache").string());
  }
};

}  // namespace

TEST_F(PrefixedPackageFixture, ABookPrefixedThroughoutOpens) {
  const auto epub = book(kContainer, prefixedOpf(kDc, ""));
  ASSERT_TRUE(epub->load(true));
  EXPECT_EQ(epub->getTitle(), "Prefixed");
  EXPECT_EQ(epub->getAuthor(), "Jane Doe");
  EXPECT_EQ(epub->getLanguage(), "de");
  EXPECT_EQ(epub->getSpineItemsCount(), 1);
  ASSERT_EQ(epub->getTocItemsCount(), 1);
  EXPECT_EQ(epub->getTocItem(0).title, "Chapter One");
}

TEST_F(PrefixedPackageFixture, ThePrefixMayBeDeclaredOnTheMetadataElement) {
  const auto epub = book(kPlainContainer, prefixedOpf("", kDc));
  ASSERT_TRUE(epub->load(true));
  EXPECT_EQ(epub->getTitle(), "Prefixed");
}

// The same local name in another namespace is another element.
TEST_F(PrefixedPackageFixture, ATitleInAnotherNamespaceIsNotTheBooksTitle) {
  const auto epub =
      book(kPlainContainer, prefixedOpf(kDc, "xmlns:foo=\"urn:example:other\"", "<foo:title>Wrong</foo:title>"));
  ASSERT_TRUE(epub->load(true));
  EXPECT_EQ(epub->getTitle(), "Prefixed");
}
