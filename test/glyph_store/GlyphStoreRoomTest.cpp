// Making room by keeping what is drawn: the use counts, kept only once the ring nears full, and the reclaim of the
// oldest segment that copies the drawn glyphs forward and drops the rest.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

using Budget = FlashGlyphStore::Budget;
using Room = FlashGlyphStore::Room;
using RoomWork = FlashGlyphStore::RoomWork;

constexpr uint32_t kFirst = 0x4E00;  // the code point of the first glyph each test stores
// The reclaim tests' glyphs: a 600-byte record, six to a 4 KB segment (3,616 of its 4,096 bytes).
constexpr uint8_t kWidth = 40;
constexpr uint8_t kHeight = 58;

class GlyphStoreRoom : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, layout)); }

  // Commits one glyph alone. 60 x 60 is a 920-byte record: four fill a 4 KB segment past its header.
  Commit commitOne(const uint32_t cp) {
    EXPECT_TRUE(stage(batch, makeKey(cp, 0, 0), 60, 60));
    return store.commit(batch);
  }

  // Commits glyphs until `free` segments are free; returns how many were stored.
  uint32_t fillTo(const uint32_t free) {
    uint32_t n = 0;
    while (store.freeSegments() > free && n < 200) {
      EXPECT_EQ(Commit::Stored, commitOne(kFirst + n)) << n;
      ++n;
    }
    return n;
  }

  const EpdGlyph* glyphOf(const uint32_t cp) {
    const EpdGlyph* glyph = nullptr;
    EXPECT_EQ(Lookup::Found, store.find(makeKey(cp, 0, 0), &glyph)) << cp;
    return glyph;
  }

  // --- The reclaim tests: record n is a kWidth x kHeight glyph under keyOf(n). ---

  static uint32_t keyOf(const uint32_t n) { return makeKey(kFirst + n, 0, 0); }

  Commit commitRecord(const uint32_t n, const uint8_t width = kWidth, const uint8_t height = kHeight) {
    EXPECT_TRUE(stage(batch, keyOf(n), width, height)) << n;
    return store.commit(batch);
  }

  bool intact(const uint32_t n) const { return storedIntact(store, keyOf(n), kWidth, kHeight); }
  Lookup lookup(const uint32_t n) const { return store.find(keyOf(n), nullptr); }
  uint8_t countOf(const uint32_t n) const { return store.useCount(keyOf(n)); }

  uint32_t dataOffsetOf(const uint32_t n) const {
    const EpdGlyph* glyph = nullptr;
    EXPECT_EQ(Lookup::Found, store.find(keyOf(n), &glyph)) << n;
    return glyph ? glyph->dataOffset : 0;
  }

  uint32_t segmentOf(const uint32_t n) const { return dataOffsetOf(n) / layout.geometry.segmentBytes; }

  void draw(const uint32_t n, const uint32_t times = 1) {
    const EpdGlyph* glyph = nullptr;
    ASSERT_EQ(Lookup::Found, store.find(keyOf(n), &glyph)) << n;
    for (uint32_t i = 0; i < times; ++i) store.noteDrawn(glyph);
  }

  // Fills the ring from record 0 until one segment is free: the counts are kept, the tail is segment 0 (records 0-5)
  // and copies have room. 85 records: segments 0-13 full, one record in segment 14.
  uint32_t fillRing() {
    uint32_t n = 0;
    while (store.freeSegments() > 1 && n < 200) {
      EXPECT_EQ(Commit::Stored, commitRecord(n)) << n;
      ++n;
    }
    EXPECT_EQ(85u, n);
    EXPECT_TRUE(store.countsKept());
    return n;
  }

  // Commits records from `n` on until the ring has no room for the next one; returns that one's n.
  uint32_t fillUntilFull(uint32_t n) {
    while (n < 1000) {
      const Commit commit = commitRecord(n);
      if (commit != Commit::Stored) {
        EXPECT_EQ(Commit::Full, commit) << n;
        break;
      }
      ++n;
    }
    return n;
  }

  // markFull() then one idle step: the tail segment reclaimed, however many are free.
  RoomWork reclaimOne() {
    RoomWork work;
    EXPECT_TRUE(store.markFull());
    EXPECT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
    EXPECT_EQ(1u, work.reclaimed);
    return work;
  }

  const Layout layout = smallLayout();
  alignas(4) uint8_t buffer[8192] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

}  // namespace

