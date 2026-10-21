// Characterization tests for BookMetadataCache's TOC href -> spineIndex resolution.
//
// These pin the behaviour so the lookup's speed work can be shown not to change any
// observable result. The entries are now looked up a table-full at a time when the
// pass ends (a per-spine href index used to answer them as they were parsed); the
// small and large corpora that once picked the scan or the index are kept as shapes,
// and must still resolve identically.
//
// The resolution is observed by reading back the temp file the pass writes the
// spine indices to (toc.spine.tmp, still on disk until cleanupTmpFiles), using the
// same serialization helpers the production reader uses. No test hooks in production
// code, and no dependency on buildBookBin (which would need a real ZIP archive).

#include <BuildArena.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "AllocationRefusal.h"
#include "BookMetadataCache.h"
#include "SpineHrefMatch.h"

namespace fs = std::filesystem;

namespace {

class BookMetadataCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
    cacheDir_ = (fs::temp_directory_path() / ("bmc_test_" + std::string(info->name()))).string();
    fs::remove_all(cacheDir_);
    fs::create_directories(cacheDir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(cacheDir_, ec);
  }

  std::string cacheDir_;
};

// Read the spineIndex each TOC entry resolved to, in TOC order, straight out of the file the TOC
// pass writes them to (toc.spine.tmp, int16 per entry; the TOC records themselves carry -1 until
// buildBookBin writes book.bin).
std::vector<int16_t> readResolvedSpineIndices(const std::string& cacheDir, size_t expectedCount) {
  std::vector<int16_t> out;
  FsFile spines;
  const std::string path = cacheDir + "/toc.spine.tmp";
  if (!Storage.openFileForRead("TEST", path, spines)) {
    ADD_FAILURE() << "could not open " << path;
    return out;
  }
  spines.seek(0);
  for (size_t i = 0; i < expectedCount; i++) {
    int16_t spineIndex = -1;
    serialization::readPod(spines, spineIndex);
    out.push_back(spineIndex);
  }
  spines.close();
  return out;
}

// Drive the build-mode API the way Epub::load does: spine pass, then TOC pass. With
// `refuseAllocationsFrom`, the TOC entries are created on a heap that cannot serve a block that
// large. Armed after beginTocPass(): opening the temp files goes through the host storage shim,
// whose path conversion allocates more than the device's FsFile ever would.
std::vector<int16_t> resolveTocToSpine(const std::string& cacheDir, const std::vector<std::string>& spineHrefs,
                                       const std::vector<std::string>& tocHrefs, const size_t refuseAllocationsFrom = 0,
                                       size_t* tocEntryBytesRead = nullptr, BuildArena* scratch = nullptr) {
  {
    BookMetadataCache cache(cacheDir);
    EXPECT_TRUE(cache.beginWrite());
    EXPECT_TRUE(cache.beginContentOpfPass());
    for (const std::string& href : spineHrefs) {
      cache.createSpineEntry(href);
    }
    EXPECT_TRUE(cache.endContentOpfPass());

    EXPECT_TRUE(cache.beginTocPass(scratch));
    std::optional<AllocationRefusal> refusal;
    if (refuseAllocationsFrom != 0) refusal.emplace(refuseAllocationsFrom);
    const size_t bytesReadBefore = FsFile::bytesRead;
    for (size_t i = 0; i < tocHrefs.size(); i++) {
      cache.createTocEntry("Title " + std::to_string(i), tocHrefs[i], /*anchor=*/"", /*level=*/0);
    }
    // The spine items are looked up when the pass ends: the reads it costs are counted there too.
    EXPECT_TRUE(cache.endTocPass());
    if (tocEntryBytesRead) *tocEntryBytesRead = FsFile::bytesRead - bytesReadBefore;
    refusal.reset();
    EXPECT_TRUE(cache.endWrite());
  }
  return readResolvedSpineIndices(cacheDir, tocHrefs.size());
}

std::vector<std::string> chapterHrefs(int count) {
  std::vector<std::string> hrefs;
  hrefs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    hrefs.push_back("OEBPS/chapter" + std::to_string(i) + ".xhtml");
  }
  return hrefs;
}

// --- A small spine ------------------------------------------------------------

TEST_F(BookMetadataCacheTest, SmallCorpusResolvesEveryTocHrefViaLinearScan) {
  const std::vector<std::string> spine = chapterHrefs(8);
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, spine);

  ASSERT_EQ(got.size(), spine.size());
  for (size_t i = 0; i < got.size(); i++) {
    EXPECT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i;
  }
}

