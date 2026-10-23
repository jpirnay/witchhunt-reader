# The glyph store makes room — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A TrueType glyph store that fills up keeps the glyphs a reader draws and drops the rest, a segment at a time, in idle time and at chapter or book open, instead of being emptied whole.

**Architecture:** The log becomes a ring of segments (64 KB on the device), each opened by a header holding its epoch. A glyph slot's value packs that epoch with the record's offset, so a slot into a reused segment is never served. A 2-bit use count per index slot (RAM only, counted on the renderer's black-and-white pass) decides what a reclaim of the oldest segment copies forward; the index is rebuilt from the log when stale slots crowd it, with a journal in the state sector. `TtfFontSet::makeRoom` runs it under RenderLock; the EPUB reader calls it in idle time, at chapter open and at book open.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), GoogleTest host tests (`test/glyph_store`, `test/ttf_font`) on the simulated chip `SimNorFlash`, the serial test API for the device runs.

**Spec:** `docs/design/cjk-ttf-fonts.md`, section 8 (the store), with sections 10 (validation), 11 (documentation) and 13 (order of work: step 3). It replaces the withdrawn plan "glyph store emptied at the next chapter"; that plan's RenderLock guard and chapter-open call are Task 7 and Task 8 here. Not prototyped: the rulings an executor makes go in the ledger.

## Global Constraints

- Base: master once PR #531 (the serial test API) and the synthetic-bold PR have merged; until then branch from `feat/serial-api-ttf-fs`. Both touch `TtfFont.cpp` (`ownsBitmap`, `noteCommit`); rebase onto whichever lands first.
- `glyphstore::kFormatVersion` becomes 4; an older store reads as not installed and is installed over. `ttf::kRenderVersion` and `SECTION_FILE_VERSION` stay: no glyph or layout changes.
- Every byte is programmed at most once between erases: every store test keeps `StoreTest::TearDown`'s `violations() == 0` and `reprograms() == 0`.
- Erases happen only at install, wipe, a segment the head moves into (`eraseWritten`, so a blank one costs a scan), the reclaimed segment, and the index at a rebuild.
- A pointer the store hands out (`find()`'s EpdGlyph, `logBase()`) stays valid until `close()`, `open()`, `beginInstall()`, `wipe()` or `makeRoom()`. `makeRoom()` and `wipe()` run only where no record of the log is in use: under RenderLock, outside a lookup or a draw.
- Integer only: `scripts/check_no_float.py` passes on the TTF objects.
- Heap: the use counts are the only new allocation (`indexSlots / 4` bytes, nothrow, freed at `close()` and `wipe()`); a reclaim copies through a 512-byte stack buffer.
- Comments say what the code does and why; no history narration. User-facing strings: none new (the busy popup stays `STR_LOADING_FONT`).
- clang-format 21.1.8 (`bin/clang-format-fix` with it first on the PATH). Host tests from Git Bash: configure with `-DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1`, build with `cmake --build build/test -j 8 -- -k 0`. Three targets do not build on Windows (`PngDecoderArenaTest`, `BookMetadataCacheTest`, `EpubPipelineTest`); CI builds them.
- Firmware: `pio run -e default` with `PLATFORMIO_CORE_DIR=C:/pio`, one `pio run` at a time, in the background. `x4pro` and `lilygo_t5s3` are left to CI.
- Devices: never pulse RTS or run esptool against an X3 (the maintainer flashes it). Read each setting with `CMD:GET` before changing it, restore it afterwards (the X4 runs in German).

## Review Focus

1. **A slot into a segment that was reclaimed and filled again** at the same offsets by other glyphs: the old key reads as not stored, never as another glyph's bytes. Task 4's `AStaleSlotIsNeverServedFromAReusedSegment`.
2. **Epochs wrapping the slot's 12 bits** after thousands of reclaims without a rebuild: a rebuild comes first. Task 5's `TheIndexIsRebuiltBeforeEpochsWrap`.
3. **A power cut at any step** of a head move, a reclaim or a rebuild, then several boots: what was stored is found intact or not at all, writing resumes, and the next `makeRoom()` finishes. Task 6.
4. **A reclaim with no counts** (before they are due, or the heap refused them): the oldest segment is dropped whole, never a wipe. Task 4's `AReclaimBeforeTheCountsAreKeptDropsTheSegmentWhole`.
5. **Room made while the reader draws**: `makeRoom` refuses without RenderLock and the idle step only tries the lock. Task 7's `MakingRoomWaitsForRenderLock`, and the reviewer reads Task 8's idle step against `serviceBackgroundWork()`.

---

### Task 1: Format 4: segments, epochs, the rebuild journal

**Files:**
- Modify: `lib/EpdFont/ttf/GlyphStoreFormat.h`, `lib/EpdFont/ttf/GlyphStoreFormat.cpp` (`planLayout`)
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.cpp` (`validGeometry`)
- Modify: `test/glyph_store/GlyphStoreTestUtil.h` (`smallLayout`)
- Test: `test/glyph_store/GlyphStoreFormatTest.cpp`, `test/glyph_store/GlyphStoreInstallTest.cpp`

**Interfaces:**
- Produces, in `namespace glyphstore`:
  - `kFormatVersion = 4`, its history comment gaining "4: the log is a ring of segments, each opened by a SegmentHeader; a glyph slot's value packs its segment's epoch with the record's offset; the state sector keeps a journal of index rebuilds."
  - `struct SegmentHeader { uint32_t magic; uint32_t epoch; uint32_t epochCheck; uint32_t retired; }` (16 bytes; `epochCheck == ~epoch`; `retired` reads `kErased` while the segment is live and is programmed 0 before it is erased).
  - `kSegmentMagic = 0x544D4753` ("SGMT"), `kSegmentHeaderBytes = 16`, `kMinSegments = 4`, `kMaxSegments = 64`, `kFreeSegmentsTarget = 2`.
  - `kSlotOffsetBits = 20`, `kSlotEpochMask = 0xFFF`, `kMaxLogBytes = 1u << (kSlotOffsetBits + 2)` (4 MiB).
  - `constexpr uint32_t glyphSlotValue(uint32_t epoch, uint32_t dataOffset)` = `((epoch & kSlotEpochMask) << kSlotOffsetBits) | (dataOffset >> 2)`; `constexpr uint32_t slotDataOffset(uint32_t value)`; `constexpr uint32_t slotEpoch(uint32_t value)`.
  - `kRebuildEpochSpan = 2048`, `kCountsAtPercent = 75`, `kRebuildStalePercent = 12` (a rebuild pays when at least this share of the index's slots is stale).
  - `struct JournalEntry { uint32_t base; uint32_t done; }`, `kJournalOffset = kWipeWordOffset + 4`, `kJournalEntries = (kStateSectorOffset + kSectorBytes - kJournalOffset) / sizeof(JournalEntry)` (479).
  - `Geometry` gains `uint32_t segmentBytes` as its last field. `planLayout` sets it to `kPageBytes` (the log already starts on a page).
  - `kLogFullPercent` is removed (a full ring is "no segment free", Task 2).
- `smallLayout(indexSlots = 1024, pairSlots = 512, logBytes = 65536, segmentBytes = 4096)`: the test store is 16 segments of one sector.

- [ ] **Step 1: Write the failing tests.** In `GlyphStoreFormatTest.cpp`:

```cpp
TEST(GlyphStoreFormat, ASlotValuePacksTheEpochAndTheOffset) {
  const uint32_t value = glyphSlotValue(0x1234, 0x3FFFFC);
  EXPECT_EQ(0x234u, slotEpoch(value));  // the low 12 bits
  EXPECT_EQ(0x3FFFFCu, slotDataOffset(value));
}

TEST(GlyphStoreFormat, AGlyphSlotValueIsNeverNoGlyphOrErased) {
  // The first record's bitmap is past its segment header and its own header; the last ends inside 4 MiB.
  const uint32_t first = kSegmentHeaderBytes + kRecordHeaderBytes;
  for (uint32_t epoch : {0u, 1u, 0xFFFu, 0x1000u}) {
    EXPECT_NE(kNoGlyph, glyphSlotValue(epoch, first));
    EXPECT_NE(kErased, glyphSlotValue(epoch, kMaxLogBytes - 4));
  }
}

TEST(GlyphStorePlan, TheLogIsCutIntoPageSegments) {
  // in, out: the PlanInput and Layout of CjkSdModeGetsTheLargestLog, planned the same way.
  ASSERT_TRUE(planLayout(in, out));
  EXPECT_EQ(kPageBytes, out.geometry.segmentBytes);
  EXPECT_EQ(0u, out.geometry.logOffset % kPageBytes);
  EXPECT_EQ(0u, out.geometry.logSize % out.geometry.segmentBytes);
  EXPECT_GE(out.geometry.logSize / out.geometry.segmentBytes, kMinSegments);
}
```

`HeaderLayoutIsStable` gets the new `sizeof(Geometry)` and `sizeof(Header)` (both grow by 4) and `static_assert(sizeof(SegmentHeader) == kSegmentHeaderBytes)`; `kJournalEntries == 479`. In `GlyphStoreInstallTest.cpp`:

```cpp
TEST(GlyphStoreInstall, ALayoutWithBadSegmentsIsRefused) {
  SimNorFlash flash(1u << 20);  // room for the 65-segment log below, so only its segment count refuses it
  FlashGlyphStore store(flash);
  for (const uint32_t segment : {0u, 1000u, 24576u /* does not divide 64 KB */, 32768u /* 2 segments */}) {
    EXPECT_FALSE(store.beginInstall(smallLayout(1024, 512, 65536, segment), kIdentityA, nullptr)) << segment;
  }
  // 65 segments: one past kMaxSegments.
  EXPECT_FALSE(store.beginInstall(smallLayout(1024, 512, 65 * kSectorBytes, kSectorBytes), kIdentityA, nullptr));
  EXPECT_TRUE(store.beginInstall(smallLayout(1024, 512, 64 * kSectorBytes, kSectorBytes), kIdentityA, nullptr));
}
```

- [ ] **Step 2: Run them to see them fail.**
Run: `cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='*Slot*:*Segment*:*Plan*:*HeaderLayout*'`
Expected: compile errors for the missing names, then FAILs on the plan and refusal tests.

- [ ] **Step 3: Add the constants, `SegmentHeader`, the slot helpers, `JournalEntry` and `Geometry::segmentBytes`; set it in `planLayout`; in `validGeometry` require `segmentBytes >= kSectorBytes`, `segmentBytes % kSectorBytes == 0`, `logSize % segmentBytes == 0`, `kMinSegments <= logSize / segmentBytes <= kMaxSegments` and `logSize <= kMaxLogBytes`.** Remove `kLogFullPercent` and its test use (Task 2 replaces that check).

- [ ] **Step 4: Run the whole store suite.**
Run: `./build/test/glyph_store/GlyphStoreTest.exe`
Expected: PASS except `ANearlyFullLogCountsAsFull`, disabled in this step with a `DISABLED_` prefix and a comment pointing at Task 2, which replaces it.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): glyph store format 4: segments, epoch-tagged slots, a rebuild journal"`

### Task 2: The log is a ring of segments

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp`
- Modify: `test/glyph_store/GlyphStoreTestUtil.h`, `GlyphStoreGlyphTest.cpp`, `GlyphStoreRecoveryTest.cpp`, `GlyphStoreSizesPairsWipeTest.cpp`, `GlyphStorePowerCutTest.cpp`
- Create: `test/glyph_store/GlyphStoreRingTest.cpp` (add it to `test/glyph_store/CMakeLists.txt`)

**Interfaces:**
- Consumes: Task 1's format.
- Produces, on `FlashGlyphStore`:
  - `bool needsRoom() const`: fewer than `kFreeSegmentsTarget` segments free, or a rebuild is due (Task 5), or `markFull()` asked for a reclaim.
  - `uint32_t segmentCount() const`, `uint32_t freeSegments() const`, `uint32_t liveSlots() const`, `uint32_t epoch() const` (the epoch the next segment gets).
  - `logUsed()`: the bytes the live segments hold, their headers included.
  - Commit semantics: no free segment for the next record → `Commit::Full`, writes stop, `wipePending()` stays false and `needsRoom()` is true. The same for an index past `kIndexLoadPercent` or a full probe chain (both make a rebuild due, Task 5). A failed write still sets wipe pending, as today.
  - Private state the later tasks use: `uint32_t segEpoch_[kMaxSegments]` (`kErased` when not live), `uint8_t head_`, `uint8_t tail_`, `uint8_t live_` (segments from tail to head), `uint32_t headUsed_` (bytes used in the head, header included), `uint32_t nextEpoch_`, `uint32_t liveSlots_`, `bool chainFull_`, `bool indexFull_`.
  - Private `bool moveHead()`: prepares the segment after the head (`eraseWritten` over it, then a header with `nextEpoch_++`) and makes it the head; false with no free segment or on a failed write.
  - Private `const EpdGlyph* validRecord(uint32_t key, uint32_t value) const` now takes the slot's value: the segment `slotDataOffset(value) / segmentBytes` is live, its epoch's low 12 bits equal `slotEpoch(value)`, the offset lies inside what that segment holds (`headUsed_` for the head), and the record there has this key and passes `plausibleRecord`.
- Test util: `writeSegmentHeader(SimNorFlash&, const Layout&, uint32_t segment, uint32_t epoch)` and `inline uint32_t inSegment(const Layout&, uint32_t segment, uint32_t offset)` (= `segment * segmentBytes + kSegmentHeaderBytes + offset`), for the recovery tests' raw records.

How the ring sits on flash and is found at `open()`:
- Segments are used in physical order: the head moves to `(head_ + 1) % count`. Epochs grow with every segment the head moves into, so along the ring from the tail they increase.
- `open()` reads every segment header. A valid one (`magic`, `epochCheck == ~epoch`, `retired == kErased`) is live material. The head is the valid segment with the largest epoch; walking back from it, each segment whose header is valid and whose epoch is smaller than the one after it is live; the first that is not ends the walk (it and everything before it back to the head are free). No valid header at all: an empty ring, head 0 not yet prepared (the first commit prepares it). `nextEpoch_` is the largest valid epoch plus one (1 for an empty ring).
- The head's write position comes from `scanLog()` limited to the head segment (start at its offset `kSegmentHeaderBytes`, end at its end; the torn-write logic as today, with sector boundaries inside the segment). Other live segments are closed: nothing is written to them again.
- `liveSlots_` at open: the records the live segments hold (a walk with `plausibleRecord`) plus the slots holding `kNoGlyph`.
- `writeBatch()`: a record never crosses a segment's end. Records go one program call per run that fits the head, the head moving on in between; a record longer than a segment's payload (`segmentBytes - kSegmentHeaderBytes`) is `Commit::Refused`. Each slot gets `glyphSlotValue(segEpoch_[segment], dataOffset)`. Record before slot, as today.

- [ ] **Step 1: Write the failing ring tests** in `GlyphStoreRingTest.cpp` (fixture `StoreTest`, layout `smallLayout()`: 16 segments of 4 KB):
  - `TheFirstRecordFollowsTheFirstSegmentsHeader`: after one commit, the log starts with `SegmentHeader{kSegmentMagic, 1, ~1u, kErased}` and the glyph's `dataOffset == kSegmentHeaderBytes + kRecordHeaderBytes`.
  - `ARecordThatDoesNotFitTheHeadOpensTheNextSegment`: records of `recordSize(60, 60)` (920 B) until the fifth; the fifth's `dataOffset == 4096 + kSegmentHeaderBytes + kRecordHeaderBytes`, the second segment's header has epoch 2; all five `storedIntact`.
  - `ABatchAcrossTwoSegmentsIsStoredWhole`: a batch of eight such records committed at once: `Commit::Stored`, all intact, `segmentCount() - freeSegments() == 2`.
  - `TheRingResumesAtItsHeadAfterAReopen`: fill three segments, reopen, commit one more: it lands in the third segment after the last record; `epoch()` is 4.
  - `AFullRingStopsWritesWithoutAWipe`: commit until `Commit::Full`; then `wipePending()` false, `writesStopped()` true, `needsRoom()` true, every glyph stored before is `storedIntact`, `freeSegments() == 0`.
  - `ARecordLargerThanASegmentIsRefused`: a 72 x 120 glyph (a 2,160-byte bitmap) is stored; one of 90 x 200 (4,600 bytes, over the 4,080-byte payload) gets `Commit::Refused` and nothing is written. The batch buffer is 8 KB.
  - `ATornSegmentHeaderIsPreparedAgain`: `cutAfterSteps` lands inside the second segment's header write; after `powerCycle()` and `open()` the head is the first segment; the next record that does not fit it prepares the second again (its header valid, epoch 2), and the store's records are intact.

- [ ] **Step 2: Run them to see them fail.**
Run: `cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='*Ring*:StoreTest.*Segment*'`
Expected: FAIL (records start at offset 0, no headers, one linear log).

- [ ] **Step 3: Implement the ring in `FlashGlyphStore`** as the Interfaces block says: `open()`, the per-segment `scanLog()`, `moveHead()`, `writeBatch()`, `find()`/`validRecord()` with slot values, `insertGlyphSlot()` and `markNoGlyph()` (a full index or probe chain: `indexFull_` / `chainFull_`, writes stop for room, no wipe), `wipe()` resetting the ring (every segment free, `nextEpoch_` 1), `close()` resetting it. Writes that stop for room go through a new private `stopForRoom()`; `stopWrites()` keeps meaning "a failure: wipe pending".

- [ ] **Step 4: Bring the existing store tests onto the ring.** The rule: an offset into the log is now `inSegment(layout, segment, offset)`, a raw record needs its segment's header first (`writeSegmentHeader(flash, layout, 0, 1)`), and "full" no longer means "wipe pending".
  - `RecordsAreLaidOutAsTheFormatSays`: records after the segment header; slot values are `glyphSlotValue(1, dataOffset)`.
  - `ANearlyFullLogCountsAsFull` (disabled in Task 1) is deleted: `AFullRingStopsWritesWithoutAWipe` replaces it.
  - `SeventyPercentOfTheIndexCountsAsFull`, `ALongProbeChainCountsAsFull`: `Commit::Full`, `needsRoom()` true, `wipePending()` false.
  - The recovery tests: each writes `writeSegmentHeader(flash, layout, 0, 1)` before its raw records and uses `inSegment()` offsets; `GarbageAtTheVeryEndLeavesTheLogFull` becomes `GarbageAtTheEndOfTheHeadClosesIt`: the next record goes to the next segment.
  - `ReinstallingErasesOnlySectorsThatHoldData`, `WipeErasesOnlySectorsThatHoldData`, `WipingAnEmptyStoreErasesOnlyTheStateSector`: recount with the segment headers (an empty store has none).
  - The power-cut sweep's layout is `smallLayout()`'s (16 segments); its expectations do not change.

- [ ] **Step 5: Run the store suite.**
Run: `./build/test/glyph_store/GlyphStoreTest.exe`
Expected: PASS, every test.

- [ ] **Step 6: Run `TtfFontTest`.**
Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
Expected: PASS but the tests that count on a full store being wiped (`AWipeAtTheNextBookOpenEndsTheRing`, `AStoreMarkedFullActsAsAFullOneAndIsCounted` and any other that reaches `Commit::Full`): they fail until Task 7. List them in the ledger; do not change them here.

- [ ] **Step 7: Commit.** `git commit -m "feat(ttf): the glyph store's log is a ring of segments"`

### Task 3: Use counts

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp`
- Create: `test/glyph_store/GlyphStoreRoomTest.cpp` (add to CMakeLists)

**Interfaces:**
- Consumes: Task 2's ring and `validRecord(key, value)`.
- Produces:
  - `void noteDrawn(const EpdGlyph* glyph) const`: when counts are kept and `glyph` is a record of the log (its address inside the mapped log, its key 4 bytes before it), adds one to its slot's count, at most 3, and to `drawsCounted()`. Anything else: nothing.
  - `bool countsKept() const`; `uint8_t useCount(uint32_t key) const` (the count of the slot `find()` would answer with; 0 when counts are not kept or the key is not stored); `uint32_t drawsCounted() const` (draws counted since `open()`).
  - Private `std::unique_ptr<uint8_t[]> useCounts_` (2 bits a slot, `indexSlots / 4` bytes, `makeUniqueNoThrow`), `bool countsTried_`, and `uint32_t findSlot(uint32_t key, uint32_t value) const` (the slot holding exactly this key and value; `UINT32_MAX` when none).
  - Counts are allocated, once a session, after a commit leaves the live segments at `kCountsAtPercent` of the log or more, or `freeSegments() <= kFreeSegmentsTarget + 1`. Refused: logged once, not asked again until `open()`. Freed by `close()` and `wipe()`.

- [ ] **Step 1: Write the failing tests** in `GlyphStoreRoomTest.cpp`:
  - `NoCountsAreKeptWhileTheRingHasRoom`: one commit; `countsKept()` false; `noteDrawn` of the found glyph changes nothing (`useCount` 0).
  - `CountsAreKeptOnceTheRingNearsFull`: commit until `freeSegments() == kFreeSegmentsTarget + 1`; `countsKept()` true.
  - `DrawsAreCountedUpToThree`: with counts kept, a glyph drawn 0, 1, 2, 3 and 5 times reads 0, 1, 2, 3, 3.
  - `ADrawOfSomethingElseIsIgnored`: `noteDrawn` of a stack `EpdGlyph`, of `nullptr`, and of a pointer one past the log: no count changes, no crash.
  - `CountsStartAtZeroAfterAReopen`: a counted glyph reads 0 after `open()` (and `countsKept()` is false until the trigger again).

- [ ] **Step 2: Run them to see them fail** (`--gtest_filter='*Count*'`). Expected: compile errors, then FAIL.

- [ ] **Step 3: Implement** `noteDrawn`, `countsKept`, `useCount`, `findSlot`, the allocation trigger in `writeBatch()`, and the frees.

- [ ] **Step 4: Run the store suite.** Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): the glyph store counts how often its glyphs are drawn"`

### Task 4: A reclaim makes room

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp`
- Test: `test/glyph_store/GlyphStoreRoomTest.cpp`

**Interfaces:**
- Consumes: Tasks 2 and 3.
- Produces:
  - `enum class Budget : uint8_t { Idle, Open }` and `enum class Room : uint8_t { Enough, More, Failed }`.
  - `struct RoomWork { uint16_t reclaimed = 0; uint16_t kept = 0; uint16_t dropped = 0; bool rebuilt = false; }`.
  - `Room makeRoom(Budget budget, RoomWork* work = nullptr)`: `Idle` does one step (one segment reclaimed, or Task 5's rebuild when the index stops writes); `Open` steps until enough, at most `segmentCount()` reclaims. `Enough` when `!needsRoom()` afterwards (writes resume: `writesStopped()` false unless wipe is pending), `More` when steps remain, `Failed` on a flash error (wipe now pending). Only where no record of the log is in use.
  - `markFull()` now: writes stop, the counts are allocated if they are not kept yet, and the next `makeRoom()` reclaims the oldest segment however many are free, when two or more are live; either way writes resume after it. False when the store is not open. The serial test API's `CMD:TTF:FULL`.

The reclaim of the tail segment T (only when two or more segments are live):
1. Walk T's records. A record is a candidate when `findSlot(key, glyphSlotValue(epoch of T, its dataOffset))` finds its slot and that slot's count is above 0; with no counts kept, none is.
2. At most half T's payload (`(segmentBytes - kSegmentHeaderBytes) / 2`) is kept: when the candidates come to more, the least used go first, and among equal counts the oldest (the earliest in T). What is kept must also fit the head's room and the free segments; otherwise nothing is kept. So the first reclaim of a ring that filled completely (no segment free, no idle time to keep two) drops its oldest segment whole, and the ones after it copy as usual.
3. Each kept record, oldest first, is copied to the head (moving it on as needed) through a 512-byte stack buffer, its `dataOffset` rewritten in the first piece; then its slot is inserted with the new segment's epoch, and the new slot's count is the old one halved (3 → 1, 2 → 1, 1 → 0).
4. T's `retired` word is programmed 0, T is erased (`eraseWritten`), and T leaves the live segments (`segEpoch_[T] = kErased`, the tail moves on). Its slots are now stale: `liveSlots_` drops by the records T had slots for, and rises by the copies.

- [ ] **Step 1: Write the failing tests** in `GlyphStoreRoomTest.cpp`. A helper fills the ring with 40 x 58 glyphs (600-byte records, six to a 4 KB segment, keys counting up) until `freeSegments() == 1`, so counts are kept, T is segment 0 and the copies have room.
  - `AReclaimKeepsDrawnGlyphsAndDropsTheRest`: draw keys 0 and 3 of segment 0; `makeRoom(Budget::Idle, &work)` returns `Enough` or `More`; `work.reclaimed == 1`, `work.kept == 2`, `work.dropped == 4`; keys 0 and 3 `storedIntact`, keys 1, 2, 4, 5 `NotStored`; everything in other segments intact; writes resume.
  - `AReclaimHalvesTheCountsOfWhatItKeeps`: counts 3, 2, 1 on three glyphs of segment 0 read 1, 1, 0 after the reclaim; the one at 0 is dropped by the reclaim that next reaches its segment.
  - `AReclaimFreesAtLeastHalfASegment`: every glyph of segment 0 drawn, two of them twice; only three records (1,800 B of the 4,080-byte payload) survive: the two drawn twice, and the newest of the rest.
  - `AStaleSlotIsNeverServedFromAReusedSegment`: reclaim segment 0 dropping everything (no draws), then commit other keys of the same size, calling `makeRoom(Budget::Open)` whenever a commit returns `Commit::Full`, until the head has come round and filled segment 0 again at the same offsets; every key of the first fill reads `NotStored` (never `Found`), every new one `storedIntact`.
  - `AFullRingsFirstReclaimDropsItsOldestWhole`: fill until `Commit::Full` (no segment free), draw every glyph of segment 0, `makeRoom(Budget::Idle, &work)`: `work.kept == 0`, `freeSegments() == 1`; the next `makeRoom(Budget::Idle, &work)` keeps segment 1's drawn glyphs.
  - `AReclaimBeforeTheCountsAreKeptDropsTheSegmentWhole`: three segments filled (counts not due), one glyph drawn (not counted: no counts yet), then `markFull()`, `makeRoom(Budget::Idle, &work)`: `work.kept == 0`, segment 0's keys `NotStored`, `wipePending()` false.
  - `OpenMakesRoomUntilEnough`: a full ring, `makeRoom(Budget::Open)` returns `Enough`, `freeSegments() >= kFreeSegmentsTarget`, a commit stores.
  - `CopiesSurviveAReopen`: reclaim with keepers, reopen, keepers `storedIntact`, the head after the copies.
  - `TheRingWearsItsSegmentsEvenly`: two hundred fill-and-reclaim rounds with no draws; every log sector's `sectorErases()` within one of the others'.

- [ ] **Step 2: Run them to see them fail** (`--gtest_filter='*Reclaim*:*Room*:*Wear*:*Stale*:*Copies*'`). Expected: compile errors, then FAIL.

- [ ] **Step 3: Implement `makeRoom()` and the reclaim**, with `markFull()`'s new meaning.

- [ ] **Step 4: Run the store suite.** Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): a full glyph store reclaims its oldest segment, keeping what is drawn"`

### Task 5: The index is rebuilt from the log

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp`
- Test: `test/glyph_store/GlyphStoreRoomTest.cpp`

**Interfaces:**
- Consumes: Task 4's `makeRoom`.
- Produces: `uint32_t rebuildBase() const` (the epoch the last rebuild started from; tests), `uint32_t rebuilds() const` (this session's), and `RoomWork::rebuilt`.

When a rebuild is due:
- the index is past `kIndexLoadPercent` (`indexFull_`) or a probe chain filled (`chainFull_`): writes are stopped, so `Idle` rebuilds too;
- `epoch() - rebuildBase() >= kRebuildEpochSpan`, or the journal's last entry has `base` written and `done` erased (a rebuild a power cut interrupted): `Open` only;
- at `Open`, also when stale slots (`usedSlots() - liveSlots()`) are at least `kRebuildStalePercent` of the index's slots.

When the index is full and stale slots are under that share, a rebuild would not help: `makeRoom` reclaims a segment first.

The rebuild:
1. The journal is full (`kJournalEntries` used): the store wipes instead (`wipe()`), and `makeRoom` returns what the wipe did.
2. A journal entry's `base` is programmed: the epoch of the tail segment (the oldest any slot written from now on can name).
3. The live records' counts are gathered, 2 bits each in walk order, into a nothrow buffer (none to be had: counts start at 0).
4. The index is erased (`eraseWritten`), then each live segment from the tail, each record in order, is inserted again with `glyphSlotValue(its segment's epoch, dataOffset)`, unless `find()` already finds its key (a duplicate a cut left keeps its first copy); counts follow. A full chain here stops writes with wipe pending.
5. The entry's `done` is programmed 0; `usedSlots_ = liveSlots_ =` the slots inserted; `chainFull_`, `indexFull_` cleared.

`open()` reads the journal: the number of entries, the last entry's `base` (0 with none), and whether it is done.

- [ ] **Step 1: Write the failing tests:**
  - `ACrowdedIndexIsRebuiltFromTheLog`: `smallLayout(256, 512, 65536)`, fill and reclaim until `Commit::Full` comes from the index (`needsRoom()` with `freeSegments() >= kFreeSegmentsTarget`); `makeRoom(Budget::Idle, &work)`: `work.rebuilt`, `usedSlots() == liveSlots()`, every live key `storedIntact`, its count kept.
  - `ARebuildForgetsTheNoGlyphMarks`: a `markNoGlyph` key reads `NoGlyph` before and `NotStored` after.
  - `TheIndexIsRebuiltBeforeEpochsWrap`: reclaim rounds (commit until `Commit::Full`, then `makeRoom(Budget::Open)`) until `epoch()` passes 5,000. After each round, `epoch() - rebuildBase() < kRebuildEpochSpan + segmentCount()`; every 250 rounds, every key committed so far is `NotStored` or `storedIntact` (keys are `makeKey(n, 0, 0)` for n counting up).
  - `AFullProbeChainAsksForARebuildNotAWipe`: the colliding-keys setup of `ALongProbeChainCountsAsFull`; `Commit::Full`, `wipePending()` false, `makeRoom(Budget::Idle)` rebuilds and the commit then stores.
  - `AFullJournalTurnsTheRebuildIntoAWipe`: program `kJournalEntries` done entries straight into the state sector, reopen, make a rebuild due: the store is wiped (`logUsed() == 0`, journal empty).
  - `IdleLeavesAnEpochRebuildToTheNextOpen`: with only the epoch span due, `Budget::Idle` does not rebuild, `Budget::Open` does.

- [ ] **Step 2: Run them to see them fail** (`--gtest_filter='*Rebuil*:*Journal*:*Wrap*:*ProbeChain*'`). Expected: FAIL.

- [ ] **Step 3: Implement the rebuild, its triggers and the journal.**

- [ ] **Step 4: Run the store suite.** Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): the glyph index is rebuilt from the log when stale slots crowd it"`

### Task 6: Power cuts during a reclaim and a rebuild

**Files:**
- Modify: `test/glyph_store/GlyphStorePowerCutTest.cpp`
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.cpp` only for what the sweeps find

**Interfaces:**
- Consumes: Tasks 2-5. Produces: nothing new.

- [ ] **Step 1: Write the sweeps**, in the shape of `sweepWipe`:
  - `EveryCutDuringAHeadMoveLeavesAConsistentStore`: a batch across a segment's end, cut at every step.
  - `EveryCutDuringAReclaimLeavesAConsistentStore`: Task 4's helper ring (one segment free, counts kept) with four glyphs of segment 0 drawn; for each cut point of `makeRoom(Budget::Idle)`: `powerCycle()`, then three sessions (open, `makeRoom(Budget::Open)`, commit two new glyphs, close). After each boot: every glyph committed before the cut is `absentOrIntact`, the new glyphs are `storedIntact`, and `wipePending()` is false.
  - `EveryCutDuringARebuildLeavesAConsistentStore`: a crowded index; the same three sessions after each cut; after the first, the interrupted entry is redone (`rebuilds() == 1` in that session) and `usedSlots() == liveSlots()`.
  - Both with `partialCut(true)` as a second pass. `TearDown`'s `violations()` and `reprograms()` stay 0.

- [ ] **Step 2: Run them.**
Run: `./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='GlyphStorePowerCut.*'`
Expected: PASS. A failure is a defect in Tasks 2-5: fix it with a focused test first (superpowers:systematic-debugging), and ledger it.

- [ ] **Step 3: Commit.** `git commit -m "test(ttf): power cuts during a reclaim and a rebuild"`

### Task 7: The font set makes room

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`
- Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: `FlashGlyphStore::makeRoom`, `needsRoom`, `noteDrawn`, `markFull`.
- Produces:
  - `TtfFont::Hooks` gains `bool (*countsDraw)() = nullptr;`: whether a glyph drawn now counts as a use (the app: the renderer's black-and-white pass, the first of a page's passes); null counts every draw.
  - `bool TtfFontSet::makeRoom(FlashGlyphStore::Budget budget, const std::function<void()>& onBusy = {})`: false, doing nothing, when the store is not open or when the `renderLockHeld` hook is set and returns false. Runs a pending wipe first (`wipeIfPending`). With room needed, calls `onBusy` for `Budget::Open` only, then `store_.makeRoom(budget, &work)`; when writes resume, `ringMode_` goes false and the ring is freed. True when it did work.
  - `wipeIfPending` takes the same RenderLock guard (the withdrawn plan's Task 1).
  - `TtfFontSet::ownsBitmap`: past the ring and `kIgnorable` checks, `store_.noteDrawn(glyph)` when `!hooks.countsDraw || hooks.countsDraw()`.
  - `noteCommit`: `Commit::Full` without wipe pending logs `"glyph store full: new glyphs are not stored until room is made"`.
  - `Stats` gains `uint16_t segments`, `uint16_t freeSegments`, `uint32_t liveSlots`, `uint32_t drawsCounted` (the store's). `StoreEvents` gains `uint32_t rooms` (makeRoom calls that did work), `uint32_t rebuilds`, `uint32_t dropped` (glyphs reclaims dropped, since boot), `uint32_t lastRoomMs`.

- [ ] **Step 1: Write the failing tests:**
  - `MakingRoomWaitsForRenderLock`: the withdrawn plan's `AWipeWaitsForRenderLock`, with a full ring (`markFull()`) and `makeRoom(Budget::Open, ...)`: false and no `onBusy` while the hook says not held; true, `onBusy` once, `ringMode()` false once it is.
  - `AWipeWaitsForRenderLock`: as the withdrawn plan's Task 1 wrote it (a failed write, so wipe pending).
  - `MakingRoomEndsTheRing`: a set whose store filled (`markFull()` then a new glyph: `ringMode()`), `makeRoom(Budget::Idle)`: `ringMode()` false, the next new glyph is a store hit on its second lookup.
  - `ADrawnStoredGlyphIsCounted`: a stored glyph (`font->glyph(kRegular, 'A')`), then `markFull()` (which keeps counts); `ownsBitmap` of it returns false and `stats().drawsCounted` is 1; with a `countsDraw` hook returning false it stays 0; `ownsBitmap` of a ring glyph is not counted.
  - `TheStatsShowTheRing`: `stats().segments`, `freeSegments`, `liveSlots` after a few glyphs; `storeEvents().rooms` and `dropped` after a reclaim.
  - The tests Task 2 left failing are brought onto the new meaning: a full store is now one that needs room (`AStoreMarkedFullActsAsAFullOneAndIsCounted`: `markFull()` → ring mode and a fill counted; `makeRoom` → `rooms` counted).

- [ ] **Step 2: Run them to see them fail.**
Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
Expected: FAIL on the new tests and the ones Task 2 listed.

- [ ] **Step 3: Implement** the hook, `makeRoom`, the guard, `ownsBitmap`'s count, `noteCommit`'s line, stats and events (atomics, as the fills and wipes).

- [ ] **Step 4: Run `TtfFontTest` and the store suite.** Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): the font set makes room in its glyph store under RenderLock"`

### Task 8: The reader makes room: idle time, chapter and book open

**Files:**
- Modify: `lib/EpdFont/SdCardFontManager.h`, `lib/EpdFont/SdCardFontManager.cpp`, `src/SdCardFontSystem.h`, `src/SdCardFontSystem.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.h`, `src/activities/reader/EpubReaderActivity.cpp`
- Modify: `src/SerialTestApi.cpp`, `docs/serial-test-api.md`
- Create: `tools/make_cjk_store_fill_epub.py` (from the prototype `C:/tmp/wt-wipe-proto/tools/make_cjk_store_fill_epub.py`, as the withdrawn plan's Task 2 Step 3 describes)
- Modify: `docs/file-formats.md` (the glyph store: format 4), `docs/future_work/cjk.md` (the store section: shipped)

**Interfaces:**
- Consumes: Task 7's `TtfFontSet::makeRoom` and hook.
- Produces:
  - `bool SdCardFontManager::makeTtfRoom(FlashGlyphStore::Budget budget, const std::function<void()>& onBusy)` and the `SdCardFontSystem` passthrough of the same name.
  - `bool EpubReaderActivity::stepGlyphStoreRoom()`: under `RenderLock lock(RenderLock::Mode::Try)` (no lock: false), `sdFontSystem.makeTtfRoom(Budget::Idle, {})`.
  - The app's `countsDraw` hook in `SdCardFontSystem::begin`: `ttfRenderer && ttfRenderer->getRenderMode() == GfxRenderer::BW`.

- [ ] **Step 1: Wire it.**
  - `SdCardFontSystem::begin`: the hook joins `setHooks`.
  - Book open, in `SdCardFontSystem` where `wipeTtfIfPending(onColdLoad)` runs for loaded TrueType families: then `makeTtfRoom(Budget::Open, onColdLoad)`.
  - Chapter open: in `EpubReaderActivity::buildSection()`, after the invalid-spine early return and before `const uint16_t viewportWidth = layout.viewportWidth;`: `sdFontSystem.makeTtfRoom(FlashGlyphStore::Budget::Open, [this] { GUI.drawPopup(renderer, tr(STR_LOADING_FONT)); });` with a comment: the render pass holds RenderLock here, before the chapter is laid out or drawn, so no record of the log is in use.
  - Idle: in `serviceBackgroundWork()`, after the current-section-build branch and before `stepBackgroundSectionBuild()`: `if (stepGlyphStoreRoom()) return;` (room before Background-B's look-ahead; the section the reader waits on and the AA pass keep priority).
  - Serial: `CMD:TTF` adds `segments= free= live_slots= draws=` (with a set) and `rooms= rebuilds= dropped= last_room_ms=`; a new event `EVT:TTF_ROOM ms=<t> rooms=<n> dropped=<n>` when `rooms` moves. `CMD:TTF:FULL`'s reply and docs: "the next safe point reclaims the oldest segment".

- [ ] **Step 2: Build the firmware.**
Run (background): `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default` with `PLATFORMIO_CORE_DIR=C:/pio PYTHONIOENCODING=utf-8 PYTHONUTF8=1`
Expected: `SUCCESS`; `check_no_float: ... no floating point`.

- [ ] **Step 3: Docs.** `docs/file-formats.md`: the glyph store's format 4 (segments and their headers, slot values, the journal, what a reclaim and a rebuild write and in what order). `docs/serial-test-api.md`: the new fields and event. `docs/future_work/cjk.md`: the store section says shipped.

- [ ] **Step 4: Format and commit.**
```bash
PATH="<clang-format 21 dir>:$PATH" bash bin/clang-format-fix
git commit -m "feat(reader): the glyph store makes room in idle time and at chapter and book open"
```

### Task 9: On the X4 and the X3

**Files:** none; the figures go into the PR description.

- [ ] **Step 1: Flash.** The X4: `pio run -e default -t upload --upload-port COM5`. The X3: the maintainer flashes it.

- [ ] **Step 2: A reclaim at 12 pt.** With `tools/devctl.py --port <port> --log /c/tmp/room-<board>.log`, `cmd:EVENTS:ON`: Noto Sans TC (or any CJK `.ttf` in `/.crosspoint/fonts`), 12 pt, the store-fill book (`tools/make_cjk_store_fill_epub.py`, uploaded to `/test/cjk_store_fill.epub`). Read until `CMD:TTF` shows `free=2` or less, then wait (`idle:60000`).
Expected: `EVT:TTF_ROOM` lines in idle time, `free` back to 2 or more, no `glyph store full` line, the pages draw from the store (`hits` rising faster than `renders`).

- [ ] **Step 3: 26 pt and a forced reclaim.** At 26 pt the store fills within a chapter. Turn pages and chapters; then `cmd:TTF:FULL` on a page and `idle:30000`.
Expected: reclaims at chapter turns behind the popup and in idle time; after `TTF:FULL`, one `EVT:TTF_ROOM`; screenshots (`shot:/c/tmp/room-<board>-<n>.png`) of the pages before and after show the same glyphs (Claude reviews them).

- [ ] **Step 4: Measure and record.** Per board: the time of one reclaim (`last_room_ms`), of a rebuild if one ran, how many glyphs a page drew again after a reclaim (`renders` per page), heap before and after (`CMD:STATE`). Restore every setting changed.
