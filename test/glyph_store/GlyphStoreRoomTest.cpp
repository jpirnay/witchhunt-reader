// Making room by keeping what is drawn: the use counts, kept only once the ring nears full, the reclaim of the
// oldest segment that copies the drawn glyphs forward and drops the rest, and the rebuild of an index crowded by the
// slots reclaims leave stale.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

// A heap with nothing to give: the store asks for its arrays through makeUniqueNoThrow<T[]>, which lands in the nothrow
// array new replaced below for this test binary. While a RefuseArrays lives, every such request is refused and counted.
bool refuseArrays = false;
uint32_t refusedArrays = 0;

struct RefuseArrays {
  RefuseArrays() { refuseArrays = true; }
  ~RefuseArrays() { refuseArrays = false; }
  RefuseArrays(const RefuseArrays&) = delete;
  RefuseArrays& operator=(const RefuseArrays&) = delete;
};

}  // namespace

// Unarmed it defers to the throwing array new, as the default does, so the default operator delete[] pairs with it.
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (refuseArrays) {
    ++refusedArrays;
    return nullptr;
  }
  try {
    return ::operator new[](size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

namespace {

using Budget = FlashGlyphStore::Budget;
using Room = FlashGlyphStore::Room;
using RoomWork = FlashGlyphStore::RoomWork;

constexpr uint32_t kFirst = 0x4E00;  // the code point of the first glyph each test stores
// The reclaim tests' glyphs: the helper ring's.
constexpr uint8_t kWidth = kRingWidth;
constexpr uint8_t kHeight = kRingHeight;

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

  static uint32_t keyOf(const uint32_t n) { return ringKey(n); }

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

  // The helper ring: segments 0-13 full, one record in segment 14, one segment free.
  uint32_t fillRing() {
    const uint32_t n = gst_test::fillRing(store, batch);
    EXPECT_EQ(85u, n);
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

  // --- The rebuild tests ---

  void reinstall(const Layout& other) {
    store.close();
    layout = other;
    ASSERT_TRUE(installAndOpen(store, layout));
  }

  // Marks glyphs the font lacks until the index is past its load limit.
  void fillIndexWithMarks() {
    uint32_t cp = 0x3000;
    Commit commit;
    while ((commit = store.markNoGlyph(makeKey(cp, 0, 0))) == Commit::Stored) ++cp;
    ASSERT_EQ(Commit::Full, commit);
    ASSERT_FALSE(store.wipePending());
  }

  // Programs journal entry `i` straight into the state sector; `done` false leaves its done word erased.
  void writeJournalEntry(const uint32_t i, const uint32_t base, const bool done) {
    const JournalEntry entry{base, done ? 0 : kErased};
    ASSERT_TRUE(flash.program(kJournalOffset + i * static_cast<uint32_t>(sizeof entry), &entry,
                              done ? sizeof entry : sizeof entry.base));
  }

  JournalEntry journalEntry(const uint32_t i) {
    JournalEntry entry{};
    EXPECT_TRUE(flash.read(kJournalOffset + i * static_cast<uint32_t>(sizeof entry), &entry, sizeof entry));
    return entry;
  }

  Layout layout = smallLayout();
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

TEST_F(GlyphStoreRoom, ACommitThatNearsAFullRingMakesTheCountsDueWithoutTakingThem) {
  fillTo(kFreeSegmentsTarget + 3);  // 11 of 16 segments live: under 75%, and 5 free
  EXPECT_FALSE(store.countsDue());
  EXPECT_FALSE(store.countsKept());
  // A commit runs inside a draw or a section build: the one that leaves the free segments within one of the target
  // asks for no memory there.
  fillTo(kFreeSegmentsTarget + 1);
  EXPECT_TRUE(store.countsDue());
  EXPECT_FALSE(store.countsKept());
  store.noteDrawn(glyphOf(kFirst));
  EXPECT_EQ(0u, store.drawsCounted());
  // The next safe point takes it.
  EXPECT_TRUE(store.keepCountsIfDue());
  EXPECT_TRUE(store.countsKept());
  EXPECT_FALSE(store.countsDue());
  store.noteDrawn(glyphOf(kFirst));
  EXPECT_EQ(1u, store.useCount(makeKey(kFirst, 0, 0)));
  EXPECT_EQ(1u, store.drawsCounted());
}

TEST_F(GlyphStoreRoom, AnOpenOfARingNearFullKeepsTheCountsFromTheStart) {
  fillTo(kFreeSegmentsTarget + 3);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_FALSE(store.countsKept());  // room to spare: none wanted yet
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.countsKept());
  EXPECT_FALSE(store.countsDue());
  // A glyph drawn right after the open is counted.
  store.noteDrawn(glyphOf(kFirst));
  EXPECT_EQ(1u, store.useCount(makeKey(kFirst, 0, 0)));
}

TEST_F(GlyphStoreRoom, CountsRefusedAreNotAskedForAgainUntilTheNextOpen) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsDue());
  const uint32_t refusedBefore = refusedArrays;
  {
    const RefuseArrays refuse;
    EXPECT_FALSE(store.keepCountsIfDue());
    EXPECT_EQ(refusedBefore + 1, refusedArrays);
    EXPECT_FALSE(store.countsDue());
    // Neither a later commit past the trigger, nor the next safe point, nor markFull() asks again.
    ASSERT_EQ(Commit::Stored, commitOne(kFirst + 100));
    EXPECT_FALSE(store.countsDue());
    EXPECT_FALSE(store.keepCountsIfDue());
    EXPECT_TRUE(store.markFull());
    EXPECT_FALSE(store.countsKept());
    EXPECT_EQ(refusedBefore + 1, refusedArrays);
  }
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.countsKept());
}