TEST_F(GlyphStoreRoom, NoCountsAreKeptWhileTheRingHasRoom) {
  ASSERT_EQ(Commit::Stored, commitOne(kFirst));
  EXPECT_FALSE(store.countsKept());
  store.noteDrawn(glyphOf(kFirst));
  EXPECT_EQ(0u, store.useCount(makeKey(kFirst, 0, 0)));
  EXPECT_EQ(0u, store.drawsCounted());
}

TEST_F(GlyphStoreRoom, CountsAreKeptOnceTheRingNearsFull) {
  fillTo(kFreeSegmentsTarget + 3);  // 11 of 16 segments live: under 75%, and 5 free
  EXPECT_FALSE(store.countsKept());
  fillTo(kFreeSegmentsTarget + 1);
  EXPECT_TRUE(store.countsKept());
}

TEST_F(GlyphStoreRoom, DrawsAreCountedUpToThree) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsKept());
  const EpdGlyph* glyph = glyphOf(kFirst);
  const uint32_t key = makeKey(kFirst, 0, 0);
  const uint8_t want[] = {0, 1, 2, 3, 3};
  uint32_t drawn = 0;
  for (const uint8_t count : want) {
    EXPECT_EQ(count, store.useCount(key)) << drawn;
    store.noteDrawn(glyph);
    ++drawn;
  }
  EXPECT_EQ(3u, store.useCount(key));
  EXPECT_EQ(5u, store.drawsCounted());
  // Another glyph is untouched.
  EXPECT_EQ(0u, store.useCount(makeKey(kFirst + 1, 0, 0)));
}

TEST_F(GlyphStoreRoom, ADrawOfSomethingElseIsIgnored) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsKept());
  const uint32_t before = store.drawsCounted();
  EpdGlyph onStack{};
  store.noteDrawn(&onStack);
  store.noteDrawn(nullptr);
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(store.logBase() + store.geometry().logSize + 4));
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(store.logBase() + store.geometry().logSize));
  // Inside the log, but not at a record: a segment header, and the middle of a bitmap.
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(store.logBase() + 4));
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(glyphOf(kFirst)->dataOffset + store.logBase() + 8));
  EXPECT_EQ(before, store.drawsCounted());
  for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ(0u, store.useCount(makeKey(kFirst + i, 0, 0))) << i;
}

TEST_F(GlyphStoreRoom, CountsStartAtZeroAfterAReopen) {
  fillTo(kFreeSegmentsTarget + 1);
  const uint32_t key = makeKey(kFirst, 0, 0);
  store.noteDrawn(glyphOf(kFirst));
  ASSERT_EQ(1u, store.useCount(key));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_FALSE(store.countsKept());
  EXPECT_EQ(0u, store.useCount(key));
  EXPECT_EQ(0u, store.drawsCounted());
}

TEST_F(GlyphStoreRoom, CountsAreFreedByAWipe) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsKept());
  ASSERT_TRUE(store.wipe());
  EXPECT_FALSE(store.countsKept());
}

// --- The reclaim ---

TEST_F(GlyphStoreRoom, AReclaimKeepsDrawnGlyphsAndDropsTheRest) {
  const uint32_t stored = fillRing();
  ASSERT_TRUE(store.needsRoom());  // one segment free, under the target
  draw(0);
  draw(3);
  const uint32_t usedSlots = store.usedSlots();
  RoomWork work;
  const Room room = store.makeRoom(Budget::Idle, &work);
  EXPECT_TRUE(room == Room::Enough || room == Room::More);
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_EQ(2u, work.kept);
  EXPECT_EQ(4u, work.dropped);
  EXPECT_FALSE(work.rebuilt);
  EXPECT_TRUE(intact(0));
  EXPECT_TRUE(intact(3));
  for (const uint32_t n : {1u, 2u, 4u, 5u}) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
  for (uint32_t n = 6; n < stored; ++n) EXPECT_TRUE(intact(n)) << n;
  // The copies took new slots; segment 0's six went stale.
  EXPECT_EQ(usedSlots + 2, store.usedSlots());
  EXPECT_EQ(stored - 4, store.liveSlots());
  EXPECT_FALSE(store.wipePending());
  EXPECT_FALSE(store.writesStopped());
  ASSERT_EQ(Commit::Stored, commitRecord(stored));
  EXPECT_TRUE(intact(stored));
}

