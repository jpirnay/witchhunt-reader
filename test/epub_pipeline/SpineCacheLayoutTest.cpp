// Per-spine caches live in buckets of Epub::SPINE_CACHE_BUCKET_SIZE spine items
// (<cache>/spines/<spine / 32>/) instead of one flat sections/ directory: a FAT open scans its
// directory linearly, and the flat one held two files per spine item, 3400+ on a 1700-spine book.
// These pin the layout and the two directory walks that used to run over a capped name list.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSpines = 40;  // two buckets

class SpineCacheLayout : public testing::Test {
 protected:
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("spine_cache_layout_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // kSpines one-paragraph chapters; chapter i carries printed page label "p<i>".
  std::shared_ptr<Epub> loadBook() {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    std::string manifest;
    std::string spine;
    for (int i = 0; i < kSpines; ++i) {
      const std::string id = "c" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>\n";
      zip.add(id + ".xhtml",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
              "<head><title>C</title></head><body><span role=\"doc-pagebreak\" aria-label=\"p" +
                  std::to_string(i) + "\"/><p>Chapter " + std::to_string(i) + " text.</p></body></html>\n");
    }
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">buckets</dc:identifier>"
            "<dc:title>Buckets</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n" +
                manifest + "</manifest>\n<spine>\n" + spine + "</spine>\n</package>\n");
    const std::string path = (work / "buckets.epub").string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    return epub;
  }

  static Section::BuildParams params(const uint16_t viewportWidth = 480) {
    Section::BuildParams p;
    p.viewportWidth = viewportWidth;
    p.viewportHeight = 800;
    p.lineCompression = TextScale();
    p.fontSizeNormalization = false;
    return p;
  }

  static int countVariants(const std::string& dir, const int spineIndex) {
    const std::string prefix = std::to_string(spineIndex) + "_";
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
      const std::string name = e.path().filename().string();
      if (name.rfind(prefix, 0) == 0 && e.path().extension() == ".bin") ++n;
    }
    return n;
  }
};

}  // namespace

TEST_F(SpineCacheLayout, BucketsHoldThirtyTwoSpineItemsEach) {
  EXPECT_EQ("/c/spines/0", Epub::spineCacheDir("/c", 0));
  EXPECT_EQ("/c/spines/0", Epub::spineCacheDir("/c", 31));
  EXPECT_EQ("/c/spines/1", Epub::spineCacheDir("/c", 32));
  EXPECT_EQ("/c/spines/54", Epub::spineCacheDir("/c", 1731));  // the last of a 1732-item book
}

TEST_F(SpineCacheLayout, ASectionBuildWritesIntoItsSpinesBucket) {
  GfxRenderer renderer;
  auto epub = loadBook();
  for (const int spine : {3, 35}) {
    Section section(epub, spine, renderer);
    ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true)) << "spine " << spine;
  }
  const std::string bucket0 = Epub::spineCacheDir(epub->getCachePath(), 3);
  const std::string bucket1 = Epub::spineCacheDir(epub->getCachePath(), 35);
  EXPECT_EQ(1, countVariants(bucket0, 3));
  EXPECT_EQ(1, countVariants(bucket1, 35));
  EXPECT_TRUE(fs::exists(Section::sectionHtmlCachePath(epub->getCachePath(), 35)));
  EXPECT_EQ(bucket1, fs::path(Section::sectionHtmlCachePath(epub->getCachePath(), 35)).parent_path().string());
  EXPECT_FALSE(fs::exists(epub->getCachePath() + "/sections")) << "nothing may use the old flat directory";

  Section reloaded(epub, 35, renderer);
  EXPECT_TRUE(reloaded.loadSectionFile(params()));
}

TEST_F(SpineCacheLayout, EvictionTrimsOnlyThatSpinesVariants) {
  GfxRenderer renderer;
  auto epub = loadBook();
  const std::string bucket = Epub::spineCacheDir(epub->getCachePath(), 35);
  // Seven layout variants of spine 35 and one of its bucket neighbour 33.
  for (uint16_t width = 400; width < 470; width += 10) {
    Section section(epub, 35, renderer);
    ASSERT_TRUE(section.createSectionFile(params(width), {}, /*skipEviction=*/true));
  }
  {
    Section neighbour(epub, 33, renderer);
    ASSERT_TRUE(neighbour.createSectionFile(params(), {}, /*skipEviction=*/true));
  }
  ASSERT_EQ(7, countVariants(bucket, 35));

  // A build that evicts: the five newest stay (dates are not modelled on the host, so which five
  // is not checked here), then the build adds its own.
  Section evicting(epub, 35, renderer);
  ASSERT_TRUE(evicting.createSectionFile(params(470), {}, /*skipEviction=*/false));
  EXPECT_EQ(6, countVariants(bucket, 35));
  EXPECT_EQ(1, countVariants(bucket, 33)) << "evicted a neighbour's cache";
}

// The sleep screen reads a printed-page label straight from a spine's cache, now by walking that
// spine's bucket. (The old scan took the first 50 names of the flat directory; on the device, where
// entries come in creation order, spine items past the first few dozen files never had a label.
// The host lists names sorted, so this pins the bucket walk rather than reproducing that.)
TEST_F(SpineCacheLayout, PrintedPageLabelIsFoundInALaterBucket) {
  GfxRenderer renderer;
  auto epub = loadBook();
  for (int spine = 0; spine < kSpines; ++spine) {
    Section section(epub, spine, renderer);
    ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true)) << "spine " << spine;
  }
  const auto label = Section::getPrintedPageLabelFromCache(epub->getCachePath(), 39, 0);
  ASSERT_TRUE(label.has_value());
  EXPECT_NE(std::string::npos, label->find("p39")) << *label;
  EXPECT_FALSE(Section::getPrintedPageLabelFromCache(epub->getCachePath(), 39, 5).has_value());
}

TEST_F(SpineCacheLayout, RemovingTheCachesAlsoClearsTheOldFlatDirectory) {
  GfxRenderer renderer;
  auto epub = loadBook();
  {
    Section section(epub, 35, renderer);
    ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
  }
  const std::string legacy = epub->getCachePath() + "/sections";
  fs::create_directories(legacy);
  {
    std::ofstream(legacy + "/35_00000000.bin") << "old";
  }

  epub->removeSpineCaches();
  EXPECT_FALSE(fs::exists(Epub::spineCacheRoot(epub->getCachePath())));
  EXPECT_FALSE(fs::exists(legacy));
}
