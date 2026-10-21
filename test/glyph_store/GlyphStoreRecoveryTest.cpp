// Where writing resumes after a power cut, and what a torn write can and cannot make a lookup return. The
// log scan must never let a byte be programmed twice.
#include <gtest/gtest.h>

#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

class GlyphStoreRecovery : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(install(store, layout)); }

  // Opens from scratch, as after a reboot.
  void reopen() {
    store.close();
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }

  // The first segment's header and two complete records after it; returns where the next record would go.
  uint32_t writeTwo() {
    writeSegmentHeader(flash, layout, 0, 1);
    writeRawRecord(flash, layout, inSegment(layout, 0, 0), makeKey('a', 0, 0), 9, 12);
    const uint32_t second = recordSize(9, 12);
    writeRawRecord(flash, layout, inSegment(layout, 0, second), makeKey('b', 0, 0), 10, 12);
    return inSegment(layout, 0, second + recordSize(10, 12));
  }

  void programSlot(const uint32_t i, const Slot& slot) {
    ASSERT_TRUE(flash.program(layout.geometry.indexOffset + i * kSlotBytes, &slot, sizeof slot));
  }

  Slot slotAt(const uint32_t i) {
    Slot slot{};
    EXPECT_TRUE(flash.read(layout.geometry.indexOffset + i * kSlotBytes, &slot, sizeof slot));
    return slot;
  }

  // Four segments of 16 KB: a torn write's sector boundaries fall inside the head, where writing resumes.
  const Layout layout = smallLayout(1024, 512, 65536, 16384);
};

}  // namespace

TEST_F(GlyphStoreRecovery, WritingResumesAfterTheLastRecord) {
  const uint32_t end = writeTwo();
  reopen();
  EXPECT_EQ(end, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ATornRecordHeaderMovesTheResumePointToTheNextErasedSector) {
  const uint32_t end = writeTwo();
  const uint8_t garbage[10] = {0x12, 0x00, 0x00, 0x18, 0x33};  // key 0x18000012: bit 28 set, not a record
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + end, garbage, sizeof garbage));
  reopen();
  EXPECT_EQ(kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, BytesBehindAnErasedKeyAreNotOverwritten) {
  // A cut can leave a record's later bytes programmed while its key still reads erased.
  const uint32_t end = writeTwo();
  const uint8_t tail[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + end + 8, tail, sizeof tail));
  reopen();
  EXPECT_EQ(kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ARecordWithACompleteHeaderButAShortBitmapIsSkippedWhole) {
  const uint32_t end = writeTwo();
  writeRawRecord(flash, layout, end, makeKey('c', 0, 0), 20, 20, 30);  // 30 of its 100 bitmap bytes
  reopen();
  EXPECT_EQ(end + recordSize(20, 20), store.logUsed());
}

TEST_F(GlyphStoreRecovery, ARecordWithAWrongDataOffsetIsNotSteppedOver) {
  const uint32_t end = writeTwo();
  EpdGlyph glyph = metrics(9, 12);
  glyph.dataOffset = end + kRecordHeaderBytes + 4;  // not just past its own header
  writeRawHeader(flash, layout, end, makeKey('c', 0, 0), glyph);
  reopen();
  EXPECT_EQ(kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ARecordWhoseLengthDoesNotMatchItsSizeIsNotSteppedOver) {
  const uint32_t end = writeTwo();
  EpdGlyph glyph = metrics(9, 12);
  glyph.dataLength = 40;  // a 9 x 12 bitmap is 27 bytes
  glyph.dataOffset = end + kRecordHeaderBytes;
  writeRawHeader(flash, layout, end, makeKey('c', 0, 0), glyph);
  reopen();
  EXPECT_EQ(kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, AGarbledPageAfterTheLastRecordIsSkipped) {
  // A real chip cut mid-page can leave a whole 256-byte page partly programmed.
  writeTwo();
  std::vector<uint8_t> page(256);
  for (uint32_t i = 0; i < page.size(); ++i) page[i] = static_cast<uint8_t>(0xFF ^ (i * 37));
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + 4096 + 100, page.data(), 256));
  reopen();
  EXPECT_EQ(2 * kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ATornRecordDoesNotHideWhatLaterSessionsWrote) {
  // Session 1: a cut tears the header of a record in a batch.
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_TRUE(claimTestSizes(store));
  alignas(4) uint8_t buffer[4096];
  GlyphBatch batch(buffer, sizeof buffer);
  for (uint32_t i = 0; i < 4; ++i) ASSERT_TRUE(stage(batch, makeKey('a' + i, 0, 0), 11, 16));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (uint32_t i = 0; i < 3; ++i) ASSERT_TRUE(stage(batch, makeKey('A' + i, 0, 0), 11, 16));
  flash.cutAfterSteps(recordSize(11, 16) + 5);  // 'A' lands, 'B' gets 5 header bytes
  EXPECT_EQ(Commit::WriteFailed, store.commit(batch));
  flash.powerCycle();
  // Session 2 writes far more than the 8 KB the scan looks past a torn record, all in the same segment.
  reopen();
  const uint32_t resume = store.logUsed();
  constexpr uint32_t kSecond = 150;
  for (uint32_t i = 0; i < kSecond; ++i) {
    if (!stage(batch, makeKey(0x4E00 + i, 0, 0), 11, 16)) {
      ASSERT_EQ(Commit::Stored, store.commit(batch));
      ASSERT_TRUE(stage(batch, makeKey(0x4E00 + i, 0, 0), 11, 16));
    }
  }
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  const uint32_t end = store.logUsed();
  ASSERT_GT(end - resume, kMaxRecordBytes);
  ASSERT_EQ(1u, store.segmentCount() - store.freeSegments());
  // Session 3: everything session 2 wrote is still there, and writing resumes after it, not on top of it.
  reopen();
  EXPECT_EQ(end, store.logUsed());
  for (uint32_t i = 0; i < kSecond; ++i) EXPECT_TRUE(storedIntact(store, makeKey(0x4E00 + i, 0, 0), 11, 16)) << i;
  for (uint32_t i = 0; i < 40; ++i) ASSERT_TRUE(stage(batch, makeKey(0x5000 + i, 0, 0), 11, 16));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  reopen();
  for (uint32_t i = 0; i < 40; ++i) EXPECT_TRUE(storedIntact(store, makeKey(0x5000 + i, 0, 0), 11, 16)) << i;
  for (uint32_t i = 0; i < kSecond; ++i) EXPECT_TRUE(storedIntact(store, makeKey(0x4E00 + i, 0, 0), 11, 16)) << i;
}

TEST_F(GlyphStoreRecovery, ASlotWhoseValueWasCutNeverReadsAsNoGlyph) {
  // A record slot cut part-way through its value reads with extra 1 bits. Whatever that leaves, the glyph
  // must read as not stored (so it is rendered again), never as "the font has no glyph for this".
  const uint32_t key = makeKey('e', 0, 0);
  const Slot torn{key, 0xFFFFFFFE};
  ASSERT_TRUE(flash.program(layout.geometry.indexOffset + homeSlot(key, layout.geometry.indexSlots) * kSlotBytes, &torn,
                            sizeof torn));
  reopen();
  EXPECT_EQ(Lookup::NotStored, store.find(key, nullptr));
}

TEST_F(GlyphStoreRecovery, ASlotCutShortIsSkippedAndDoesNotEndAProbe) {
  // On a real chip a slot can be cut with its value programmed and its key still reading erased.
  const uint32_t key = makeKey('k', 0, 0);
  const uint32_t home = homeSlot(key, layout.geometry.indexSlots);
  const uint32_t value = 0x12345678;
  ASSERT_TRUE(flash.program(layout.geometry.indexOffset + home * kSlotBytes + 4, &value, sizeof value));
  reopen();
  ASSERT_TRUE(claimTestSizes(store));
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, key, 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));  // the insert steps over the spent slot...
  EXPECT_EQ(key, slotAt((home + 1) % layout.geometry.indexSlots).key);
  EXPECT_TRUE(storedIntact(store, key, 9, 12));  // ...and the lookup probes past it
}

