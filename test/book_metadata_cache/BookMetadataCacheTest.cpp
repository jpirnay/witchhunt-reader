// Characterization tests for BookMetadataCache's TOC href -> spineIndex resolution.
//
// These pin the CURRENT behaviour so the allocation-hardening work (nothrow growth
// for the per-spine index containers) can be shown not to change any observable
// result. The property that matters: the fast binary-search index and the
// linear-scan fallback must resolve IDENTICALLY. The index is a pure speed
// optimisation, never correctness — so degrading to the scan on OOM must be safe.
//
// LARGE_SPINE_THRESHOLD is 16: a corpus below it exercises the linear scan, one
// above it exercises the index. Both are asserted against the same expectations.
//
// The resolution is observed by reading back the temp TOC file the pass writes
// (toc.bin.tmp, still on disk until cleanupTmpFiles), using the same
// serialization helpers the production reader uses. No test hooks in production
// code, and no dependency on buildBookBin (which would need a real ZIP archive).

#include "BookMetadataCache.h"

#include <HalStorage.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

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

// Read the spineIndex each TOC entry resolved to, in TOC order, straight out of
// the temp file the TOC pass wrote.
std::vector<int16_t> readResolvedSpineIndices(const std::string& cacheDir, size_t expectedCount) {
  std::vector<int16_t> out;
  FsFile toc;
  const std::string tocPath = cacheDir + "/toc.bin.tmp";
  if (!Storage.openFileForRead("TEST", tocPath, toc)) {
    ADD_FAILURE() << "could not open " << tocPath;
    return out;
  }
  toc.seek(0);
  for (size_t i = 0; i < expectedCount; i++) {
    std::string title, href, anchor;
    uint8_t level = 0;
    int16_t spineIndex = -1;
    serialization::readString(toc, title);
    serialization::readString(toc, href);
    serialization::readString(toc, anchor);
    serialization::readPod(toc, level);
    serialization::readPod(toc, spineIndex);
    out.push_back(spineIndex);
  }
  toc.close();
  return out;
}

// Drive the build-mode API the way Epub::load does: spine pass, then TOC pass.
std::vector<int16_t> resolveTocToSpine(const std::string& cacheDir, const std::vector<std::string>& spineHrefs,
                                       const std::vector<std::string>& tocHrefs) {
  {
    BookMetadataCache cache(cacheDir);
    EXPECT_TRUE(cache.beginWrite());
    EXPECT_TRUE(cache.beginContentOpfPass());
    for (const std::string& href : spineHrefs) {
      cache.createSpineEntry(href);
    }
    EXPECT_TRUE(cache.endContentOpfPass());

    EXPECT_TRUE(cache.beginTocPass());
    for (size_t i = 0; i < tocHrefs.size(); i++) {
      cache.createTocEntry("Title " + std::to_string(i), tocHrefs[i], /*anchor=*/"", /*level=*/0);
    }
    EXPECT_TRUE(cache.endTocPass());
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

// --- Below LARGE_SPINE_THRESHOLD: the linear scan runs -----------------------

TEST_F(BookMetadataCacheTest, SmallCorpusResolvesEveryTocHrefViaLinearScan) {
  const std::vector<std::string> spine = chapterHrefs(8);
  const std::vector<int16_t> got = resolveTocToSpine(cacheDir_, spine, spine);

  ASSERT_EQ(got.size(), spine.size());
  for (size_t i = 0; i < got.size(); i++) {
    EXPECT_EQ(got[i], static_cast<int16_t>(i)) << "toc entry " << i;
  }
}

// --- Above the threshold: the fast binary-search index runs ------------------

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

}  // namespace
