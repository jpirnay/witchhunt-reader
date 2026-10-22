// Host tests for the pure Range helpers behind chunked downloads (lib/SecureNet HttpRange).

#include <HttpRange.h>
#include <gtest/gtest.h>

#include <vector>

namespace hr = crosspoint::http_range;

TEST(HttpRangeHeader, FormatsAnInclusiveByteRange) {
  EXPECT_EQ(hr::rangeHeaderValue(0, 8191), "bytes=0-8191");
  EXPECT_EQ(hr::rangeHeaderValue(262144, 269829), "bytes=262144-269829");
  EXPECT_EQ(hr::rangeHeaderValue(7, 7), "bytes=7-7");
}

TEST(HttpContentRange, ParsesARangeWithItsTotal) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 0-8191/269830", r));
  EXPECT_TRUE(r.satisfied);
  EXPECT_EQ(r.first, 0u);
  EXPECT_EQ(r.last, 8191u);
  EXPECT_TRUE(r.totalKnown);
  EXPECT_EQ(r.total, 269830u);
}

TEST(HttpContentRange, ParsesAnUnknownTotal) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 8192-16383/*", r));
  EXPECT_TRUE(r.satisfied);
  EXPECT_EQ(r.first, 8192u);
  EXPECT_EQ(r.last, 16383u);
  EXPECT_FALSE(r.totalKnown);
}

TEST(HttpContentRange, ParsesTheUnsatisfiedForm) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes */269830", r));
  EXPECT_FALSE(r.satisfied);
  EXPECT_TRUE(r.totalKnown);
  EXPECT_EQ(r.total, 269830u);
}

TEST(HttpContentRange, AcceptsAnyUnitCaseAndSurroundingSpaces) {
  hr::ContentRange r;
  EXPECT_TRUE(hr::parseContentRange("  Bytes 0-0/1 ", r));
  EXPECT_TRUE(hr::parseContentRange("BYTES   10-19/20", r));
  EXPECT_EQ(r.first, 10u);
}

TEST(HttpContentRange, RejectsMalformedValues) {
  hr::ContentRange r;
  const std::vector<const char*> bad = {
      "",                                   // empty
      "bytes",                              // no range
      "byte 0-1/2",                         // wrong unit
      "items 0-1/2",                        // other unit
      "bytes0-1/2",                         // no space after the unit
      "bytes 0-1",                          // no total
      "bytes 5-4/10",                       // last before first
      "bytes 0-10/10",                      // last past the end
      "bytes */*",                          // says nothing
      "bytes -1/10",                        // no first
      "bytes 0-/10",                        // no last
      "bytes 0-1/2x",                       // trailing garbage
      "bytes 0-1/-2",                       // negative total
      "bytes 1-2/99999999999999999999999",  // does not fit
  };
  for (const char* value : bad) {
    EXPECT_FALSE(hr::parseContentRange(value, r)) << value;
    EXPECT_FALSE(r.satisfied) << value;
    EXPECT_FALSE(r.totalKnown) << value;
  }
  EXPECT_FALSE(hr::parseContentRange(nullptr, r));
}

TEST(HttpRangeReply, A206ForTheRequestedStartIsPartial) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 8192-16383/269830", r));
  EXPECT_EQ(hr::classifyReply(206, true, r, 8192), hr::RangeReply::Partial);
}

TEST(HttpRangeReply, A206ForAnotherStartOrWithoutContentRangeIsAMismatch) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 0-8191/269830", r));
  EXPECT_EQ(hr::classifyReply(206, true, r, 8192), hr::RangeReply::Mismatch);
  EXPECT_EQ(hr::classifyReply(206, false, hr::ContentRange{}, 0), hr::RangeReply::Mismatch);
  ASSERT_TRUE(hr::parseContentRange("bytes */269830", r));
  EXPECT_EQ(hr::classifyReply(206, true, r, 0), hr::RangeReply::Mismatch);
}

