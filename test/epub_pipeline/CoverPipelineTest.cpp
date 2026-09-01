// Two cover-pipeline costs the home screen was paying on every cold start, measured on X4 with a
// five-book carousel:
//
//   ~3.3 s  re-parsing content.opf. loadForCover() parses it whenever book.bin is absent, and the
//           SAME book is asked three to five times in a row (try the thumb, check cover.img, start
//           the extract, then once per thumb size). One Cyrillic book paid 5 x 301 ms.
//   ~6.0 s  copying covers to cover.img before any of them could be decoded -- pure byte-shuffling
//           whenever the ZIP stores the entry uncompressed.
//
// Both fixes are only sound if they change nothing observable, which is what these pin: the memo
// must never answer for the wrong book, and an in-place decode must produce the same thumbnail as
// one that went through cover.img.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// Minimal EPUB whose OPF declares `coverName` as the cover image.
std::string makeEpub(const fs::path& path, const std::string& coverName, const std::string& coverBytes,
                     const std::string& title) {
  const std::string opf =
      "<?xml version=\"1.0\"?><package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" +
      title +
      "</dc:title><meta name=\"cover\" content=\"cov\"/></metadata>"
      "<manifest><item id=\"cov\" href=\"" +
      coverName +
      "\" media-type=\"image/png\"/>"
      "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
      "<spine><itemref idref=\"c1\"/></spine></package>";
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?><container version=\"1.0\" "
          "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
          "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
  zip.add("content.opf", opf);
  zip.add(coverName, coverBytes);
  zip.add("c1.xhtml", "<html><body><p>x</p></body></html>");
  zip.write(path.string());
  return path.string();
}

struct CoverPipelineFixture : testing::Test {
  fs::path work;
  std::string cacheDir;
  std::vector<uint8_t> pngBytes;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("cover_pipe_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);

    // A real PNG, pulled out of the corpus book (which deflates it). cache_test_1 rather than
    // scaling_test: the latter is 1200x1500, above the thumbnail path's MAX_PNG_PIXELS cap, so it
    // would fail here for a reason that has nothing to do with what is under test.
    const std::string extracted = (work / "src.png").string();
    Epub source(CORPUS_DIR "/test_png_images.epub", cacheDir);
    ASSERT_TRUE(source.extractItemToFile("OEBPS/images/cache_test_1.png", extracted, nullptr));
    std::ifstream in(extracted, std::ios::binary);
    pngBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    ASSERT_GT(pngBytes.size(), 500u);
    Epub::clearCoverMetadataMemo();
  }
  void TearDown() override {
    Epub::clearCoverMetadataMemo();
    fs::remove_all(work);
  }

  std::string pngString() const { return std::string(pngBytes.begin(), pngBytes.end()); }
};

// The memo must not leak one book's cover into another's — the failure that would put the wrong
// picture on the home screen.
TEST_F(CoverPipelineFixture, MemoDoesNotAnswerForADifferentBook) {
  const std::string a = makeEpub(work / "a.epub", "coverA.png", pngString(), "A");
  const std::string b = makeEpub(work / "b.epub", "coverB.png", pngString() + "padding-to-differ", "B");

  Epub epubA(a, (work / "ca").string());
  ASSERT_TRUE(epubA.loadForCover());
  EXPECT_EQ(epubA.getCoverItemHref(), "coverA.png");

  Epub epubB(b, (work / "cb").string());
  ASSERT_TRUE(epubB.loadForCover());
  EXPECT_EQ(epubB.getCoverItemHref(), "coverB.png") << "the memo served the previous book";

  // And back again — a replaced entry must not have poisoned the first answer either.
  Epub epubA2(a, (work / "ca").string());
  ASSERT_TRUE(epubA2.loadForCover());
  EXPECT_EQ(epubA2.getCoverItemHref(), "coverA.png");
}

