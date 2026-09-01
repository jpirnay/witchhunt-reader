// Decoding a PNG in place, straight out of the EPUB, when the ZIP stores it uncompressed.
//
// The extraction this replaces is pure copying, and on device it dominated: 857 KB at ~255 KB/s
// = 3.36 s of the 14.93 s an uncached cover page cost, plus 857 KB written to the card. Nothing
// has to be inflated to reach a stored entry, so the decoder can read the archive directly.
//
// What has to hold:
//   1. A stored entry's reported range really is the file (offset and length both right).
//   2. A deflated entry reports NO range -- those bytes are a DEFLATE stream, and handing them
//      to a PNG decoder would be reading garbage.
//   3. Decoding in place produces exactly the pixels that decoding the extracted copy does.
//   4. ImageBlock actually takes the shortcut: the image renders and is cached WITHOUT the
//      extracted file ever appearing on disk.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

// The corpus book deflates every image (verified: method 8 throughout), which is exactly why it
// cannot serve as the fixture here -- it is reused only as a convenient source of a real PNG.
const char* kDeflatedBook = CORPUS_DIR "/test_png_images.epub";
const char* kEntry = "OEBPS/images/scaling_test.png";

struct StoredEntryFixture : testing::Test {
  fs::path work;
  std::string storedBook;
  std::string cacheDir;
  std::vector<uint8_t> pngBytes;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("png_stored_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);

    // Pull a real PNG out of the (deflated) corpus book, then repack it STORED.
    const std::string extracted = (work / "extracted.png").string();
    Epub source(kDeflatedBook, cacheDir);
    ASSERT_TRUE(source.extractItemToFile(kEntry, extracted, nullptr));
    std::ifstream in(extracted, std::ios::binary);
    pngBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    ASSERT_GT(pngBytes.size(), 1000u);

    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add(kEntry, std::string(pngBytes.begin(), pngBytes.end()));
    storedBook = (work / "stored.epub").string();
    zip.write(storedBook);
  }
  void TearDown() override { fs::remove_all(work); }

  RenderConfig configFor(const std::string& cachePath) const {
    RenderConfig config;
    config.x = 0;
    config.y = 0;
    config.maxWidth = 120;
    config.maxHeight = 160;
    config.useExactDimensions = true;
    config.monochromeOutput = true;
    config.cachePath = cachePath;
    return config;
  }

  std::vector<uint8_t> readFile(const fs::path& p) const {
    std::ifstream in(p.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

TEST_F(StoredEntryFixture, StoredRangePointsAtTheEntryBytes) {
  Epub epub(storedBook, cacheDir);
  uint32_t offset = 0;
  uint32_t size = 0;
  ASSERT_TRUE(epub.getStoredItemRange(kEntry, &offset, &size));
  EXPECT_EQ(size, pngBytes.size());

  std::ifstream in(storedBook, std::ios::binary);
  in.seekg(offset);
  std::vector<uint8_t> raw(size);
  in.read(reinterpret_cast<char*>(raw.data()), size);
  EXPECT_EQ(raw, pngBytes) << "the range must be the file, not a header or a neighbour";
}

// The guard that keeps this from ever being wrong: a deflated entry has no such range.
TEST_F(StoredEntryFixture, DeflatedEntryReportsNoRange) {
  Epub epub(kDeflatedBook, cacheDir);
  uint32_t offset = 0;
  uint32_t size = 0;
  EXPECT_FALSE(epub.getStoredItemRange(kEntry, &offset, &size));
}

TEST_F(StoredEntryFixture, MissingEntryReportsNoRange) {
  Epub epub(storedBook, cacheDir);
  uint32_t offset = 0;
  uint32_t size = 0;
  EXPECT_FALSE(epub.getStoredItemRange("OEBPS/images/not_here.png", &offset, &size));
}

// The contract: reading the archive in place is indistinguishable from reading a copy of it.
TEST_F(StoredEntryFixture, InPlaceDecodeMatchesDecodingTheExtractedCopy) {
  const std::string standalone = (work / "standalone.png").string();
  {
    std::ofstream out(standalone, std::ios::binary);
    out.write(reinterpret_cast<const char*>(pngBytes.data()), static_cast<std::streamsize>(pngBytes.size()));
  }
  PngToFramebufferConverter converter;
  ASSERT_TRUE(converter.decodeToFramebuffer(standalone, renderer, configFor((work / "from_file.pxc").string())));

  Epub epub(storedBook, cacheDir);
  uint32_t offset = 0;
  uint32_t size = 0;
  ASSERT_TRUE(epub.getStoredItemRange(kEntry, &offset, &size));
  FsFile archive;
  ASSERT_TRUE(Storage.openFileForRead("TST", storedBook, archive));
  ASSERT_TRUE(archive.seekSet(offset));
  EXPECT_TRUE(PngToFramebufferConverter::decodeOpenFile(archive, kEntry, renderer,
                                                        configFor((work / "in_place.pxc").string())));
  archive.close();

  const auto fromFile = readFile(work / "from_file.pxc");
  ASSERT_FALSE(fromFile.empty());
  EXPECT_EQ(readFile(work / "in_place.pxc"), fromFile);
}

// End to end through the caller that decides: the image must render and cache with the extracted
// file never being created at all. That absence IS the optimisation.
TEST_F(StoredEntryFixture, ImageBlockSkipsExtractionEntirely) {
  const fs::path target = work / "img_lazy.png";
  ASSERT_FALSE(fs::exists(target));

  ImageBlock block(target.string(), 120, 160, "", storedBook, kEntry);
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true, /*alsoCacheOtherVariant=*/true);

  EXPECT_TRUE(block.hasPixelCache()) << "the BW cache should have been written";
  EXPECT_TRUE(block.hasGrayscaleCache()) << "and the companion alongside it, from the same pass";
  EXPECT_FALSE(fs::exists(target)) << "the whole point: no copy of the image on the card";
}

// A book that deflates the entry still works -- it just extracts, exactly as before.
TEST_F(StoredEntryFixture, DeflatedEntryStillRendersViaExtraction) {
  const fs::path target = work / "img_deflated.png";
  ImageBlock block(target.string(), 120, 160, "", kDeflatedBook, kEntry);
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);

  EXPECT_TRUE(block.hasPixelCache());
  EXPECT_TRUE(fs::exists(target)) << "the fallback path must still extract";
}

}  // namespace