TEST(HttpRangeReply, A200MeansTheServerIgnoredRange) {
  // The 200-instead-of-206 fallback: whatever was written must be rewound before this body.
  EXPECT_EQ(hr::classifyReply(200, false, hr::ContentRange{}, 0), hr::RangeReply::WholeBody);
  EXPECT_EQ(hr::classifyReply(200, false, hr::ContentRange{}, 8192), hr::RangeReply::WholeBody);
}

TEST(HttpRangeReply, OtherStatuses) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes */16384", r));
  EXPECT_EQ(hr::classifyReply(416, true, r, 16384), hr::RangeReply::Unsatisfiable);
  EXPECT_EQ(hr::classifyReply(404, false, hr::ContentRange{}, 0), hr::RangeReply::Error);
  EXPECT_EQ(hr::classifyReply(500, false, hr::ContentRange{}, 0), hr::RangeReply::Error);
  EXPECT_EQ(hr::classifyReply(302, false, hr::ContentRange{}, 0), hr::RangeReply::Error);
}

TEST(HttpRangeChunks, PlansOffsetsUpToAShortFinalChunk) {
  // 20,000 bytes in 8 KB chunks: two full chunks, then the 3,616-byte remainder.
  std::vector<hr::Chunk> plan;
  size_t offset = 0;
  hr::Chunk c;
  while (hr::nextChunk(offset, 8192, true, 20000, c)) {
    plan.push_back(c);
    offset += c.length();
  }
  ASSERT_EQ(plan.size(), 3u);
  EXPECT_EQ(plan[0].first, 0u);
  EXPECT_EQ(plan[0].last, 8191u);
  EXPECT_EQ(plan[1].first, 8192u);
  EXPECT_EQ(plan[1].last, 16383u);
  EXPECT_EQ(plan[2].first, 16384u);
  EXPECT_EQ(plan[2].last, 19999u);
  EXPECT_EQ(plan[2].length(), 3616u);
  EXPECT_EQ(offset, 20000u);
}

TEST(HttpRangeChunks, AnExactMultipleEndsOnAFullChunk) {
  hr::Chunk c;
  ASSERT_TRUE(hr::nextChunk(8192, 8192, true, 16384, c));
  EXPECT_EQ(c.last, 16383u);
  EXPECT_FALSE(hr::nextChunk(16384, 8192, true, 16384, c));
}

TEST(HttpRangeChunks, AnUnknownTotalAsksForFullChunksAndNeverCompletesByItself) {
  hr::Chunk c;
  ASSERT_TRUE(hr::nextChunk(0, 4096, false, 0, c));
  EXPECT_EQ(c.first, 0u);
  EXPECT_EQ(c.last, 4095u);
  ASSERT_TRUE(hr::nextChunk(4096, 4096, false, 0, c));
  EXPECT_EQ(c.last, 8191u);
  // Without a total nothing is ever complete by itself, not even after a short chunk: only a 416
  // for the next byte ends it (the download loop checks that).
  EXPECT_FALSE(hr::transferComplete(8192, false, 0));
  EXPECT_FALSE(hr::transferComplete(9000, false, 0));
}

TEST(HttpRangeChunks, AKnownTotalCompletesAtTheTotal) {
  EXPECT_FALSE(hr::transferComplete(8192, true, 20000));
  EXPECT_TRUE(hr::transferComplete(20000, true, 20000));
}

TEST(HttpRangeChunks, AZeroChunkPlansNothing) {
  hr::Chunk c;
  EXPECT_FALSE(hr::nextChunk(0, 0, false, 0, c));
}

TEST(HttpRangeHeap, RoundsLikeTheC3Heap) {
  // A full TLS 1.3 record (2^14 + 17) occupies the 17,408 B block seen on the device.
  EXPECT_EQ(hr::heapBlockFor(16401), 17408u);
  // An 8 KB range sent as one record (8,209 B, measured from GitHub) needs 8,704 B.
  EXPECT_EQ(hr::heapBlockFor(8209), 8704u);
  EXPECT_EQ(hr::heapBlockFor(1), 16u);
}