// Repeat queries for the same book are what the memo exists for.
TEST_F(CoverPipelineFixture, RepeatedLoadForCoverAgrees) {
  const std::string a = makeEpub(work / "a.epub", "coverA.png", pngString(), "A");
  for (int i = 0; i < 5; i++) {
    Epub epub(a, (work / "ca").string());
    ASSERT_TRUE(epub.loadForCover()) << "attempt " << i;
    EXPECT_EQ(epub.getCoverItemHref(), "coverA.png") << "attempt " << i;
  }
}

// Same path, different content: the size key must force a re-parse rather than serve the old cover.
TEST_F(CoverPipelineFixture, ReplacedBookAtTheSamePathIsNotServedStale) {
  const fs::path path = work / "book.epub";
  makeEpub(path, "coverA.png", pngString(), "A");
  {
    Epub epub(path.string(), (work / "c1").string());
    ASSERT_TRUE(epub.loadForCover());
    ASSERT_EQ(epub.getCoverItemHref(), "coverA.png");
  }
  fs::remove(path);
  makeEpub(path, "coverB.png", pngString() + "different-length", "B");
  {
    Epub epub(path.string(), (work / "c2").string());
    ASSERT_TRUE(epub.loadForCover());
    EXPECT_EQ(epub.getCoverItemHref(), "coverB.png") << "stale memo served after the book changed";
  }
}

// A book with no cover is memoized too — the "no cover" answer is exactly what the next three
// calls would otherwise re-parse the OPF to rediscover.
TEST_F(CoverPipelineFixture, BookWithoutACoverStaysWithoutOne) {
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?><container version=\"1.0\" "
          "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
          "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
  zip.add("content.opf",
          "<?xml version=\"1.0\"?><package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\">"
          "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>NoCover</dc:title></metadata>"
          "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
          "<spine><itemref idref=\"c1\"/></spine></package>");
  zip.add("c1.xhtml", "<html><body><p>x</p></body></html>");
  const std::string path = (work / "nocover.epub").string();
  zip.write(path);

  for (int i = 0; i < 3; i++) {
    Epub epub(path, (work / "cn").string());
    EXPECT_FALSE(epub.loadForCover()) << "attempt " << i;
  }
}

// The in-place decode must produce exactly the thumbnail the extract-then-decode path does.
TEST_F(CoverPipelineFixture, StoredCoverThumbMatchesTheExtractedPath) {
  const std::string book = makeEpub(work / "book.epub", "cover.png", pngString(), "T");

  const auto read = [](const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };

  // Route 1: nothing cached -> the stored entry is decoded straight out of the archive.
  std::vector<uint8_t> viaInPlace;
  std::string inPlaceCoverImg;
  {
    Epub epub(book, (work / "inplace").string());
    ASSERT_TRUE(epub.loadForCover());
    inPlaceCoverImg = epub.getCoverImageCachePath();
    ASSERT_EQ(epub.generateThumbBmp(120, 160, /*allowExtract=*/false), ThumbResult::Ok)
        << "a stored cover should need no extraction at all";
    viaInPlace = read(epub.getThumbBmpPath(120, 160));
  }
  ASSERT_FALSE(viaInPlace.empty());
  EXPECT_FALSE(fs::exists(inPlaceCoverImg)) << "the whole point: no copy of the cover on the card";

  // Route 2: hand it a cover.img first, so the ordinary cached path runs instead.
  Epub::clearCoverMetadataMemo();
  {
    Epub epub(book, (work / "extracted").string());
    ASSERT_TRUE(epub.loadForCover());
    const fs::path img = epub.getCoverImageCachePath();
    fs::create_directories(img.parent_path());
    {
      std::ofstream out(img.string(), std::ios::binary);
      out.write(reinterpret_cast<const char*>(pngBytes.data()), static_cast<std::streamsize>(pngBytes.size()));
    }
    ASSERT_EQ(epub.generateThumbBmp(120, 160, /*allowExtract=*/false), ThumbResult::Ok);
    EXPECT_EQ(read(epub.getThumbBmpPath(120, 160)), viaInPlace)
        << "decoding in place must be indistinguishable from decoding the extracted copy";
  }
}

}  // namespace
