// A background section build runs in the borrowed secondary framebuffer with ~46 KB of heap. The
// SAX parser's ~10 KB state used to sit on that heap for the whole parse, with the build's other
// long-lived buffers, leaving ~16 KB for layout -- which fragmented to a low-heap abort partway
// through an image-heavy chapter (X3 2026-09-25: page 67 of 180, then a 10 s blocking rebuild).
// With a lent arena the state lives there instead.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SaxParser/SaxParser.h"

namespace fs = std::filesystem;

namespace {

// A long chapter, so the parse reports progress partway through: that callback is where the
// arena is sampled, after the extraction ring is gone and while the parser state is live.
const char* kBook = CORPUS_DIR "/../fixtures/moby-dick.epub";
const char* kChapter = "2701-h-3.htm";

struct SaxStateInArenaFixture : testing::Test {
  fs::path work;
  std::string cacheDir;
  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_saxarena_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
  }
  void TearDown() override { fs::remove_all(work); }

  // Builds the long chapter with `arena` lent as the build scratch; returns the most arena in
  // use at any progress tick during the parse.
  size_t arenaUsedDuringParse(BuildArena* arena) {
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
    params.lineCompression = 1.0f;
    GfxRenderer renderer;
    Section section(epub, spine, renderer);
    section.clearCache();
    if (arena) section.setExternalBuildScratch(arena);
    size_t maxUsed = 0;
    int ticks = 0;
    EXPECT_TRUE(section.createSectionFile(
        params,
        [&](int) {
          ++ticks;
          if (arena) maxUsed = std::max(maxUsed, arena->used());
        },
        /*skipEviction=*/true));
    EXPECT_GT(section.pageCount, 10);
    EXPECT_GT(ticks, 1) << "the chapter must be long enough to report progress mid-parse";
    return maxUsed;
  }
};

TEST_F(SaxStateInArenaFixture, ALentArenaHoldsTheSaxParserStateDuringTheParse) {
  BuildArena arena(52 * 1024);  // the borrowed secondary framebuffer's size
  ASSERT_TRUE(arena.valid());
  const size_t used = arenaUsedDuringParse(&arena);
  EXPECT_GE(used, SaxParser::stateBytes()) << "the parser state must come out of the lent arena, not the heap";
  // The state is a plain allocation inside the feed-chunk block's scope; the section releases
  // that block only after finalize() has last used the parser, and every block release in the
  // build must have been in LIFO order for the cursor to come back to 0 (memory audit
  // 2026-09, F2a/F2b).
  EXPECT_EQ(arena.used(), 0u) << "the build rewinds everything it took";
  EXPECT_EQ(arena.releaseFailures(), 0u) << "every block release was in order";
}

TEST_F(SaxStateInArenaFixture, NoArenaStillBuilds) { EXPECT_EQ(arenaUsedDuringParse(nullptr), 0u); }

}  // namespace
