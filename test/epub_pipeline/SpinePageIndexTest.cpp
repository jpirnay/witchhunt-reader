// SpinePageIndex keeps each spine item's page count on SD (`<book cache>/pages.bin`) so the reader
// can count a chapter split over several spine items (#325) without a per-spine table in RAM or an
// open per sibling section cache. Section records into it; the reader sums ranges of it.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "Epub/SpinePageIndex.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "spine_page_index_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams defaultParams() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  return p;
}

std::shared_ptr<Epub> loadTables(const std::string& tag) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_tables.epub", freshDir(tag));
  EXPECT_TRUE(epub->load(true));
  return epub;
}

// Everything recorded for the book's first spine item, under `p`. current = -1 puts every entry
// on the "after" side.
SpinePageIndex::Totals firstSpine(const Epub& epub, const Section::BuildParams& p) {
  return Section::indexedPageTotals(epub.getCachePath(), p, epub.getSpineItemsCount(), 0, 0, /*current=*/-1);
}

constexpr SpinePageIndex::Variant kVariant{0x12345678u, 7};

}  // namespace

TEST(SpinePageIndex, SumsTheRecordedEntriesOnEachSideOfTheCurrentSpine) {
  const std::string dir = freshDir("sums");
  SpinePageIndex::record(dir, kVariant, 1700, 10, 12, 24000);
  SpinePageIndex::record(dir, kVariant, 1700, 11, 1, 700);
  SpinePageIndex::record(dir, kVariant, 1700, 12, 30, 60000);  // the current spine: never summed
  SpinePageIndex::record(dir, kVariant, 1700, 14, 8, 16000);   // 13 never recorded

  const auto t = SpinePageIndex::sumRange(dir, kVariant, 1700, 10, 15, /*current=*/12);
  EXPECT_EQ(13u, t.pagesBefore);
  EXPECT_EQ(24700u, t.bytesBefore);
  EXPECT_EQ(2, t.filesBefore);
  EXPECT_EQ(8u, t.pagesAfter);
  EXPECT_EQ(16000u, t.bytesAfter);
  EXPECT_EQ(1, t.filesAfter);
}

TEST(SpinePageIndex, ReadsARangeLongerThanOneChunk) {
  const std::string dir = freshDir("long_range");
  for (int spine = 100; spine < 200; ++spine) SpinePageIndex::record(dir, kVariant, 1700, spine, 2, 1000);
  const auto t = SpinePageIndex::sumRange(dir, kVariant, 1700, 100, 199, /*current=*/150);
  EXPECT_EQ(100u, t.pagesBefore);
  EXPECT_EQ(50, t.filesBefore);
  EXPECT_EQ(98u, t.pagesAfter);
  EXPECT_EQ(49, t.filesAfter);
}

TEST(SpinePageIndex, ReRecordingOverwritesTheEntry) {
  const std::string dir = freshDir("overwrite");
  SpinePageIndex::record(dir, kVariant, 40, 3, 9, 18000);
  SpinePageIndex::record(dir, kVariant, 40, 3, 11, 18000);  // a clean rebuild of a degraded one
  EXPECT_EQ(11u, SpinePageIndex::sumRange(dir, kVariant, 40, 3, 3, -1).pagesAfter);
}

