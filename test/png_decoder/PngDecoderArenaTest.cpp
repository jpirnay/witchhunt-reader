// PngStreamDecoder::setScratchArena must be a pure allocation-source swap: identical decoded
// pixels whether the inflate ring and scanline buffers come from a caller-owned BuildArena or
// from the heap.
//
// Why this suite exists: the arena path is reached only from Section::warmAllImageCaches, which
// the epub_pipeline harness never calls — so the golden dumps give it zero coverage. Without
// these tests the fallback branches (arena too small, arena absent, ring doesn't fit) are
// entirely unexercised on the host, and the only signal would be an on-device crash.
//
// See docs/memory-allocation-strategy.md: the point of the arena is to stop each decode in a
// multi-image warm pass from taking and returning its own 32 KB ring, which fragments the
// contiguous region the framebuffer realloc later needs.
#include <BuildArena.h>
#include <HalStorage.h>
#include <PngStreamDecoder.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::string fixture(const char* name) { return std::string(FIXTURE_DIR) + "/" + name; }

struct Decoded {
  PngStreamDecoder::Info info{};
  std::vector<uint8_t> gray;
  bool ok = false;
};

// Decode every row to grayscale. `arena` may be null (heap path).
Decoded decodeAll(const std::string& path, BuildArena* arena) {
  Decoded out;
  FsFile file;
  if (!Storage.openFileForRead("TST", path, file)) return out;

  PngStreamDecoder decoder;
  decoder.setScratchArena(arena);
  if (!decoder.begin(file, out.info)) {
    file.close();
    return out;
  }
  std::vector<uint8_t> row(out.info.width);
  for (uint32_t y = 0; y < out.info.height; ++y) {
    if (!decoder.nextRow(row.data())) {
      file.close();
      return out;
    }
    out.gray.insert(out.gray.end(), row.begin(), row.end());
  }
  decoder.end();
  file.close();
  out.ok = true;
  return out;
}

// Comfortably larger than one decode's needs for the fixture, so the arena path is really taken.
size_t generousArenaBytes(const uint32_t width) {
  // rawRowBytes for 8-bit RGB = width*3; expected output = height*(rawRowBytes+1) > 32 KB here,
  // so the ring lands at the full 32 KB cap.
  return PngStreamDecoder::scratchBytesFor(64 * 1024, width * 3) + 1024;
}

TEST(PngDecoderArena, ArenaAndHeapProduceIdenticalPixels) {
  const std::string path = fixture("gradient_rgb.png");

  const Decoded viaHeap = decodeAll(path, nullptr);
  ASSERT_TRUE(viaHeap.ok) << "heap decode failed — fixture missing or unreadable?";
  ASSERT_GT(viaHeap.gray.size(), 0u);

  BuildArena arena(generousArenaBytes(viaHeap.info.width));
  ASSERT_TRUE(arena.valid());
  const Decoded viaArena = decodeAll(path, &arena);
  ASSERT_TRUE(viaArena.ok);

  EXPECT_EQ(viaHeap.info.width, viaArena.info.width);
  EXPECT_EQ(viaHeap.info.height, viaArena.info.height);
  EXPECT_EQ(viaHeap.gray, viaArena.gray);
  // The arena must actually have been used, otherwise this test would pass trivially while
  // both runs silently used the heap.
  EXPECT_GT(arena.highWater(), 32u * 1024u) << "ring did not come from the arena";
  EXPECT_EQ(arena.failedAllocSize(), 0u);
}

TEST(PngDecoderArena, ArenaIsReusedAcrossConsecutiveDecodes) {
  // The whole point of the pass-wide arena: N decodes must not stack N ring allocations.
  // A bump arena reuses the same bytes only if each decoder releases its scope on end().
  const std::string path = fixture("gradient_rgb.png");
  const Decoded probe = decodeAll(path, nullptr);
  ASSERT_TRUE(probe.ok);

  BuildArena arena(generousArenaBytes(probe.info.width));
  ASSERT_TRUE(arena.valid());

  size_t highWaterAfterFirst = 0;
  for (int i = 0; i < 5; ++i) {
    const Decoded d = decodeAll(path, &arena);
    ASSERT_TRUE(d.ok) << "decode " << i << " failed — arena not released by the previous one?";
    EXPECT_EQ(d.gray, probe.gray) << "decode " << i << " diverged";
    if (i == 0) {
      highWaterAfterFirst = arena.highWater();
    } else {
      // Flat high-water across iterations is the proof of reuse.
      EXPECT_EQ(arena.highWater(), highWaterAfterFirst) << "arena grew on decode " << i;
    }
    EXPECT_EQ(arena.used(), 0u) << "decode " << i << " leaked its scope";
  }
  EXPECT_EQ(arena.failedAllocSize(), 0u);
}

TEST(PngDecoderArena, TooSmallArenaFallsBackToHeap) {
  // Degrade in speed, never in correctness: an arena that cannot fit the ring (or the scanline
  // buffers) must still decode, using the heap for whatever did not fit.
  const std::string path = fixture("gradient_rgb.png");
  const Decoded viaHeap = decodeAll(path, nullptr);
  ASSERT_TRUE(viaHeap.ok);

  // Big enough for the two scanline buffers but far too small for a 32 KB ring.
  BuildArena tinyArena(4 * 1024);
  ASSERT_TRUE(tinyArena.valid());
  const Decoded viaTiny = decodeAll(path, &tinyArena);
  ASSERT_TRUE(viaTiny.ok) << "decode must survive an undersized arena";
  EXPECT_EQ(viaHeap.gray, viaTiny.gray);
}

TEST(PngDecoderArena, ZeroSizedArenaBehavesLikeNoArena) {
  const std::string path = fixture("gradient_rgb.png");
  const Decoded viaHeap = decodeAll(path, nullptr);
  ASSERT_TRUE(viaHeap.ok);

  BuildArena empty(0);
  const Decoded viaEmpty = decodeAll(path, &empty);
  ASSERT_TRUE(viaEmpty.ok);
  EXPECT_EQ(viaHeap.gray, viaEmpty.gray);
}

TEST(PngDecoderArena, ScratchBytesForCoversAnActualDecode) {
  // A caller budgeting from scratchBytesFor() must not come up short — otherwise the arena
  // silently half-works and the churn it was meant to remove comes back.
  const std::string path = fixture("gradient_rgb.png");
  const Decoded probe = decodeAll(path, nullptr);
  ASSERT_TRUE(probe.ok);

  const uint32_t rawRowBytes = probe.info.width * 3;  // 8-bit RGB fixture
  const size_t expectedOutput = static_cast<size_t>(probe.info.height) * (rawRowBytes + 1);
  const size_t budget = PngStreamDecoder::scratchBytesFor(expectedOutput, rawRowBytes);

  BuildArena exact(budget);
  ASSERT_TRUE(exact.valid());
  const Decoded d = decodeAll(path, &exact);
  ASSERT_TRUE(d.ok);
  EXPECT_EQ(d.gray, probe.gray);
  EXPECT_EQ(exact.failedAllocSize(), 0u) << "scratchBytesFor under-budgeted the decode";
  EXPECT_LE(exact.highWater(), budget);
}

}  // namespace
