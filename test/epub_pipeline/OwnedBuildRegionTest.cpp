// A board with PSRAM gives a build with nothing lent a region of its own (Section::setOwnedBuildRegionBytes), which
// the build uses as it would the lent one: the X4 Pro built the chapter it opened on the 10 KB parse arena at 75 ms a
// page, against 32 ms a page in the lent PSRAM arena. Where a build keeps its scratch never changes what it lays out.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Section.h"
#include "Epub/WordShapeCache.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/../fixtures/moby-dick.epub";
const char* kChapter = "2701-h-3.htm";

struct OwnedBuildRegionFixture : testing::Test {
  fs::path work;
  std::string cacheDir;
  void SetUp() override {
    work = fs::temp_directory_path() / (std::string("epub_ownedregion_") + std::to_string(::getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
  }
  void TearDown() override {
    Section::setOwnedBuildRegionBytes(0);
    fs::remove_all(work);
  }

  struct Built {
    std::vector<char> bytes;   // the section file
    size_t arenaCapacity = 0;  // of the arena the build ran in, seen mid-parse
    int pages = 0;
  };

  // Builds the chapter with the book's CSS on, `lent` as its scratch (nullptr: none), and returns its section file.
  Built build(BuildArena* lent) {
    Built out;
    auto epub = std::make_shared<Epub>(kBook, cacheDir);
    EXPECT_TRUE(epub->load(true));
    int spine = -1;
    for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
      if (epub->getSpineItem(i).href.find(kChapter) != std::string::npos) spine = i;
    }
    EXPECT_GE(spine, 0);
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = TextScale();
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, spine, renderer);
    section.clearCache();
    section.setExternalBuildScratch(lent);
    EXPECT_TRUE(section.createSectionFile(
        params,
        [&](int) {
          if (const BuildArena* arena = section.activeBuildArena()) out.arenaCapacity = arena->capacity();
        },
        /*skipEviction=*/true));
    out.pages = section.pageCount;
    // The one section file the build wrote.
    for (const auto& entry : fs::recursive_directory_iterator(cacheDir)) {
      const std::string path = entry.path().string();
      if (!entry.is_regular_file() || path.find("/spines/") == std::string::npos) continue;
      if (entry.path().filename().string().rfind(std::to_string(spine) + "_", 0) != 0) continue;
      std::ifstream in(path, std::ios::binary);
      out.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    section.clearCache();
    return out;
  }
};

TEST_F(OwnedBuildRegionFixture, ABuildWithNothingLentTakesTheRegionAndLaysOutTheSame) {
  const Built small = build(nullptr);
  ASSERT_GT(small.pages, 10) << "the chapter must be long enough to report progress mid-parse";
  ASSERT_FALSE(small.bytes.empty());
  EXPECT_LT(small.arenaCapacity, size_t{16 * 1024}) << "without a region the build keeps the 10 KB parse arena";

  Section::setOwnedBuildRegionBytes(WordShapeCache::kLargeArenaBytes);
  const Built region = build(nullptr);
  EXPECT_EQ(region.arenaCapacity, WordShapeCache::kLargeArenaBytes) << "the build runs in a region of its own";
  EXPECT_EQ(region.pages, small.pages);
  EXPECT_TRUE(region.bytes == small.bytes) << "the region changes where the build keeps its scratch, not the layout";
}

TEST_F(OwnedBuildRegionFixture, ALentArenaStillWinsOverTheRegion) {
  Section::setOwnedBuildRegionBytes(WordShapeCache::kLargeArenaBytes);
  BuildArena lent(52 * 1024);  // the X3's borrowed framebuffer
  ASSERT_TRUE(lent.valid());
  const Built built = build(&lent);
  EXPECT_EQ(built.arenaCapacity, lent.capacity()) << "a lent arena is used as before";

  Section::setOwnedBuildRegionBytes(0);
  const Built small = build(nullptr);
  EXPECT_TRUE(built.bytes == small.bytes);
}

}  // namespace