TEST(HttpRangeHeap, ChunkSizeFollowsTheLargestFreeBlock) {
  // X3: 16,372 B after a handshake, 12,276 B before the manifest, 4.6-6.1 KB once a long
  // download had fragmented the heap.
  EXPECT_EQ(hr::ChunkSizer().next(16372), 6144u);
  EXPECT_EQ(hr::ChunkSizer().next(12276), 6144u);
  EXPECT_EQ(hr::ChunkSizer().next(9000), 4096u);
  EXPECT_EQ(hr::ChunkSizer().next(7680), 3072u);
  EXPECT_EQ(hr::ChunkSizer().next(6656), 2048u);
  EXPECT_EQ(hr::ChunkSizer().next(6132), 1024u);
  // Nothing fits: the smallest chunk is still the best chance.
  EXPECT_EQ(hr::ChunkSizer().next(4596), hr::MIN_CHUNK_BYTES);
}

TEST(HttpRangeHeap, EachSizeStartsExactlyAtItsBudget) {
  for (size_t i = 0; i < hr::CHUNK_LADDER_SIZE; ++i) {
    const size_t chunk = hr::CHUNK_LADDER[i];
    EXPECT_EQ(hr::ChunkSizer().next(hr::chunkBudget(chunk)), chunk) << chunk;
    if (i + 1 < hr::CHUNK_LADDER_SIZE) {
      EXPECT_EQ(hr::ChunkSizer().next(hr::chunkBudget(chunk) - 1), hr::CHUNK_LADDER[i + 1]) << chunk;
    }
  }
}

TEST(HttpRangeHeap, ChunksOnlyWhenTheLargestBlockIsSmall) {
  EXPECT_TRUE(hr::shouldChunk(23540));  // X3 Font Manager before the connect
  EXPECT_TRUE(hr::shouldChunk(40 * 1024 - 1));
  EXPECT_FALSE(hr::shouldChunk(40 * 1024));
  EXPECT_FALSE(hr::shouldChunk(4 * 1024 * 1024));  // S3 with PSRAM in the default heap
}

TEST(HttpRangeSizer, DownloadsStartAt6KB) {
  // Device round 2: every first 8 KB request failed on the X3 while 6 KB ran cleanly.
  EXPECT_EQ(hr::DEFAULT_CHUNK_BYTES, 6144u);
  EXPECT_EQ(hr::CHUNK_LADDER[0], 6144u);
  EXPECT_EQ(hr::ChunkSizer().next(17396), 6144u);    // the reading that let 8 KB through
  EXPECT_EQ(hr::ChunkSizer().next(1 << 20), 6144u);  // however large the block
}

TEST(HttpRangeSizer, SizesEveryRequestFromTheBlockAtThatMoment) {
  hr::ChunkSizer sizer;
  EXPECT_EQ(sizer.next(16372), 6144u);
  EXPECT_EQ(sizer.next(9000), 4096u);   // the heap fragmented: smaller
  EXPECT_EQ(sizer.next(16372), 6144u);  // and back up to the ceiling, which nothing has lowered
}

TEST(HttpRangeSizer, AnOutOfMemoryReadLowersTheCeilingForGood) {
  // Device round 2: 8 KB was let through at 17,396 B and failed every time. The same holds for
  // whatever size fails now.
  hr::ChunkSizer sizer;
  EXPECT_EQ(sizer.next(17396), 6144u);
  sizer.onOutOfMemory();
  EXPECT_EQ(sizer.ceiling(), 4096u);
  // However many clean requests follow, and however large the block reads, 6 KB stays out of reach.
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(sizer.next(17396), 4096u) << i;
  EXPECT_EQ(sizer.next(1 << 20), 4096u);
  EXPECT_EQ(sizer.ceiling(), 4096u);
  // A smaller block still wins over the ceiling.
  EXPECT_EQ(sizer.next(6132), 1024u);
}

TEST(HttpRangeSizer, AnOutOfMemoryReadBelowTheCeilingStepsBelowThatSize) {
  hr::ChunkSizer sizer;
  EXPECT_EQ(sizer.next(7680), 3072u);  // the heap, not the ceiling, chose 3 KB
  sizer.onOutOfMemory();
  EXPECT_EQ(sizer.ceiling(), 2048u);
  EXPECT_EQ(sizer.next(16372), 2048u);
}