TEST_F(GlyphStoreRoom, AReclaimHalvesTheCountsOfWhatItKeeps) {
  uint32_t n = fillRing();
  draw(0, 3);
  draw(1, 2);
  draw(2, 1);
  RoomWork work;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
  ASSERT_EQ(3u, work.kept);
  EXPECT_EQ(1u, countOf(0));
  EXPECT_EQ(1u, countOf(1));
  EXPECT_EQ(0u, countOf(2));
  for (uint32_t k = 0; k < 3; ++k) EXPECT_TRUE(intact(k)) << k;
  // The copies went to the head. Move the head past their segment, then reclaim up to it: the ones in between keep
  // all three, and the reclaim of the copies' segment drops the one at 0.
  const uint32_t copies = segmentOf(2);
  ASSERT_EQ(copies, segmentOf(0));
  do {
    ASSERT_EQ(Commit::Stored, commitRecord(n));
  } while (segmentOf(n++) == copies);
  for (uint32_t s = 1; s < copies; ++s) {
    EXPECT_EQ(0u, reclaimOne().kept) << s;
    EXPECT_TRUE(intact(2)) << s;
  }
  EXPECT_EQ(2u, reclaimOne().kept);
  EXPECT_EQ(Lookup::NotStored, lookup(2));
  EXPECT_TRUE(intact(0));
  EXPECT_TRUE(intact(1));
  EXPECT_EQ(0u, countOf(0));
  EXPECT_EQ(0u, countOf(1));
}

TEST_F(GlyphStoreRoom, AReclaimFreesAtLeastHalfASegment) {
  fillRing();
  for (uint32_t n = 0; n < 6; ++n) draw(n);
  draw(1);
  draw(2);
  RoomWork work;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
  // 1,800 B kept of the 4,080-byte payload: the two drawn twice, then the newest of the rest. A fourth would pass
  // half the payload.
  EXPECT_EQ(3u, work.kept);
  EXPECT_EQ(3u, work.dropped);
  EXPECT_LE(3 * recordSize(kWidth, kHeight), (layout.geometry.segmentBytes - kSegmentHeaderBytes) / 2);
  EXPECT_GT(4 * recordSize(kWidth, kHeight), (layout.geometry.segmentBytes - kSegmentHeaderBytes) / 2);
  EXPECT_TRUE(intact(1));
  EXPECT_TRUE(intact(2));
  EXPECT_TRUE(intact(5));
  for (const uint32_t n : {0u, 3u, 4u}) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
}

TEST_F(GlyphStoreRoom, AStaleSlotIsNeverServedFromAReusedSegment) {
  const uint32_t first = fillRing();
  std::vector<uint32_t> offsets;  // where segment 0's first records were
  for (uint32_t n = 0; n < 6; ++n) offsets.push_back(dataOffsetOf(n));
  RoomWork work;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
  ASSERT_EQ(0u, work.kept);
  ASSERT_EQ(6u, work.dropped);
  // Other keys of the same size, until the head has come round and filled segment 0 again at the same offsets.
  uint32_t n = first;
  uint32_t refilled = 0;
  while (refilled < offsets.size() && n < first + 200) {
    Commit commit = commitRecord(n);
    if (commit == Commit::Full) {
      ASSERT_NE(Room::Failed, store.makeRoom(Budget::Open));
      commit = commitRecord(n);
    }
    ASSERT_EQ(Commit::Stored, commit) << n;
    if (segmentOf(n) == 0) {
      EXPECT_EQ(offsets[refilled], dataOffsetOf(n)) << n;
      ++refilled;
    }
    ++n;
  }
  ASSERT_EQ(offsets.size(), refilled);
  // Segment 0's first keys still have their slots, which point at the new records' offsets: never served.
  for (uint32_t k = 0; k < offsets.size(); ++k) EXPECT_EQ(Lookup::NotStored, lookup(k)) << k;
  for (uint32_t k = first; k < n; ++k) EXPECT_TRUE(intact(k)) << k;
  for (uint32_t k = offsets.size(); k < first; ++k) {
    EXPECT_TRUE(absentOrIntact(store, keyOf(k), kWidth, kHeight)) << k;
  }
}