TEST_F(GlyphStoreRoom, MarkFullKeepsTheCountsAtOnce) {
  ASSERT_EQ(Commit::Stored, commitOne(kFirst));
  ASSERT_FALSE(store.countsDue());
  ASSERT_TRUE(store.markFull());
  EXPECT_TRUE(store.countsKept());
  EXPECT_FALSE(store.countsDue());
}

TEST_F(GlyphStoreRoom, DrawsAreCountedUpToThree) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.keepCountsIfDue());
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
  ASSERT_TRUE(store.keepCountsIfDue());
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
  ASSERT_TRUE(store.keepCountsIfDue());
  const uint32_t key = makeKey(kFirst, 0, 0);
  store.noteDrawn(glyphOf(kFirst));
  ASSERT_EQ(1u, store.useCount(key));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.countsKept());  // the ring is still near full
  EXPECT_EQ(0u, store.useCount(key));
  EXPECT_EQ(0u, store.drawsCounted());
}

TEST_F(GlyphStoreRoom, CountsAreFreedByAWipe) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.keepCountsIfDue());
  ASSERT_TRUE(store.wipe());
  EXPECT_FALSE(store.countsKept());
  EXPECT_FALSE(store.countsDue());
}

TEST_F(GlyphStoreRoom, AWipeDropsCountsThatWereDue) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsDue());
  ASSERT_TRUE(store.wipe());
  EXPECT_FALSE(store.countsDue());
  EXPECT_FALSE(store.keepCountsIfDue());
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
  ASSERT_EQ(0u, store.rebuilds());  // a rebuild would have cleared the stale slots this is about
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
  ASSERT_TRUE(store.keepCountsIfDue());
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
  // Thirteen records need three new segments; two are free. The batch is refused whole: nothing is written.
  for (uint32_t n = 84; n < 97; ++n) ASSERT_TRUE(stage(batch, keyOf(n), kWidth, kHeight)) << n;
  const uint32_t programs = flash.programCalls();
  const uint32_t erases = flash.eraseCalls();
  const uint32_t used = store.logUsed();
  ASSERT_EQ(Commit::Full, store.commit(batch));
  EXPECT_EQ(programs, flash.programCalls());
  EXPECT_EQ(erases, flash.eraseCalls());
  EXPECT_EQ(used, store.logUsed());
  EXPECT_EQ(kFreeSegmentsTarget, store.freeSegments());
  for (uint32_t n = 84; n < 97; ++n) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
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

TEST_F(GlyphStoreRoom, AFailedRetireStopsTheReclaimWithEverythingServedWhole) {
  const uint32_t stored = fillRing();
  draw(0);
  draw(3);
  // Two copies, each two program calls (512 + 88 bytes) and a slot: the seventh call is segment 0's retire word.
  flash.failNextProgram(6);
  RoomWork work;
  EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(2u, work.kept);
  EXPECT_EQ(0u, work.reclaimed);
  EXPECT_TRUE(store.wipePending());
  EXPECT_TRUE(store.writesStopped());
  SegmentHeader header{};
  ASSERT_TRUE(flash.read(layout.geometry.logOffset, &header, sizeof header));
  EXPECT_EQ(kErased, header.retired);
  // Segment 0 never left the ring: its glyphs and the copies are served whole, now and after a reboot.
  for (uint32_t n = 0; n < stored; ++n) EXPECT_TRUE(intact(n)) << n;
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.wipePending());
  EXPECT_EQ(1u, store.freeSegments());
  for (uint32_t n = 0; n < stored; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Open));  // only a wipe goes on from here
  ASSERT_TRUE(store.wipe());
  EXPECT_EQ(0u, store.logUsed());
}