// --- A larger one ---------------------------------------------------------------

TEST_F(BookMetadataCacheTest, LargeCorpusResolvesEveryTocHrefViaFastIndex) {
  const std::vector<std::string> spine = chapterHrefs(64);
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, spine);

  ASSERT_EQ(got.size(), spine.size());
  for (size_t i = 0; i < got.size(); i++) {
    EXPECT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i;
  }
}

// The core invariant the hardening must preserve: both modes agree.
TEST_F(BookMetadataCacheTest, FastIndexAndLinearScanAgreeOnTheSameTocHrefs) {
  const std::vector<std::string> toc = chapterHrefs(8);

  const std::vector<int16_t> viaScan = resolveTocToSpine(cacheDir_, chapterHrefs(8), toc);
  // Same TOC hrefs, but a spine large enough to trigger the index. The extra
  // spines are never referenced by the TOC and must not shift any resolution.
  const std::vector<int16_t> viaIndex = resolveTocToSpine(cacheDir_, chapterHrefs(64), toc);

  ASSERT_EQ(viaScan.size(), viaIndex.size());
  EXPECT_EQ(viaScan, viaIndex) << "the fast index resolved differently than the linear scan";
}

TEST_F(BookMetadataCacheTest, UnmatchedTocHrefResolvesToMinusOneInBothModes) {
  const std::vector<std::string> toc = {"OEBPS/chapter0.xhtml", "OEBPS/nowhere.xhtml"};

  const std::vector<int16_t> viaScan = resolveTocToSpine(cacheDir_, chapterHrefs(8), toc);
  ASSERT_EQ(viaScan.size(), 2u);
  EXPECT_EQ(viaScan[0], 0);
  EXPECT_EQ(viaScan[1], -1) << "an href with no spine match must resolve to -1, not to a neighbour";

  const std::vector<int16_t> viaIndex = resolveTocToSpine(cacheDir_, chapterHrefs(64), toc);
  ASSERT_EQ(viaIndex.size(), 2u);
  EXPECT_EQ(viaIndex[0], 0);
  EXPECT_EQ(viaIndex[1], -1);
}

// Many spines, sparse TOC — the shape the allocation hardening targets, since
// the index containers are sized by spineCount regardless of how small the TOC is.
TEST_F(BookMetadataCacheTest, LargeSpineCountWithSparseTocResolvesCorrectly) {
  const std::vector<std::string> spine = chapterHrefs(1000);
  const std::vector<std::string> toc = {spine[0], spine[499], spine[999]};

  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, toc);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], 0);
  EXPECT_EQ(got[1], 499);
  EXPECT_EQ(got[2], 999);
}

// --- A heap that cannot hold the index ----------------------------------------
//
// First-open indexing runs with the secondary framebuffer lent, not freed: ~50 KB of fragmented
// heap. A 2956-spine book's index (16 B/spine) does not fit there, and the deque that held it
// allocated through the throwing operator new -- abort() on the device. Refusing every block from
// 512 B up leaves the index (one block, 1 KB even for 64 spines) no room, while the small
// allocations a lookup makes still succeed. The 4 KB read/write buffers are refused as well and
// degrade to pass-through, as they would on such a heap.
constexpr size_t kIndexDoesNotFit = 512;

TEST_F(BookMetadataCacheTest, TocPassSurvivesAnIndexTheHeapCannotHold) {
  const std::vector<std::string> spine = chapterHrefs(64);
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, spine, kIndexDoesNotFit);

  ASSERT_EQ(got.size(), spine.size());
  for (size_t i = 0; i < got.size(); i++) {
    EXPECT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i;
  }
}

// Without the index, each TOC entry used to rescan the spine from its first record: ~4.4 million
// spine records for a 2956-entry TOC. A TOC lists its chapters in reading order, so resuming after
// the previous match finds the next one at once, and the whole TOC reads the spine file about
// once. Measured in bytes read, not time: on a host a from-the-start scan still finishes in under
// a second, on the device it is SD reads at 160 MHz.
TEST_F(BookMetadataCacheTest, AnInOrderTocReadsTheSpineAboutOnceWithoutTheIndex) {
  const std::vector<std::string> spine = chapterHrefs(3000);

  size_t bytesRead = 0;
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, spine, kIndexDoesNotFit, &bytesRead);

  ASSERT_EQ(got.size(), spine.size());
  for (size_t i = 0; i < got.size(); i++) {
    ASSERT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i;
  }
  const auto spineFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "spine.bin.tmp"));
  // The TOC records are read back once to look their hrefs up.
  const auto tocFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "toc.bin.tmp"));
  EXPECT_LE(bytesRead, 2 * spineFileBytes + tocFileBytes)
      << "spine file is " << spineFileBytes << " bytes, TOC file " << tocFileBytes;
}