TEST_F(GlyphStoreRoom, AStaleSlotNeverServesATornRewriteOfItsGlyph) {
  const uint32_t first = fillRing();
  RoomWork work;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
  ASSERT_EQ(6u, work.dropped);
  // Fill segments 14 and 15 (five records and six), so segment 0 is the next the head moves into.
  uint32_t n = first;
  for (; n < first + 11; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  ASSERT_EQ(15u, segmentOf(n - 1));
  ASSERT_EQ(15u, segmentOf(n - 6));
  ASSERT_EQ(1u, store.freeSegments());
  // The head moves into segment 0, and a write of record 0's glyph, at its old offset, is cut in its bitmap. Record
  // 0's old slot names that offset: only the epoch tells it is stale.
  writeSegmentHeader(flash, layout, 0, store.epoch());
  writeRawRecord(flash, layout, inSegment(layout, 0, 0), keyOf(0), kWidth, kHeight, 100);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(Lookup::NotStored, lookup(0));
  for (uint32_t k = 1; k < 6; ++k) EXPECT_EQ(Lookup::NotStored, lookup(k)) << k;
  for (uint32_t k = 6; k < n; ++k) EXPECT_TRUE(intact(k)) << k;
  // Stored again, after the torn record, it is found.
  ASSERT_EQ(Commit::Stored, commitRecord(0));
  EXPECT_TRUE(intact(0));
}

TEST_F(GlyphStoreRoom, AFullRingsFirstReclaimDropsItsOldestWhole) {
  const uint32_t stored = fillUntilFull(0);
  ASSERT_EQ(96u, stored);
  ASSERT_EQ(0u, store.freeSegments());
  ASSERT_TRUE(store.countsKept());
  for (uint32_t n = 0; n < 12; ++n) draw(n);  // segments 0 and 1
  // The head's 480 bytes take no record, and no segment is free: nothing of segment 0 can be kept.
  RoomWork work;
  EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_EQ(0u, work.kept);
  EXPECT_EQ(6u, work.dropped);
  EXPECT_EQ(1u, store.freeSegments());
  EXPECT_FALSE(store.writesStopped());
  for (uint32_t n = 0; n < 6; ++n) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
  // The next one copies into the free segment: segment 1's newest drawn glyphs, up to half a segment.
  RoomWork next;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &next));
  EXPECT_EQ(1u, next.reclaimed);
  EXPECT_EQ(3u, next.kept);
  EXPECT_EQ(3u, next.dropped);
  for (uint32_t n = 9; n < 12; ++n) EXPECT_TRUE(intact(n)) << n;
  for (uint32_t n = 6; n < 9; ++n) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
  for (uint32_t n = 12; n < stored; ++n) EXPECT_TRUE(intact(n)) << n;
}