// A heading file the next item's layout takes in has no page of its own: a file of 0 pages that counts as known, so a
// chapter of it and its text is exact once the text's count is in. Stored as 0 pages, which older firmware reads as
// "not recorded", as it would have before the file was carried.
TEST(SpinePageIndex, ACarriedItemCountsAsAFileOfNoPages) {
  const std::string dir = freshDir("carried");
  SpinePageIndex::recordCarried(dir, kVariant, 40, 22);
  SpinePageIndex::record(dir, kVariant, 40, 23, 41, 22926);
  const auto t = SpinePageIndex::sumRange(dir, kVariant, 40, 22, 23, /*current=*/23);
  EXPECT_EQ(1, t.filesBefore);
  EXPECT_EQ(0u, t.pagesBefore);
  EXPECT_EQ(0u, t.bytesBefore);

  std::ifstream in(fs::path(dir) / "pages.bin", std::ios::binary);
  std::string table((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const size_t entry = 10 + 22 * 6;
  ASSERT_GE(table.size(), entry + 6);
  EXPECT_EQ(std::string("\0\0\xFF\xFF\xFF\xFF", 6), table.substr(entry, 6));

  // Laid out on its own again (the rule no longer applies, a vertical layout): its count replaces the mark.
  SpinePageIndex::record(dir, kVariant, 40, 22, 1, 865);
  EXPECT_EQ(1u, SpinePageIndex::sumRange(dir, kVariant, 40, 22, 23, 23).pagesBefore);
}

TEST(SpinePageIndex, AnotherVariantStartsTheTableOver) {
  const std::string dir = freshDir("variant");
  SpinePageIndex::record(dir, kVariant, 40, 3, 9, 18000);

  const SpinePageIndex::Variant biggerFont{0x9abcdef0u, 7};
  EXPECT_EQ(0, SpinePageIndex::sumRange(dir, biggerFont, 40, 0, 39, -1).filesAfter) << "counts of other settings";

  SpinePageIndex::record(dir, biggerFont, 40, 5, 14, 20000);
  EXPECT_EQ(0, SpinePageIndex::sumRange(dir, kVariant, 40, 0, 39, -1).filesAfter) << "the old variant survived";
  EXPECT_EQ(14u, SpinePageIndex::sumRange(dir, biggerFont, 40, 0, 39, -1).pagesAfter);

  const SpinePageIndex::Variant newLayoutVersion{0x9abcdef0u, 8};
  EXPECT_EQ(0, SpinePageIndex::sumRange(dir, newLayoutVersion, 40, 0, 39, -1).filesAfter)
      << "a section layout version bump must not reuse counts laid out by the old code";
}

TEST(SpinePageIndex, NoTableMeansNothingRecorded) {
  const std::string dir = freshDir("none");
  const auto t = SpinePageIndex::sumRange(dir, kVariant, 40, 0, 39, 5);
  EXPECT_EQ(0, t.filesBefore + t.filesAfter);
}

TEST(SpinePageIndex, ASectionBuildRecordsItsPageCount) {
  GfxRenderer renderer;
  auto epub = loadTables("build");
  const Section::BuildParams p = defaultParams();
  EXPECT_EQ(0, firstSpine(*epub, p).filesAfter);

  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_GT(section.pageCount, 0);

  const auto t = firstSpine(*epub, p);
  EXPECT_EQ(1, t.filesAfter);
  EXPECT_EQ(section.pageCount, t.pagesAfter);
  size_t inflated = 0;
  ASSERT_TRUE(epub->getSpineItemInflatedSize(0, &inflated));
  EXPECT_EQ(inflated, t.bytesAfter);

  Section::BuildParams narrower = p;
  narrower.viewportWidth = 400;
  EXPECT_EQ(0, firstSpine(*epub, narrower).filesAfter) << "counted under settings it was not built for";
}

// Background-B builds a sibling in slices while the reader keeps turning pages: nothing may be
// recorded until the build has finished, and then its count must be there without a reload.
TEST(SpinePageIndex, ASlicedBuildRecordsOnlyOnceItHasFinished) {
  GfxRenderer renderer;
  auto epub = loadTables("sliced");
  const Section::BuildParams p = defaultParams();

  // Without a ticking clock the host's millis() is frozen and the sliced build finishes in one call.
  const host_clock::Ticking tick(1);
  Section section(epub, 0, renderer);
  int liveSlices = 0;
  Section::BuildStep step = Section::BuildStep::More;
  for (int i = 0; i < 20000; ++i) {
    step = section.stepSectionBuild(p, /*budgetMs=*/1);
    if (step == Section::BuildStep::Done || step == Section::BuildStep::Failed) break;
    ++liveSlices;
    EXPECT_EQ(0, firstSpine(*epub, p).filesAfter) << "recorded a build still in flight after slice " << liveSlices;
  }
  ASSERT_EQ(Section::BuildStep::Done, step);
  ASSERT_GT(liveSlices, 0) << "the build finished in one slice; the in-flight state was never reached";
  EXPECT_EQ(section.pageCount, firstSpine(*epub, p).pagesAfter);
}

// Caches built before the index existed (or while it belonged to other settings) are recorded
// the next time the reader loads them.
TEST(SpinePageIndex, LoadingACachedSectionRecordsIt) {
  GfxRenderer renderer;
  auto epub = loadTables("load");
  const Section::BuildParams p = defaultParams();
  {
    Section built(epub, 0, renderer);
    ASSERT_TRUE(built.createSectionFile(p, {}, /*skipEviction=*/true));
  }
  fs::remove(epub->getCachePath() + "/pages.bin");
  ASSERT_EQ(0, firstSpine(*epub, p).filesAfter);

  Section loaded(epub, 0, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(p));
  EXPECT_EQ(loaded.pageCount, firstSpine(*epub, p).pagesAfter);
}

// The reader asks with embedded CSS on and may be answered by the no-CSS fallback variant; the
// count belongs to what it asked for, or the next lookup (with CSS on) would never find it.
TEST(SpinePageIndex, ANoCssFallbackLoadIsRecordedUnderTheRequestedVariant) {
  GfxRenderer renderer;
  auto epub = loadTables("no_css_fallback");
  const Section::BuildParams noCss = defaultParams();
  {
    Section built(epub, 0, renderer);
    ASSERT_TRUE(built.createSectionFile(noCss, {}, /*skipEviction=*/true));
  }
  Section::BuildParams withCss = noCss;
  withCss.embeddedStyle = true;

  Section loaded(epub, 0, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(withCss)) << "precondition: the reader would open the no-CSS cache";
  EXPECT_EQ(loaded.pageCount, firstSpine(*epub, withCss).pagesAfter);
}