// In any other order, the entries are looked up a table-full at a time, one pass over the spine
// file each; the table lives in the lent framebuffer during a first open, which holds thousands.
// Each entry's own scan from where the previous one matched cost about a spine read per entry for
// this book: a TOC that lists the newest chapter first.
TEST_F(BookMetadataCacheTest, AReversedTocReadsTheSpineAboutOnceFromTheLentRegion) {
  const std::vector<std::string> spine = chapterHrefs(3000);
  const std::vector<std::string> toc(spine.rbegin(), spine.rend());
  constexpr size_t kRegionBytes = 48 * 1024 + 256;
  std::unique_ptr<uint8_t[]> region(new uint8_t[kRegionBytes]);
  BuildArena arena(region.get(), kRegionBytes);

  size_t bytesRead = 0;
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, toc, kIndexDoesNotFit, &bytesRead, &arena);
  const auto spineFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "spine.bin.tmp"));
  const auto tocFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "toc.bin.tmp"));
  std::printf("[ toc      ] reversed TOC of %zu, lent region: %zu bytes read (spine file %zu, TOC file %zu)\n",
              toc.size(), bytesRead, spineFileBytes, tocFileBytes);
  EXPECT_LE(bytesRead, 2 * spineFileBytes + tocFileBytes);

  ASSERT_EQ(got.size(), toc.size());
  for (size_t i = 0; i < got.size(); i++) {
    ASSERT_EQ(got[i], static_cast<int16_t>(toc.size() - 1 - i)) << "toc entry " << i;
  }
  EXPECT_EQ(arena.used(), 0u) << "the table is handed back to the region";
}

// Without a lent region, on a heap that serves only small blocks, the table is small and the
// passes many: correct, and still well short of a spine read per entry.
TEST_F(BookMetadataCacheTest, AReversedTocOnASmallHeapStillResolvesEveryEntry) {
  const std::vector<std::string> spine = chapterHrefs(1000);
  const std::vector<std::string> toc(spine.rbegin(), spine.rend());

  size_t bytesRead = 0;
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, toc, kIndexDoesNotFit, &bytesRead);
  const auto spineFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "spine.bin.tmp"));
  std::printf("[ toc      ] reversed TOC of %zu, small heap: %zu bytes read (spine file %zu)\n", toc.size(), bytesRead,
              spineFileBytes);
  // 16 entries a pass on this heap (a 256 B table): 63 passes for 1000 entries, not 1000.
  EXPECT_LE(bytesRead, 70 * spineFileBytes);

  ASSERT_EQ(got.size(), toc.size());
  for (size_t i = 0; i < got.size(); i++) {
    ASSERT_EQ(got[i], static_cast<int16_t>(toc.size() - 1 - i)) << "toc entry " << i;
  }
}

// Resuming must not cost correctness: entries out of order, repeated, before the previous match
// (the scan wraps) or absent from the spine all resolve as a from-the-start scan would.
TEST_F(BookMetadataCacheTest, TheResumedScanResolvesAnyOrderLikeAFullScan) {
  const std::vector<std::string> spine = chapterHrefs(64);
  const std::vector<std::string> toc = {
      spine[5], spine[2], spine[63], spine[63], spine[0], spine[40], "OEBPS/nowhere.xhtml", spine[41], spine[1]};

  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, toc, kIndexDoesNotFit);
  const std::vector<int16_t> expected = {5, 2, 63, 63, 0, 40, -1, 41, 1};
  EXPECT_EQ(got, expected);
}

// --- TOC hrefs that only match a spine item by filename -------------------------------------
//
// A nav in OEBPS/ that links "chapter1.xhtml" while the spine carries OEBPS/text/chapter1.xhtml:
// the parser resolves the link to OEBPS/chapter1.xhtml, which no spine href matches exactly. Every
// entry used to be written with spineIndex -1, so the contents list showed titles that could not
// be opened. The filename alone now maps it, as it already did for in-book links.

std::vector<std::string> hrefsIn(const std::string& dir, int count) {
  std::vector<std::string> hrefs;
  hrefs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    hrefs.push_back(dir + "chapter" + std::to_string(i) + ".xhtml");
  }
  return hrefs;
}