TEST_F(GlyphStoreRoom, AReclaimBeforeTheCountsAreKeptDropsTheSegmentWhole) {
  for (uint32_t n = 0; n < 18; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;  // three segments
  ASSERT_FALSE(store.countsKept());
  draw(0);  // not counted: no counts yet
  ASSERT_TRUE(store.markFull());
  EXPECT_TRUE(store.writesStopped());
  EXPECT_TRUE(store.needsRoom());
  EXPECT_FALSE(store.wipePending());
  EXPECT_TRUE(store.countsKept());
  EXPECT_EQ(Commit::Full, commitRecord(18));
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_EQ(0u, work.kept);
  EXPECT_EQ(6u, work.dropped);
  for (uint32_t n = 0; n < 6; ++n) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
  for (uint32_t n = 6; n < 18; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_FALSE(store.wipePending());
  EXPECT_FALSE(store.writesStopped());
  EXPECT_FALSE(store.needsRoom());
  EXPECT_EQ(Commit::Stored, commitRecord(18));
}

TEST_F(GlyphStoreRoom, ABatchLargerThanTheFreeSegmentsAsksForRoom) {
  for (uint32_t n = 0; n < 84; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;  // fourteen full segments
  ASSERT_EQ(kFreeSegmentsTarget, store.freeSegments());
  ASSERT_FALSE(store.needsRoom());
  // Thirteen records need three new segments; two are free.
  for (uint32_t n = 84; n < 97; ++n) ASSERT_TRUE(stage(batch, keyOf(n), kWidth, kHeight)) << n;
  ASSERT_EQ(Commit::Full, store.commit(batch));
  EXPECT_TRUE(store.writesStopped());
  EXPECT_TRUE(store.needsRoom());
  EXPECT_FALSE(store.wipePending());
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_FALSE(store.writesStopped());
  for (uint32_t n = 84; n < 97; ++n) ASSERT_TRUE(stage(batch, keyOf(n), kWidth, kHeight)) << n;
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (uint32_t n = 84; n < 97; ++n) EXPECT_TRUE(intact(n)) << n;
}

TEST_F(GlyphStoreRoom, MarkFullWithOneSegmentLiveResumesWritesWithoutAReclaim) {
  ASSERT_EQ(Commit::Stored, commitRecord(0));
  ASSERT_TRUE(store.markFull());
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(0u, work.reclaimed);
  EXPECT_TRUE(intact(0));
  EXPECT_FALSE(store.writesStopped());
  EXPECT_EQ(Commit::Stored, commitRecord(1));
}

TEST_F(GlyphStoreRoom, OpenMakesRoomUntilEnough) {
  const uint32_t stored = fillUntilFull(0);
  ASSERT_EQ(0u, store.freeSegments());
  ASSERT_TRUE(store.needsRoom());
  ASSERT_TRUE(store.writesStopped());
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_EQ(kFreeSegmentsTarget, work.reclaimed);
  EXPECT_GE(store.freeSegments(), kFreeSegmentsTarget);
  EXPECT_FALSE(store.needsRoom());
  EXPECT_FALSE(store.writesStopped());
  ASSERT_EQ(Commit::Stored, commitRecord(stored));
  EXPECT_TRUE(intact(stored));
}

TEST_F(GlyphStoreRoom, IdleMakesOneStepAtATime) {
  fillUntilFull(0);
  RoomWork work;
  EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(1u, work.reclaimed);  // what this call did
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(0u, work.reclaimed);  // nothing needed
  EXPECT_EQ(kFreeSegmentsTarget, store.freeSegments());
}

TEST_F(GlyphStoreRoom, AFailedReclaimSetsTheWipePending) {
  fillRing();
  draw(0);
  flash.failNextProgram();  // the copy
  RoomWork work;
  EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Idle, &work));
  EXPECT_TRUE(store.wipePending());
  EXPECT_TRUE(store.writesStopped());
  EXPECT_EQ(0u, work.reclaimed);
  EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Open));  // only a wipe helps now
  ASSERT_TRUE(store.wipe());
  ASSERT_TRUE(claimTestSizes(store));
  EXPECT_EQ(Commit::Stored, commitRecord(0));
}

TEST_F(GlyphStoreRoom, CopiesSurviveAReopen) {
  const uint32_t stored = fillRing();
  draw(0);
  draw(3);
  RoomWork work;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
  ASSERT_EQ(2u, work.kept);
  const uint32_t lastCopy = dataOffsetOf(3);
  ASSERT_GT(lastCopy, dataOffsetOf(stored - 1));  // copied after the newest record, at the head
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(intact(0));
  EXPECT_TRUE(intact(3));
  for (const uint32_t n : {1u, 2u, 4u, 5u}) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
  for (uint32_t n = 6; n < stored; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_EQ(stored - 4, store.liveSlots());
  // The head resumes after the copies.
  ASSERT_EQ(Commit::Stored, commitRecord(stored));
  EXPECT_EQ(lastCopy + recordSize(kWidth, kHeight), dataOffsetOf(stored));
}

TEST_F(GlyphStoreRoom, TheRingWearsItsSegmentsEvenly) {
  // 100 x 100 is a 2,520-byte record, one to a segment: two hundred rounds of two take 400 of the index's slots.
  constexpr uint8_t kSide = 100;
  uint32_t n = 0;
  for (uint32_t round = 0; round < 200; ++round) {
    Commit commit;
    while ((commit = commitRecord(n, kSide, kSide)) == Commit::Stored) ++n;
    ASSERT_EQ(Commit::Full, commit) << round;
    ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Open)) << round;
  }
  const uint32_t first = layout.geometry.logOffset / kSectorBytes;
  uint32_t fewest = UINT32_MAX;
  uint32_t most = 0;
  for (uint32_t s = 0; s < layout.geometry.logSize / kSectorBytes; ++s) {
    fewest = std::min(fewest, flash.sectorErases(first + s));
    most = std::max(most, flash.sectorErases(first + s));
  }
  EXPECT_GE(fewest, 20u);
  EXPECT_LE(most - fewest, 1u);
}
