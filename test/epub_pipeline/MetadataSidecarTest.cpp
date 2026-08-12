// Calibre-style "<book>.opf" metadata sidecar: a file beside the book wins over
// what is embedded in it, mirroring the cover sidecar rule.
//
// The property that matters, and the reason the overlay is applied on every load
// rather than baked into book.bin: editing the sidecar must take effect on the
// next open, even though the EPUB's own bytes never changed and the cache
// therefore stays valid. A baked implementation passes the first case below and
// fails SidecarEditTakesEffectOnCachedLoad.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Epub.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_headings.epub";
const char* kEmbeddedTitle = "Heading Font Size Test";

// Minimal Calibre-shaped metadata OPF. Calibre writes series as
// <meta name="calibre:series">, which ContentOpfParser already understands.
std::string sidecarXml(const std::string& title, const std::string& author, const std::string& series = "",
                       const std::string& seriesIndex = "") {
  std::string s =
      "<?xml version='1.0' encoding='utf-8'?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uuid_id\">\n"
      "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:opf=\"http://www.idpf.org/2007/opf\">\n";
  if (!title.empty()) s += "    <dc:title>" + title + "</dc:title>\n";
  if (!author.empty())
    s += "    <dc:creator opf:file-as=\"Sorted, Name\" opf:role=\"aut\">" + author + "</dc:creator>\n";
  if (!series.empty()) s += "    <meta name=\"calibre:series\" content=\"" + series + "\"/>\n";
  if (!seriesIndex.empty()) s += "    <meta name=\"calibre:series_index\" content=\"" + seriesIndex + "\"/>\n";
  s += "  </metadata>\n</package>\n";
  return s;
}

struct MetadataSidecarFixture : testing::Test {
  fs::path work;
  fs::path bookPath;
  fs::path sidecarPath;
  std::string cacheDir;

  void SetUp() override {
    // Per-test dir: ctest -j runs these as parallel processes.
    work = fs::temp_directory_path() /
           (std::string("epub_sidecar_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    bookPath = work / "book.epub";
    sidecarPath = work / "book.opf";
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
    fs::copy_file(kBook, bookPath);
  }
  void TearDown() override { fs::remove_all(work); }

  void writeSidecar(const std::string& xml) { std::ofstream(sidecarPath, std::ios::binary) << xml; }
};

TEST_F(MetadataSidecarFixture, NoSidecarLeavesEmbeddedMetadata) {
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.getTitle(), kEmbeddedTitle);
}

TEST_F(MetadataSidecarFixture, SidecarOverridesEmbeddedMetadata) {
  writeSidecar(sidecarXml("Sidecar Title", "Sidecar Author", "Sidecar Series", "3"));
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.getTitle(), "Sidecar Title");
  EXPECT_EQ(epub.getAuthor(), "Sidecar Author");
  EXPECT_EQ(epub.getSeries(), "Sidecar Series");
  EXPECT_EQ(epub.getSeriesIndex(), "3");
}

// The reason for overlaying live instead of baking into book.bin. First load
// builds and caches; the second hits the cached path, where parseContentOpf is
// never called - the sidecar must still be applied.
TEST_F(MetadataSidecarFixture, SidecarEditTakesEffectOnCachedLoad) {
  writeSidecar(sidecarXml("First Title", "First Author"));
  {
    Epub epub(bookPath.string(), cacheDir);
    ASSERT_TRUE(epub.load(true));
    ASSERT_EQ(epub.getTitle(), "First Title");
  }

  // Book bytes unchanged, so the cache stays valid and book.bin still holds
  // "First Title". Only the sidecar changed.
  writeSidecar(sidecarXml("Second Title", "Second Author"));
  {
    Epub epub(bookPath.string(), cacheDir);
    ASSERT_TRUE(epub.load(true));
    EXPECT_EQ(epub.getTitle(), "Second Title") << "sidecar was baked into book.bin instead of applied on load";
    EXPECT_EQ(epub.getAuthor(), "Second Author");
  }
}