TEST_F(GlyphStoreRoom, ACutInAReclaimsEraseLeavesItsSegmentFree) {
  const uint32_t stored = fillRing();
  draw(0);
  draw(3);
  // Two copies, each its record then its slot, and segment 0's retire word: the power fails in the erase after them.
  flash.cutAfterSteps(2 * (recordSize(kWidth, kHeight) + kSlotBytes) + sizeof(uint32_t));
  RoomWork work;
  EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Idle, &work));
  ASSERT_TRUE(flash.dead());
  EXPECT_EQ(2u, work.kept);
  EXPECT_TRUE(store.wipePending());  // for this session: the chip took no more writes
  // Segment 0 is out of the ring: of its glyphs only the copies are served.
  const auto expectSegment0Gone = [&] {
    EXPECT_TRUE(intact(0));
    EXPECT_TRUE(intact(3));
    for (const uint32_t n : {1u, 2u, 4u, 5u}) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
    for (uint32_t n = 6; n < stored; ++n) EXPECT_TRUE(intact(n)) << n;
  };
  expectSegment0Gone();
  // After the reboot no wipe is pending (it never reached the chip), and segment 0, half erased, is free.
  flash.powerCycle();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_FALSE(store.wipePending());
  EXPECT_EQ(2u, store.freeSegments());
  expectSegment0Gone();
  // The head comes round to segment 0 only through a whole erase of it.
  const uint32_t sector = layout.geometry.logOffset / kSectorBytes;
  const uint32_t erases = flash.sectorErases(sector);
  uint32_t n = stored;
  do {
    ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  } while (segmentOf(n++) != 0 && n < stored + 50);
  EXPECT_EQ(erases + 1, flash.sectorErases(sector));
  expectSegment0Gone();
  for (uint32_t k = stored; k < n; ++k) EXPECT_TRUE(intact(k)) << k;
}

TEST_F(GlyphStoreRoom, CopiesSplitAcrossAHeadMoveSurviveAReopen) {
  uint32_t n = fillRing();
  // Four more records leave the head, segment 14, room for one.
  for (const uint32_t end = n + 4; n < end; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  ASSERT_EQ(14u, segmentOf(n - 1));
  ASSERT_EQ(1u, store.freeSegments());
  draw(0);
  draw(2);
  draw(4);
  RoomWork work;
  ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle, &work));
  ASSERT_EQ(3u, work.kept);
  EXPECT_EQ(14u, segmentOf(0));  // the first copy ends the head
  EXPECT_EQ(15u, segmentOf(2));  // the other two open the segment after it
  EXPECT_EQ(15u, segmentOf(4));
  const uint32_t lastCopy = dataOffsetOf(4);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  for (const uint32_t k : {0u, 2u, 4u}) EXPECT_TRUE(intact(k)) << k;
  for (const uint32_t k : {1u, 3u, 5u}) EXPECT_EQ(Lookup::NotStored, lookup(k)) << k;
  for (uint32_t k = 6; k < n; ++k) EXPECT_TRUE(intact(k)) << k;
  EXPECT_EQ(1u, store.freeSegments());
  EXPECT_EQ(n - 3, store.liveSlots());  // segment 0's six went stale, the three copies took new ones
  // The head resumes after the last copy.
  ASSERT_EQ(Commit::Stored, commitRecord(n));
  EXPECT_EQ(lastCopy + recordSize(kWidth, kHeight), dataOffsetOf(n));
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

// --- The rebuild ---

TEST_F(GlyphStoreRoom, ACrowdedIndexIsRebuiltFromTheLog) {
  reinstall(smallLayout(256, 512, 65536));
  // An idle reclaim whenever a commit leaves fewer than two segments free, so what stops the commits is the index:
  // 179 slots, 70 % of 256.
  uint32_t n = 0;
  Commit commit;
  while ((commit = commitRecord(n)) == Commit::Stored) {
    ++n;
    if (store.freeSegments() < kFreeSegmentsTarget) {
      ASSERT_NE(Room::Failed, store.makeRoom(Budget::Idle)) << n;
      ASSERT_GE(store.freeSegments(), kFreeSegmentsTarget) << n;
    }
  }
  ASSERT_EQ(Commit::Full, commit);
  ASSERT_TRUE(store.needsRoom());
  ASSERT_GE(store.freeSegments(), kFreeSegmentsTarget);
  ASSERT_TRUE(store.keepCountsIfDue());
  std::vector<uint32_t> live;
  for (uint32_t k = 0; k < n; ++k) {
    if (lookup(k) != Lookup::Found) continue;
    live.push_back(k);
    draw(k, k % 4);
  }
  ASSERT_LT(live.size() + 30, store.usedSlots());  // stale slots, over kRebuildStalePercent of the index
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(0u, work.reclaimed);
  EXPECT_EQ(1u, store.rebuilds());
  EXPECT_EQ(live.size(), store.usedSlots());
  EXPECT_EQ(store.usedSlots(), store.liveSlots());
  for (const uint32_t k : live) {
    EXPECT_TRUE(intact(k)) << k;
    EXPECT_EQ(k % 4, countOf(k)) << k;
  }
  for (uint32_t k = 0; k < n; ++k) {
    if (std::find(live.begin(), live.end(), k) == live.end()) {
      EXPECT_EQ(Lookup::NotStored, lookup(k)) << k;
    }
  }
  EXPECT_FALSE(store.writesStopped());
  ASSERT_EQ(Commit::Stored, commitRecord(n));
  EXPECT_TRUE(intact(n));
}

