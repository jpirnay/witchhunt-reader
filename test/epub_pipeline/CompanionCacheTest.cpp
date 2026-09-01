// One inflate, two pixel caches (RenderConfig::companionCachePath).
//
// The reader needs two .pxc per image — 1-bit Atkinson for the BW plane, 4-level Bayer for the
// grayscale planes — and used to decode the PNG twice to get them. On an X4 cover that second
// inflate measured 5.72 s of the 14.93 s an uncached image page cost.
//
// The merge is only ever safe as a PURE OPTIMISATION: the bytes it writes must be
// indistinguishable from the two separate decodes it replaces. That is the whole contract, and
// it is what these tests pin. It is easy to break in ways nothing else would notice — the two
// ditherers share a source sample now, so a stray tone application, a diffusion-state update
// skipped for an off-screen column, or a row advanced on one sink and not the other would all
// still produce a plausible-looking image while silently changing every cached pixel.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_png_images.epub";
const char* kEntry = "OEBPS/images/scaling_test.png";

struct CompanionCacheFixture : testing::Test {
  fs::path work;
  std::string png;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("png_companion_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    const std::string cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
    png = (work / "source.png").string();
    Epub epub(kBook, cacheDir);
    ASSERT_TRUE(epub.extractItemToFile(kEntry, png, nullptr));
  }
  void TearDown() override { fs::remove_all(work); }

  // Decode once. `companion`, when set, asks for the other variant in the same pass.
  bool decode(const char* primaryName, bool monochrome, const char* companionName = nullptr) {
    RenderConfig config;
    config.x = 0;
    config.y = 0;
    config.maxWidth = 120;
    config.maxHeight = 160;
    config.useExactDimensions = true;
    config.monochromeOutput = monochrome;
    config.cachePath = (work / primaryName).string();
    if (companionName) config.companionCachePath = (work / companionName).string();
    PngToFramebufferConverter converter;
    return converter.decodeToFramebuffer(png, renderer, config);
  }

  std::vector<uint8_t> read(const char* name) const {
    std::ifstream in((work / name).string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

// The contract. Two separate decodes vs one merged decode: same four files, byte for byte.
TEST_F(CompanionCacheFixture, MergedPassMatchesTwoSeparateDecodes) {
  ASSERT_TRUE(decode("sep_bw.pxc", /*monochrome=*/true));
  ASSERT_TRUE(decode("sep_grey.pxc", /*monochrome=*/false));
  ASSERT_TRUE(decode("merged_bw.pxc", /*monochrome=*/true, "merged_grey.pxc"));

  const auto sepBw = read("sep_bw.pxc");
  const auto sepGrey = read("sep_grey.pxc");
  ASSERT_FALSE(sepBw.empty());
  ASSERT_FALSE(sepGrey.empty());

  EXPECT_EQ(read("merged_bw.pxc"), sepBw) << "the drawn variant must not change";
  EXPECT_EQ(read("merged_grey.pxc"), sepGrey) << "the companion must equal its own separate decode";
}

// Same guarantee with the roles swapped: a grayscale primary carries a 1-bit companion, which is
// the direction that actually allocates a second (Atkinson) ditherer.
TEST_F(CompanionCacheFixture, CompanionWorksWithGrayscalePrimary) {
  ASSERT_TRUE(decode("sep_bw.pxc", /*monochrome=*/true));
  ASSERT_TRUE(decode("sep_grey.pxc", /*monochrome=*/false));
  ASSERT_TRUE(decode("merged_grey.pxc", /*monochrome=*/false, "merged_bw.pxc"));

  EXPECT_EQ(read("merged_grey.pxc"), read("sep_grey.pxc"));
  EXPECT_EQ(read("merged_bw.pxc"), read("sep_bw.pxc"));
}

// The two variants are genuinely different renditions. Without this, both assertions above would
// still pass if the companion simply copied the primary — the exact bug they exist to catch.
TEST_F(CompanionCacheFixture, TheTwoVariantsAreNotTheSameBytes) {
  ASSERT_TRUE(decode("bw.pxc", /*monochrome=*/true, "grey.pxc"));
  const auto bw = read("bw.pxc");
  const auto grey = read("grey.pxc");
  ASSERT_FALSE(bw.empty());
  ASSERT_EQ(bw.size(), grey.size()) << "same 2bpp format and geometry";
  EXPECT_NE(bw, grey);
}

// A companion path that cannot be opened must cost the caller nothing but the companion: the
// primary cache and the return value are unaffected, so warmImageCaches() falls back to the
// second decode exactly as it did before.
TEST_F(CompanionCacheFixture, UnwritableCompanionLeavesThePrimaryIntact) {
  ASSERT_TRUE(decode("sep_bw.pxc", /*monochrome=*/true));

  // A directory sitting where the companion file would go: openFileForWrite cannot succeed,
  // whatever the platform. (A merely missing parent directory is not enough — the host storage
  // shim creates one.)
  const fs::path blocked = work / "companion.pxc";
  fs::create_directories(blocked);

  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = 120;
  config.maxHeight = 160;
  config.useExactDimensions = true;
  config.monochromeOutput = true;
  config.cachePath = (work / "primary.pxc").string();
  config.companionCachePath = blocked.string();
  PngToFramebufferConverter converter;
  EXPECT_TRUE(converter.decodeToFramebuffer(png, renderer, config)) << "a lost companion is not a failed decode";

  EXPECT_EQ(read("primary.pxc"), read("sep_bw.pxc"));
  EXPECT_TRUE(fs::is_directory(blocked)) << "nothing should have been written over it";
}

}  // namespace
