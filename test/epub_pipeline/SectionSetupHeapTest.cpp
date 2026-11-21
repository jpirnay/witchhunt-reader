// The build's setup asked the heap for two things with a throwing allocation: the chapter parser object
// (std::make_unique) and the spine's TOC anchors (a vector of strings, as long as the book makes it: a book that is
// one XHTML file puts every TOC entry in one spine). Under -fno-exceptions a refusal aborts, and the next open of the
// chapter built into the same abort. Now the parser is made with nothrow new, the anchors are one packed pool taken
// from the build's region when the board builds in PSRAM and it has room (a quarter of it at most) and from the heap
// otherwise (the C3s: the pool would crowd the lent region's peaks), and a refusal of either ends the build before it
// starts. SECTION_SETUP_FAULT_INJECTION refuses them and lets the host stand in for a PSRAM board (Section.h).
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

// 150 TOC entries, all in the book's one spine item, ids of 20 characters: a 3,150 byte pool.
const char* const kBook = CORPUS_DIR "/test_toc_one_file.epub";
constexpr size_t kPoolBytes = 150 * (20 + 1);

struct SectionSetupHeap : testing::Test {
  fs::path work;
  std::string cacheDir;

  void SetUp() override {
    work = fs::temp_directory_path() / ("section_setup_" + std::to_string(static_cast<long>(getpid())) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
    sectionSetupFaults() = SectionSetupFaults{};
  }
  void TearDown() override {
    sectionSetupFaults() = SectionSetupFaults{};
    Section::setOwnedBuildRegionBytes(0);
    fs::remove_all(work);
  }

  struct Built {
    bool ok = false;
    uint16_t pages = 0;
    std::vector<char> bytes;  // the section file
  };

  Built build(BuildArena* lent = nullptr) {
    Built out;
    auto epub = std::make_shared<Epub>(kBook, cacheDir);
    EXPECT_TRUE(epub->load(true));
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = TextScale();
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    section.clearCache();
    section.setExternalBuildScratch(lent);
    out.ok = section.createSectionFile(params, {}, /*skipEviction=*/true);
    out.pages = section.pageCount;
    for (const auto& entry : fs::recursive_directory_iterator(cacheDir)) {
      const std::string path = entry.path().generic_string();
      if (!entry.is_regular_file() || path.find("/spines/") == std::string::npos) continue;
      if (entry.path().filename().string().rfind("0_", 0) != 0) continue;
      std::ifstream in(path, std::ios::binary);
      out.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    section.clearCache();
    return out;
  }
};

using Source = SectionSetupFaults::PoolSource;

}  // namespace

TEST_F(SectionSetupHeap, ARefusedParserDoesNotStartTheBuild) {
  sectionSetupFaults().refuseParser = true;
  const Built refused = build();
  EXPECT_FALSE(refused.ok) << "no parser: the build does not start";
  EXPECT_TRUE(refused.bytes.empty()) << "and leaves no section file behind";
  sectionSetupFaults().refuseParser = false;
  const Built after = build();
  EXPECT_TRUE(after.ok) << "the next build is as any other";
  EXPECT_GT(after.pages, 100);
}

TEST_F(SectionSetupHeap, ARefusedParserNeverAborts) {
  EXPECT_EXIT(
      {
        sectionSetupFaults().refuseParser = true;
        const Built refused = build();
        std::exit(refused.ok ? 1 : 0);
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(SectionSetupHeap, ThePoolIsTakenFromTheArenaWhenTheBoardBuildsInPsramAndItHasRoom) {
  sectionSetupFaults().poolInRegion = true;
  Section::setOwnedBuildRegionBytes(WordShapeCache::kLargeArenaBytes);
  const Built built = build();
  ASSERT_TRUE(built.ok);
  EXPECT_EQ(Source::Arena, sectionSetupFaults().lastPoolSource);
  EXPECT_EQ(kPoolBytes, sectionSetupFaults().lastPoolBytes);
}

TEST_F(SectionSetupHeap, ThePoolIsTakenFromTheHeapWithoutARegionOrOverAQuarterOfIt) {
  sectionSetupFaults().poolInRegion = true;
  const Built none = build();  // the 10 KB parse arena has no room for it
  ASSERT_TRUE(none.ok);
  EXPECT_EQ(Source::Heap, sectionSetupFaults().lastPoolSource);
  EXPECT_EQ(kPoolBytes, sectionSetupFaults().lastPoolBytes);

  sectionSetupFaults() = SectionSetupFaults{};
  Section::setOwnedBuildRegionBytes(8 * 1024);  // a quarter is 2,048 B: the 3,150 B pool is over it
  (void)build();
  EXPECT_EQ(Source::Heap, sectionSetupFaults().lastPoolSource);
}

// A C3 board: its lent region has no room for a build-long block beside the extraction ring and the image walk, so
// the pool is one heap block whatever the region holds, and the ring stays in the region (nothing it asked was
// refused).
TEST_F(SectionSetupHeap, WithoutPsramThePoolIsAHeapBlockAndTheRegionKeepsItsRoom) {
  BuildArena lent(48000);  // the X4's lent region
  ASSERT_TRUE(lent.valid());
  const Built built = build(&lent);
  ASSERT_TRUE(built.ok);
  EXPECT_EQ(Source::Heap, sectionSetupFaults().lastPoolSource);
  EXPECT_EQ(0u, lent.failedAllocSize()) << "the region refused nothing the build asked of it";
  EXPECT_EQ(0u, lent.used());
  EXPECT_EQ(0u, lent.releaseFailures());
}

TEST_F(SectionSetupHeap, ARefusedArenaFallsBackToTheHeap) {
  sectionSetupFaults().poolInRegion = true;
  Section::setOwnedBuildRegionBytes(WordShapeCache::kLargeArenaBytes);
  sectionSetupFaults().refuseTocPoolArena = true;
  const Built built = build();
  ASSERT_TRUE(built.ok);
  EXPECT_EQ(Source::Heap, sectionSetupFaults().lastPoolSource);
}

TEST_F(SectionSetupHeap, ARefusalOfBothDoesNotStartTheBuild) {
  sectionSetupFaults().poolInRegion = true;
  Section::setOwnedBuildRegionBytes(WordShapeCache::kLargeArenaBytes);
  sectionSetupFaults().refuseTocPoolArena = true;
  sectionSetupFaults().refuseTocPoolHeap = true;
  const Built refused = build();
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(refused.bytes.empty()) << "no section file is left behind";
  EXPECT_EQ(Source::None, sectionSetupFaults().lastPoolSource);
  // And with no region, where the heap is the only source.
  Section::setOwnedBuildRegionBytes(0);
  EXPECT_FALSE(build().ok);
}

TEST_F(SectionSetupHeap, WhereThePoolLivesNeverChangesTheLayout) {
  const Built heap = build();
  ASSERT_TRUE(heap.ok);
  ASSERT_FALSE(heap.bytes.empty());
  ASSERT_EQ(Source::Heap, sectionSetupFaults().lastPoolSource);
  Section::setOwnedBuildRegionBytes(WordShapeCache::kLargeArenaBytes);
  sectionSetupFaults().poolInRegion = true;
  const Built arena = build();
  ASSERT_TRUE(arena.ok);
  ASSERT_EQ(Source::Arena, sectionSetupFaults().lastPoolSource);
  EXPECT_EQ(heap.pages, arena.pages);
  EXPECT_EQ(150, heap.pages) << "every TOC anchor starts a page, and the two ids that only start like one do not";
  EXPECT_TRUE(heap.bytes == arena.bytes);
}

// The pool is a block of the lent arena like the others: the build gives back all it took, in order.
TEST_F(SectionSetupHeap, ALentArenaTakesThePoolAndGetsItBack) {
  sectionSetupFaults().poolInRegion = true;
  BuildArena lent(52 * 1024);  // a lent arena
  ASSERT_TRUE(lent.valid());
  const Built built = build(&lent);
  ASSERT_TRUE(built.ok);
  EXPECT_EQ(Source::Arena, sectionSetupFaults().lastPoolSource);
  EXPECT_EQ(0u, lent.used()) << "the build rewinds everything it took";
  EXPECT_EQ(0u, lent.releaseFailures()) << "every release was in order";
}
