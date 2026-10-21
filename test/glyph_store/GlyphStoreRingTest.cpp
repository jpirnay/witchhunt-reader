// The log as a ring of segments: each opened by its header, no record crossing a segment's end, the head found again
// at open, and a full ring that stops writes without a wipe.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

using Budget = FlashGlyphStore::Budget;
using Room = FlashGlyphStore::Room;
using RoomWork = FlashGlyphStore::RoomWork;

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

  uint32_t segmentOf(const uint32_t cp) { return dataOffsetOf(cp) / layout.geometry.segmentBytes; }

  bool intact(const uint32_t cp) const { return storedIntact(store, makeKey(cp, 0, 0), 60, 60); }
  Lookup lookup(const uint32_t cp) const { return store.find(makeKey(cp, 0, 0), nullptr); }

  // Commits glyphs until the ring is full; returns how many were stored: four in each of the 16 segments.
  uint32_t fillUntilFull() {
    uint32_t stored = 0;
    while (stored < 100 && commitOne(kFirst + stored) == Commit::Stored) ++stored;
    EXPECT_EQ(64u, stored);
    return stored;
  }

  // markFull() then one idle step: the tail segment is reclaimed, however many are free.
  void reclaimTail() {
    ASSERT_TRUE(store.markFull());
    RoomWork work;
    ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
    ASSERT_EQ(1u, work.reclaimed);
  }

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

TEST_F(GlyphStoreRing, AFullRingIsFoundAgainAtOpen) {
  const uint32_t stored = fillUntilFull();
  const uint32_t used = store.logUsed();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(16u, liveSegments());
  EXPECT_EQ(used, store.logUsed());
  EXPECT_EQ(17u, store.epoch());  // the head is segment 15, opened with epoch 16
  for (uint32_t i = 0; i < stored; ++i) EXPECT_TRUE(intact(kFirst + i)) << i;
  // The tail is segment 0: a reclaim takes its four glyphs and nothing else.
  reclaimTail();
  for (uint32_t i = 0; i < 4; ++i) EXPECT_EQ(Lookup::NotStored, lookup(kFirst + i)) << i;
  for (uint32_t i = 4; i < stored; ++i) EXPECT_TRUE(intact(kFirst + i)) << i;
  // The head, full, moves into the segment after it: segment 0, with the next epoch.
  ASSERT_EQ(Commit::Stored, commitOne(kFirst + stored));
  EXPECT_EQ(kSegmentHeaderBytes + kRecordHeaderBytes, dataOffsetOf(kFirst + stored));
  expectHeader(0, 17);
}

TEST_F(GlyphStoreRing, AWrappedRingIsFoundAgainAtOpen) {
  const uint32_t stored = fillUntilFull();
  // Room is made (segments 0 and 1 are reclaimed), then two glyphs take the head round into segment 0: the head is now
  // before the tail, segment 2.
  ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Open));
  for (uint32_t i = stored; i < stored + 2; ++i) ASSERT_EQ(Commit::Stored, commitOne(kFirst + i)) << i;
  ASSERT_EQ(0u, segmentOf(kFirst + stored + 1));
  const uint32_t used = store.logUsed();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(15u, liveSegments());
  EXPECT_EQ(used, store.logUsed());
  EXPECT_EQ(18u, store.epoch());
  for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ(Lookup::NotStored, lookup(kFirst + i)) << i;
  for (uint32_t i = 8; i < stored + 2; ++i) EXPECT_TRUE(intact(kFirst + i)) << i;
  // The head resumes in segment 0, right after its two glyphs.
  ASSERT_EQ(Commit::Stored, commitOne(kFirst + stored + 2));
  EXPECT_EQ(kSegmentHeaderBytes + 2 * recordSize(60, 60) + kRecordHeaderBytes, dataOffsetOf(kFirst + stored + 2));
  // The tail is segment 2: a reclaim takes its four glyphs and nothing else.
  reclaimTail();
  for (uint32_t i = 8; i < 12; ++i) EXPECT_EQ(Lookup::NotStored, lookup(kFirst + i)) << i;
  for (uint32_t i = 12; i < stored + 3; ++i) EXPECT_TRUE(intact(kFirst + i)) << i;
}

TEST_F(GlyphStoreRing, ARetiredSegmentBehindTheTailIsFreeAtOpen) {
  // Ten full segments, then the three oldest reclaimed: the tail is segment 3.
  for (uint32_t i = 0; i < 40; ++i) ASSERT_EQ(Commit::Stored, commitOne(kFirst + i)) << i;
  for (uint32_t s = 0; s < 3; ++s) reclaimTail();
  // Segment 3 is reclaimed in turn and the power fails between its retire word and its erase: its header and glyphs
  // are whole, and the retire word alone says it left the ring.
  const uint32_t segment3 = layout.geometry.logOffset + 3 * layout.geometry.segmentBytes;
  const uint32_t retired = 0;
  ASSERT_TRUE(flash.program(segment3 + offsetof(SegmentHeader, retired), &retired, sizeof retired));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(6u, liveSegments());  // 4 to 9
  EXPECT_EQ(11u, store.epoch());
  EXPECT_EQ(6 * layout.geometry.segmentBytes - 400, store.logUsed());  // the head, segment 9, ends 400 bytes short
  for (uint32_t i = 0; i < 16; ++i) EXPECT_EQ(Lookup::NotStored, lookup(kFirst + i)) << i;
  for (uint32_t i = 16; i < 40; ++i) EXPECT_TRUE(intact(kFirst + i)) << i;
  // The head goes round the free segments; segment 3, the last of them, is erased before it gets its new header.
  const uint32_t sector = segment3 / kSectorBytes;
  const uint32_t erases = flash.sectorErases(sector);
  uint32_t n = 40;
  do {
    ASSERT_EQ(Commit::Stored, commitOne(kFirst + n)) << n;
  } while (segmentOf(kFirst + n++) != 3 && n < 100);
  EXPECT_EQ(77u, n);  // nine segments of four, then the first in segment 3
  EXPECT_EQ(erases + 1, flash.sectorErases(sector));
  expectHeader(3, 20);
  for (uint32_t i = 16; i < n; ++i) EXPECT_TRUE(intact(kFirst + i)) << i;
}