TEST_F(GlyphStoreRecovery, ASlotPointingAtAnotherKeysRecordReadsAsNotStored) {
  writeSegmentHeader(flash, layout, 0, 1);
  writeRawRecord(flash, layout, inSegment(layout, 0, 0), makeKey('J', 0, 0), 9, 12);
  const uint32_t key = makeKey('K', 0, 0);
  programSlot(homeSlot(key, layout.geometry.indexSlots),
              {key, glyphSlotValue(1, inSegment(layout, 0, 0) + kRecordHeaderBytes)});  // 'J''s record
  reopen();
  EXPECT_EQ(Lookup::NotStored, store.find(key, nullptr));
}

TEST_F(GlyphStoreRecovery, ASlotNamingAnotherEpochIsStaleAndTheProbeGoesOn) {
  // A slot written while its segment had another epoch points at whatever the segment holds now: never served.
  writeSegmentHeader(flash, layout, 0, 1);
  const uint32_t key = makeKey('J', 0, 0);
  writeRawRecord(flash, layout, inSegment(layout, 0, 0), key, 9, 12);
  const uint32_t home = homeSlot(key, layout.geometry.indexSlots);
  const uint32_t dataOffset = inSegment(layout, 0, 0) + kRecordHeaderBytes;
  programSlot(home, {key, glyphSlotValue(2, dataOffset)});
  reopen();
  EXPECT_EQ(Lookup::NotStored, store.find(key, nullptr));
  // A slot with the segment's epoch, further along the chain, as a copied glyph's is.
  programSlot((home + 1) % layout.geometry.indexSlots, {key, glyphSlotValue(1, dataOffset)});
  EXPECT_TRUE(storedIntact(store, key, 9, 12));
}

TEST_F(GlyphStoreRecovery, ATornWipeWordReadsAsPending) {
  const uint8_t oneByte = 0x00;
  ASSERT_TRUE(flash.program(kWipeWordOffset + 2, &oneByte, 1));
  reopen();
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreRecovery, GarbageAtTheEndOfTheHeadClosesIt) {
  // Records up to 8 KB into the head, so the scan's window past the walk's end reaches the segment's last byte.
  writeSegmentHeader(flash, layout, 0, 1);
  for (uint32_t at = inSegment(layout, 0, 0); at < 8192; at += recordSize(9, 12)) {
    writeRawRecord(flash, layout, at, makeKey('a', 0, 0), 9, 12);
  }
  const uint8_t junk = 0x00;
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + layout.geometry.segmentBytes - 1, &junk, 1));
  reopen();
  EXPECT_EQ(layout.geometry.segmentBytes, store.logUsed());  // nothing more goes into the head
  ASSERT_TRUE(claimTestSizes(store));
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('z', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  const EpdGlyph* glyph = nullptr;
  ASSERT_EQ(Lookup::Found, store.find(makeKey('z', 0, 0), &glyph));
  EXPECT_EQ(inSegment(layout, 1, 0) + kRecordHeaderBytes, glyph->dataOffset);  // the next segment's first record
  EXPECT_TRUE(storedIntact(store, makeKey('z', 0, 0), 9, 12));
  EXPECT_EQ(2u, store.segmentCount() - store.freeSegments());
}