TEST_F(GlyphStoreRoom, ARebuildForgetsTheNoGlyphMarks) {
  for (uint32_t n = 0; n < 6; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  const uint32_t lacked = makeKey(0x2603, 0, 0);
  ASSERT_EQ(Commit::Stored, store.markNoGlyph(lacked));
  EXPECT_EQ(Lookup::NoGlyph, store.find(lacked, nullptr));
  fillIndexWithMarks();
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(Lookup::NotStored, store.find(lacked, nullptr));
  for (uint32_t n = 0; n < 6; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_EQ(6u, store.usedSlots());
  EXPECT_EQ(6u, store.liveSlots());
  // Learned again.
  ASSERT_EQ(Commit::Stored, store.markNoGlyph(lacked));
  EXPECT_EQ(Lookup::NoGlyph, store.find(lacked, nullptr));
}

// A family that lacks a whole script (a Latin font in a CJK book) marks each code point of it it is asked for: 130
// marks, over kRebuildStalePercent of the 1,024-slot index, and not one stale slot. A rebuild would only forget them,
// to learn them again.
constexpr uint32_t kScriptMarks = 130;

TEST_F(GlyphStoreRoom, NoGlyphMarksAloneDoNotCallForARebuildAtAnOpen) {
  ASSERT_GE(kScriptMarks * 100, layout.geometry.indexSlots * kRebuildStalePercent);
  for (uint32_t n = 0; n < 6; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  for (uint32_t i = 0; i < kScriptMarks; ++i) {
    ASSERT_EQ(Commit::Stored, store.markNoGlyph(makeKey(0x3000 + i, 0, 0))) << i;
  }
  ASSERT_EQ(6u + kScriptMarks, store.usedSlots());
  ASSERT_EQ(6u, store.liveSlots());
  const auto expectNoRebuild = [&](const char* when) {
    EXPECT_FALSE(store.needsRoom(Budget::Open)) << when;
    RoomWork work;
    EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work)) << when;
    EXPECT_FALSE(work.rebuilt) << when;
    EXPECT_EQ(0u, store.rebuilds()) << when;
    EXPECT_EQ(Lookup::NoGlyph, store.find(makeKey(0x3000, 0, 0), nullptr)) << when;
  };
  expectNoRebuild("as marked");
  // Counted again by the next session's open.
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  expectNoRebuild("after a reopen");
}

TEST_F(GlyphStoreRoom, AnIndexFilledByMarksIsStillRebuiltAtAnOpen) {
  for (uint32_t n = 0; n < 6; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  fillIndexWithMarks();
  EXPECT_TRUE(store.needsRoom(Budget::Open));
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey(0x3000, 0, 0), nullptr));
  EXPECT_EQ(6u, store.usedSlots());
  EXPECT_EQ(6u, store.liveSlots());
  for (uint32_t n = 0; n < 6; ++n) EXPECT_TRUE(intact(n)) << n;
  // The marks went with the rebuild: as many again as a script's do not call for another.
  for (uint32_t i = 0; i < kScriptMarks; ++i) {
    ASSERT_EQ(Commit::Stored, store.markNoGlyph(makeKey(0x3000 + i, 0, 0))) << i;
  }
  EXPECT_FALSE(store.needsRoom(Budget::Open));
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_FALSE(work.rebuilt);
}

TEST_F(GlyphStoreRoom, AWipeForgetsTheMarks) {
  fillIndexWithMarks();
  ASSERT_TRUE(store.wipe());
  ASSERT_TRUE(claimTestSizes(store));
  for (uint32_t i = 0; i < kScriptMarks; ++i) {
    ASSERT_EQ(Commit::Stored, store.markNoGlyph(makeKey(0x3000 + i, 0, 0))) << i;
  }
  EXPECT_FALSE(store.needsRoom(Budget::Open));
}

TEST_F(GlyphStoreRoom, AFullProbeChainAsksForARebuildNotAWipe) {
  const std::vector<uint32_t> keys = collidingKeys(layout.geometry.indexSlots, kMaxProbe + 1);
  ASSERT_EQ(kMaxProbe + 1, keys.size());
  for (uint32_t i = 0; i < kMaxProbe; ++i) ASSERT_EQ(Commit::Stored, store.markNoGlyph(keys[i])) << i;
  ASSERT_TRUE(stage(batch, keys[kMaxProbe], kWidth, kHeight));
  EXPECT_EQ(Commit::Full, store.commit(batch));
  EXPECT_FALSE(store.wipePending());
  EXPECT_TRUE(store.needsRoom());
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_FALSE(store.writesStopped());
  ASSERT_TRUE(stage(batch, keys[kMaxProbe], kWidth, kHeight));
  EXPECT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_TRUE(storedIntact(store, keys[kMaxProbe], kWidth, kHeight));
}

