// PixelCache's on-disk bytes, and the batching that produces them.
//
// The cache is written through a small band buffer, and it used to flush one row per
// file.write(): a 464x618 cache went out as 618 separate 116-byte writes. Device-measured on
// X4 that was ~2.3 s per cache -- about 40% of a whole PNG decode, and why adding a second
// cache to the same decode pass cost 2274 ms despite its dither and packing being nearly free.
// Rows now accumulate and go out a band at a time.
//
// Batching changes WHEN bytes are written, so what has to be pinned is that it changes nothing
// about WHICH bytes: the file must be byte-identical to the naive row-at-a-time writer,
// including the fill used for rows a decode never covered.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub/converters/PixelCache.h"

namespace fs = std::filesystem;

namespace {

struct PixelCacheFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("pxc_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::vector<uint8_t> readFile(const std::string& p) const {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  // What the file must contain: the header, then every row, with uncovered rows at FILL_BYTE.
  // Written out independently of PixelCache so the two cannot drift together.
  static std::vector<uint8_t> expected(int w, int h, int coveredRows, uint8_t value) {
    const int bytesPerRow = (w + 3) / 4;
    std::vector<uint8_t> out;
    const uint16_t magic = PixelCache::PXC_MAGIC;
    const uint16_t w16 = static_cast<uint16_t>(w);
    const uint16_t h16 = static_cast<uint16_t>(h);
    for (uint16_t v : {magic, w16, h16}) {
      out.push_back(static_cast<uint8_t>(v & 0xFF));
      out.push_back(static_cast<uint8_t>(v >> 8));
    }
    uint8_t packed = 0;
    for (int i = 0; i < 4; i++) packed = static_cast<uint8_t>((packed << 2) | (value & 0x03));
    for (int r = 0; r < h; r++) {
      const uint8_t fill = (r < coveredRows) ? packed : PixelCache::FILL_BYTE;
      out.insert(out.end(), static_cast<size_t>(bytesPerRow), fill);
    }
    return out;
  }

  // Drive the cache exactly the way PngToFramebufferConverter does: advanceTo(row) per row,
  // then paint that row. `coveredRows` short of `h` mimics a decode clipped by the screen.
  std::string writeCache(int w, int h, int coveredRows, uint8_t value, int maxBlockRows = 1) {
    const std::string path = (work / "out.pxc").string();
    PixelCache cache;
    EXPECT_TRUE(cache.begin(path, w, h, 0, 0, maxBlockRows));
    const int bytesPerRow = (w + 3) / 4;
    for (int row = 0; row < coveredRows; row++) {
      EXPECT_TRUE(cache.advanceTo(row));
      uint8_t* rowPtr = cache.buffer + static_cast<size_t>(row - cache.bandStart) * bytesPerRow;
      for (int b = 0; b < bytesPerRow; b++) {
        uint8_t packed = 0;
        for (int i = 0; i < 4; i++) packed = static_cast<uint8_t>((packed << 2) | (value & 0x03));
        rowPtr[b] = packed;
      }
    }
    EXPECT_TRUE(cache.finalize());
    return path;
  }
};

// The band is 16 rows, so this crosses it many times -- the case the batching is for.
TEST_F(PixelCacheFixture, FullyCoveredImageMatchesRowByRowOutput) {
  const std::string path = writeCache(64, 100, 100, 2);
  EXPECT_EQ(readFile(path), expected(64, 100, 100, 2));
}

// Fewer rows than the box: the tail must come out as FILL_BYTE, or a short decode replays as a
// black band under the picture on every later view (the bug FILL_BYTE was introduced for).
TEST_F(PixelCacheFixture, UncoveredTailIsFilledNotLeftShort) {
  const std::string path = writeCache(64, 100, 37, 1);
  const auto bytes = readFile(path);
  EXPECT_EQ(bytes, expected(64, 100, 37, 1));
  EXPECT_EQ(bytes.size(), PixelCache::PXC_HEADER_BYTES + 16u * 100u);
}

// Nothing decoded at all: still a complete, all-white file rather than a truncated one.
TEST_F(PixelCacheFixture, NoRowsCoveredStillWritesEveryRow) {
  const std::string path = writeCache(64, 40, 0, 0);
  EXPECT_EQ(readFile(path), expected(64, 40, 0, 0));
}

// An image shorter than one band never triggers a mid-stream flush; finalize() alone must
// produce the whole file.
TEST_F(PixelCacheFixture, ImageShorterThanTheBand) {
  const std::string path = writeCache(64, 5, 5, 3);
  EXPECT_EQ(readFile(path), expected(64, 5, 5, 3));
}

// A block-at-a-time decoder (JPEG MCU rows) reserves headroom: rows written after an
// advanceTo() must still land inside the band, so the flush cannot be deferred as far.
TEST_F(PixelCacheFixture, BlockDecoderHeadroomIsRespected) {
  const std::string path = writeCache(64, 100, 100, 2, /*maxBlockRows=*/8);
  EXPECT_EQ(readFile(path), expected(64, 100, 100, 2));
}

}  // namespace
