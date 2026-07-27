#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/BookMetadataCache.h"
#include "../../lib/Epub/Epub/parsers/ContentOpfParser.h"

namespace {

std::string makeTempDir() {
  std::error_code ec;
  const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return {};
  }

  std::random_device rd;
  std::array<uint32_t, 4> parts = {rd(), rd(), rd(), rd()};
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::filesystem::path base =
        tempRoot / ("opf-test-" + std::to_string(parts[0]) + "-" + std::to_string(parts[1]) + "-" +
                    std::to_string(parts[2]) + "-" + std::to_string(parts[3]) + "-" + std::to_string(attempt));
    ec.clear();
    if (std::filesystem::create_directory(base, ec)) {
      return base.string();
    }

    // Retry only on collisions; other filesystem errors should fail fast.
    if (ec && ec != std::errc::file_exists) {
      return {};
    }
  }

  return {};
}

struct TempDirGuard {
  explicit TempDirGuard(std::string path) : path(std::move(path)) {}
  ~TempDirGuard() {
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  std::string path;
};

bool parseOpfXml(ContentOpfParser& parser, const std::string& xml) {
  if (!parser.setup()) {
    return false;
  }

  const size_t mid = xml.size() / 2;
  const auto* data = reinterpret_cast<const uint8_t*>(xml.data());
  const size_t first = parser.write(data, mid);
  const size_t second = parser.write(data + mid, xml.size() - mid);
  return first == mid && second == xml.size() - mid;
}

std::string repeatedChar(const char c, const size_t count) { return std::string(count, c); }

std::string buildManifestItems(const int count) {
  std::string out;
  out.reserve(static_cast<size_t>(count) * 80);
  for (int i = 0; i < count; ++i) {
    out += "<item id='ch" + std::to_string(i) + "' href='text/ch" + std::to_string(i) +
           ".xhtml' media-type='application/xhtml+xml'/>";
  }
  return out;
}

}  // namespace

namespace opf_test_hooks {
extern std::vector<std::string>* g_spineHrefSink;
}

namespace {
struct ScopedSpineHrefSink {
  explicit ScopedSpineHrefSink(std::vector<std::string>* sink) { opf_test_hooks::g_spineHrefSink = sink; }
  ~ScopedSpineHrefSink() { opf_test_hooks::g_spineHrefSink = nullptr; }
};
}  // namespace

TEST(ContentOpfParser, ExtractsMetadataManifestAndGuideFields) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>"
      "<dc:title>Main Title</dc:title>"
      "<dc:title>Ignored Subtitle</dc:title>"
      "<dc:creator>Author One</dc:creator>"
      "<dc:creator>Author Two</dc:creator>"
      "<dc:language>en</dc:language>"
      "<dc:description>&lt;p&gt; Hello &lt;b&gt;World&lt;/b&gt; &lt;/p&gt;</dc:description>"
      "<meta name='cover' content='cover-xhtml'/>"
      "<meta name='calibre:series' content='Series Name'/>"
      "<meta name='calibre:series_index' content='2'/>"
      "</metadata>"
      "<manifest>"
      "<item id='cover-xhtml' href='text/cover.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='cover-image' href='images/cover.jpg' media-type='image/jpeg' properties='cover-image'/>"
      "<item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
      "<item id='nav' href='toc-nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>"
      "<item id='css' href='styles/main.css' media-type='text/css'/>"
      "<item id='pagemap' href='page-map.xml' media-type='application/oebps-page-map+xml'/>"
      "</manifest>"
      "<guide>"
      "<reference type='text' href='text/chapter-1.xhtml'/>"
      "<reference type='cover' href='text/cover.xhtml'/>"
      "</guide>"
      "</package>";

  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  EXPECT_EQ(parser.title, "Main Title");
  EXPECT_EQ(parser.author, "Author One, Author Two");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(parser.description, "Hello World");
  EXPECT_EQ(parser.series, "Series Name");
  EXPECT_EQ(parser.seriesIndex, "2");

  EXPECT_EQ(parser.tocNcxPath, "book/OEBPS/toc.ncx");
  EXPECT_EQ(parser.tocNavPath, "book/OEBPS/toc-nav.xhtml");
  EXPECT_EQ(parser.pageMapPath, "book/OEBPS/page-map.xml");
  ASSERT_EQ(parser.cssFiles.size(), 1u);
  EXPECT_EQ(parser.cssFiles[0], "book/OEBPS/styles/main.css");

  // meta cover points to XHTML wrapper, so it should be ignored in favor of
  // EPUB3 properties="cover-image" image item.
  EXPECT_EQ(parser.coverItemHref, "book/OEBPS/images/cover.jpg");
  EXPECT_EQ(parser.textReferenceHref, "book/OEBPS/text/chapter-1.xhtml");
  EXPECT_EQ(parser.guideCoverPageHref, "book/OEBPS/text/cover.xhtml");
}