TEST_F(GlyphStoreRoom, AFullJournalTurnsTheRebuildIntoAWipe) {
  for (uint32_t n = 0; n < 6; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  for (uint32_t i = 0; i < kJournalEntries; ++i) writeJournalEntry(i, 1, true);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_EQ(1u, store.rebuildBase());
  fillIndexWithMarks();
  const uint32_t generation = store.generation();
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_FALSE(work.rebuilt);
  EXPECT_EQ(generation + 1, store.generation());
  EXPECT_FALSE(store.wipePending());
  EXPECT_EQ(0u, store.logUsed());
  EXPECT_EQ(0u, store.usedSlots());
  EXPECT_EQ(0u, store.rebuildBase());
  for (uint32_t n = 0; n < 6; ++n) EXPECT_EQ(Lookup::NotStored, lookup(n)) << n;
  const uint8_t* journal = flash.raw() + kJournalOffset;
  EXPECT_TRUE(std::all_of(journal, journal + kJournalEntries * sizeof(JournalEntry),
                          [](const uint8_t b) { return b == 0xFF; }));
  ASSERT_TRUE(claimTestSizes(store));
  EXPECT_EQ(Commit::Stored, commitRecord(0));
}

TEST_F(GlyphStoreRoom, IdleLeavesAnEpochRebuildToTheNextOpen) {
  // The first segment at epoch 5000, and a finished rebuild from epoch 2500: only the epoch span calls for one, past
  // kRebuildEpochSpan and short of kRebuildEpochLimit.
  writeSegmentHeader(flash, layout, 0, 5000);
  writeJournalEntry(0, 2500, true);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_EQ(5001u, store.epoch());
  ASSERT_EQ(2500u, store.rebuildBase());
  for (uint32_t n = 0; n < 3; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  ASSERT_EQ(store.usedSlots(), store.liveSlots());  // the index is not crowded
  EXPECT_FALSE(store.needsRoom());
  EXPECT_TRUE(store.needsRoom(Budget::Open));
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_FALSE(work.rebuilt);
  EXPECT_EQ(0u, store.rebuilds());
  EXPECT_EQ(2500u, store.rebuildBase());
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(1u, store.rebuilds());
  EXPECT_EQ(5000u, store.rebuildBase());  // the tail's epoch
  EXPECT_FALSE(store.needsRoom(Budget::Open));
  for (uint32_t n = 0; n < 3; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_EQ(5000u, journalEntry(1).base);
  EXPECT_EQ(0u, journalEntry(1).done);
}

TEST_F(GlyphStoreRoom, TheEpochLimitCountsFromTheLastFinishedRebuild) {
  // A rebuild from epoch 1000 finished; a later one, from 64 epochs short of `kLast`, was cut short. kLast is the last
  // epoch the limit lets a segment have past the finished rebuild's base; segment 0 has the one before it.
  constexpr uint32_t kFloor = 1000;
  constexpr uint32_t kLast = kFloor + kRebuildEpochLimit - 1;
  writeSegmentHeader(flash, layout, 0, kLast - 1);
  writeJournalEntry(0, kFloor, true);
  writeJournalEntry(1, kLast - 64, false);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_EQ(kLast, store.epoch());
  ASSERT_EQ(kLast - 64, store.rebuildBase());  // the interrupted entry's
  EXPECT_FALSE(store.needsRoom());             // one epoch is left
  EXPECT_TRUE(store.needsRoom(Budget::Open));  // the interrupted rebuild waits for an Open
  // Seven records: six fill segment 0, the seventh opens segment 1 with the last epoch.
  for (uint32_t n = 0; n < 7; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  ASSERT_EQ(1u, segmentOf(6));
  ASSERT_EQ(kLast + 1, store.epoch());
  // Counted from the finished rebuild's base no epoch is left; from the interrupted one's, only 65 would be spent.
  EXPECT_TRUE(store.needsRoom());
  // A batch that would open a segment is refused whole, and nothing is written.
  for (uint32_t n = 7; n < 13; ++n) ASSERT_TRUE(stage(batch, keyOf(n), kWidth, kHeight)) << n;
  const uint32_t programs = flash.programCalls();
  EXPECT_EQ(Commit::Full, store.commit(batch));
  EXPECT_EQ(programs, flash.programCalls());
  EXPECT_TRUE(store.writesStopped());
  EXPECT_FALSE(store.wipePending());
  // Idle time rebuilds the index: the limit does not wait for an Open.
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(kLast - 1, store.rebuildBase());  // the tail's epoch
  EXPECT_FALSE(store.writesStopped());
  for (uint32_t n = 0; n < 7; ++n) EXPECT_TRUE(intact(n)) << n;
  for (uint32_t n = 7; n < 13; ++n) ASSERT_TRUE(stage(batch, keyOf(n), kWidth, kHeight)) << n;
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (uint32_t n = 0; n < 13; ++n) EXPECT_TRUE(intact(n)) << n;
}

TEST_F(GlyphStoreRoom, ARebuildAPowerCutInterruptedIsDoneAgainAtOpen) {
  for (uint32_t n = 0; n < 8; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;  // two segments
  writeJournalEntry(0, 1, false);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1u, store.rebuildBase());
  EXPECT_FALSE(store.needsRoom());
  EXPECT_TRUE(store.needsRoom(Budget::Open));
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_FALSE(work.rebuilt);
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(1u, store.rebuilds());
  for (uint32_t n = 0; n < 8; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_EQ(kErased, journalEntry(0).done);  // the interrupted entry stays as it was; the next one is done
  EXPECT_EQ(1u, journalEntry(1).base);
  EXPECT_EQ(0u, journalEntry(1).done);
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_FALSE(store.needsRoom(Budget::Open));
}

TEST_F(GlyphStoreRoom, ARebuildNeverMakesATornRecordFindable) {
  for (uint32_t n = 0; n < 3; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  // Record 3's write was cut in its bitmap, before its slot: its header reads complete.
  writeRawRecord(flash, layout, inSegment(layout, 0, 3 * recordSize(kWidth, kHeight)), keyOf(3), kWidth, kHeight, 100);
  writeJournalEntry(0, 1, false);  // a rebuild is due at the next open
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(3u, store.liveSlots());  // no slot names the torn record
  EXPECT_EQ(Lookup::NotStored, lookup(3));
  RoomWork work;
  ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  ASSERT_TRUE(work.rebuilt);
  EXPECT_EQ(Lookup::NotStored, lookup(3));
  for (uint32_t n = 0; n < 3; ++n) EXPECT_TRUE(intact(n)) << n;
  EXPECT_EQ(3u, store.usedSlots());
  ASSERT_EQ(Commit::Stored, commitRecord(3));
  EXPECT_TRUE(intact(3));
}

TEST_F(GlyphStoreRoom, ARebuildKeepsTheFirstOfTwoCopiesACutLeft) {
  for (uint32_t n = 0; n < 13; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;  // the head holds one record
  ASSERT_TRUE(store.markFull());
  draw(0);
  // The reclaim copies record 0 to the head, record then slot (608 bytes), and the power fails as segment 0 retires:
  // both copies live, each with a slot.
  flash.cutAfterSteps(recordSize(kWidth, kHeight) + kSlotBytes);
  EXPECT_EQ(Room::Failed, store.makeRoom(Budget::Idle));
  flash.powerCycle();
  writeJournalEntry(0, 1, false);  // a rebuild is due at the next open
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_FALSE(store.wipePending());
  ASSERT_EQ(14u, store.liveSlots());
  RoomWork work;
  ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  ASSERT_TRUE(work.rebuilt);
  EXPECT_EQ(0u, work.reclaimed);
  EXPECT_EQ(13u, store.usedSlots());
  EXPECT_EQ(13u, store.liveSlots());
  EXPECT_EQ(0u, segmentOf(0));  // the first copy, in the oldest segment
  for (uint32_t n = 0; n < 13; ++n) EXPECT_TRUE(intact(n)) << n;
}

TEST_F(GlyphStoreRoom, IdleWorkIsDueOnlyWhereAnIdleStepWouldDoSomething) {
  for (uint32_t n = 0; n < 18; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;  // three segments live
  EXPECT_FALSE(store.idleWorkDue());
  ASSERT_TRUE(store.markFull());
  EXPECT_TRUE(store.needsRoom(Budget::Idle));
  EXPECT_TRUE(store.idleWorkDue());
  RoomWork work;
  store.makeRoom(Budget::Idle, &work);
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_FALSE(store.idleWorkDue());
}

TEST_F(GlyphStoreRoom, IdleWorkIsNotDueWhileARebuildWaitsForMemory) {
  for (uint32_t n = 0; n < 6; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  fillIndexWithMarks();
  ASSERT_FALSE(store.countsKept());
  EXPECT_TRUE(store.idleWorkDue());  // a rebuild is due in idle time
  RoomWork work;
  {
    const RefuseArrays refuse;
    EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
    EXPECT_FALSE(work.rebuilt);
  }
  // The index still stops writes, but idle time leaves the rebuild to an Open.
  EXPECT_TRUE(store.needsRoom(Budget::Idle));
  EXPECT_FALSE(store.idleWorkDue());
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_FALSE(store.needsRoom(Budget::Idle));
  EXPECT_FALSE(store.idleWorkDue());
}

TEST_F(GlyphStoreRoom, ARebuildWithoutMemoryWaitsForTheNextOpen) {
  for (uint32_t n = 0; n < 6; ++n) ASSERT_EQ(Commit::Stored, commitRecord(n)) << n;
  fillIndexWithMarks();              // the index stops writes: a rebuild is due in idle time
  ASSERT_FALSE(store.countsKept());  // so a rebuild asks for one array, its plan
  const uint32_t refusedBefore = refusedArrays;
  RoomWork work;
  {
    const RefuseArrays refuse;
    EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
    EXPECT_FALSE(work.rebuilt);
    EXPECT_EQ(refusedBefore + 1, refusedArrays);
    EXPECT_FALSE(store.wipePending());
    EXPECT_TRUE(store.writesStopped());
    // Idle time does not try again: no walk of the log, no request.
    for (uint32_t tick = 0; tick < 3; ++tick) {
      EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work)) << tick;
      EXPECT_FALSE(work.rebuilt) << tick;
    }
    EXPECT_EQ(refusedBefore + 1, refusedArrays);
    // An Open does.
    EXPECT_EQ(Room::More, store.makeRoom(Budget::Open, &work));
    EXPECT_EQ(refusedBefore + 2, refusedArrays);
  }
  // With memory again, idle time still leaves it to the next Open.
  EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
  EXPECT_FALSE(work.rebuilt);
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_TRUE(work.rebuilt);
  EXPECT_FALSE(store.writesStopped());
  for (uint32_t n = 0; n < 6; ++n) EXPECT_TRUE(intact(n)) << n;
  // Once a rebuild got through, idle time tries the next one due.
  fillIndexWithMarks();
  {
    const RefuseArrays refuse;
    EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
    EXPECT_EQ(refusedBefore + 3, refusedArrays);
  }
  // So does a new session.
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_EQ(Commit::Full, store.markNoGlyph(makeKey(0x2FFF, 0, 0)));  // the index is still past its load limit
  {
    const RefuseArrays refuse;
    EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
    EXPECT_EQ(refusedBefore + 4, refusedArrays);
  }
  EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
  EXPECT_FALSE(work.rebuilt);
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_TRUE(work.rebuilt);
}

// --- A full index with few stale slots: 68-byte records (12 x 16), sixty to a segment, fill the 179 slots of a
// 256-slot index over three segments, none of them stale. ---

constexpr uint8_t kSmallWidth = 12;
constexpr uint8_t kSmallHeight = 16;

TEST_F(GlyphStoreRoom, AFullIndexWithFewStaleSlotsReclaimsBeforeItRebuilds) {
  reinstall(smallLayout(256, 512, 65536));
  uint32_t n = 0;
  while (commitRecord(n, kSmallWidth, kSmallHeight) == Commit::Stored) ++n;
  ASSERT_EQ(179u, n);
  ASSERT_EQ(13u, store.freeSegments());
  ASSERT_EQ(store.usedSlots(), store.liveSlots());
  // A rebuild now would free nothing: the oldest segment goes first, its sixty slots going stale.
  RoomWork work;
  EXPECT_EQ(Room::More, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(1u, work.reclaimed);
  EXPECT_FALSE(work.rebuilt);
  EXPECT_TRUE(store.writesStopped());
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Idle, &work));
  EXPECT_EQ(0u, work.reclaimed);
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(119u, store.usedSlots());
  EXPECT_EQ(119u, store.liveSlots());
  EXPECT_FALSE(store.writesStopped());
  for (uint32_t k = 60; k < n; ++k) EXPECT_TRUE(storedIntact(store, keyOf(k), kSmallWidth, kSmallHeight)) << k;
  EXPECT_EQ(Commit::Stored, commitRecord(n, kSmallWidth, kSmallHeight));
}

TEST_F(GlyphStoreRoom, OpenRebuildsAFullIndexInsteadOfReclaimingTheRing) {
  reinstall(smallLayout(256, 512, 65536));
  uint32_t n = 0;
  while (commitRecord(n, kSmallWidth, kSmallHeight) == Commit::Stored) ++n;
  ASSERT_EQ(179u, n);
  RoomWork work;
  EXPECT_EQ(Room::Enough, store.makeRoom(Budget::Open, &work));
  EXPECT_EQ(1u, work.reclaimed);  // the one reclaim that makes the rebuild pay, not the whole ring
  EXPECT_TRUE(work.rebuilt);
  EXPECT_EQ(14u, store.freeSegments());
  EXPECT_EQ(119u, store.usedSlots());
  for (uint32_t k = 60; k < n; ++k) EXPECT_TRUE(storedIntact(store, keyOf(k), kSmallWidth, kSmallHeight)) << k;
}

// --- Epochs ---

namespace {

// An index of 20,480 slots on a chip of its own: with one record a segment, the slots 2,048 epochs leave stale stay
// under kRebuildStalePercent of it, so the epoch span is what calls for the rebuild.
class GlyphStoreEpochs : public ::testing::Test {
 protected:
  void TearDown() override {
    EXPECT_EQ(0u, flash.violations());
    EXPECT_EQ(0u, flash.reprograms());
  }

  SimNorFlash flash{327680};
  FlashGlyphStore store{flash};
};

}  // namespace

TEST_F(GlyphStoreEpochs, TheIndexIsRebuiltBeforeEpochsWrap) {
  const Layout layout = smallLayout(20480, kMaxProbe, 4 * kSectorBytes, kSectorBytes);
  ASSERT_TRUE(installAndOpen(store, layout));
  constexpr uint8_t kSide = 100;  // a 2,520-byte record: one to a segment
  alignas(4) uint8_t buffer[4096] = {};
  GlyphBatch batch{buffer, sizeof buffer};
  const auto key = [](const uint32_t n) { return makeKey(n, 0, 0); };
  uint32_t n = 0;
  for (uint32_t round = 1; store.epoch() <= 5000; ++round) {
    Commit commit;
    for (;;) {
      ASSERT_TRUE(stage(batch, key(n), kSide, kSide)) << n;
      if ((commit = store.commit(batch)) != Commit::Stored) break;
      ++n;
    }
    ASSERT_EQ(Commit::Full, commit) << round;
    const uint32_t base = store.rebuildBase();
    const bool spanDue = store.epoch() - base >= kRebuildEpochSpan;
    ASSERT_EQ(Room::Enough, store.makeRoom(Budget::Open)) << round;
    if (store.rebuildBase() != base) {
      EXPECT_TRUE(spanDue) << round;
    }
    ASSERT_LT(store.epoch() - store.rebuildBase(), kRebuildEpochSpan + store.segmentCount()) << round;
    ASSERT_LT(uint64_t{store.usedSlots() - store.liveSlots()} * 100,
              uint64_t{layout.geometry.indexSlots} * kRebuildStalePercent)
        << round;
    if (round % 250 == 0) {
      for (uint32_t k = 0; k < n; ++k) ASSERT_TRUE(absentOrIntact(store, key(k), kSide, kSide)) << k;
      ASSERT_TRUE(storedIntact(store, key(n - 1), kSide, kSide)) << round;
    }
  }
  EXPECT_EQ(2u, store.rebuilds());
  for (uint32_t k = 0; k < n; ++k) ASSERT_TRUE(absentOrIntact(store, key(k), kSide, kSide)) << k;
}

TEST_F(GlyphStoreEpochs, WithoutOpensTheEpochsStopShortOfWrapping) {
  const Layout layout = smallLayout(20480, kMaxProbe, 4 * kSectorBytes, kSectorBytes);
  ASSERT_TRUE(installAndOpen(store, layout));
  constexpr uint8_t kSide = 100;  // a 2,520-byte record: one to a segment
  alignas(4) uint8_t buffer[4096] = {};
  GlyphBatch batch{buffer, sizeof buffer};
  const auto key = [](const uint32_t n) { return makeKey(n, 0, 0); };
  // No draws, so nothing is copied: only the newest records, one a live segment, are found, and intact.
  const auto onlyTheNewestFound = [&](const uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) {
      if (store.find(key(k), nullptr) == Lookup::NotStored) continue;
      ASSERT_GE(k + store.segmentCount(), n) << k;
      ASSERT_TRUE(storedIntact(store, key(k), kSide, kSide)) << k;
    }
  };
  uint32_t n = 0;
  bool pastTheSpan = false;        // kRebuildEpochSpan passed, and idle time left it
  bool stoppedAtTheLimit = false;  // a commit stopped for want of an epoch
  for (uint32_t round = 1; store.epoch() <= 5000; ++round) {
    Commit commit;
    for (;;) {
      ASSERT_TRUE(stage(batch, key(n), kSide, kSide)) << n;
      if ((commit = store.commit(batch)) != Commit::Stored) break;
      ++n;
    }
    ASSERT_EQ(Commit::Full, commit) << round;
    ASSERT_FALSE(store.wipePending()) << round;
    const uint32_t span = store.epoch() - store.rebuildBase();
    ASSERT_LE(span, kRebuildEpochLimit) << round;
    pastTheSpan = pastTheSpan || (span >= kRebuildEpochSpan && store.rebuilds() == 0);
    stoppedAtTheLimit = stoppedAtTheLimit || span == kRebuildEpochLimit;
    Room room = Room::More;
    for (uint32_t step = 0; step < 4 && room == Room::More; ++step) room = store.makeRoom(Budget::Idle);
    ASSERT_EQ(Room::Enough, room) << round;
    ASSERT_LE(store.epoch() - store.rebuildBase(), kRebuildEpochLimit) << round;
    if (round % 250 == 0) onlyTheNewestFound(n);
  }
  EXPECT_TRUE(pastTheSpan);
  EXPECT_TRUE(stoppedAtTheLimit);
  EXPECT_EQ(1u, store.rebuilds());
  onlyTheNewestFound(n);
}
