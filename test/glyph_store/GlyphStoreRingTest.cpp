// The log as a ring of segments: each opened by its header, no record crossing a segment's end, the head found again
// at open, and a full ring that stops writes without a wipe.
#include <gtest/gtest.h>

#include <cstdint>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

constexpr uint32_t kFirst = 0x4E00;  // the code point of the first glyph each test stores

class GlyphStoreRing : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, layout)); }

  // Commits one glyph alone. 60 x 60 is a 920-byte record: four fill a 4 KB segment past its header.
  Commit commitOne(const uint32_t cp, const uint8_t width = 60, const uint8_t height = 60) {
    EXPECT_TRUE(stage(batch, makeKey(cp, 0, 0), width, height));
    return store.commit(batch);
  }

  SegmentHeader segmentHeader(const uint32_t segment) {
    SegmentHeader header{};
    EXPECT_TRUE(flash.read(layout.geometry.logOffset + segment * layout.geometry.segmentBytes, &header, sizeof header));
    return header;
  }

  void expectHeader(const uint32_t segment, const uint32_t epoch) {
    const SegmentHeader header = segmentHeader(segment);
    EXPECT_EQ(kSegmentMagic, header.magic) << segment;
    EXPECT_EQ(epoch, header.epoch) << segment;
    EXPECT_EQ(~epoch, header.epochCheck) << segment;
    EXPECT_EQ(kErased, header.retired) << segment;
  }

  uint32_t dataOffsetOf(const uint32_t cp) {
    const EpdGlyph* glyph = nullptr;
    EXPECT_EQ(Lookup::Found, store.find(makeKey(cp, 0, 0), &glyph)) << cp;
    return glyph ? glyph->dataOffset : 0;
  }

  uint32_t liveSegments() const { return store.segmentCount() - store.freeSegments(); }

  const Layout layout = smallLayout();
  alignas(4) uint8_t buffer[8192] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

}  // namespace

TEST_F(GlyphStoreRing, TheFirstRecordFollowsTheFirstSegmentsHeader) {
  EXPECT_EQ(16u, store.segmentCount());
  EXPECT_EQ(16u, store.freeSegments());  // the first commit opens the first segment
  ASSERT_EQ(Commit::Stored, commitOne('A', 9, 12));
  expectHeader(0, 1);
  EXPECT_EQ(kSegmentHeaderBytes + kRecordHeaderBytes, dataOffsetOf('A'));
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, 0), 9, 12));
  EXPECT_EQ(kSegmentHeaderBytes + recordSize(9, 12), store.logUsed());
  EXPECT_EQ(2u, store.epoch());
}

TEST_F(GlyphStoreRing, ARecordThatDoesNotFitTheHeadOpensTheNextSegment) {
  ASSERT_EQ(920u, recordSize(60, 60));
  for (uint32_t i = 0; i < 5; ++i) ASSERT_EQ(Commit::Stored, commitOne(kFirst + i)) << i;
  EXPECT_EQ(4096u + kSegmentHeaderBytes + kRecordHeaderBytes, dataOffsetOf(kFirst + 4));
  expectHeader(1, 2);
  for (uint32_t i = 0; i < 5; ++i) EXPECT_TRUE(storedIntact(store, makeKey(kFirst + i, 0, 0), 60, 60)) << i;
  EXPECT_EQ(2u, liveSegments());
}

TEST_F(GlyphStoreRing, ABatchAcrossTwoSegmentsIsStoredWhole) {
  for (uint32_t i = 0; i < 8; ++i) ASSERT_TRUE(stage(batch, makeKey(kFirst + i, 0, 0), 60, 60)) << i;
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (uint32_t i = 0; i < 8; ++i) EXPECT_TRUE(storedIntact(store, makeKey(kFirst + i, 0, 0), 60, 60)) << i;
  EXPECT_EQ(2u, store.segmentCount() - store.freeSegments());
  EXPECT_EQ(4096u + kSegmentHeaderBytes + kRecordHeaderBytes, dataOffsetOf(kFirst + 4));  // the head moved on
}

