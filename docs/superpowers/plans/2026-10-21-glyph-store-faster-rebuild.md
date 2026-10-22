# Glyph store: a faster index rebuild — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An index rebuild of the TrueType glyph store holds the loop task for a fraction of today's 1.8 s on the X4, and a CJK store on the X3 keeps the glyphs its log has room for.

**Architecture:** First the rebuild is timed (a clock hook, one debug line a rebuild, a flash bench) and measured on the X4. The SD-mode index is then sized for 12 pt CJK records. Then the index region gets a spare table beside the one in use (format 5): a rebuild copies the live slots of the table in use into the spare in slot order, through a 2 KB window, programming it a 1 KB piece at a time (about 128 program calls instead of one per slot), and switches tables with its journal entry's `done` word. The table in use is never erased by a rebuild; the one left behind is erased in later idle steps.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3/S3), GoogleTest host tests on `SimNorFlash` (`test/glyph_store`, `test/ttf_font`), the serial test API (`docs/serial-test-api.md`), the `bench_glyph_store` sketch.

**Spec:** `docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md` section 8 (this plan replaces 8.2's "How: the index is erased, then every live record inserted again"); the shipped plan `docs/superpowers/plans/2026-10-20-glyph-store-makes-room.md`; `docs/file-formats.md` (CPTT); `docs/future_work/cjk.md`, "Glyph store: a faster index rebuild" (the four ideas).

## Analysis

### Where the 1.8 s goes

From the code (`FlashGlyphStore::rebuildIndex`, `FlashGlyphStore.cpp:1124-1220`), the X3's measured flash timings (`docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, Appendix A) and host counts. The timings: a program call costs 109 µs + 1.95 µs a byte (the fit of the measured 8, 100 and 252-byte writes: 124, 297, 601 µs); erasing 64 KB of written flash 138 ms (ESP-IDF erases an aligned 64 KB run as one block), a blank sector 4-6 ms; a probe through mapped flash about 1.5 µs. The counts come from a device-sized store on `SimNorFlash` (16,384 slots, 28 segments of 64 KB, records of 160-204 B, about 180 on average, as in the store-fill book; a scratch program, not committed): the index stops writes at 11,450 slots in use, 9,280 of them live.

| Phase | Code | Work (host counts) | Estimate, X4 |
|---|---|---|---|
| Count walk | `walkLive` #1 | 9,280 record headers, each a flash-cache miss | ~20 ms |
| Plan walk | `walkLive` #2, `findSlot` | 9,280 × (a header, 1.3-1.5 probes) | ~45 ms |
| Journal `base` | `program` | 1 call | 0.1 ms |
| Index erase | `eraseWritten` | 32 sectors in one call: two 64 KB blocks | ~280 ms |
| Insert walk, reads | `walkLive` #3, `find`, `insert`'s probe | 9,280 × (a header, ~2 probes on the page the last program invalidated) | ~45 ms |
| Insert walk, programs | `insert` → `program` | **9,280 calls of 8 B** | **~1,150 ms** |
| Journal `done` | `program` | 1 call | 0.1 ms |
| **Sum** | | | **~1.54 s of 1.8 s** |

So: the slot programs about 1.15 s (65%), the index erase about 0.28 s (15%), the three log walks with their probes about 0.11 s (6%), and about 0.25 s (14%) unaccounted (the X4's chip may program slower than the X3's; cache refills). Nothing is read back after a write (`CONFIG_SPI_FLASH_VERIFY_WRITE` is not set); the only read-backs are each insert's `find()` and probe on the index page the previous program invalidated, counted in the reads row.

**How often** (the same host store, fed as the store-fill book feeds it: 1,700 new glyphs a chapter, `makeRoom(Open)` at each chapter, an idle step after each page; keys shuffled):

| Store | Index | Log | Live slots after a rebuild | Reclaims per rebuild | Rebuilds in idle time |
|---|---|---|---|---|---|
| X4 (a budget that plans its 2 + 28 pages) | 2 pages | 28 segments | 9,280 | 6.1-7.5 | 3 of 4 per 20,000 glyphs (11 of 14 per 40,000) |
| X3 (32 free map pages) | 1 page | 21 segments | 4,614 | 3.0 | 14 of 14 |

**The X3 finding.** Its one-page index fills when the log holds about 4,600 glyphs, 13 of its 21 segments: at 12 pt the last 40% of the log never holds a glyph, and every third reclaim brings a rebuild. The cause is `kExpectedRecordSd = 250` against records of 150-185 B at 12 pt.

### The ideas

| | Gain | Cost | Wear | Power cut, against the journal | RAM | Mapping budget |
|---|---|---|---|---|---|---|
| **1. Size the index for 12 pt CJK records** (`kExpectedRecordSd` 250 → 160) | X3: index 1 → 2 pages, live glyphs after a rebuild 4,614 → about 6,400 (+40%), a rebuild every ~10 reclaims instead of 3, all at opens (sim). X4: none at 160 B (2 pages either way) | X3 log 21 → 20 pages. No format change: a store keeps its geometry until its next install | fewer rebuilds, fewer index erases | unchanged | the use counts are `indexSlots / 4`: the X3's CJK store's grow 2 → 4 KB once kept (spec 8.2's range: 2.5-6 KB) | X3: one page from the log to the index |
| **2. Program the index a sector at a time** (a RAM bitmap of occupied slots, one log walk per index sector) | removes ~9,300 calls (−1.15 s) but adds a log walk per index sector, 32 of ~20 ms (~0.65 s), and 32 × 4 KB programs (~0.26 s): about 1.2 s | ~120 lines, no format change | as today | as today: a cut after the erase loses every slot not yet programmed | 2 KB bitmap + 4 KB sector buffer + today's plan (3.5 KB): ~9.5 KB transient | none |
| **3. Rebuild more at opens** (`kRebuildStalePercent` 12 → 6) | X4 sim: idle rebuilds 3 of 4 → 0 of 6 (2 of 8 with draws) | +50% rebuilds, each 1.8 s behind the chapter popup on top of a ~1.1 s cold chapter; works only while chapters bring few new glyphs | +50% index erases | unchanged | none | none |
| **4. An A/B index rebuilt in slices** | no long block | slicing needs the counts frozen or a second count array, and the window held across idle ticks while the reader allocates; both tables mapped (+2-3 pages) | half per table | a cut leaves the old table whole | window held for seconds | +index pages |
| **5. (Better) An A/B index copied from the table in use, in slot order** | 128 program calls instead of 9,280; ~129 KB programmed (74 KB of slots, the rest erased gaps): 266 ms if erased bytes cost program time, 159 ms if not, plus a ~30 ms scan. The step that holds the loop: **~0.2-0.3 s instead of 1.8 s**. The spare's erase (~280 ms) leaves the rebuild for idle steps of one page (~140 ms, as a reclaim). Splitting program calls at erased runs of 4-16 slots saves nothing (sim: 265-280 ms) | format 5 (every store installs again once), ~250 lines; Latin flash-mode stores give 1-2 log pages to the spare table (partition-bound: Bookerly 17 → 15, Noto Sans 9 → 8); EspNorFlash keeps up to 5 mappings | one table erased per rebuild, as today, alternating: each table half today's wear | **better than today:** a rebuild never erases the table in use, so a cut before `done` loses nothing (today a cut after the erase loses every slot not yet re-inserted); the switch is the `done` word, written after the spare is complete (any programmed bit reads finished); the table in use is the parity of finished entries; a wipe keeps it | a 2 KB window for one step, instead of today's 3.5 KB plan; counts move in place | only the table in use is mapped; the spare is mapped for the switch before `done`, the old table unmapped after: `max(kSparePages, index pages)` spare pages, the same as today for 1-2 page indexes (every CJK store above) |

Why 5 works with 2 KB: the rebuild only drops slots, so a slot copied in slot order lands between its home and its old place, less than `kMaxProbe` slots behind the scan. A window of two 128-slot pieces therefore holds every place a slot can land, and each piece is programmed once, whole, when the scan leaves it (host sim: every live slot copied, the longest probe 24, no order violated). It needs no log walk: `validRecord()` reads one record header per slot in use.

### My call

- **Measure first** (Tasks 1-2). Idea 5's gain rests on the slot programs: go on only when they are at least 40% of the measured rebuild.
- **Take 1, at 160 B** (Task 3): small, no format change, and it uncaps the X3's log. It can ship alone. 160 rather than 150: at 150 B the X4's index takes a third page and the log a segment (two with idea 5's switch reserve), for rebuilds that idea 5 makes cheap.
- **Take 5 instead of 2** (Tasks 4-7): the same 2 KB as idea 2's bitmap buys 6-9× instead of 1.5×, takes the erase out of the step, and makes a cut rebuild lose nothing. Its price is a format bump and 1-2 log pages on Latin flash-mode stores, which rarely fill.
- **Not 3:** more rebuilds and erases, to hide what 5 removes.
- **Not 4's slicing:** a ~0.25 s step does not need it. It is the follow-up if Task 8 measures the copy step above 250 ms on the X4.

### Measuring it on a device

Task 1 adds the instrumentation: a clock hook, `RebuildTiming` and one `LOG_DBG` line per rebuild, and `BENCH flash_*` lines (program-call cost, whether erased bytes cost program time, sector against block erase). Task 2 reads them on the X4 with the store-fill book (`tools/make_cjk_store_fill_epub.py`) at 12 pt.

## Maintainer decisions (2026-10-22)

1. Glyph store format 5 (the A/B index) is acceptable: every TrueType/OTF store installs again once after the update.
2. The index is sized at 160 B per expected SD record (`kExpectedRecordSd = 160`).
3. Latin flash-mode stores losing 1-2 log pages to the spare index table is acceptable: one code path for all families.

## Global Constraints

- Base: master. `glyphstore::kFormatVersion` becomes 5 (Task 4); an older store reads as not installed and is installed over. `ttf::kRenderVersion` and `SECTION_FILE_VERSION` stay.
- Every byte is programmed at most once between erases: `StoreTest::TearDown`'s `violations() == 0` and `reprograms() == 0` hold in every store test.
- Erases happen only at install, wipe, a segment the head moves into, a reclaimed segment, and the spare index table (at a rebuild, or in an idle step). A rebuild never erases the table in use.
- Pointer lifetimes as today (`FlashGlyphStore.h:66-67`); the log's mapping no longer moves at all.
- Heap: one new allocation, the copy's window, `2 * kRebuildPieceSlots` slots (2,048 B), nothrow, held for one `makeRoom()` step; it replaces the rebuild's plan (`records / 8 + records / 4` B, 3.5 KB at 9,300 records). Stack rejected (over the 256-byte frame rule), static rejected (2 KB of DRAM for a rare step). No new stack buffer over 256 B. Task 3 doubles the X3 CJK store's index, so its use counts (`indexSlots / 4`, taken only once the ring nears full) grow from 2 to 4 KB, inside spec 8.2's 2.5-6 KB.
- Integer only: `scripts/check_no_float.py` passes.
- Comments say what the code does and why; no history. No new user-facing strings.
- clang-format 21 (`bin/clang-format-fix` with it first on the PATH).
- Host tests from Git Bash: configure `cmake -S test -B build/test -G Ninja -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1 -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<googletest> -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=<ArduinoJson>`; build `cmake --build build/test --target <T> -j 8`.
- Firmware: `pio run -e default` (and `gh_release` before the PR) with `PLATFORMIO_CORE_DIR=C:/pio`, one `pio run` at a time, in the background; `x4pro` and `lilygo_t5s3` are left to CI.
- Devices: never pulse RTS or run esptool against an X3 (the maintainer flashes it). Read each setting with `CMD:GET` before changing it and restore it afterwards (the X4 runs in German).

## Review Focus

1. **A cut at any step of a copy, a second copy, the spare's idle erase, or a wipe with table 1 in use:** the table the journal's parity names is whole, and a copy cut before its `done` word loses nothing. Task 6's sweeps, Task 7's `EveryCutWhileTheSpareIsErasedLeavesAConsistentStore`.
2. **A probe chain across a piece boundary or the table's end:** every live slot is found after the copy, with its count. Task 5's `AChainAcrossAPieceBoundaryIsCopiedWhole`, `AChainAcrossTheTablesEndIsCopiedWhole`.
3. **A spare that cannot be mapped for the switch, or a table with no erased slot:** idle time leaves the rebuild to an Open, the Open wipes; never a rebuild every tick, never a ring mode that lasts. Task 5's `ASpareThatCannotBeMappedLeavesTheRebuildToAnOpen`, `AnOpenThatCannotMapTheSpareWipesTheStore`, `ATableWithNoErasedSlotIsWipedAtAnOpen`.
4. **A Latin family at the edge of the partition:** listed by `fitsStore` only if it fits with the second table, and the tight-partition font-set cases keep their intent. Task 4, Step 5.
5. **The idle erase of the spare** runs a page a step, counts as idle work (`CMD:IDLE` waits for it) but not as a room made, never runs at an Open, and leaves the next rebuild nothing to erase. Task 7's tests.

## Files

| File | Tasks | What changes |
|---|---|---|
| `lib/EpdFont/ttf/FlashGlyphStore.h`, `.cpp` | 1, 4, 5, 7 | timing; the table in use mapped alone; the copy; the idle erase |
| `lib/EpdFont/ttf/GlyphStoreFormat.h`, `.cpp` | 3, 4 | `kExpectedRecordSd`; format 5, two tables, `planLayout` |
| `lib/hal/EspNorFlash.h` | 4 | `kMaxMaps` 5 |
| `lib/EpdFont/ttf/TtfFont.h`, `.cpp` | 1, 7 | `Hooks::nowUs`; `makeRoom` runs the idle erase; `Stats` |
| `src/SdCardFontSystem.cpp`, `src/SerialTestApi.cpp` | 1, 7 | the clock; `CMD:TTF` fields |
| `bench/glyph_store_main.cpp` | 1 | `BENCH flash_*` |
| `test/glyph_store/*` | 1, 3-7 | tests, sweeps, `smallLayout`, `keysHomedAt` |
| `test/ttf_font/TtfFontTest.cpp`, `TtfFamilyTest.cpp` | 4, 7 | partition edges; the idle erase |
| `docs/file-formats.md`, `docs/serial-test-api.md`, `docs/future_work/cjk.md` | 4, 5, 7, 8 | format 5; `CMD:TTF`, idle work; measured |

---

### Task 1: Time the rebuild; bench the flash

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp` (`rebuildIndex`)
- Modify: `lib/EpdFont/ttf/TtfFont.h` (`Hooks`), `lib/EpdFont/ttf/TtfFont.cpp` (`TtfFontSet::load`), `src/SdCardFontSystem.cpp` (`ttfNowUs`, the `setHooks` call at line 332)
- Modify: `bench/glyph_store_main.cpp`
- Test: `test/glyph_store/GlyphStoreRoomTest.cpp`

**Interfaces:**
- Produces, on `FlashGlyphStore`:
  ```cpp
  /// Where the last index rebuild's time went: the debug log and the device measurement. Times are 0 without a clock.
  struct RebuildTiming {
    uint32_t walkUs = 0;     ///< the log walks before the index is touched
    uint32_t eraseUs = 0;    ///< erasing the table the slots go to
    uint32_t insertUs = 0;   ///< writing the slots, programs included
    uint32_t programUs = 0;  ///< of insertUs, inside the calls that write slots (their probes included)
    uint32_t programs = 0;   ///< those calls
    uint32_t records = 0;    ///< records the walks visited
    uint32_t slots = 0;      ///< slots written
  };
  void setClock(uint32_t (*nowUs)());  // null: nothing is timed; kept across close() and open()
  const RebuildTiming& lastRebuild() const;
  ```
- `TtfFont::Hooks` gains, as its last member, `uint32_t (*nowUs)() = nullptr;` (microseconds since boot, wrapping). `TtfFontSet::load()` calls `store_.setClock(hooks.nowUs)` before its first `store_.open()`. The app passes `ttfNowUs` (`return micros();`, beside `ttfNowMs`) as the last initializer of `TtfFont::setHooks`.
- The rebuild's one line, at its end: `LOG_DBG("GST", "rebuild: %u records, %u slots; walks %u ms, erase %u ms, inserts %u ms (programs %u ms in %u calls)", ...)`.

- [ ] **Step 1: Write the failing tests.** In `GlyphStoreRoomTest.cpp`, move `ACrowdedIndexIsRebuiltFromTheLog`'s setup into a fixture helper `uint32_t crowdIndex()` (reinstalls `smallLayout(256, 512, 65536)`, commits with idle reclaims until the index stops a commit, returns how many were stored; the test then uses it), and add a fake clock `uint32_t fakeNowUs() { return fakeTicks += 7; }` (a file-static counter).
  - `ARebuildReportsWhereItsTimeWent`: `store.setClock(&fakeNowUs)`, `crowdIndex()`, `makeRoom(Budget::Idle, &work)` rebuilds; then `t = store.lastRebuild()`: `t.slots == store.usedSlots()`, `t.programs == t.slots`, `t.records >= t.slots`, `t.walkUs > 0`, `t.eraseUs > 0`, `t.programUs > 0`, `t.insertUs >= t.programUs`.
  - `WithoutAClockARebuildCountsButDoesNotTime`: the same without `setClock`: the four times are 0, the counts as above.

- [ ] **Step 2: Run them to see them fail.**
  Run: `cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='GlyphStoreRoom.*Time*'`
  Expected: compile errors (`setClock`, `lastRebuild`).

- [ ] **Step 3: Implement.** `rebuildIndex` reads the clock at its phase boundaries (before the count walk; after the plan walk; around `eraseWritten`; around the insert walk) and around each `insert()` of the insert walk. `programs` counts those calls.

- [ ] **Step 4: Run the store suite and the font tests.**
  Run: `./build/test/glyph_store/GlyphStoreTest.exe && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
  Expected: `[  PASSED  ]` for both.

- [ ] **Step 5: The bench.** In `bench/glyph_store_main.cpp`, after `BENCH wipe` and before the header sector's erase, in the log region the wipe left erased (`g.logOffset`, through `flash` directly):
  - `BENCH flash_slot_calls  per_call=<us>`: 128 programs of 8 B into consecutive slots of one sector;
  - `BENCH flash_piece  slots_us=<t> data_us=<t> erased_us=<t>`: one 1 KB program each of a piece with 57% of its slots written (slot `i` written when `i * 7 % 16 < 9`, the rest `0xFF`), of 1 KB of data, and of 1 KB of `0xFF`;
  - `BENCH flash_erase  sector_written_ms=<t> sector_blank_ms=<t> block_written_ms=<t>`: the sector of the slot calls, a blank sector, and a page-aligned 64 KB run programmed first.

- [ ] **Step 6: Build the firmware and the bench** (background, one at a time).
  Run: `pio run -e default`, then `pio run -e bench_glyph_store`
  Expected: `SUCCESS` for both; `check_no_float: ... no floating point`.

- [ ] **Step 7: Commit.** `git commit -m "feat(ttf): time the glyph index rebuild; bench the flash's program and erase granularity"`

### Task 2: Measure on the X4 (and the X3)

**Files:** none; the figures go into the PR and, in Task 8, into `docs/future_work/cjk.md`.

**Interfaces:** consumes Task 1's debug line and bench.

- [ ] **Step 1: Flash the X4** with Task 1's `default` build (`pio run -e default -t upload --upload-port <X4 port>`). The X3 only if the maintainer flashes it.
- [ ] **Step 2: Set up.** Build the book (`python tools/make_cjk_store_fill_epub.py`) and upload it to `/test/cjk_store_fill.epub`. With `tools/devctl.py --port <port> --log /c/tmp/rebuild-x4.log`: `CMD:GET` the family, the font size and the language and note them; `CMD:SET:fontFamily=NotoSansTC` (a CJK family in SD mode), the reader font size to 12 pt (its key from `CMD:GET:*`), `CMD:EVENTS:ON`, `CMD:LOG:DBG`.
- [ ] **Step 3: Fill the store.** `CMD:ACT:READER:/test/cjk_store_fill.epub@1:1`, `CMD:IDLE:120000`, `CMD:TTF`; the same for spine items 2 to 8.
  Expected: from the sixth chapter on, `EVT:TTF_ROOM` lines, `rebuilds=` rising, and one `[GST] rebuild:` line per rebuild.
- [ ] **Step 4: Record each rebuild:** records, slots, walks, erase, inserts, programs (ms and calls), and its `EVT:TTF_ROOM ms=`.
  Expected, roughly: walks 50-100 ms, erase 250-300 ms, inserts 1.2-1.4 s of which programs 1.1-1.3 s in about 9,300 calls; 1.6-1.9 s in all.
- [ ] **Step 5: The gate.** Programs at least 40% of walks + erase + inserts: go on with Task 3. Below: stop, and report the split; Task 5's gain rests on the program calls.
- [ ] **Step 6: The bench** (it erases the font area, so after Step 4: the store installs again at the next book open). `pio run -e bench_glyph_store -t upload --upload-port <X4 port>`; read the `BENCH flash_*` lines; flash `default` back.
  Expected, roughly: `per_call` about 120 µs; a 1 KB piece 1.2-2.2 ms (`erased_us` near `data_us` means erased bytes cost program time: the copy's programs then take ~270 ms, else ~160 ms); a written block about 140 ms, a written sector 30-50 ms, a blank one 4-6 ms.
- [ ] **Step 7: Restore** every setting changed.
- [ ] **Step 8: The X3,** if the maintainer runs Steps 2-4 there: at the first rebuild, `CMD:TTF`'s `live_slots` near 4,600 (the one-page index's cap).

### Task 3: Size an SD-mode index for 12 pt CJK records

**Files:**
- Modify: `lib/EpdFont/ttf/GlyphStoreFormat.h` (`kExpectedRecordSd` and its comment)
- Test: `test/glyph_store/GlyphStoreFormatTest.cpp`

**Interfaces:** produces `glyphstore::kExpectedRecordSd = 160`. Its comment: a CJK record at 12 pt is 150-185 B (150 in prose, TTF spec Appendix A; about 185 in the store-fill book on the X4); 160 B keeps a 12 pt log within its index's load limit and the X4's index at two pages.

- [ ] **Step 1: Write the failing tests** in `GlyphStoreFormatTest.cpp`:
  - `CjkSdModeGetsTheLargestLog` (the X3's budget, 32 pages): `EXPECT_EQ(2 * kSlotsPerPage, g.indexSlots)` and `EXPECT_EQ(20 * 65536u, g.logSize)` (were one page and 21).
  - `TheX4sCjkStoreKeepsItsGeometry`: `input(kModeSd, 1, 430000, 40)`: `2 * kSlotsPerPage` slots, a 28-page log.
- [ ] **Step 2: Run them to see them fail.**
  Run: `./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='GlyphStorePlan.*'`
  Expected: `CjkSdModeGetsTheLargestLog` FAILS (1 page, 21); the new test passes already.
- [ ] **Step 3: Set `kExpectedRecordSd = 160`** with the comment above.
- [ ] **Step 4: Run the store suite, `TtfFamilyTest` and `TtfFontTest`.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "fix(ttf): size an SD-mode glyph index for 12 pt CJK records"`

### Task 4: Format 5: a spare index table

**Files:**
- Modify: `lib/EpdFont/ttf/GlyphStoreFormat.h`, `lib/EpdFont/ttf/GlyphStoreFormat.cpp` (`planLayout`)
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp` (`validGeometry`, `beginInstall`, `open`, `close`, `isBlank`, `wipe`; `tableBytes` moves to the format header)
- Modify: `lib/hal/EspNorFlash.h` (`kMaxMaps`)
- Modify: `test/glyph_store/GlyphStoreTestUtil.h` (`smallLayout`), `test/glyph_store/GlyphStoreRoomTest.cpp` (`GlyphStoreEpochs`' chip: `SimNorFlash flash{524288}`, two 160 KB tables)
- Modify: `docs/file-formats.md` (the format table's version, the `indexOffset` row)
- Test: `GlyphStoreFormatTest.cpp`, `GlyphStoreInstallTest.cpp`, `GlyphStoreSizesPairsWipeTest.cpp`, `test/ttf_font/TtfFamilyTest.cpp`, `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Produces, in `namespace glyphstore`:
  - `kFormatVersion = 5`, its history gaining "5: the index region holds two tables of `indexSlots` slots, the one in use and a spare that a rebuild copies the live slots into; the journal's finished entries say which is in use."
  - `constexpr uint32_t kIndexTables = 2;`
  - `constexpr uint32_t tableBytes(uint32_t slots)` (= `alignUp(slots * kSlotBytes, kSectorBytes)`, moved from `FlashGlyphStore.cpp:51`).
  - `constexpr uint32_t indexTableOffset(const Geometry& g, uint32_t table)` = `g.indexOffset + table * tableBytes(g.indexSlots)`.
  - `constexpr uint32_t kRebuildPieceSlots = kMaxProbe;` with `static_assert(kRebuildPieceSlots >= kMaxProbe)`: a slot copied in slot order lands less than `kMaxProbe` slots behind the scan.
  - `Geometry::indexSlots` documented as each table's slots.
  - `planLayout`: the partition takes `kIndexTables * index` pages; the mapping budget takes `index` pages and `max(kSparePages, index)` spare pages (a rebuild maps the spare table before it unmaps the one in use).
- `validGeometry` also requires `indexOffset + kIndexTables * tableBytes(indexSlots) <= pairOffset`, `indexSlots % kRebuildPieceSlots == 0` and `indexSlots >= 2 * kRebuildPieceSlots` (the copy's window holds two distinct pieces).
- `FlashGlyphStore`: the table in use is its own mapping (`index_`); `region_` maps the pairs and the log (`pairOffset` to the log's end). `uint8_t table_` (always 0 in this task; Task 5 sets it) and public `uint8_t indexTable() const { return table_; }`. `open()` maps the table in use, then the pairs and the log, then the SD-mode styles; any failing is `MapFailed`. `close()` unmaps all three. `beginInstall()` and `wipe()` erase both tables (`eraseWritten(g.indexOffset, kIndexTables * tableBytes(g.indexSlots))`). `isBlank()` reads through the table's mapping or the region's, else in chunks (the spare).
- `EspNorFlash::kMaxMaps = 5`: the table in use, the spare during a switch, the pairs and the log, a flash-mode style, the SD-mode styles.
- `smallLayout()`: `pairOffset = indexOffset + kIndexTables * alignUp(indexSlots * kSlotBytes, kSectorBytes)`.

- [ ] **Step 1: Write the failing tests.**
  - `GlyphStoreFormatTest.cpp`: `TheIndexRegionHoldsTwoTables` (the Bookerly input: `pairOffset - indexOffset == 2 * indexSlots * kSlotBytes`); `TheSpareTableTakesPartitionPagesNotMappingPages` (`input(kModeSd, 1, 430000, 40)`: still 2 pages and a 28-page log); `AThreePageIndexKeepsThreePagesFreeToSwitch` (`input(kModeSd, 1, 430000, 44)`: 3 pages, a 30-page log). Updated, from the planner's new arithmetic: `LatinFlashModeFillsThePartition` log 15 pages (`pairOffset == indexOffset + 4 * 65536u`); `LargeStylesShrinkTheLog` 1 page and a log of 8; `AFamilySetMapsItsLargestStyleNotTheirSum` a log of 8; `AMixedSetMapsItsLargestFlashStyleAndEverySdModeBlock` 2 pages and a log of 11 (its comment: the budget leaves 15 pages for the table in use, the pairs and the log, the partition 16 for both tables, the pairs and the log: 2 + 2 + 1 + 11).
  - `GlyphStoreInstallTest.cpp`: `ALayoutWhoseSpareTableOverlapsThePairsIsRefused` (`smallLayout()` with `pairOffset` one table lower: `beginInstall` false); `AnIndexOfPartPiecesIsRefused` (1,000 slots and 128 slots refused, 256 and 1,024 accepted); `InstallingErasesBothTables` (program junk into the spare's first sector, install: it reads erased); every `flash.mappings()` expectation of an open store one higher (the table in use is a mapping of its own: lines 175, 202, 209, 225).
  - `GlyphStoreSizesPairsWipeTest.cpp`: `AWipeErasesBothTables`.
  - `TtfFamilyTest.cpp`, `FitsStoreCountsBothIndexTables` (Review Focus 4): two flash-mode styles of 23 pages each (`1507328` B) and no SD-mode blocks (`UINT32_MAX`) on the X3's font area with 32 map pages: `fitsStore` false (one table would leave a 4-page log, two do not fit); with the second style at 22 pages (`1441792` B): true.
- [ ] **Step 2: Run them to see them fail.**
  Run: `./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='GlyphStorePlan.*:GlyphStoreFormat.*:GlyphStoreInstall.*:*Tables*'`
  Expected: compile errors, then FAILs on the plan numbers and the refusals.
- [ ] **Step 3: Implement** the constants, `planLayout`, `validGeometry`, the mappings, the erases, `kMaxMaps`, `smallLayout`, and `GlyphStoreEpochs`' larger chip.
- [ ] **Step 4: Run the store suite.** Expected: PASS.
- [ ] **Step 5: The font tests.** Run `TtfFamilyTest` and `TtfFontTest`. The partition-edge cases (`AnAccentStaysWholeWhenNothingWithMoreOnTheCardFits`, `TheBodyStaysInFlashModeWhileTheAccentFamilyGoesToTheCard`, `AnAccentFamilyKeepsFlashModeOnlyWhileTheLogKeepsEightPages`, `ABodyTooLargeForFlashModeTakesTheAccentFamiliesToTheCardWithIt`, `AnSdModeBlockThatHoldsItsTablesIsKept`, and `TtfFamilyTest`'s `fitsStore` cases) were sized against one table: grow each failing case's chip by the pages its second table takes, so its comment's arithmetic gains that term and the case tests the same edge. Never change an expected mode or an expected `fitsStore` answer to make one pass.
  Expected: PASS.
- [ ] **Step 6: Docs.** `docs/file-formats.md`: `CPTT`'s version 5 in the format table; the `indexOffset` row: two tables of whole pages each, the one in use and its spare (kept erased until Task 5).
- [ ] **Step 7: Commit.** `git commit -m "feat(ttf): glyph store format 5: a spare index table beside the one in use"`

### Task 5: A rebuild copies the table in use into the spare

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp` (`Journal`, `readJournal`, `open`, `rebuildIndex`, `makeRoom`, `wipe`)
- Modify: `test/glyph_store/GlyphStoreTestUtil.h` (`keysHomedAt`), `docs/file-formats.md` (the journal and rebuild bullets)
- Test: `test/glyph_store/GlyphStoreRoomTest.cpp`

**Interfaces:**
- Consumes: Task 4's tables, `indexTableOffset`, `kRebuildPieceSlots`; Task 1's `RebuildTiming`.
- Produces:
  - `Journal` gains `uint32_t finished = 0;` (entries whose `done` reads programmed). `open()` reads the journal before it maps, and maps table `journal_.finished & 1`.
  - `Rebuilt` gains `Stuck`: the table in use has no erased slot, or the spare could not be mapped for the switch. `makeRoom()`: `Stuck` under `Budget::Open` wipes the store (as a full journal does: `Wiped`, or `Failed`); under `Budget::Idle` it sets `rebuildStarved_`, so the next Open tries again.
  - The window: `makeUniqueNoThrow<Slot[]>(2 * kRebuildPieceSlots)`; refused, `NoMemory` as the plan was.
  - `RebuildTiming` for a copy: `walkUs` and `records` 0; `eraseUs` the spare's erase; `insertUs` the scan, programs included; `programUs` and `programs` the piece programs; `slots` the slots copied.
  - `wipe()`: with table 1 in use, after the state sector's erase, programs `JournalEntry{1, 0}` at `kJournalOffset`, so the table in use stays (journal: one entry, finished, base and floor 1). A failed program stops writes with wipe pending.
  - Test util: `inline std::vector<uint32_t> keysHomedAt(uint32_t slots, uint32_t home, size_t count)`: keys `makeKey(cp, 0, 0)` for `cp` from `0x4E00` up whose `homeSlot(key, slots) == home`.

The rebuild, in order (the journal-full wipe stays first):
```
N = indexSlots, P = kRebuildPieceSlots; `from` = the table in use, `spare` = the other
e = the first erased slot of `from`; none → Stuck
s0 = (e + 1) % N; H = s0 / P                        // H, the head piece, is written in two ranges
window = two pieces: `older`, `current` (both erased); older = H - 1, current = H (piece numbers mod N / P)
program the journal entry's base (as today); eraseWritten(spare)
for i in 0 .. N - 1:
  p = (s0 + i) % N
  if p / P != current: flush(older); older = current; current = p / P; current's buffer erased
  c = countAt(p); setCount(p, 0)                     // read before anything can land on p
  s = from[p]; next if erased, s.value == kNoGlyph, or !validRecord(s.key, s.value)
  next if (p + N - homeSlot(s.key, N)) % N >= kMaxProbe   // no lookup reaches it (only a damaged table has one)
  q = homeSlot(s.key, N)                             // q, and every slot up to its place, is in older or current
  while window[q] holds a slot: if its key is s.key → next p (a duplicate a cut left: the first stays); q = (q + 1) % N
  window[q] = s; setCount(q, c); ++copied
flush(older); flush(current)
flush(k): its range is the whole piece, but for H: [s0, end of H) the first time, [start of H, s0) the second;
          the slots of the range from its first to its last used one, one program call; none, no call
map the spare (tableBytes); null → Stuck (the entry stays unfinished)
program the entry's done; failed → unmap the spare, stop writes (wipe pending), Failed
unmap the table in use; index_ = the spare's mapping; table_ ^= 1; journal: done, floor = base, ++finished
usedSlots_ = liveSlots_ = copied; marks_ = 0; indexFull_ = chainFull_ = false; ++rebuilds_; work.rebuilt
```
A slot's new place lies between its home and its old place (the rebuild only drops slots), so it is a place the scan has read: the counts move in place, and only the head piece is programmed twice, in two disjoint ranges.

- [ ] **Step 1: Write the failing tests** in `GlyphStoreRoomTest.cpp` (fixture `GlyphStoreRoom`; `crowdIndex()` from Task 1):
  - `ARebuildCopiesTheTableInUseIntoTheSpare`: `crowdIndex()`, draw the live keys `k % 4` times, copy table 0's bytes; rebuild: `indexTable() == 1`; table 0's bytes unchanged; every live key `intact` with `countOf(k) == k % 4`; reopen: `indexTable() == 1`, every live key `intact`.
  - `ARebuildProgramsTheSpareAPieceAtATime`: around the rebuild after `crowdIndex()`, `flash.programCalls()` grows by at most `256 / kRebuildPieceSlots + 1 + 2` (the pieces, the head piece's second range, the entry's two words) and `flash.eraseCalls()` by 0.
  - `TwoRebuildsComeBackToTheFirstTable`: a rebuild, then commits with idle reclaims until the index stops one again, then a rebuild: `eraseCalls()` grew (table 0 erased first), `indexTable() == 0`, every live key `intact` with its count; reopen: `indexTable() == 0`.
  - `AChainAcrossAPieceBoundaryIsCopiedWhole`: `smallLayout(256, 512, 65536)`; `keysHomedAt(256, 122, 12)` committed one by one (two segments: slots 122-133); `markFull()`, draw the last six `1 + i % 3` times, `makeRoom(Budget::Idle)` reclaims segment 0 (its six go stale); `fillIndexWithMarks()`, `makeRoom(Budget::Idle)` rebuilds: the last six `storedIntact` with their counts, the first six `NotStored`, and slot 122 of table 1 (`flash.raw() + indexTableOffset(layout.geometry, 1)`) holds the seventh key.
  - `AChainAcrossTheTablesEndIsCopiedWhole`: the same with home 250 (slots 250-255 and 0-5); slot 250 of table 1 holds the seventh key.
  - `ATableWithNoErasedSlotIsWipedAtAnOpen`: close the store, program all 1,024 slots of table 0 with `Slot{0x0FFF0000 + i, 0x12345678}` (garbage no record matches), open again; `markNoGlyph` returns `Commit::Full`; `makeRoom(Budget::Idle, &work)`: `Room::More`, `!work.rebuilt`, the generation unchanged, `idleWorkDue()` false; `makeRoom(Budget::Open)`: the generation one higher, `usedSlots() == 0`, `wipePending()` false.
  - `ASpareThatCannotBeMappedLeavesTheRebuildToAnOpen`: `crowdIndex()`, then `flash.setFreePages(32 - flash.freeMapPages())` (no page free); `makeRoom(Budget::Idle, &work)`: `Room::More`, `!work.rebuilt`, `indexTable() == 0`, every live key `intact`, `journalEntry(0).base != kErased` and `journalEntry(0).done == kErased`; `flash.setFreePages(32)`; `makeRoom(Budget::Open, &work)`: `work.rebuilt`, `indexTable() == 1`.
  - `AnOpenThatCannotMapTheSpareWipesTheStore`: the same with no page free at the Open: the generation one higher, `indexTable() == 0`.
  - `AWipeKeepsTheTableInUse`: a rebuild (`indexTable() == 1`), `wipe()`: `indexTable() == 1`, `journalEntry(0).base == 1`, `journalEntry(0).done == 0`; `claimTestSizes`, a commit `intact`; reopen: `indexTable() == 1`, still `intact`.
  - Brought onto the new rule: `AFullJournalTurnsTheRebuildIntoAWipe` commits its six records after the reopen (479 finished entries put table 1 in use).
- [ ] **Step 2: Run them to see them fail.**
  Run: `./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='GlyphStoreRoom.*Rebuil*:GlyphStoreRoom.*Table*:GlyphStoreRoom.*Chain*:GlyphStoreRoom.*Spare*:GlyphStoreRoom.*Journal*'`
  Expected: compile errors (`indexTable` exists from Task 4; `keysHomedAt` does not), then FAILs.
- [ ] **Step 3: Implement** the journal's `finished`, the table at open, the copy, `Stuck`, and the wipe's entry. The existing rebuild tests (`ACrowdedIndexIsRebuiltFromTheLog`, `ARebuildKeepsTheFirstOfTwoCopiesACutLeft`, `ARebuildNeverMakesATornRecordFindable`, `ARebuildWithoutMemoryWaitsForTheNextOpen` with its one refused array, the epoch tests) pass unchanged.
- [ ] **Step 4: Run the store suite and `TtfFontTest`.** Expected: PASS, except the power-cut rebuild sweep, whose step count Task 6 rewrites (disable it here with `DISABLED_` and a comment naming Task 6).
- [ ] **Step 5: Docs.** `docs/file-formats.md`: the journal bullet (an entry's `done` is the switch: the spare is complete before it; the table in use is the parity of finished entries; a wipe with table 1 in use writes the entry `{1, 0}`), and how a rebuild copies (slot order from after an erased slot, a piece at a time, the stale slots, the marks and a duplicate's second copy left out).
- [ ] **Step 6: Commit.** `git commit -m "feat(ttf): an index rebuild copies the table in use into the spare, a piece at a time"`

### Task 6: Power cuts during a copy, a second copy, and a wipe with table 1 in use

**Files:**
- Modify: `test/glyph_store/GlyphStorePowerCutTest.cpp`
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.cpp` only for what the sweeps find

**Interfaces:**
- Consumes: Task 5. Produces, in the test file: `uint32_t pieceExtentBytes(const SimNorFlash&, uint32_t tableOffset, uint32_t slots)`: over each `kRebuildPieceSlots` piece, the bytes from its first to its last non-erased slot (plus the head piece's split when the copy started inside it: compute it from the first erased slot of the table copied from, as the store does).

- [ ] **Step 1: Write the sweeps.**
  - `sweepRebuild` (re-enabled): the dry run's steps are `sizeof(uint32_t) + pieceExtentBytes(dry, indexTableOffset(g, 1), kCrowdedSlots) + sizeof(uint32_t)` (the spare was blank: no erase). The dry session also lists the keys found before the rebuild. After each cut, in `atBoot`: `indexTable() == (finished ? 1 : 0)`; when not finished every key found before the rebuild is `storedIntact` (a cut copy loses nothing). `afterRoom` as today.
  - `EveryCutDuringASecondRebuildLeavesAConsistentStore`: `crowdedIndex()`, one session that rebuilds (table 1) and then commits with idle reclaims until the index stops a commit again; the sweep cuts every step of the second rebuild: the entry's base, table 0's erase (one sector), the pieces, the done word. After each cut: `indexTable() == (finished ? 0 : 1)`, and the same checks as `sweepRebuild`.
  - `EveryCutDuringAWipeWithTheSpareTableInUseIsRecoverable`: `sweepWipe` with a flag that programs `JournalEntry{1, 0}` into the chip right after the install, so `populate()` stores into table 1. After each cut and a finished wipe, the store works (`claim`, `commitAll`, a reopen finds them).
  - Each with `partialCut(true)` as a second pass.
- [ ] **Step 2: Run them.**
  Run: `./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='GlyphStorePowerCut.*'`
  Expected: PASS (the suite stays under about 10 s). A failure is a defect in Task 5: pin it with a focused test first (superpowers:systematic-debugging).
- [ ] **Step 3: Commit.** `git commit -m "test(ttf): power cuts during an index copy, a second copy and a wipe with the spare table in use"`

### Task 7: The table a rebuild leaves is erased in idle time

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp` (`Step`, `nextStep`, `makeRoom`, `open`, `wipe`, `rebuildIndex`)
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp` (`TtfFontSet::makeRoom`, `Stats`), `src/SerialTestApi.cpp` (`CMD:TTF`), `docs/serial-test-api.md`
- Test: `test/glyph_store/GlyphStoreRoomTest.cpp`, `test/glyph_store/GlyphStorePowerCutTest.cpp`, `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: Task 5's switch.
- Produces:
  - `Step::ClearSpare`, chosen last and only under `Budget::Idle`, while the spare is not known erased. Private `bool spareClean_`, `uint32_t spareChecked_` (bytes from the spare's start found or made blank).
  - `open()`: `spareClean_ = journal_.used == 0` (no rebuild since the last install or wipe), `spareChecked_` the table's bytes when clean, else 0. A switch: false and 0. A wipe or an install: clean.
  - The step: `eraseWritten(indexTableOffset(g, spare) + spareChecked_, min(kPageBytes, tableBytes(indexSlots) - spareChecked_))`, then `spareChecked_` moves on; at the table's end the spare is clean. A failed erase stops writes with wipe pending (`Room::Failed`). `RoomWork` gains `uint32_t spareChecked = 0;` (bytes this call checked). `LOG_DBG("GST", "spare table: %u of %u KB checked", ...)`.
  - `rebuildIndex()`: erases only the spare's unchecked rest (`spareChecked_` on).
  - `needsRoom()` is unchanged (the spare is not room); `idleWorkDue()` includes the step through `nextStep`.
  - `TtfFontSet::makeRoom()`: goes on when `needsRoom(budget)`, or for `Budget::Idle` when `idleWorkDue()`; returns true also when `work.spareChecked > 0`, which counts no room (`rooms`, `EVT:TTF_ROOM` unchanged), so the reader's idle tick yields after it.
  - `TtfFontSet::Stats` gains `uint32_t indexSlots` and `uint8_t indexTable`; `CMD:TTF` adds `index=<slots> table=<0|1>` after `live_slots=`.

- [ ] **Step 1: Write the failing tests.**
  - `GlyphStoreRoomTest.cpp`: `AfterARebuildIdleStepsEraseTheOldTable` (`smallLayout()`, a rebuild at an Open: `idleWorkDue()` true, `needsRoom(Budget::Idle)` false; one `makeRoom(Budget::Idle, &work)`: `work.spareChecked == tableBytes(1024)`, `idleWorkDue()` false, table 0 reads erased; the next rebuild's `eraseCalls()` delta 0); `AnOpenLeavesTheSpareToIdleTime` (after a rebuild, `makeRoom(Budget::Open)` with no room due: `eraseCalls()` unchanged, `idleWorkDue()` still true); `AStoreThatNeverRebuiltHasNoSpareToErase` (a fresh store: `idleWorkDue()` false); `AReopenedStoreChecksItsSpareOnce` (after a rebuild and its idle erase, reopen: `idleWorkDue()` true, one step erases nothing, then false); `TheSpareIsErasedAPageAStep` (a 1 MB chip and `smallLayout(16384, 512, 65536)`: two-page tables; after a rebuild, two idle steps, `work.spareChecked == kPageBytes` each).
  - `GlyphStorePowerCutTest.cpp`: `EveryCutWhileTheSpareIsErasedLeavesAConsistentStore` (after a rebuild, table 0 holds data; a cut at every sector of the idle step; after each: open, `indexTable() == 1`, every live key `storedIntact`, the next idle steps finish, a commit lands).
  - `TtfFontTest.cpp`: `TheSpareTableIsCheckedInIdleTimeWithoutCountingARoom` (load a set; program `JournalEntry{1, 0}` into the chip's journal; load again: `needsIdleRoom()` true; `makeRoom(Budget::Idle)` true, `storeEvents().rooms` unchanged; then `needsIdleRoom()` false; `stats().indexTable == 1`).
- [ ] **Step 2: Run them to see them fail.**
  Run: `./build/test/glyph_store/GlyphStoreTest.exe --gtest_filter='*Spare*' && ./build/test/ttf_font/TtfFontTest.exe --gtest_filter='*Spare*'`
  Expected: compile errors (`spareChecked`, `indexTable` in `Stats`), then FAILs.
- [ ] **Step 3: Implement** the step, its state, the rebuild's partial erase, `TtfFontSet::makeRoom`, `Stats` and `CMD:TTF`.
- [ ] **Step 4: Run the store suite, `TtfFontTest`, and every test target that builds on Windows** (`cmake --build build/test -j 8 -- -k 0`, then `ctest --test-dir build/test`).
  Expected: PASS but the three targets that do not build on Windows (CI builds them).
- [ ] **Step 5: Docs.** `docs/serial-test-api.md`: the `background` row ("… or the index table a rebuild left, to erase"); `CMD:TTF`'s example line and its `index`/`table` fields.
- [ ] **Step 6: Format and commit.**
  ```bash
  PATH="<clang-format 21 dir>:$PATH" bash bin/clang-format-fix
  git commit -m "feat(ttf): the index table a rebuild leaves is erased in idle steps"
  ```

### Task 8: On the X4 and the X3

**Files:** `docs/future_work/cjk.md` (the section "Glyph store: a faster index rebuild").

- [ ] **Step 1: Build** `default` and `gh_release` (background, one at a time). Expected: `SUCCESS`, `check_no_float` clean.
- [ ] **Step 2: The X4.** Flash `default`; the store installs again (format 5) at the first book open. Task 2's Steps 2-4.
  Expected per rebuild: `walks 0 ms`, `erase 0 ms` (the idle steps cleared the spare), `inserts` 0.2-0.35 s with about 128 program calls; `EVT:TTF_ROOM ms=` of a rebuild alone under 400 ms; `spare table` lines between rebuilds, one per page, each about 140 ms; `CMD:TTF` `table=` alternating, `index=16384`.
- [ ] **Step 3: A reboot after a rebuild** (`CMD:REBOOT`, reopen the book): `table=` as before the reboot; the page draws from the store (`hits` rising, `renders` not).
- [ ] **Step 4: The X3,** when the maintainer flashes it: after the install `index=16384`; at the first rebuild `live_slots` about 6,000 or more (Task 2's cap was about 4,600).
- [ ] **Step 5: Record and restore.** The figures into `docs/future_work/cjk.md` (the section says shipped, with the measured split before and after); slicing the copy listed as the follow-up only if a rebuild step measured over 250 ms. Restore every setting changed. `git commit -m "docs(cjk): the faster index rebuild, measured"`
