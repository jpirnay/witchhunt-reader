// BufferedPrint: the BMP writers emit one file write per output row, and rows are small (a
// 1-bit 340x540 thumbnail is 44 bytes), so they used to reach the card as hundreds of separate
// calls. This batches them.
//
// Batching changes call COUNT, never bytes -- so that is what these pin: the sink must receive
// exactly the same stream, and it must receive materially fewer calls.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "BufferedPrint.h"

namespace {

// Records everything written and how many calls carried it.
struct CountingSink : Print {
  std::vector<uint8_t> bytes;
  int calls = 0;
  bool failNext = false;

  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, size_t n) override {
    if (failNext) return 0;
    calls++;
    bytes.insert(bytes.end(), data, data + n);
    return n;
  }
};

std::vector<uint8_t> rowPattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> row(n);
  for (size_t i = 0; i < n; i++) row[i] = static_cast<uint8_t>(seed + i);
  return row;
}

// The real shape: a 340x540 1-bit thumbnail, 44 bytes a row.
TEST(BufferedPrintTest, ThumbnailRowsCoalesceWithoutChangingBytes) {
  CountingSink sink;
  std::vector<uint8_t> expected;
  {
    BufferedPrint out(sink);
    for (int row = 0; row < 540; row++) {
      const auto data = rowPattern(44, static_cast<uint8_t>(row));
      EXPECT_EQ(out.write(data.data(), data.size()), data.size());
      expected.insert(expected.end(), data.begin(), data.end());
    }
    EXPECT_TRUE(out.flushBuffer());
  }
  EXPECT_EQ(sink.bytes, expected);
  // 540 * 44 = 23760 bytes through a 4 KB buffer: single digits, not 540.
  EXPECT_LE(sink.calls, 10) << "rows should be batched, got " << sink.calls << " calls";
}

// The destructor is the backstop for paths that bail out without flushing.
TEST(BufferedPrintTest, DestructorFlushesPendingBytes) {
  CountingSink sink;
  {
    BufferedPrint out(sink);
    const uint8_t data[] = {1, 2, 3};
    out.write(data, sizeof(data));
    EXPECT_TRUE(sink.bytes.empty()) << "nothing should reach the sink until it has to";
  }
  EXPECT_EQ(sink.bytes, (std::vector<uint8_t>{1, 2, 3}));
}

// A payload larger than the buffer is not worth splitting -- but it must not jump the queue.
TEST(BufferedPrintTest, OversizePayloadPreservesOrdering) {
  CountingSink sink;
  const std::vector<uint8_t> small = rowPattern(10, 1);
  const std::vector<uint8_t> big = rowPattern(9000, 7);
  {
    BufferedPrint out(sink, 128);
    out.write(small.data(), small.size());
    out.write(big.data(), big.size());
    EXPECT_TRUE(out.flushBuffer());
  }
  std::vector<uint8_t> expected = small;
  expected.insert(expected.end(), big.begin(), big.end());
  EXPECT_EQ(sink.bytes, expected);
}

// Single-byte writes go through the same buffer (Print's char-at-a-time API).
TEST(BufferedPrintTest, ByteWritesAreBufferedToo) {
  CountingSink sink;
  {
    BufferedPrint out(sink);
    for (int i = 0; i < 500; i++) out.write(static_cast<uint8_t>(i));
    EXPECT_TRUE(out.flushBuffer());
  }
  EXPECT_EQ(sink.bytes.size(), 500u);
  EXPECT_LE(sink.calls, 2);
}

// A failing sink must be reported, not swallowed -- the caller folds this into its own result
// so a truncated BMP is never cached as complete.
TEST(BufferedPrintTest, FlushReportsSinkFailure) {
  CountingSink sink;
  BufferedPrint out(sink);
  const uint8_t data[] = {9, 9, 9};
  out.write(data, sizeof(data));
  sink.failNext = true;
  EXPECT_FALSE(out.flushBuffer());
  // And it must not keep retrying the same bytes on every later flush.
  sink.failNext = false;
  EXPECT_TRUE(out.flushBuffer());
  EXPECT_TRUE(sink.bytes.empty());
}

}  // namespace
