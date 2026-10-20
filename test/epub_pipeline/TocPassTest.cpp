// The TOC pass of a first open on a book with thousands of TOC entries: the NCX's parse and the
// mapping of every entry to its spine item.
//
// X3, 2026-10-20: the TOC pass of King's Avatar (1732 spine items, an NCX entry each) took 1.28 s.
// Host-profiled on a deflated NCX of the same shape, about a quarter of that is inflate, three
// fifths the XML tokenizer, and a fifth the mapping -- most of which was heap churn: a dozen-odd
// std::strings built and freed per entry to join, split, decode, normalise and copy its href.
#include <BuildArena.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "BigSpineBook.h"
#include "CardCost.h"
#include "Epub.h"
#include "Epub/BookMetadataCache.h"
#include "Epub/parsers/TocNcxParser.h"

namespace fs = std::filesystem;

namespace {

struct TocPass {
  size_t entries = 0;
  size_t allocations = 0;
  card_cost::Counters cost;
  std::vector<int16_t> spineOf;  // each TOC entry's spine item, in TOC order
};

// The spine pass and the NCX's TOC pass as Epub::load runs them, on `book`, every block over
// `refuseAbove` refused (0: none).
TocPass runTocPass(const fs::path& work, const big_spine_book::Book& book, const size_t refuseAbove = 0) {
  TocPass out;
  const std::string cacheDir = (work / "toc_cache").string();
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  auto epub = std::make_shared<Epub>(book.path, (work / "epub_cache").string());
  size_t ncxSize = 0;
  if (!epub->getItemSize("OEBPS/toc.ncx", &ncxSize)) return out;

  BookMetadataCache cache(cacheDir);
  cache.beginWrite();
  cache.beginContentOpfPass();
  for (const auto& href : book.spineHrefs) cache.createSpineEntry(href);
  cache.endContentOpfPass();
  auto lent = std::make_unique<uint8_t[]>(52272);
  BuildArena region(lent.get(), 52272);
  cache.beginTocPass(&region);
  {
    std::optional<AllocationRefusal> refusal;
    if (refuseAbove != 0) refusal.emplace(refuseAbove + 1);
    const std::string base = "OEBPS/";
    TocNcxParser parser(base, ncxSize, &cache, nullptr);
    if (!parser.setup()) return out;
    card_cost::reset();
    const size_t allocationsBefore = heapAllocations();
    epub->readItemContentsToStream("OEBPS/toc.ncx", parser, 1024, nullptr, &region);
    out.allocations = heapAllocations() - allocationsBefore;
    out.cost = card_cost::now();
  }
  cache.endTocPass();
  cache.endWrite();
  out.entries = static_cast<size_t>(cache.getTocCount());

  // Read the mapping back from the file the pass writes each entry's spine item to, in TOC order.
  FsFile spines;
  if (Storage.openFileForRead("TEST", cacheDir + "/toc.spine.tmp", spines)) {
    for (size_t i = 0; i < out.entries; ++i) {
      int16_t spineIndex = -1;
      serialization::readPod(spines, spineIndex);
      out.spineOf.push_back(spineIndex);
    }
    spines.close();
  }
  return out;
}

class TocPassTest : public testing::Test {
 protected:
  void SetUp() override {
    work_ = fs::temp_directory_path() / ("toc_pass_" + std::to_string(getpid()));
    fs::remove_all(work_);
    fs::create_directories(work_);
  }
  void TearDown() override { fs::remove_all(work_); }
  fs::path work_;
};

TEST_F(TocPassTest, AnNcxEntryCostsAFewAllocationsNotADozen) {
  const auto book = big_spine_book::write((work_ / "book.epub").string(), 1732, 1600);
  const TocPass pass = runTocPass(work_, book);
  ASSERT_EQ(pass.entries, 1732u);
  for (size_t i = 0; i < pass.spineOf.size(); ++i) ASSERT_EQ(pass.spineOf[i], static_cast<int16_t>(i)) << i;
  const double perEntry = static_cast<double>(pass.allocations) / static_cast<double>(pass.entries);
  std::printf("[ toc      ] %zu NCX entries: %zu allocations (%.1f an entry), %zu card calls\n", pass.entries,
              pass.allocations, perEntry, pass.cost.calls);
  EXPECT_LT(perEntry, 4.0);
}

}  // namespace