TEST(SpineHrefMatchTest, APathMatchesExactlyOrByItsFilename) {
  EXPECT_EQ(spine_href::match("OEBPS/text/c1.xhtml", "OEBPS/text/c1.xhtml"), SpineHrefMatch::Exact);
  EXPECT_EQ(spine_href::match("OEBPS/c1.xhtml", "OEBPS/text/c1.xhtml"), SpineHrefMatch::Filename);
  EXPECT_EQ(spine_href::match("c1.xhtml", "OEBPS/text/c1.xhtml"), SpineHrefMatch::Filename);
  EXPECT_EQ(spine_href::match("OEBPS/c1.xhtml", "OEBPS/text/c11.xhtml"), SpineHrefMatch::None);
  EXPECT_EQ(spine_href::match("OEBPS/", "OEBPS/text/"), SpineHrefMatch::None) << "no filename, nothing to match";
}

TEST_F(BookMetadataCacheTest, TocHrefsOffByAFolderResolveByFilenameInBothModes) {
  for (const int spines : {8, 64}) {  // the linear scan, then the index
    const std::vector<int16_t> got =
        resolveTocToSpine(cacheDir_, hrefsIn("OEBPS/text/", spines), hrefsIn("OEBPS/", spines));
    ASSERT_EQ(got.size(), static_cast<size_t>(spines));
    for (int i = 0; i < spines; i++) {
      EXPECT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i << " of " << spines;
    }
  }
}

TEST_F(BookMetadataCacheTest, AFilenameTwoSpineItemsShareIsNotGuessed) {
  std::vector<std::string> spine = chapterHrefs(8);
  spine.push_back("OEBPS/a/notes.xhtml");  // 8
  spine.push_back("OEBPS/b/notes.xhtml");  // 9
  const std::vector<std::string> toc = {"OEBPS/notes.xhtml", "OEBPS/b/notes.xhtml", "OEBPS/a/notes.xhtml",
                                        "OEBPS/x/chapter3.xhtml"};

  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, toc);
  const std::vector<int16_t> expected = {-1, 9, 8, 3};
  EXPECT_EQ(got, expected);
}

// The book this exists for has every entry off by a folder. Rereading the spine file for each of
// them would be the O(entries x spine) cost the resumed scan was written to remove; the folder the
// first match was found in answers the rest, found in reading order.
TEST_F(BookMetadataCacheTest, AnOffByAFolderTocReadsTheSpineAFewTimes) {
  const std::vector<std::string> spine = hrefsIn("OEBPS/text/", 3000);

  size_t bytesRead = 0;
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, hrefsIn("OEBPS/", 3000), 0, &bytesRead);

  ASSERT_EQ(got.size(), spine.size());
  for (size_t i = 0; i < got.size(); i++) {
    ASSERT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i;
  }
  const auto spineFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "spine.bin.tmp"));
  const auto tocFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "toc.bin.tmp"));
  // The exact pass (which matches nothing), the first entry's filename scan, then the shifted
  // hrefs found in reading order: about three spine reads. The TOC records are read back twice,
  // for the exact pass and for the misses.
  EXPECT_LE(bytesRead, 4 * spineFileBytes + 2 * tocFileBytes)
      << "spine file is " << spineFileBytes << " bytes, TOC file " << tocFileBytes;
}

// Hrefs that match nothing, not even by filename, each cost a read of the whole spine; on a book
// with thousands of spine items that must stop well short of one read per entry.
TEST_F(BookMetadataCacheTest, TocHrefsThatMatchNothingStopCostingWholeSpineReads) {
  const std::vector<std::string> spine = chapterHrefs(3000);
  std::vector<std::string> toc;
  toc.reserve(3000);
  for (int i = 0; i < 3000; i++) toc.push_back("OEBPS/missing" + std::to_string(i) + ".xhtml");

  size_t bytesRead = 0;
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, toc, 0, &bytesRead);
  ASSERT_EQ(got.size(), toc.size());
  EXPECT_EQ(got.back(), -1);
  const auto spineFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "spine.bin.tmp"));
  const auto tocFileBytes = static_cast<size_t>(fs::file_size(fs::path(cacheDir_) / "toc.bin.tmp"));
  // 32768 spine records of full scans per parse: 10 reads of a 3000-item spine, plus the exact
  // pass; the TOC records read back twice.
  EXPECT_LE(bytesRead, 12 * spineFileBytes + 2 * tocFileBytes)
      << "spine file is " << spineFileBytes << " bytes, TOC file " << tocFileBytes;
}

}  // namespace