TEST(HttpRangeSizer, ACeilingIsRoundedDownToALadderSize) {
  EXPECT_EQ(hr::ChunkSizer(6144).ceiling(), 6144u);
  EXPECT_EQ(hr::ChunkSizer(8192).ceiling(), 6144u);  // a ceiling above the ladder is its top
  EXPECT_EQ(hr::ChunkSizer(100000).ceiling(), 6144u);
  EXPECT_EQ(hr::ChunkSizer(5000).ceiling(), 4096u);
  EXPECT_EQ(hr::ChunkSizer(0).ceiling(), hr::MIN_CHUNK_BYTES);
}

TEST(HttpRangeSizer, TheSmallestSizeIsAFloor) {
  hr::ChunkSizer sizer;
  EXPECT_EQ(sizer.next(4596), 1024u);
  sizer.onOutOfMemory();
  sizer.onOutOfMemory();
  EXPECT_EQ(sizer.ceiling(), 1024u);
  EXPECT_EQ(sizer.next(16372), 1024u);
}

TEST(HttpRangeRetries, TheHardCapScalesWithTheFile) {
  EXPECT_EQ(hr::maxReconnectsFor(0), 20u);
  EXPECT_EQ(hr::maxReconnectsFor(64 * 1024 - 1), 20u);
  EXPECT_EQ(hr::maxReconnectsFor(64 * 1024), 21u);
  // Arimo_18 (987,450 B) used exactly the old flat cap of 20 while still making progress.
  EXPECT_EQ(hr::maxReconnectsFor(987450), 35u);
}

TEST(HttpRangeRetries, FailuresThatWroteBytesDoNotCountAsStalls) {
  hr::RetryBudget retries;
  const size_t file = 987450;
  const unsigned cap = hr::maxReconnectsFor(file);
  for (unsigned i = 0; i < cap; ++i) EXPECT_TRUE(retries.reconnectAfterFailure(true, file)) << i;
  // ...but the hard cap still ends it.
  EXPECT_FALSE(retries.reconnectAfterFailure(true, file));
  EXPECT_EQ(retries.reconnects(), cap);
}

TEST(HttpRangeRetries, ASmallFileKeepsTheBaseCap) {
  hr::RetryBudget retries;
  for (unsigned i = 0; i < hr::MAX_RECONNECTS_BASE; ++i) EXPECT_TRUE(retries.reconnectAfterFailure(true, 24089)) << i;
  EXPECT_FALSE(retries.reconnectAfterFailure(true, 24089));
}

TEST(HttpRangeRetries, ConsecutiveStallsEndTheDownload) {
  hr::RetryBudget retries;
  for (unsigned i = 0; i < hr::MAX_STALLED_RETRIES; ++i) EXPECT_TRUE(retries.reconnectAfterFailure(false, 987450)) << i;
  EXPECT_FALSE(retries.reconnectAfterFailure(false, 987450));
}

TEST(HttpRangeRetries, AnyNewBytesResetTheStallCount) {
  hr::RetryBudget retries;
  const size_t file = 987450;
  EXPECT_TRUE(retries.reconnectAfterFailure(false, file));
  EXPECT_TRUE(retries.reconnectAfterFailure(false, file));
  EXPECT_TRUE(retries.reconnectAfterFailure(true, file));  // this failed request still wrote bytes
  for (unsigned i = 0; i < hr::MAX_STALLED_RETRIES; ++i) EXPECT_TRUE(retries.reconnectAfterFailure(false, file)) << i;
  retries.onProgress();  // a request that completed with new bytes
  for (unsigned i = 0; i < hr::MAX_STALLED_RETRIES; ++i) EXPECT_TRUE(retries.reconnectAfterFailure(false, file)) << i;
  EXPECT_FALSE(retries.reconnectAfterFailure(false, file));
  EXPECT_EQ(retries.reconnects(), 3 + 2 * hr::MAX_STALLED_RETRIES);
}