TEST_F(GlyphStoreRing, TheRingResumesAtItsHeadAfterAReopen) {
  // Ten records: four in each of the first two segments, two in the third.
  for (uint32_t i = 0; i < 10; ++i) ASSERT_EQ(Commit::Stored, commitOne(kFirst + i)) << i;
  const uint32_t last = dataOffsetOf(kFirst + 9);
  const uint32_t used = store.logUsed();
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(3u, liveSegments());
  EXPECT_EQ(used, store.logUsed());
  ASSERT_EQ(Commit::Stored, commitOne(kFirst + 10));
  EXPECT_EQ(last + recordSize(60, 60), dataOffsetOf(kFirst + 10));  // right after the last record, in the third
  EXPECT_EQ(2u, dataOffsetOf(kFirst + 10) / layout.geometry.segmentBytes);
  EXPECT_EQ(4u, store.epoch());
  for (uint32_t i = 0; i <= 10; ++i) EXPECT_TRUE(storedIntact(store, makeKey(kFirst + i, 0, 0), 60, 60)) << i;
}

TEST_F(GlyphStoreRing, AFullRingStopsWritesWithoutAWipe) {
  uint32_t stored = 0;
  for (;; ++stored) {
    ASSERT_LT(stored, 100u);
    const Commit result = commitOne(kFirst + stored);
    if (result != Commit::Stored) {
      EXPECT_EQ(Commit::Full, result);
      break;
    }
  }
  EXPECT_EQ(64u, stored);  // four in each of the 16 segments
  EXPECT_FALSE(store.wipePending());
  EXPECT_TRUE(store.writesStopped());
  EXPECT_TRUE(store.needsRoom());
  EXPECT_EQ(0u, store.freeSegments());
  for (uint32_t i = 0; i < stored; ++i) EXPECT_TRUE(storedIntact(store, makeKey(kFirst + i, 0, 0), 60, 60)) << i;
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey(kFirst + stored, 0, 0), nullptr));
}

TEST_F(GlyphStoreRing, ARecordLargerThanASegmentIsRefused) {
  ASSERT_EQ(Commit::Stored, commitOne(kFirst, 72, 120));  // a 2,160-byte bitmap
  EXPECT_TRUE(storedIntact(store, makeKey(kFirst, 0, 0), 72, 120));
  ASSERT_GT(recordSize(90, 200), layout.geometry.segmentBytes - kSegmentHeaderBytes);  // over a segment's payload
  const uint32_t calls = flash.programCalls();
  const uint32_t used = store.logUsed();
  EXPECT_EQ(Commit::Refused, commitOne(kFirst + 1, 90, 200));
  EXPECT_EQ(calls, flash.programCalls());  // nothing written
  EXPECT_EQ(used, store.logUsed());
  EXPECT_EQ(1u, liveSegments());
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey(kFirst + 1, 0, 0), nullptr));
  EXPECT_FALSE(store.writesStopped());
  EXPECT_FALSE(store.wipePending());
}

TEST_F(GlyphStoreRing, ATornSegmentHeaderIsPreparedAgain) {
  for (uint32_t i = 0; i < 4; ++i) ASSERT_EQ(Commit::Stored, commitOne(kFirst + i)) << i;  // the first is full
  const uint32_t used = store.logUsed();
  // The second segment is blank, so readying it erases nothing: the cut lands in its header's write.
  flash.cutAfterSteps(6);
  EXPECT_EQ(Commit::WriteFailed, commitOne(kFirst + 4));
  ASSERT_TRUE(flash.dead());
  flash.powerCycle();
  const SegmentHeader torn = segmentHeader(1);
  ASSERT_EQ(kSegmentMagic, torn.magic);
  ASSERT_NE(~torn.epoch, torn.epochCheck);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1u, liveSegments());  // the head is the first segment
  EXPECT_EQ(used, store.logUsed());
  EXPECT_EQ(2u, store.epoch());
  EXPECT_FALSE(store.wipePending());
  ASSERT_EQ(Commit::Stored, commitOne(kFirst + 4));
  expectHeader(1, 2);
  EXPECT_EQ(4096u + kSegmentHeaderBytes + kRecordHeaderBytes, dataOffsetOf(kFirst + 4));
  for (uint32_t i = 0; i < 5; ++i) EXPECT_TRUE(storedIntact(store, makeKey(kFirst + i, 0, 0), 60, 60)) << i;
}
