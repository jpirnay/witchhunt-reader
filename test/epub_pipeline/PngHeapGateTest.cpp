// The PNG decode's memory gate (PngToFramebufferConverter::decodeHeapNeed). A small image is charged what it takes: its
// ring is its whole inflated stream, a few hundred bytes for an icon. A large one keeps the worst-case floors: 36 KB,
// or 16 KB when the scratch arena can serve a full ring and two 4 KB rows.
//
// The X3's case (2026-11-11): beside a section build the image lane works in the build's free tail, about 35 KB, which
// cannot serve the floors' 40 KB worst case, and the heap had 35.2 KB of the 36 KB asked. Every icon of the page on
// screen was refused, 26 times in 3.5 s, and drawn only once the build was done.
#include <BuildArena.h>
#include <PngStreamDecoder.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "Epub.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const char* kInlineBook = CORPUS_DIR "/test_inline_images.epub";
const char* kStar = "OEBPS/images/star.png";  // 18 by 18, 8-bit gray: a 512 B ring
const char* kPngBook = CORPUS_DIR "/test_png_images.epub";
const char* kScaling = "OEBPS/images/scaling_test.png";  // 1200 by 1500: a full 32 KB ring

// PngToFramebufferConverter.cpp's floors, and the arena worst case that earns the lower one.
constexpr size_t kFloor = 36 * 1024;
constexpr size_t kFloorWithArena = 16 * 1024;
constexpr size_t kArenaWorstCase = 32 * 1024 + 2 * 4096;
// The X3's free heap and build tail at the refusals (s6_inline.log: "35208 free, need 36864"; the lane's tail ~35 KB).
constexpr uint32_t kX3FreeHeap = 35208;
constexpr size_t kX3BuildTail = 35 * 1024;

struct PngHeapGateFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;
  std::string star;
  std::string scaling;
  uint32_t savedHeap = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / (std::string("png_heap_gate_") + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work / "cache");
    savedHeap = ESP.getFreeHeap();
    star = (work / "star.png").string();
    scaling = (work / "scaling.png").string();
    ASSERT_TRUE(Epub(kInlineBook, (work / "cache").string()).extractItemToFile(kStar, star, nullptr));
    ASSERT_TRUE(Epub(kPngBook, (work / "cache").string()).extractItemToFile(kScaling, scaling, nullptr));
  }
  void TearDown() override {
    ESP.setFreeHeap(savedHeap);
    fs::remove_all(work);
  }

  // As the image lane asks: the 1-bit cache, and the 4-level one beside it from the same inflate.
  RenderConfig config(const std::string& name, const int width, const int height) const {
    RenderConfig c;
    c.maxWidth = width;
    c.maxHeight = height;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = (work / (name + ".1bit.pxc")).string();
    c.companionCachePath = (work / (name + ".bayer.pxc")).string();
    return c;
  }

  // The gate for `png`, read from an open file whose position it must leave where it was.
  static size_t need(const std::string& png, const RenderConfig& c) {
    FsFile file;
    EXPECT_TRUE(Storage.openFileForRead("TEST", png, file));
    const size_t bytes = PngToFramebufferConverter::decodeHeapNeed(file, c);
    EXPECT_EQ(0u, file.position()) << "the header is read and the position put back";
    file.close();
    return bytes;
  }

  bool decodeWith(const uint32_t freeHeap, const std::string& png, const RenderConfig& c) {
    ESP.setFreeHeap(freeHeap);
    PngToFramebufferConverter converter;
    const bool ok = converter.decodeToFramebuffer(png, renderer, c);
    ESP.setFreeHeap(savedHeap);
    return ok;
  }

  static PngStreamDecoder::Scratch scratchOf(const uint32_t width, const uint32_t height) {
    PngStreamDecoder::Info info;
    info.width = width;
    info.height = height;
    info.bitDepth = 8;
    info.colorType = 0;  // gray
    return PngStreamDecoder::scratchFor(info);
  }
};

TEST_F(PngHeapGateFixture, TheScratchIsTheImagesWholeInflatedStreamAtMostTheWindow) {
  const auto icon = scratchOf(18, 18);
  EXPECT_EQ(18u, icon.rowBytes);
  EXPECT_EQ(512u, icon.ringBytes) << "18 rows of 19 bytes, raised to the ring's 512 B floor";
  EXPECT_FALSE(icon.wholeWindow);

  const auto formula = scratchOf(280, 28);
  EXPECT_EQ(28u * 281, formula.ringBytes);
  EXPECT_FALSE(formula.wholeWindow);

  const auto picture = scratchOf(1200, 1500);
  EXPECT_EQ(32u * 1024, picture.ringBytes);
  EXPECT_TRUE(picture.wholeWindow);

  PngStreamDecoder::Info bad;
  bad.width = 18;
  bad.height = 18;
  bad.bitDepth = 3;  // no colour type takes it
  EXPECT_EQ(0u, PngStreamDecoder::scratchFor(bad).rowBytes) << "a header begin() refuses";
}

