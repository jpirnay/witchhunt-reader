// Lazy image extraction (ImageBlock::ensureExtracted -> Epub::extractItemToFile).
//
// The inflate ring this path needs is up to 32 KB CONTIGUOUS, and until it could be served
// from an arena it was the first allocation to fail on a fragmented heap: measured on X4 at
// contig=13300, every image on the page logged "Failed to init inflate reader" and rendered
// as nothing at all. The reader is already holding a borrowed 48 KB arena for the decoders at
// that moment (image_scratch), so the fix is to hand it to the extractor too.
//
// What has to hold:
//   1. The arena path produces exactly the same bytes as the heap path.
//   2. The ring really comes from the arena, and is given back afterwards.
//   3. An arena too small to serve the reader falls back to the heap instead of failing —
//      EntryReader itself does NOT fall back once it has been handed one.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_png_images.epub";
// 36353 bytes uncompressed, so ringSizeFor() asks for the full 32 KB cap — the worst case,
// and the one the device actually failed on.
const char* kEntry = "OEBPS/images/scaling_test.png";
constexpr size_t kEntryBytes = 36353;

struct ImageExtractionFixture : testing::Test {
  fs::path work;
  std::string cacheDir;

  void SetUp() override {
    // Per-test dir: ctest -j runs these as parallel processes.
    work = fs::temp_directory_path() /
           (std::string("epub_extract_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
  }
  void TearDown() override { fs::remove_all(work); }

  std::vector<uint8_t> extract(const char* name, BuildArena* arena) {
    const std::string dest = (work / name).string();
    Epub epub(kBook, cacheDir);
    if (!epub.extractItemToFile(kEntry, dest, arena)) return {};
    std::ifstream in(dest, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

TEST_F(ImageExtractionFixture, HeapPathExtractsWholeEntry) {
  const auto bytes = extract("heap.png", nullptr);
  ASSERT_EQ(bytes.size(), kEntryBytes);
  EXPECT_EQ(bytes[0], 0x89);  // PNG signature survived the round trip
  EXPECT_EQ(bytes[1], 'P');
}

TEST_F(ImageExtractionFixture, ArenaPathMatchesHeapByteForByte) {
  const auto viaHeap = extract("heap.png", nullptr);
  ASSERT_EQ(viaHeap.size(), kEntryBytes);

  BuildArena arena(Epub::EXTRACT_ARENA_BYTES + 1024);  // budget + alignment slack
  ASSERT_TRUE(arena.valid());
  const auto viaArena = extract("arena.png", &arena);

  EXPECT_EQ(viaArena, viaHeap);
  EXPECT_GT(arena.highWater(), 32u * 1024u) << "the ring should have come from the arena";
  EXPECT_EQ(arena.used(), 0u) << "EntryReader::close must give the block back";
  EXPECT_EQ(arena.failedAllocSize(), 0u);
}

// EXTRACT_ARENA_BYTES is what callers gate on; if it were too small for the reader, every
// extraction would silently take the slow fallback and the fix would do nothing.
TEST_F(ImageExtractionFixture, BudgetConstantActuallyCoversTheReader) {
  BuildArena arena(Epub::EXTRACT_ARENA_BYTES);
  ASSERT_TRUE(arena.valid());
  const auto bytes = extract("budget.png", &arena);

  ASSERT_EQ(bytes.size(), kEntryBytes);
  EXPECT_EQ(arena.failedAllocSize(), 0u) << "EXTRACT_ARENA_BYTES must fit readBuf + ring + alignment";
}

TEST_F(ImageExtractionFixture, TooSmallArenaFallsBackToHeapInsteadOfFailing) {
  const auto viaHeap = extract("heap.png", nullptr);
  ASSERT_EQ(viaHeap.size(), kEntryBytes);

  BuildArena arena(2 * 1024);  // fits the read buffer, nowhere near the ring
  ASSERT_TRUE(arena.valid());
  const auto bytes = extract("small.png", &arena);

  EXPECT_EQ(bytes, viaHeap) << "a short arena must not turn a working extraction into a failure";
  EXPECT_GT(arena.failedAllocSize(), 0u) << "the arena path should have been tried first";
  EXPECT_EQ(arena.used(), 0u) << "the failed open must release its scope";
}

}  // namespace