TEST(ContentOpfParser, DecodesNumericCharacterReferencesInDescription) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  const std::string base = "/book/OEBPS/";
  // Calibre-style descriptions frequently double-escape the inner HTML, so the
  // parser sees the literal "&#8212;" after the outer &amp; is resolved. It must
  // decode both decimal (&#8212; → em dash) and hex (&#x2019; → right quote).
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>"
      "<dc:title>T</dc:title>"
      "<dc:description>A &amp;#8212; B&amp;#x2019;s tale</dc:description>"
      "</metadata>"
      "<manifest>"
      "<item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
      "</manifest>"
      "<spine/>"
      "</package>";

  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  // U+2014 EM DASH = e2 80 94, U+2019 RIGHT SINGLE QUOTATION MARK = e2 80 99
  EXPECT_EQ(parser.description, "A \xE2\x80\x94 B\xE2\x80\x99s tale");
}

TEST(ContentOpfParser, ResolvesSpineIdrefsUsingManifestItems) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>"
      "<item id='ch1' href='text/ch1.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ch2' href='text/ch2.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ch2'/>"
      "<itemref idref='missing'/>"
      "<itemref idref='ch1'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 2u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch2.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch1.xhtml");
}

TEST(ContentOpfParser, DoesNotHangOnOversizedManifestIdDuringSpineLookup) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  // Oversized ID pushes serialization::readString(FsFile, ...) down its
  // failure path during spine idref lookup in host tests.
  const std::string oversizedId = repeatedChar('x', 5000);
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>"
      "<item id='" +
      oversizedId +
      "' href='text/huge-id.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ch1' href='text/ch1.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ch1'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  // The key invariant for this regression is completion without an endless
  // loop while still resolving valid idrefs.
  ASSERT_EQ(capturedSpineHrefs.size(), 1u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch1.xhtml");
}

TEST(ContentOpfParser, SkipsMalformedFirstManifestEntryAndResolvesLaterIdrefs) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  // First entry is malformed for the temp store read path (oversized id),
  // while later entries remain valid and should still be resolved.
  const std::string oversizedId = repeatedChar('m', 5000);
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>"
      "<item id='" +
      oversizedId +
      "' href='text/bad.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ok1' href='text/ok1.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ok2' href='text/ok2.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ok1'/>"
      "<itemref idref='ok2'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  // Parser should complete, skip malformed entry safely, and still resolve
  // later valid idrefs.
  ASSERT_EQ(capturedSpineHrefs.size(), 2u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ok1.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ok2.xhtml");
}

TEST(ContentOpfParser, ResolvesSpineIdrefsUsingIndexedLookupForLargeManifest) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  constexpr int kItemCount = 420;  // > LARGE_SPINE_THRESHOLD to force index path
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) +
      "</manifest>"
      "<spine>"
      "<itemref idref='ch419'/>"
      "<itemref idref='ch10'/>"
      "<itemref idref='missing'/>"
      "<itemref idref='ch0'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 3u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch419.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch10.xhtml");
  EXPECT_EQ(capturedSpineHrefs[2], "book/OEBPS/text/ch0.xhtml");
}

// A huge manifest (the King's Avatar case: 1732 items) must parse without aborting and resolve every
// spine idref. The in-RAM index grows with NOTHROW allocation — kept when memory allows (the common
// case, and what the host has), dropped for the exact linear scan only on genuine OOM. Either way the
// book OPENS and resolves correctly. This is the regression guard for "a huge book crashes the device"
// (it used to abort building the index on -fno-exceptions) AND for "the fallback is O(N^2) slow" (the
// index is kept when there's memory, so a big book stays fast).
TEST(ContentOpfParser, HugeManifestResolvesCorrectly) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  constexpr int kItemCount = 1500;  // > MAX_INDEX_ENTRIES (1200) → index capped/disabled, linear scan
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>Huge</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) +
      "</manifest>"
      "<spine>"
      "<itemref idref='ch1499'/>"  // last item — resolvable only if the whole manifest was stored
      "<itemref idref='ch750'/>"   // middle
      "<itemref idref='missing'/>"
      "<itemref idref='ch0'/>"     // first
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml)) << "a 1500-item manifest must parse without aborting";

  ASSERT_EQ(capturedSpineHrefs.size(), 3u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch1499.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch750.xhtml");
  EXPECT_EQ(capturedSpineHrefs[2], "book/OEBPS/text/ch0.xhtml");
}

TEST(ContentOpfParser, DisablesHashTrustedIndexOnDuplicateIdsAndStillResolves) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  // The indexed lookup trusts (idHash, idLen) without reading the id back, so two manifest
  // items sharing an id (equal hash AND length — the same key shape a genuine 32-bit collision
  // would produce) must disable the index for the whole book. The exact linear scan then
  // resolves the ambiguous idref to its FIRST manifest occurrence, and unrelated idrefs keep
  // resolving normally.
  constexpr int kItemCount = 420;  // > LARGE_SPINE_THRESHOLD so the index path would engage
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) +
      "<item id='ch5' href='text/duplicate.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ch5'/>"
      "<itemref idref='ch419'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 2u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch5.xhtml");  // first occurrence wins
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch419.xhtml");
}