// Big PNGs gate exactly as they did: the floor without an arena, the lower floor with one that serves the worst case,
// and the full floor again beside a build, whose tail serves this image's ring but not the worst case.
TEST_F(PngHeapGateFixture, ALargePngKeepsTheWorstCaseFloors) {
  const RenderConfig c = config("scaling", 400, 500);
  EXPECT_EQ(kFloor, need(scaling, c));
  {
    BuildArena arena(kArenaWorstCase + 64);
    const image_scratch::ScopedArena scope(&arena);
    EXPECT_EQ(kFloorWithArena, need(scaling, c));
  }
  {
    BuildArena tail(kX3BuildTail);
    const image_scratch::ScopedArena scope(&tail);
    ASSERT_FALSE(image_scratch::canServe(kArenaWorstCase));
    ASSERT_TRUE(image_scratch::canServe(32 * 1024 + 2 * 1200 + 2 * alignof(std::max_align_t)));
    EXPECT_EQ(kFloor, need(scaling, c)) << "a ring the tail could hold changes nothing for a large image";
  }
}

// A small one is charged its blocks, the rest of the decode and the headroom: well under either floor, more than the
// headroom alone, and its blocks less when the arena serves them.
TEST_F(PngHeapGateFixture, ATinyPngIsChargedWhatItTakes) {
  const RenderConfig c = config("star", 18, 18);
  const size_t onHeap = need(star, c);
  EXPECT_LT(onHeap, kFloorWithArena);
  EXPECT_GT(onHeap, 8u * 1024) << "the headroom every decode leaves";

  BuildArena tail(kX3BuildTail);
  const image_scratch::ScopedArena scope(&tail);
  const size_t besideBuild = need(star, c);
  const auto scratch = scratchOf(18, 18);
  EXPECT_EQ(onHeap - besideBuild, 2 * scratch.rowBytes + scratch.ringBytes) << "the arena serves the rows and ring";
}

// The X3's moment: the build's tail and its free heap. The icon decodes, both variants cached; the large picture is
// still refused there, as before, and waits for the lane after the build.
TEST_F(PngHeapGateFixture, BesideABuildAnIconDecodesWhereTheFloorRefusedIt) {
  BuildArena tail(kX3BuildTail);
  const image_scratch::ScopedArena scope(&tail);

  const RenderConfig icon = config("star", 18, 18);
  uint32_t shortBefore = image_scratch::heapShortCount();
  EXPECT_TRUE(decodeWith(kX3FreeHeap, star, icon));
  EXPECT_EQ(shortBefore, image_scratch::heapShortCount());
  EXPECT_TRUE(fs::exists(icon.cachePath));
  EXPECT_TRUE(fs::exists(icon.companionCachePath));

  const RenderConfig picture = config("scaling", 400, 500);
  shortBefore = image_scratch::heapShortCount();
  EXPECT_FALSE(decodeWith(kX3FreeHeap, scaling, picture));
  EXPECT_NE(shortBefore, image_scratch::heapShortCount()) << "refused for memory: the ladder's signal";
  EXPECT_FALSE(fs::exists(picture.cachePath));
}

// The gate is the decode's: a byte short is refused and counted short of memory, the need itself decodes.
TEST_F(PngHeapGateFixture, TheGateIsWhereTheDecodeStarts) {
  for (const bool withArena : {false, true}) {
    SCOPED_TRACE(withArena ? "beside a build" : "on the heap");
    BuildArena tail(kX3BuildTail);
    const image_scratch::ScopedArena scope(withArena ? &tail : nullptr);
    const RenderConfig c = config(withArena ? "arena" : "heap", 18, 18);
    const auto threshold = static_cast<uint32_t>(need(star, c));

    const uint32_t shortBefore = image_scratch::heapShortCount();
    EXPECT_FALSE(decodeWith(threshold - 1, star, c));
    EXPECT_EQ(shortBefore + 1, image_scratch::heapShortCount());
    EXPECT_FALSE(fs::exists(c.cachePath));

    EXPECT_TRUE(decodeWith(threshold, star, c));
    EXPECT_TRUE(fs::exists(c.cachePath));
  }
}

// A header the gate cannot read is charged the worst case; begin() then says what is wrong with it.
TEST_F(PngHeapGateFixture, AnUnreadableHeaderIsChargedTheWorstCase) {
  const std::string broken = (work / "broken.png").string();
  {
    std::ofstream out(broken, std::ios::binary);
    out << "\x89PNG\r\n\x1a\n not a png at all";
  }
  EXPECT_EQ(kFloor, need(broken, config("broken", 18, 18)));
}

}  // namespace
