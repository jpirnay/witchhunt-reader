// The long-block split (ChapterHtmlSlimParser, more than 96 tokens in a block) must make progress. When one laid-out
// line holds more than 96 tokens the block stays above 96 after the split, and a split that fires on every following
// word lays the whole block out again: quadratic time and heap churn. test_shaped_arabic's stress paragraph (200 Allah
// words, each cut by a span into two glued tokens) at a 1,600 px viewport put 186 splits in the book before the fix.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "LogObserver.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

int gSplits = 0;
void countSplits(const char* /*level*/, const char* origin, const char* message) {
  if (std::string(origin) == "EHP" && std::string(message).find("Text block too long") != std::string::npos) ++gSplits;
}

TEST(LongBlockSplit, AWideLineOfManyTokensDoesNotSplitOnEveryWord) {
  const fs::path work = fs::temp_directory_path() / "epub_long_block_split";
  fs::remove_all(work);
  fs::create_directories(work);
  pipeline_harness::Profile profile;
  profile.viewportWidth = 1600;
  profile.shapingFontFile = "NotoNaskhArabic-Book-Regular.ttf";
  gSplits = 0;
  setLogObserver(countSplits);
  int pages = 0;
  const std::string epub = (fs::path(CORPUS_DIR) / "test_shaped_arabic.epub").string();
  const bool ok = pipeline_harness::forEachPage(epub, (work / "cache").string(), profile,
                                                [&](int, const Section&, const Page&, GfxRenderer&) { ++pages; });
  setLogObserver(nullptr);
  fs::remove_all(work);
  ASSERT_TRUE(ok);
  EXPECT_GT(pages, 0);
  std::printf("[ LongBlockSplit ] splits: %d\n", gSplits);
  EXPECT_LE(gSplits, 12);
}

}  // namespace