// Removing the sidecar must fall back to the book's own metadata, not keep
// serving the overridden values from a previous run.
TEST_F(MetadataSidecarFixture, RemovingSidecarRestoresEmbeddedMetadata) {
  writeSidecar(sidecarXml("Sidecar Title", "Sidecar Author"));
  {
    Epub epub(bookPath.string(), cacheDir);
    ASSERT_TRUE(epub.load(true));
    ASSERT_EQ(epub.getTitle(), "Sidecar Title");
  }
  fs::remove(sidecarPath);
  {
    Epub epub(bookPath.string(), cacheDir);
    ASSERT_TRUE(epub.load(true));
    EXPECT_EQ(epub.getTitle(), kEmbeddedTitle);
  }
}

// A sidecar supplying only some fields must not blank out the rest.
TEST_F(MetadataSidecarFixture, PartialSidecarLeavesOtherFieldsIntact) {
  writeSidecar(sidecarXml("", "Only The Author"));
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.getAuthor(), "Only The Author");
  EXPECT_EQ(epub.getTitle(), kEmbeddedTitle) << "empty sidecar field must not overwrite embedded metadata";
}

// Malformed XML must leave the book usable with its embedded metadata rather
// than failing the load.
TEST_F(MetadataSidecarFixture, MalformedSidecarIsIgnored) {
  writeSidecar("<package><metadata><dc:title>Broken");
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true)) << "a bad sidecar must not make the book unopenable";
  EXPECT_EQ(epub.getTitle(), kEmbeddedTitle);
}

// Guards the size cap: a large file that happens to share the basename is not
// read into the heap.
TEST_F(MetadataSidecarFixture, OversizedSidecarIsIgnored) {
  std::string big = sidecarXml("Should Be Ignored", "Nobody");
  big += std::string(Epub::MAX_METADATA_SIDECAR_BYTES, ' ');
  writeSidecar(big);
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.getTitle(), kEmbeddedTitle);
}

// Contract with the metadata-editor plugin (plugins/metadata-editor): the exact
// document its freshDoc() + writeInto() produce must be readable here, for all
// six fields it edits. The plugin's XML handling runs in a browser and cannot be
// unit-tested from this suite, so this pins the format they agree on - if the
// editor's output shape ever drifts, this is what catches it.
TEST_F(MetadataSidecarFixture, ReadsTheShapeTheMetadataEditorWrites) {
  writeSidecar(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uuid_id\">\n"
      "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">"
      "<dc:title>Edited Title</dc:title>"
      "<dc:creator>Edited Author</dc:creator>"
      "<dc:language>de</dc:language>"
      "<meta name=\"calibre:series\" content=\"Edited Series\"/>"
      "<meta name=\"calibre:series_index\" content=\"7\"/>"
      "<dc:description>Edited description text.</dc:description>"
      "</metadata>\n"
      "</package>\n");

  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.getTitle(), "Edited Title");
  EXPECT_EQ(epub.getAuthor(), "Edited Author");
  EXPECT_EQ(epub.getLanguage(), "de");
  EXPECT_EQ(epub.getSeries(), "Edited Series");
  EXPECT_EQ(epub.getSeriesIndex(), "7");
  EXPECT_EQ(epub.getDescription(), "Edited description text.");
}

TEST_F(MetadataSidecarFixture, MetadataSidecarPathResolves) {
  EXPECT_EQ(Epub::metadataSidecarPath(bookPath.string()), "");
  writeSidecar(sidecarXml("T", "A"));
  EXPECT_EQ(Epub::metadataSidecarPath(bookPath.string()), sidecarPath.string());
  // An extension-less path has nothing to swap.
  EXPECT_EQ(Epub::metadataSidecarPath((work / "noext").string()), "");
}

}  // namespace
