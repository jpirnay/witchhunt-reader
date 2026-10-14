# TTF Stage 2a: the flash glyph store. Implementation plan

> **Status (2026-10-14): paused after Task 3 of 8.**
> - **Done on `feat/ttf-glyph-store`** (pushed, no PR yet), one commit per task: `d40fd9625` (Task 1),
>   `cc8939782` (Task 2), `e21e2d8fb` (Task 3). GlyphStoreTest is 41/41 with 0 warnings.
> - **Resume** at Task 4 with BASE = `e21e2d8fb`. Tasks 4-8 are unchanged below.
> - **Verified before execution:** every code block below was extracted, compiled with the suite's flags
>   and run. All 70 tests pass. Breaking the recovery scan on purpose (Task 6 Step 3) fails 5 of them.
> - **Execution notes:**
>   - Where a task first lists a new `.cpp` in CMake, the RED is CMake's "Cannot find source file", not a
>     compiler error. The cause is the same.
>   - Run the full host suite and the firmware builds only in Tasks 7-8.
> - **After Task 8, the user asked for a full audit sweep** (flaws, inconsistencies,
>   improvement/simplification potential) before merge.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `FlashGlyphStore`, the persistent store of rendered TTF glyphs in the `spiffs` partition,
with host tests on a simulated NOR flash (power cuts at every write step) and a device self-test. Nothing
calls the store yet; Stage 2c wires it to `TtfFont`.

**Architecture:**

- **`NorFlash`** (`lib/hal/NorFlash.h`) is a thin interface over NOR flash: read, program (1 → 0 only),
  erase 4 KB sectors, map for in-place reads, free mapping pages.
  - `EspNorFlash` (`lib/hal`) implements it over the `spiffs` partition below the language slot, and is
    the only code that calls `esp_partition_*`.
  - `SimNorFlash` (test only) implements it in RAM. It enforces NOR rules, counts erases per sector, and
    cuts power after any number of steps.
- **`FlashGlyphStore`** (`lib/EpdFont/ttf/`) holds the store logic, including the format, layout planning
  and recovery. It knows `EpdGlyph`, so it lives with the font code.
- **`GlyphBatch`** stages glyph records in a caller-owned buffer, laid out exactly as they will sit in the
  log, so a whole batch is written with one program call.

**Tech Stack:** C++20 host tests (gtest via CMake, UCRT64 gcc); firmware gnu++2a (PlatformIO, ESP32-C3/S3,
ESP-IDF `esp_partition_*`); clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (branch `docs/ttf-fonts-design`):
sections 4, 6.2, 6.3, 7.5, 8.1, 9.3 and Appendix A. The spec was updated together with this plan; see
"Spec changes made with this plan" below.

## Global Constraints

- **Branch:** `feat/ttf-glyph-store`, created from `master`. It does not depend on Stage 1 (PR #468).
- **No behaviour change:** nothing in the firmware calls the store. `default` and `x4pro` must build, and
  their flash size may change by at most a few hundred bytes (the linker drops unreferenced code).
- **NOR discipline:** outside the header sector, every byte is programmed at most once between erases.
  Every store test asserts `SimNorFlash::violations() == 0`.
- **Erases only where needed:** install and wipe erase only sectors that hold data. Erases wear the chip,
  and a blank sector still costs 4-6 ms to erase (Appendix A).
- **Write order:** a record is written before the slot that points at it. A probe must never find a slot
  whose record is incomplete.
- **Language slot:** the last 64 KB of the partition is never touched. `EspNorFlash::size()` is
  `FlashFontPartition::fontUsableSize()`.
- **Only `EspNorFlash` calls ESP-IDF partition APIs.** The store reaches flash only through `NorFlash`.
- **Heap:**
  - No allocation on any lookup, commit or probe path.
  - No bare `new`. Cold-path temporaries such as the 648 B header use `makeUniqueNoThrow`, null-checked
    with a `LOG_ERR`.
  - Stack frames stay small: a 256 B read chunk and a 128 B size table at most.
- **Concurrency:** the store is not thread-safe. Stage 2c calls it only under `RenderLock` (spec 3.4).
- **Formatting:** clang-format **21.1.8**, the CI version. Local 22.x formats differently.
- **Toolchain** (Git Bash, never PowerShell, for builds and tests):
  - host tests: `export PATH=/c/msys64/ucrt64/bin:$PATH`;
  - pio: `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
    `"$HOME/.platformio/penv/Scripts/pio.exe"`;
  - firmware builds take 10-25 min: run them in the background, one at a time;
  - never `pio run -t clean`;
  - `epub_build_inventory` fails to build on Windows by design (`<dlfcn.h>`).

## Review Focus

1. **A real chip interrupted mid-page can leave a whole 256-byte page partly programmed,** not just one
   byte. Recovery must still never program a byte twice.
   Tests: Task 3 `AGarbledPageAfterTheLastRecordIsSkipped`; Task 6 sweeps with `partialCut`.
2. **A firmware update can shrink the free mapping pages** below what the store was laid out for. `open()`
   must report `MapFailed` and hold no mapping, not crash or half-open.
   Test: Task 3 `OpenFailsCleanlyWhenTheRegionsCannotBeMapped`.
3. **A full store must keep serving what it holds:** after wipe-pending, stored glyphs are still found;
   only new ones are refused.
   Test: Task 4 `StoredGlyphsStayReadableWhileWipeIsPending`.
4. **A torn slot or size entry must not be reused or misread.**
   - A slot whose key reads erased but whose value is programmed is "cut short": inserts skip it, and it
     does not end a probe.
   - A size entry is valid only once its zeroed pad bytes, written last, landed.
   - Tests: Task 5 `ASizeEntryCutShortIsNeverReused`; Task 6 session sweep.
5. **The language slot and the `.cpfont` mapping are device-only hazards no host test can reach.**
   - `EspNorFlash` must refuse writes while a `.cpfont` is mapped (`FlashFontPartition::isMapped()`).
   - Its size must stop below the language slot.
   - Checks: Task 7 by review; Task 8's self-test checksums the language slot before and after.

## Spec changes made with this plan

The user asked, while this plan was being written, to batch flash writes and bring down flash wear.
Wear comes from erases (about 100,000 cycles per sector); programming erased bytes does not wear the
chip. So the plan does both:
- it batches writes, which saves time (about 100 µs of fixed cost per write call, Appendix A);
- it cuts erases, which is what saves wear.

The spec now says:

1. **Placement:** `FlashGlyphStore` lives in `lib/EpdFont/ttf/`, because it knows `EpdGlyph`. Only the
   flash backend, `EspNorFlash`, is HAL (`lib/hal`), per the project's HAL rules.
2. **Batched commits:** a `GlyphBatch` of records is written with one program call, then one 8-byte slot
   per record. The order "records, then slots" is unchanged.
3. **Erase only sectors that hold data,** at install and at wipe. A wipe after a size-table overflow then
   erases a few sectors, not 1.3 MB.
4. **The index is sized to the log:** enough pages that 70% of its slots holds the records the log holds
   (100 B each in flash mode, 250 B in SD mode). The spec's fixed 8,192 slots would hit 70% at about
   5,700 Latin glyphs, with the log only half full: a premature wipe, so needless erases.
5. **`homeSlot = (uint64(key * 2654435761) * slots) >> 32`.** This equals the spec formula for a power of
   two, and allows any whole number of index pages.
6. **Torn writes:**
   - The header carries an FNV-1a check, so a torn header reads as not installed.
   - A slot is free only when both words are erased.
   - A size entry is valid once its zeroed pad landed.
   - The log scan accepts a record only if:
     - its key's bits 27-31 are zero;
     - `dataOffset` is the record offset + 20;
     - `dataLength` is `glyphDataBytes(w, h, 2-bit)` and at most 8 KB.
   - Writing resumes past any programmed byte in the 8,212 bytes after the last record, at a sector
     boundary.
7. **A full pair index** only stops storing pairs. It never triggers a wipe.
8. **Layout:** sector 0 is the header (the size table at 0x400, the wipe word at 0x480). The style
   blocks start at 64 KB, followed by the index, the pair index and the log. The index, pair index and
   log form one mapping.

## File structure

| File | Responsibility | Task |
| --- | --- | --- |
| `lib/hal/NorFlash.h` | the flash interface and its NOR contract | 1 |
| `test/glyph_store/SimNorFlash.h` | simulated chip: NOR rules, erase counts, power cuts, mapping budget | 1 |
| `lib/EpdFont/ttf/GlyphStoreFormat.h` | on-flash structs, keys, hashing, constants | 2 |
| `lib/EpdFont/ttf/GlyphStoreFormat.cpp` | `planLayout`: region sizes from the mapping budget | 2 |
| `lib/EpdFont/ttf/FlashGlyphStore.h` | `GlyphBatch`, `FlashGlyphStore` API | 3 |
| `lib/EpdFont/ttf/FlashGlyphStore.cpp` | install, open, recovery scan (3); glyphs (4); sizes, pairs, wipe (5) | 3-5 |
| `test/glyph_store/GlyphStoreTestUtil.h` | shared test helpers and the `StoreTest` fixture | 3-4 |
| `test/glyph_store/*Test.cpp` | one file per task | 1-6 |
| `lib/hal/EspNorFlash.{h,cpp}` | `NorFlash` over the `spiffs` partition | 7 |
| `bench/glyph_store_main.cpp`, `[env:bench_glyph_store]` | device self-test and timings | 8 |

---

### Task 1: `NorFlash` and the simulated chip

**Files:**
- Create: `lib/hal/NorFlash.h`
- Create: `test/glyph_store/SimNorFlash.h`, `test/glyph_store/SimNorFlashTest.cpp`,
  `test/glyph_store/CMakeLists.txt`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(glyph_store)` on the line after
  `add_subdirectory(glyph_fallback)`)

**Interfaces:**
- Produces: `class NorFlash` with `kSectorBytes = 4096`, `kMapPageBytes = 65536` and these virtuals:
  - `uint32_t size() const`
  - `bool read(uint32_t offset, void* dst, uint32_t len)`
  - `bool program(uint32_t offset, const void* src, uint32_t len)`
  - `bool erase(uint32_t offset, uint32_t len)`
  - `const uint8_t* map(uint32_t offset, uint32_t len)`
  - `void unmap(const uint8_t* mapped)`
  - `uint32_t freeMapPages() const`
- Produces: `class SimNorFlash final : public NorFlash` with `SimNorFlash(uint32_t bytes, uint32_t
  freePages = 32)` and these controls and counters:
  - `cutAfterSteps(int64_t)`, `partialCut(bool)`, `powerCycle()`, `failNextProgram()`,
    `setFreePages(uint32_t)`
  - `dead()`, `steps()`, `violations()`, `programCalls()`, `eraseCalls()`, `sectorErases(uint32_t)`,
    `totalSectorErases()`, `mappings()`, `raw()`

- [x] **Step 1: Create the branch**

```bash
cd /c/_development/witchhunt-reader
git switch master && git pull --ff-only && git switch -c feat/ttf-glyph-store
```

- [x] **Step 2: Write the failing test**

Create `test/glyph_store/CMakeLists.txt`:

```cmake
add_executable(GlyphStoreTest
  SimNorFlashTest.cpp
)

target_include_directories(GlyphStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(GlyphStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphStoreTest)
```

Add `add_subdirectory(glyph_store)` to `test/CMakeLists.txt` on the line after
`add_subdirectory(glyph_fallback)`.

Create `test/glyph_store/SimNorFlashTest.cpp`:

```cpp
// The simulated NOR flash the glyph store's host tests run on: it has to behave like the chip.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "SimNorFlash.h"

TEST(SimNorFlash, StartsErased) {
  SimNorFlash flash(8192);
  uint8_t bytes[4] = {};
  ASSERT_TRUE(flash.read(100, bytes, sizeof bytes));
  for (const uint8_t b : bytes) EXPECT_EQ(0xFF, b);
}

TEST(SimNorFlash, ProgramClearsBitsOnly) {
  SimNorFlash flash(8192);
  const uint8_t first = 0xF0;
  const uint8_t second = 0x3C;  // bits 2-3 would have to go from 0 back to 1
  ASSERT_TRUE(flash.program(10, &first, 1));
  ASSERT_TRUE(flash.program(10, &second, 1));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(10, &cell, 1));
  EXPECT_EQ(0x30, cell);
  EXPECT_EQ(1u, flash.violations());
}

TEST(SimNorFlash, ReprogrammingTheSameValueIsNoViolation) {
  SimNorFlash flash(8192);
  const uint8_t value = 0xA5;
  ASSERT_TRUE(flash.program(10, &value, 1));
  ASSERT_TRUE(flash.program(10, &value, 1));
  EXPECT_EQ(0u, flash.violations());
}

TEST(SimNorFlash, EraseSetsWholeSectorsAndCountsThem) {
  SimNorFlash flash(3 * 4096);
  const uint8_t zero = 0;
  ASSERT_TRUE(flash.program(4096 + 5, &zero, 1));
  ASSERT_TRUE(flash.erase(4096, 4096));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(4096 + 5, &cell, 1));
  EXPECT_EQ(0xFF, cell);
  EXPECT_EQ(0u, flash.sectorErases(0));
  EXPECT_EQ(1u, flash.sectorErases(1));
  EXPECT_FALSE(flash.erase(100, 4096));  // not sector-aligned
  EXPECT_FALSE(flash.erase(4096, 100));
  EXPECT_EQ(1u, flash.totalSectorErases());
}

TEST(SimNorFlash, OutOfRangeIsRefused) {
  SimNorFlash flash(8192);
  uint8_t bytes[8] = {};
  EXPECT_FALSE(flash.read(8190, bytes, sizeof bytes));
  EXPECT_FALSE(flash.program(8190, bytes, sizeof bytes));
  EXPECT_FALSE(flash.erase(8192, 4096));
  EXPECT_EQ(nullptr, flash.map(8190, 8));
}

TEST(SimNorFlash, MappingSeesLaterWrites) {
  SimNorFlash flash(8192);
  const uint8_t* view = flash.map(0, 4096);
  ASSERT_NE(nullptr, view);
  const uint8_t value = 0x42;
  ASSERT_TRUE(flash.program(3, &value, 1));
  EXPECT_EQ(0x42, view[3]);
  ASSERT_TRUE(flash.erase(0, 4096));
  EXPECT_EQ(0xFF, view[3]);
}

TEST(SimNorFlash, MappingsAreLimitedByFreePages) {
  SimNorFlash flash(4 * 65536, 2);
  const uint8_t* a = flash.map(0, 65536);
  const uint8_t* b = flash.map(65536, 65536);
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(0u, flash.freeMapPages());
  EXPECT_EQ(nullptr, flash.map(131072, 10));
  flash.unmap(a);
  EXPECT_EQ(1u, flash.freeMapPages());
  EXPECT_EQ(1u, flash.mappings());
}

TEST(SimNorFlash, AMappingAcrossAPageBoundaryCostsTwoPages) {
  SimNorFlash flash(4 * 65536, 4);
  ASSERT_NE(nullptr, flash.map(60000, 10000));
  EXPECT_EQ(2u, flash.freeMapPages());
}

TEST(SimNorFlash, PowerCutStopsAWriteAndEverythingAfter) {
  SimNorFlash flash(8192);
  const uint8_t zeros[8] = {};
  flash.cutAfterSteps(3);
  EXPECT_FALSE(flash.program(0, zeros, sizeof zeros));
  EXPECT_TRUE(flash.dead());
  uint8_t cells[8] = {};
  ASSERT_TRUE(flash.read(0, cells, sizeof cells));
  for (int i = 0; i < 3; ++i) EXPECT_EQ(0x00, cells[i]) << i;
  for (int i = 3; i < 8; ++i) EXPECT_EQ(0xFF, cells[i]) << i;
  EXPECT_FALSE(flash.program(100, zeros, 1));
  EXPECT_FALSE(flash.erase(4096, 4096));
  flash.powerCycle();
  EXPECT_TRUE(flash.program(100, zeros, 1));
}

TEST(SimNorFlash, PartialCutLeavesSomeBitsOfTheByteInFlight) {
  SimNorFlash flash(8192);
  flash.partialCut(true);
  flash.cutAfterSteps(0);
  const uint8_t zero = 0x00;
  EXPECT_FALSE(flash.program(7, &zero, 1));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(7, &cell, 1));
  EXPECT_EQ(0x5A, cell);
}

TEST(SimNorFlash, PowerCutDuringEraseLeavesTheSectorHalfErased) {
  SimNorFlash flash(8192);
  const std::vector<uint8_t> zeros(4096, 0);
  ASSERT_TRUE(flash.program(0, zeros.data(), 4096));
  flash.cutAfterSteps(0);
  EXPECT_FALSE(flash.erase(0, 4096));
  uint8_t first = 0;
  uint8_t last = 0xFF;
  ASSERT_TRUE(flash.read(0, &first, 1));
  ASSERT_TRUE(flash.read(4095, &last, 1));
  EXPECT_EQ(0xFF, first);
  EXPECT_EQ(0x00, last);
}

TEST(SimNorFlash, StepsCountBytesAndSectors) {
  SimNorFlash flash(3 * 4096);
  const uint8_t zeros[10] = {};
  ASSERT_TRUE(flash.program(0, zeros, sizeof zeros));
  EXPECT_EQ(10u, flash.steps());
  ASSERT_TRUE(flash.erase(4096, 8192));
  EXPECT_EQ(12u, flash.steps());
}

TEST(SimNorFlash, PowerCycleDropsMappings) {
  SimNorFlash flash(4 * 65536, 3);
  ASSERT_NE(nullptr, flash.map(0, 65536));
  flash.powerCycle();
  EXPECT_EQ(0u, flash.mappings());
  EXPECT_EQ(3u, flash.freeMapPages());
}
```

- [x] **Step 3: Run the test to verify it fails**

```bash
cd /c/_development/witchhunt-reader
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest
```

Expected: a compile error, `SimNorFlash.h: No such file or directory`.

- [x] **Step 4: Create `lib/hal/NorFlash.h`**

```cpp
#pragma once

#include <cstdint>

/// A region of NOR flash. Bits program from 1 to 0 only; only an erase, in whole 4 KB sectors, sets them
/// back to 1. FlashGlyphStore works through this interface, so its write-once rules run unchanged on
/// the device (EspNorFlash, over the `spiffs` partition) and in host tests (a simulated chip).
///
/// Offsets are relative to the start of the region. The device region stops below the language slot at
/// the end of the partition (FlashFontPartition::LANG_RESERVED_BYTES).
class NorFlash {
 public:
  static constexpr uint32_t kSectorBytes = 4096;    ///< erase granularity
  static constexpr uint32_t kMapPageBytes = 65536;  ///< one MMU page; mappings are paid in whole pages

  virtual ~NorFlash() = default;

  /// Usable bytes in the region.
  virtual uint32_t size() const = 0;
  /// Copies `len` bytes at `offset` to `dst`. False on an I/O error or a range outside the region.
  virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;
  /// Programs `len` bytes. A target bit can only go from 1 to 0; the caller never asks for 0 -> 1.
  virtual bool program(uint32_t offset, const void* src, uint32_t len) = 0;
  /// Erases whole sectors to 0xFF. `offset` and `len` must be multiples of kSectorBytes.
  virtual bool erase(uint32_t offset, uint32_t len) = 0;
  /// Maps `len` bytes at `offset` for reading in place; nullptr when the mapping budget is exhausted.
  /// Reads through a mapping see every later program() and erase().
  virtual const uint8_t* map(uint32_t offset, uint32_t len) = 0;
  /// Releases a mapping returned by map().
  virtual void unmap(const uint8_t* mapped) = 0;
  /// MMU pages still free for data mappings.
  virtual uint32_t freeMapPages() const = 0;
};
```

- [x] **Step 5: Create `test/glyph_store/SimNorFlash.h`**

```cpp
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "NorFlash.h"

/// NOR flash for host tests.
///
/// - program() can only clear bits. A write that needs a 0 -> 1 change is counted in violations(); the
///   bit stays 0, as on the chip. Store tests assert violations() == 0: no byte is programmed twice.
/// - erase() works in whole 4 KB sectors and counts erases per sector (the wear tests).
/// - Power cuts: after cutAfterSteps(n), n more steps succeed (one step = one programmed byte or one
///   erased sector). The operation in flight then stops part-way, and every later program or erase fails
///   until powerCycle(). With partialCut(true), the byte being programmed at the cut keeps some of its 1
///   bits, as a page program interrupted mid-way can. A sector being erased at the cut is left half
///   erased. Reads keep working, so a test can inspect the chip.
/// - map() returns a live view of the memory: later programs and erases show through it, as ESP-IDF keeps
///   a mapping coherent after a write. Mappings are limited to freePages MMU pages.
class SimNorFlash final : public NorFlash {
 public:
  explicit SimNorFlash(const uint32_t bytes, const uint32_t freePages = 32)
      : mem_(bytes, 0xFF), sectorErases_(bytes / kSectorBytes, 0), freePages_(freePages) {}

  uint32_t size() const override { return static_cast<uint32_t>(mem_.size()); }

  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    if (!inRange(offset, len)) return false;
    std::memcpy(dst, mem_.data() + offset, len);
    return true;
  }

  bool program(const uint32_t offset, const void* src, const uint32_t len) override {
    if (dead_ || !inRange(offset, len)) return false;
    if (failNextProgram_) {
      failNextProgram_ = false;
      return false;
    }
    ++programCalls_;
    const auto* bytes = static_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i < len; ++i) {
      uint8_t& cell = mem_[offset + i];
      if (!takeStep()) {
        if (partialCut_) cell &= static_cast<uint8_t>(bytes[i] | 0x5A);  // only some of its 0 bits land
        return false;
      }
      if ((cell & bytes[i]) != bytes[i]) ++violations_;
      cell &= bytes[i];
    }
    return true;
  }

  bool erase(const uint32_t offset, const uint32_t len) override {
    if (dead_ || !inRange(offset, len) || offset % kSectorBytes != 0 || len % kSectorBytes != 0) return false;
    ++eraseCalls_;
    for (uint32_t sector = offset; sector < offset + len; sector += kSectorBytes) {
      if (!takeStep()) {
        std::fill_n(mem_.begin() + sector, kSectorBytes / 2, 0xFF);  // the cut leaves it half erased
        return false;
      }
      std::fill_n(mem_.begin() + sector, kSectorBytes, 0xFF);
      ++sectorErases_[sector / kSectorBytes];
    }
    return true;
  }

  const uint8_t* map(const uint32_t offset, const uint32_t len) override {
    if (len == 0 || !inRange(offset, len)) return nullptr;
    const uint32_t pages = (offset % kMapPageBytes + len + kMapPageBytes - 1) / kMapPageBytes;
    if (mappedPages_ + pages > freePages_) return nullptr;
    mappedPages_ += pages;
    maps_.push_back({mem_.data() + offset, pages});
    return mem_.data() + offset;
  }

  void unmap(const uint8_t* mapped) override {
    for (auto it = maps_.begin(); it != maps_.end(); ++it) {
      if (it->ptr == mapped) {
        mappedPages_ -= it->pages;
        maps_.erase(it);
        return;
      }
    }
  }

  uint32_t freeMapPages() const override { return freePages_ - mappedPages_; }

  // --- test controls ---
  void cutAfterSteps(const int64_t steps) { budget_ = steps; }
  void partialCut(const bool on) { partialCut_ = on; }
  /// A reboot: writes work again and every mapping is gone.
  void powerCycle() {
    dead_ = false;
    budget_ = -1;
    maps_.clear();
    mappedPages_ = 0;
  }
  void failNextProgram() { failNextProgram_ = true; }
  void setFreePages(const uint32_t pages) { freePages_ = pages; }

  // --- what happened ---
  bool dead() const { return dead_; }
  uint64_t steps() const { return steps_; }
  uint32_t violations() const { return violations_; }
  uint32_t programCalls() const { return programCalls_; }
  uint32_t eraseCalls() const { return eraseCalls_; }
  uint32_t sectorErases(const uint32_t sector) const { return sectorErases_.at(sector); }
  uint32_t totalSectorErases() const {
    uint32_t total = 0;
    for (const uint32_t n : sectorErases_) total += n;
    return total;
  }
  uint32_t mappings() const { return static_cast<uint32_t>(maps_.size()); }
  uint8_t* raw() { return mem_.data(); }

 private:
  struct Mapping {
    const uint8_t* ptr;
    uint32_t pages;
  };

  bool inRange(const uint32_t offset, const uint32_t len) const {
    return offset <= mem_.size() && len <= mem_.size() - offset;
  }

  // One step of work; false when the power fails on it.
  bool takeStep() {
    if (budget_ == 0) {
      dead_ = true;
      return false;
    }
    if (budget_ > 0) --budget_;
    ++steps_;
    return true;
  }

  std::vector<uint8_t> mem_;
  std::vector<uint32_t> sectorErases_;
  std::vector<Mapping> maps_;
  uint32_t freePages_;
  uint32_t mappedPages_ = 0;
  int64_t budget_ = -1;
  uint64_t steps_ = 0;
  bool dead_ = false;
  bool partialCut_ = false;
  bool failNextProgram_ = false;
  uint32_t violations_ = 0;
  uint32_t programCalls_ = 0;
  uint32_t eraseCalls_ = 0;
};
```

- [x] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest
```

Expected: `[  PASSED  ] 13 tests.`

- [x] **Step 7: Commit**

```bash
git add lib/hal/NorFlash.h test/glyph_store test/CMakeLists.txt
git commit -m "hal: NorFlash interface; host tests get a simulated NOR chip with power cuts"
```

---

### Task 2: On-flash format and the layout planner

**Files:**
- Create: `lib/EpdFont/ttf/GlyphStoreFormat.h`, `lib/EpdFont/ttf/GlyphStoreFormat.cpp`
- Create: `test/glyph_store/GlyphStoreFormatTest.cpp`
- Modify: `test/glyph_store/CMakeLists.txt` (sources and include directory)

**Interfaces:**
- Consumes: nothing from Task 1. The format is plain data.
- Produces, in `namespace glyphstore`:
  - constants:
    - `kMagic`, `kFormatVersion`, `kModeFlash`, `kModeSd`, `kMaxStyles`, `kMaxTables`, `kSizeEntries`
    - `kSectorBytes`, `kPageBytes`, `kHeaderRegionBytes`, `kSizeTableOffset`, `kWipeWordOffset`
    - `kErased`, `kMissing`, `kSlotBytes`, `kSlotsPerPage`, `kMaxProbe`, `kIndexLoadPercent`,
      `kLogFullPercent`
    - `kRecordHeaderBytes`, `kMaxBitmapBytes`, `kMaxRecordBytes`
    - `kMinLogPages`, `kPairPages`, `kSparePages`, `kExpectedRecordFlash`, `kExpectedRecordSd`
  - functions: `makeKey`, `isGlyphKey`, `pairKey`, `homeSlot`, `align4`, `alignUp`, `pagesFor`, `fnv1a`
  - structs: `Slot`, `TableEntry`, `StyleInfo`, `Geometry`, `Header` (648 B), `SizeEntry`, `PlanInput`,
    `Layout`
  - `bool planLayout(const PlanInput&, Layout&)`

- [x] **Step 1: Write the failing test**

In `test/glyph_store/CMakeLists.txt`, change the source list and the include list to:

```cmake
add_executable(GlyphStoreTest
  SimNorFlashTest.cpp
  GlyphStoreFormatTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
)

target_include_directories(GlyphStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont/ttf
)
```

Create `test/glyph_store/GlyphStoreFormatTest.cpp`:

```cpp
// The glyph store's on-flash format and its install-time layout planner (spec 4.1-4.4).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "GlyphStoreFormat.h"

using namespace glyphstore;

namespace {

constexpr uint32_t kX3UsableBytes = 3538944 - 65536;  // the X3's spiffs partition below the language slot: 53 pages

PlanInput input(const uint8_t mode, const uint8_t styles, const uint32_t styleBytes, const uint32_t freePages = 32) {
  PlanInput in{};
  in.mode = mode;
  in.styleCount = styles;
  for (uint8_t s = 0; s < styles && s < kMaxStyles; ++s) in.styleBytes[s] = styleBytes;
  in.usableBytes = kX3UsableBytes;
  in.freeMapPages = freePages;
  return in;
}

}  // namespace

TEST(GlyphStoreFormat, KeysNeverLookErased) {
  const uint32_t largest = makeKey(0x10FFFF, 3, 15);
  EXPECT_TRUE(isGlyphKey(largest));
  EXPECT_NE(kErased, largest);
  EXPECT_NE(kMissing, largest);
  EXPECT_FALSE(isGlyphKey(kErased));
  EXPECT_EQ(0x10FFFFu, largest & 0x1FFFFFu);
  EXPECT_EQ(3u, (largest >> 21) & 0x3u);
  EXPECT_EQ(15u, (largest >> 23) & 0xFu);
}

TEST(GlyphStoreFormat, HomeSlotMatchesTheSpecFormulaForPowersOfTwo) {
  for (const uint32_t bits : {10u, 13u, 15u}) {
    const uint32_t slots = 1u << bits;
    for (uint32_t cp = 0x20; cp < 0x2000; cp += 7) {
      const uint32_t key = makeKey(cp, cp % 4, cp % 16);
      EXPECT_EQ((key * 2654435761u) >> (32 - bits), homeSlot(key, slots));
    }
  }
}

TEST(GlyphStoreFormat, HomeSlotStaysInRangeForAnySlotCount) {
  const uint32_t slots = 3 * kSlotsPerPage;  // three pages: not a power of two
  for (uint32_t cp = 0; cp < 0x30000; cp += 13) EXPECT_LT(homeSlot(makeKey(cp, 0, 0), slots), slots);
}

TEST(GlyphStoreFormat, HeaderLayoutIsStable) {
  EXPECT_EQ(648u, sizeof(Header));
  EXPECT_EQ(644u, offsetof(Header, check));
  EXPECT_EQ(8u, sizeof(Slot));
  EXPECT_EQ(8u, sizeof(SizeEntry));
}

TEST(GlyphStorePlan, LatinFlashModeFillsThePartition) {
  // Bookerly-sized: four styles of 450 KB, 8 pages each.
  Layout layout{};
  ASSERT_TRUE(planLayout(input(kModeFlash, 4, 460800), layout));
  const Geometry& g = layout.geometry;
  EXPECT_EQ(65536u, layout.styleOffset[0]);
  EXPECT_EQ(65536u + 3 * 8 * 65536u, layout.styleOffset[3]);
  EXPECT_EQ(33 * 65536u, g.indexOffset);
  EXPECT_EQ(2 * kSlotsPerPage, g.indexSlots);
  EXPECT_EQ(g.indexOffset + 2 * 65536u, g.pairOffset);
  EXPECT_EQ(kSlotsPerPage, g.pairSlots);
  EXPECT_EQ(g.pairOffset + 65536u, g.logOffset);
  EXPECT_EQ(17 * 65536u, g.logSize);
  EXPECT_EQ(kX3UsableBytes, g.logOffset + g.logSize);  // the partition, not the budget, was the limit
}

TEST(GlyphStorePlan, CjkSdModeGetsTheLargestLog) {
  // One style whose copied tables take 7 pages. They stay mapped, so they count against the budget.
  Layout layout{};
  ASSERT_TRUE(planLayout(input(kModeSd, 1, 430000), layout));
  const Geometry& g = layout.geometry;
  EXPECT_EQ(kModeSd, g.mode);
  EXPECT_EQ(8 * 65536u, g.indexOffset);
  EXPECT_EQ(kSlotsPerPage, g.indexSlots);
  EXPECT_EQ(21 * 65536u, g.logSize);
}

TEST(GlyphStorePlan, LargeStylesShrinkTheLog) {
  // Noto Sans-sized: four styles of 629 KB, 10 pages each.
  Layout layout{};
  ASSERT_TRUE(planLayout(input(kModeFlash, 4, 644096), layout));
  EXPECT_EQ(9 * 65536u, layout.geometry.logSize);
  EXPECT_EQ(2 * kSlotsPerPage, layout.geometry.indexSlots);
}

TEST(GlyphStorePlan, StylesThatDoNotFitAreRefused) {
  Layout layout{};
  EXPECT_FALSE(planLayout(input(kModeFlash, 4, 900000), layout));
}

TEST(GlyphStorePlan, ATightMappingBudgetIsRefused) {
  Layout layout{};
  EXPECT_FALSE(planLayout(input(kModeFlash, 4, 460800, 12), layout));
}

TEST(GlyphStorePlan, TheIndexHoldsWhatTheLogHolds) {
  // The index reaches 70 % no earlier than the log fills with typical records, so the store is not
  // wiped (and erased) while half the log is still empty.
  for (const uint32_t bytes : {200000u, 460800u, 644096u}) {
    Layout layout{};
    ASSERT_TRUE(planLayout(input(kModeFlash, 4, bytes), layout));
    const Geometry& g = layout.geometry;
    EXPECT_GE(uint64_t{g.indexSlots} * kIndexLoadPercent / 100, uint64_t{g.logSize} / kExpectedRecordFlash) << bytes;
  }
}

TEST(GlyphStorePlan, BadInputIsRefused) {
  Layout layout{};
  EXPECT_FALSE(planLayout(input(kModeFlash, 0, 1000), layout));
  EXPECT_FALSE(planLayout(input(kModeFlash, 5, 1000), layout));
  EXPECT_FALSE(planLayout(input(2, 1, 1000), layout));
  PlanInput empty = input(kModeFlash, 2, 1000);
  empty.styleBytes[1] = 0;
  EXPECT_FALSE(planLayout(empty, layout));
}
```

- [x] **Step 2: Run the test to verify it fails**

```bash
cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest
```

Expected: a compile error, `GlyphStoreFormat.h: No such file or directory`.

- [x] **Step 3: Create `lib/EpdFont/ttf/GlyphStoreFormat.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

/// On-flash format of the TTF glyph store in the `spiffs` partition (magic "CPTT"). Spec:
/// docs/superpowers/specs/2026-10-13-ttf-fonts-design.md, section 4.
///
///   0            header sector (4 KB): Header, then the size table (0x400) and the wipe word (0x480)
///   64 KB        style blocks, each starting on a 64 KB page so one maps alone (flash mode: the whole
///                TTF; SD mode: the tables copied out of it)
///   indexOffset  glyph index: Slot {key, log offset}, open addressing
///   pairOffset   kerning pair index: Slot {left << 16 | right, style << 16 | font units}
///   logOffset    append-only glyph records: [u32 key][EpdGlyph][2-bit bitmap], each 4-byte aligned
///
/// Outside the header sector every byte is programmed at most once between erases. Erased flash reads
/// 0xFF, so "erased" doubles as "empty".
namespace glyphstore {

constexpr uint32_t kMagic = 0x54545043;  // "CPTT"
constexpr uint16_t kFormatVersion = 1;
constexpr uint8_t kModeFlash = 0;  ///< every style's whole TTF is in the partition
constexpr uint8_t kModeSd = 1;     ///< the tables are in the partition; outlines are read from the SD card
constexpr uint8_t kMaxStyles = 4;
constexpr uint8_t kMaxTables = 8;
constexpr uint8_t kSizeEntries = 16;

constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kPageBytes = 65536;               ///< one MMU page: mappings are paid in whole pages
constexpr uint32_t kHeaderRegionBytes = kPageBytes;  ///< sector 0 is the header; the regions start a page later
constexpr uint32_t kSizeTableOffset = 0x400;
constexpr uint32_t kWipeWordOffset = 0x480;

constexpr uint32_t kErased = 0xFFFFFFFF;
constexpr uint32_t kMissing = 0xFFFFFFFE;  ///< slot value: the font has no glyph for this key; no record
constexpr uint32_t kSlotBytes = 8;
constexpr uint32_t kSlotsPerPage = kPageBytes / kSlotBytes;  // 8,192
constexpr uint32_t kMaxProbe = 32;
constexpr uint32_t kIndexLoadPercent = 70;  ///< more slots in use than this counts as full (4.6)
constexpr uint32_t kLogFullPercent = 95;

constexpr uint32_t kRecordHeaderBytes = 20;  ///< u32 key + 16-byte EpdGlyph
constexpr uint32_t kMaxBitmapBytes = 8192;
constexpr uint32_t kMaxRecordBytes = kRecordHeaderBytes + kMaxBitmapBytes;

constexpr uint32_t kMinLogPages = 4;
constexpr uint32_t kPairPages = 1;
constexpr uint32_t kSparePages = 2;
/// Expected bytes per stored record, used only to size the index against the log: a Latin glyph at
/// 14 pt is about 100 B, a CJK glyph about 250 B (Appendix A).
constexpr uint32_t kExpectedRecordFlash = 100;
constexpr uint32_t kExpectedRecordSd = 250;

/// Glyph key: code point (21 bits), style (2), size-table index (4). Bits 27-31 stay zero, so a key is
/// never the erased value.
constexpr uint32_t makeKey(const uint32_t codepoint, const uint8_t style, const uint8_t sizeIndex) {
  return (codepoint & 0x1FFFFFu) | (static_cast<uint32_t>(style & 0x3u) << 21) |
         (static_cast<uint32_t>(sizeIndex & 0xFu) << 23);
}
constexpr bool isGlyphKey(const uint32_t key) { return (key >> 27) == 0; }
constexpr uint32_t pairKey(const uint16_t left, const uint16_t right) {
  return (static_cast<uint32_t>(left) << 16) | right;
}

/// Home slot of `key` in a table of `slots`: Fibonacci hashing, then a multiply-shift into the range. For
/// a power-of-two table this is the spec's `(key * 2654435761) >> (32 - log2(slots))`; it also serves any
/// slot count, so the index can be sized in whole pages.
constexpr uint32_t homeSlot(const uint32_t key, const uint32_t slots) {
  return static_cast<uint32_t>((static_cast<uint64_t>(key * 2654435761u) * slots) >> 32);
}

constexpr uint32_t align4(const uint32_t n) { return (n + 3u) & ~3u; }
constexpr uint32_t alignUp(const uint32_t n, const uint32_t to) { return (n + to - 1) / to * to; }
constexpr uint32_t pagesFor(const uint32_t bytes) { return (bytes + kPageBytes - 1) / kPageBytes; }

struct Slot {
  uint32_t key;
  uint32_t value;
};
static_assert(sizeof(Slot) == kSlotBytes, "a slot is 8 bytes");

/// SD mode: where a table copied out of the TTF sits.
struct TableEntry {
  uint32_t tag;          ///< the OpenType tag as read from the file
  uint32_t fileOffset;   ///< offset in the TTF file
  uint32_t blockOffset;  ///< offset in this style's block
  uint32_t length;
};

struct StyleInfo {
  uint32_t offset;      ///< partition offset of the style block
  uint32_t size;        ///< bytes of style data in the block
  uint32_t sdPathHash;  ///< SD mode: the TTF file the outlines come from
  uint32_t fileSize;    ///< SD mode
  uint32_t tableCount;  ///< SD mode: entries used in tables
  TableEntry tables[kMaxTables];
};

struct Geometry {
  uint8_t mode;
  uint8_t styleCount;
  uint16_t reserved;
  uint32_t indexOffset;
  uint32_t indexSlots;
  uint32_t pairOffset;
  uint32_t pairSlots;
  uint32_t logOffset;
  uint32_t logSize;
};

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;  ///< sizeof(Header) when written
  uint8_t identity[16];  ///< the family identity (4.2), compared at open
  Geometry geometry;
  StyleInfo styles[kMaxStyles];
  uint32_t check;  ///< FNV-1a over every byte before it: a torn header write fails it
};
static_assert(sizeof(Header) <= kSizeTableOffset, "the header overlaps the size table");

/// A size-table entry. It is in use once `pad` reads zero: the pad is written last, so an entry cut short
/// reads as neither free nor in use, and is never claimed again.
struct SizeEntry {
  uint32_t ppem26_6;
  uint8_t flags;
  uint8_t pad[3];
};
static_assert(sizeof(SizeEntry) == 8, "a size entry is 8 bytes");
static_assert(kSizeTableOffset + kSizeEntries * sizeof(SizeEntry) <= kWipeWordOffset,
              "the size table overlaps the wipe word");

inline uint32_t fnv1a(const void* data, const size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

/// What the caller knows before an install (7.1): the mode, and per style the bytes to copy (flash
/// mode: the TTF file; SD mode: the copied tables, summed).
struct PlanInput {
  uint8_t mode;
  uint8_t styleCount;
  uint32_t styleBytes[kMaxStyles];
  uint32_t usableBytes;   ///< NorFlash::size(): the partition below the language slot
  uint32_t freeMapPages;  ///< NorFlash::freeMapPages(), measured while nothing of the store is mapped
};

struct Layout {
  Geometry geometry;
  uint32_t styleOffset[kMaxStyles];
  uint32_t styleSize[kMaxStyles];
};

/// Region sizes from the mapping budget (4.1):
///   log pages = free pages - mapped style pages - index pages - pair pages - 2 spare,
/// also bounded by the partition. The index gets enough pages that 70 % of its slots holds the
/// records the log holds (kExpectedRecord*), so it does not fill first. Flash mode maps one style at a
/// time, so the largest style counts; SD mode maps all copied tables together. False when the log
/// would be under kMinLogPages.
bool planLayout(const PlanInput& in, Layout& out);

}  // namespace glyphstore
```

- [x] **Step 4: Create `lib/EpdFont/ttf/GlyphStoreFormat.cpp`**

```cpp
#include "GlyphStoreFormat.h"

namespace glyphstore {

namespace {

// Index pages so that kIndexLoadPercent of the slots holds logPages of expectedRecord-byte records.
uint32_t indexPagesFor(const uint32_t logPages, const uint32_t expectedRecord) {
  const uint64_t logBytes = static_cast<uint64_t>(logPages) * kPageBytes;
  const uint64_t denominator = static_cast<uint64_t>(expectedRecord) * kIndexLoadPercent;
  const uint64_t slots = (logBytes * 100 + denominator - 1) / denominator;
  const uint64_t pages = (slots + kSlotsPerPage - 1) / kSlotsPerPage;
  return pages == 0 ? 1 : static_cast<uint32_t>(pages);
}

}  // namespace

bool planLayout(const PlanInput& in, Layout& out) {
  out = Layout{};
  if (in.styleCount == 0 || in.styleCount > kMaxStyles) return false;
  if (in.mode != kModeFlash && in.mode != kModeSd) return false;

  uint32_t offset = kHeaderRegionBytes;
  uint32_t stylePages = 0;
  uint32_t largestStyle = 0;
  for (uint8_t s = 0; s < in.styleCount; ++s) {
    const uint32_t pages = pagesFor(in.styleBytes[s]);
    if (pages == 0) return false;
    out.styleOffset[s] = offset;
    out.styleSize[s] = in.styleBytes[s];
    offset += pages * kPageBytes;
    stylePages += pages;
    if (pages > largestStyle) largestStyle = pages;
  }

  const int64_t usablePages = in.usableBytes / kPageBytes;
  const int64_t fixedPages = 1 + static_cast<int64_t>(stylePages);  // the header page and the style blocks
  const int64_t mappedStyles = in.mode == kModeFlash ? largestStyle : stylePages;
  const uint32_t expected = in.mode == kModeFlash ? kExpectedRecordFlash : kExpectedRecordSd;

  // The largest log either limit allows with a one-page index; then shrink it until the index it needs
  // fits as well.
  const int64_t byBudget = static_cast<int64_t>(in.freeMapPages) - mappedStyles - kPairPages - kSparePages - 1;
  const int64_t byPartition = usablePages - fixedPages - kPairPages - 1;
  for (int64_t log = byBudget < byPartition ? byBudget : byPartition; log >= kMinLogPages; --log) {
    const int64_t index = indexPagesFor(static_cast<uint32_t>(log), expected);
    if (mappedStyles + index + kPairPages + log + kSparePages > in.freeMapPages) continue;
    if (fixedPages + index + kPairPages + log > usablePages) continue;
    Geometry& g = out.geometry;
    g.mode = in.mode;
    g.styleCount = in.styleCount;
    g.indexOffset = offset;
    g.indexSlots = static_cast<uint32_t>(index) * kSlotsPerPage;
    g.pairOffset = g.indexOffset + static_cast<uint32_t>(index) * kPageBytes;
    g.pairSlots = kPairPages * kSlotsPerPage;
    g.logOffset = g.pairOffset + kPairPages * kPageBytes;
    g.logSize = static_cast<uint32_t>(log) * kPageBytes;
    return true;
  }
  out = Layout{};
  return false;
}

}  // namespace glyphstore
```

- [x] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest
```

Expected: `[  PASSED  ] 24 tests.` (13 from Task 1, 11 new)

- [x] **Step 6: Commit**

```bash
git add lib/EpdFont/ttf/GlyphStoreFormat.h lib/EpdFont/ttf/GlyphStoreFormat.cpp test/glyph_store
git commit -m "ttf: glyph store format and layout planner (index sized to the log)"
```

---
### Task 3: Install, open and the recovery scan

**Files:**
- Create: `lib/EpdFont/ttf/FlashGlyphStore.h` (the whole API; Tasks 4 and 5 define the rest of it)
- Create: `lib/EpdFont/ttf/FlashGlyphStore.cpp` (install, open, close, style mapping, log scan, erasing)
- Create: `test/glyph_store/GlyphStoreTestUtil.h`, `test/glyph_store/GlyphStoreInstallTest.cpp`,
  `test/glyph_store/GlyphStoreRecoveryTest.cpp`
- Modify: `test/glyph_store/CMakeLists.txt`

**Interfaces:**
- Consumes: `NorFlash` (Task 1) and everything in `glyphstore` (Task 2).
- Produces: `class GlyphBatch` (its `add()` is defined in Task 4), and `class FlashGlyphStore` with:
  - **install:** `beginInstall(const Layout&, const uint8_t identity[16], const StyleInfo* sdInfo)`,
    `writeStyleData(uint8_t style, uint32_t offset, const void*, uint32_t len)`, `finishInstall()`
  - **session:** `OpenResult open(const uint8_t identity[16])`, `close()`, `isOpen()`, `mode()`,
    `styleCount()`, `logBase()`, `mapStyle(uint8_t)`, `styleInfo(uint8_t, StyleInfo&)`
  - **glyphs** (Task 4): `find(uint32_t key, const EpdGlyph**)`, `commit(GlyphBatch&)`,
    `markMissing(uint32_t)`
  - **sizes, pairs, wipe** (Task 5): `sizeIndex(uint32_t ppem26_6, uint8_t flags)`,
    `findPair(uint16_t, uint16_t, uint8_t, int16_t*)`, `storePair(uint16_t, uint16_t, uint8_t, int16_t)`,
    `wipePending()`, `wipe()`
  - **stats:** `logUsed()`, `usedSlots()`, `usedPairSlots()`, `geometry()`
  - **enums:**
    - `OpenResult {Ready, NotInstalled, OtherFamily, MapFailed, NoMemory}`
    - `Lookup {Absent, Missing, Found}`
    - `Commit {Stored, Full, WriteFailed}`
- Produces in `gst_test` (`GlyphStoreTestUtil.h`): `kIdentityA`, `kIdentityB`, `kFlashBytes`,
  `smallLayout()`, `styleByte()`, `install()`, `installAndOpen()`, `metrics()`, `pattern()`,
  `writeRawRecord()`, `recordSize()`, and the fixture `StoreTest`, which asserts no NOR violations in
  `TearDown`.

- [x] **Step 1: Write the failing tests**

In `test/glyph_store/CMakeLists.txt`, replace the source and include lists with:

```cmake
add_executable(GlyphStoreTest
  SimNorFlashTest.cpp
  GlyphStoreFormatTest.cpp
  GlyphStoreInstallTest.cpp
  GlyphStoreRecoveryTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/FlashGlyphStore.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(GlyphStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)
```

(`test/shims` supplies `HardwareSerial.h` for `Logging.h`; on the host `LOG_*` compile to nothing.)

Create `test/glyph_store/GlyphStoreTestUtil.h`:

```cpp
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "EpdFontData.h"
#include "FlashGlyphStore.h"
#include "GlyphStoreFormat.h"
#include "SimNorFlash.h"

namespace gst_test {

constexpr uint8_t kIdentityA[16] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'A'};
constexpr uint8_t kIdentityB[16] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'B'};
constexpr uint32_t kFlashBytes = 262144;

/// A small store: one 4 KB style at 64 KB, then the index, the pair index and the log, each on a sector.
inline glyphstore::Layout smallLayout(const uint32_t indexSlots = 1024, const uint32_t pairSlots = 512,
                                      const uint32_t logBytes = 65536) {
  glyphstore::Layout layout{};
  glyphstore::Geometry& g = layout.geometry;
  g.mode = glyphstore::kModeFlash;
  g.styleCount = 1;
  layout.styleOffset[0] = 65536;
  layout.styleSize[0] = 4096;
  g.indexOffset = 131072;
  g.indexSlots = indexSlots;
  g.pairOffset = g.indexOffset + glyphstore::alignUp(indexSlots * glyphstore::kSlotBytes, glyphstore::kSectorBytes);
  g.pairSlots = pairSlots;
  g.logOffset = g.pairOffset + glyphstore::alignUp(pairSlots * glyphstore::kSlotBytes, glyphstore::kSectorBytes);
  g.logSize = logBytes;
  return layout;
}

inline uint8_t styleByte(const uint8_t style, const uint32_t i) { return static_cast<uint8_t>(i * 7 + style * 13 + 1); }

/// Installs `layout` under `identity`, every style filled with styleByte().
inline bool install(FlashGlyphStore& store, const glyphstore::Layout& layout, const uint8_t* identity = kIdentityA,
                    const glyphstore::StyleInfo* sdInfo = nullptr) {
  if (!store.beginInstall(layout, identity, sdInfo)) return false;
  for (uint8_t s = 0; s < layout.geometry.styleCount; ++s) {
    std::vector<uint8_t> data(layout.styleSize[s]);
    for (uint32_t i = 0; i < data.size(); ++i) data[i] = styleByte(s, i);
    if (!store.writeStyleData(s, 0, data.data(), static_cast<uint32_t>(data.size()))) return false;
  }
  return store.finishInstall();
}

inline bool installAndOpen(FlashGlyphStore& store, const glyphstore::Layout& layout,
                           const uint8_t* identity = kIdentityA) {
  return install(store, layout, identity) && store.open(identity) == FlashGlyphStore::OpenResult::Ready;
}

inline EpdGlyph metrics(const uint8_t width, const uint8_t height) {
  EpdGlyph glyph{};
  glyph.width = width;
  glyph.height = height;
  glyph.advanceX = static_cast<uint16_t>((width + 1) << 4);
  glyph.left = 1;
  glyph.top = height;
  glyph.dataLength = glyphDataBytes(width, height, true);
  return glyph;
}

/// Bitmap byte i of the glyph stored under `key`: a pattern any test can recompute.
inline uint8_t pattern(const uint32_t key, const uint32_t i) { return static_cast<uint8_t>((key * 31u + i * 7u) ^ 0x5Au); }

/// Bytes a record with a width x height bitmap takes in the log.
inline uint32_t recordSize(const uint8_t width, const uint8_t height) {
  return glyphstore::align4(glyphstore::kRecordHeaderBytes + glyphDataBytes(width, height, true));
}

/// Programs a record straight into the log at `offset`, bypassing the store (the recovery tests). Only the
/// first `bitmapBytes` of its bitmap are written, as a cut write would leave it.
inline void writeRawRecord(SimNorFlash& flash, const glyphstore::Layout& layout, const uint32_t offset,
                           const uint32_t key, const uint8_t width, const uint8_t height,
                           const uint32_t bitmapBytes = UINT32_MAX) {
  EpdGlyph glyph = metrics(width, height);
  glyph.dataOffset = offset + glyphstore::kRecordHeaderBytes;
  std::vector<uint8_t> record(glyphstore::kRecordHeaderBytes + glyph.dataLength);
  std::memcpy(record.data(), &key, sizeof key);
  std::memcpy(record.data() + 4, &glyph, sizeof glyph);
  for (uint32_t i = 0; i < glyph.dataLength; ++i) record[glyphstore::kRecordHeaderBytes + i] = pattern(key, i);
  const uint32_t written = bitmapBytes < glyph.dataLength ? bitmapBytes : glyph.dataLength;
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + offset, record.data(),
                            glyphstore::kRecordHeaderBytes + written));
}

/// Every store test runs on its own simulated chip, and no test may ever program a byte twice.
class StoreTest : public ::testing::Test {
 protected:
  void TearDown() override { EXPECT_EQ(0u, flash.violations()); }

  SimNorFlash flash{kFlashBytes};
  FlashGlyphStore store{flash};
};

}  // namespace gst_test
```

Create `test/glyph_store/GlyphStoreInstallTest.cpp`:

```cpp
// Installing a family into the glyph store and opening it (spec 4.1, 4.2, 7.1).
#include <gtest/gtest.h>

#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

class GlyphStoreInstall : public StoreTest {};

Layout twoStyleLayout(const uint8_t mode) {
  Layout layout = smallLayout();
  layout.geometry.mode = mode;
  layout.geometry.styleCount = 2;
  layout.styleOffset[1] = layout.styleOffset[0] + 4096;
  layout.styleSize[1] = 4096;
  return layout;
}

}  // namespace

TEST_F(GlyphStoreInstall, ABlankChipIsNotInstalled) { EXPECT_EQ(OpenResult::NotInstalled, store.open(kIdentityA)); }

TEST_F(GlyphStoreInstall, AnInstalledStoreOpensEmpty) {
  ASSERT_TRUE(installAndOpen(store, smallLayout()));
  EXPECT_TRUE(store.isOpen());
  EXPECT_EQ(kModeFlash, store.mode());
  EXPECT_EQ(1, store.styleCount());
  EXPECT_EQ(0u, store.logUsed());
  EXPECT_EQ(0u, store.usedSlots());
  EXPECT_EQ(0u, store.usedPairSlots());
  EXPECT_FALSE(store.wipePending());
  EXPECT_NE(nullptr, store.logBase());
}

TEST_F(GlyphStoreInstall, AnotherFamilyIsReported) {
  ASSERT_TRUE(install(store, smallLayout(), kIdentityA));
  EXPECT_EQ(OpenResult::OtherFamily, store.open(kIdentityB));
  EXPECT_FALSE(store.isOpen());
  EXPECT_EQ(0u, flash.mappings());
}

TEST_F(GlyphStoreInstall, StyleDataIsReadableThroughTheMapping) {
  ASSERT_TRUE(installAndOpen(store, smallLayout()));
  const uint8_t* style = store.mapStyle(0);
  ASSERT_NE(nullptr, style);
  for (uint32_t i = 0; i < 4096; ++i) ASSERT_EQ(styleByte(0, i), style[i]) << i;
  EXPECT_EQ(nullptr, store.mapStyle(1));
}

TEST_F(GlyphStoreInstall, UntilFinishTheStoreIsNotInstalled) {
  const Layout layout = smallLayout();
  ASSERT_TRUE(store.beginInstall(layout, kIdentityA, nullptr));
  const std::vector<uint8_t> data(layout.styleSize[0], 0x11);
  ASSERT_TRUE(store.writeStyleData(0, 0, data.data(), static_cast<uint32_t>(data.size())));
  {
    FlashGlyphStore other(flash);
    EXPECT_EQ(OpenResult::NotInstalled, other.open(kIdentityA));
  }
  ASSERT_TRUE(store.finishInstall());
  EXPECT_EQ(OpenResult::Ready, store.open(kIdentityA));
}

TEST(GlyphStoreInstallCut, ACutAnywhereInTheHeaderWriteReadsAsNotInstalled) {
  const Layout layout = smallLayout();
  for (const bool partial : {false, true}) {
    for (uint32_t cut = 0; cut < sizeof(Header); ++cut) {
      SimNorFlash flash(kFlashBytes);
      FlashGlyphStore store(flash);
      ASSERT_TRUE(store.beginInstall(layout, kIdentityA, nullptr));
      flash.partialCut(partial);
      flash.cutAfterSteps(cut);
      EXPECT_FALSE(store.finishInstall());
      flash.powerCycle();
      FlashGlyphStore reopened(flash);
      const OpenResult result = reopened.open(kIdentityA);
      // Only a partly programmed last byte can complete the header by chance.
      const bool lastByteByChance = partial && cut == sizeof(Header) - 1 && result == OpenResult::Ready;
      EXPECT_TRUE(result == OpenResult::NotInstalled || lastByteByChance) << "cut " << cut << " partial " << partial;
    }
  }
}

TEST_F(GlyphStoreInstall, InstallingOnABlankChipErasesNothing) {
  ASSERT_TRUE(installAndOpen(store, smallLayout()));
  EXPECT_EQ(0u, flash.eraseCalls());
}

TEST_F(GlyphStoreInstall, OpenFailsCleanlyWhenTheRegionsCannotBeMapped) {
  ASSERT_TRUE(install(store, smallLayout()));
  flash.setFreePages(0);
  EXPECT_EQ(OpenResult::MapFailed, store.open(kIdentityA));
  EXPECT_FALSE(store.isOpen());
  EXPECT_EQ(0u, flash.mappings());
  flash.setFreePages(32);
  EXPECT_EQ(OpenResult::Ready, store.open(kIdentityA));
}

TEST_F(GlyphStoreInstall, ALayoutOutsideTheChipIsRefused) {
  Layout tooLong = smallLayout();
  tooLong.geometry.logSize = kFlashBytes;  // runs past the end
  EXPECT_FALSE(store.beginInstall(tooLong, kIdentityA, nullptr));
  Layout overlapping = smallLayout();
  overlapping.styleSize[0] = 70000;  // into the index
  EXPECT_FALSE(store.beginInstall(overlapping, kIdentityA, nullptr));
  EXPECT_EQ(0u, flash.eraseCalls());
  EXPECT_EQ(0u, flash.programCalls());
}

TEST_F(GlyphStoreInstall, SdModeMapsAllStylesTogether) {
  const Layout layout = twoStyleLayout(kModeSd);
  StyleInfo info[kMaxStyles] = {};
  info[1].sdPathHash = 0xCAFE;
  info[1].fileSize = 19700000;
  info[1].tableCount = 1;
  info[1].tables[0] = {0x636D6170, 1234, 0, 4096};  // 'cmap'
  ASSERT_TRUE(install(store, layout, kIdentityA, info));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  const uint8_t* regular = store.mapStyle(0);
  const uint8_t* italic = store.mapStyle(1);
  ASSERT_NE(nullptr, regular);
  ASSERT_NE(nullptr, italic);
  EXPECT_EQ(styleByte(0, 5), regular[5]);  // the first pointer is still valid
  EXPECT_EQ(styleByte(1, 5), italic[5]);
  EXPECT_EQ(2u, flash.mappings());  // the glyph regions, and one mapping for both styles
  StyleInfo read{};
  ASSERT_TRUE(store.styleInfo(1, read));
  EXPECT_EQ(0xCAFEu, read.sdPathHash);
  EXPECT_EQ(1u, read.tableCount);
  EXPECT_EQ(1234u, read.tables[0].fileOffset);
  EXPECT_EQ(layout.styleOffset[1], read.offset);
}

TEST_F(GlyphStoreInstall, FlashModeMapsOneStyleAtATime) {
  ASSERT_TRUE(install(store, twoStyleLayout(kModeFlash)));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_NE(nullptr, store.mapStyle(0));
  const uint8_t* italic = store.mapStyle(1);
  ASSERT_NE(nullptr, italic);
  EXPECT_EQ(styleByte(1, 9), italic[9]);
  EXPECT_EQ(2u, flash.mappings());  // the glyph regions and the one style
  store.close();
  EXPECT_EQ(0u, flash.mappings());
}
```

Create `test/glyph_store/GlyphStoreRecoveryTest.cpp`:

```cpp
// Where writing resumes after a power cut (spec 4.5): the log scan never lets a byte be programmed twice.
#include <gtest/gtest.h>

#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

class GlyphStoreRecovery : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(install(store, layout)); }

  // Opens from scratch, as after a reboot.
  void reopen() {
    store.close();
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }

  // Two complete records at the start of the log; returns where the next one would go.
  uint32_t writeTwo() {
    writeRawRecord(flash, layout, 0, makeKey('a', 0, 0), 9, 12);
    const uint32_t second = recordSize(9, 12);
    writeRawRecord(flash, layout, second, makeKey('b', 0, 0), 10, 12);
    return second + recordSize(10, 12);
  }

  const Layout layout = smallLayout();
};

}  // namespace

TEST_F(GlyphStoreRecovery, WritingResumesAfterTheLastRecord) {
  const uint32_t end = writeTwo();
  reopen();
  EXPECT_EQ(end, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ATornHeaderMovesTheResumePointToTheNextErasedSector) {
  const uint32_t end = writeTwo();
  const uint8_t garbage[10] = {0x12, 0x00, 0x00, 0x08, 0x33};  // key 0x08000012: bit 27 set, not a record
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

TEST_F(GlyphStoreRecovery, AGarbledPageAfterTheLastRecordIsSkipped) {
  // A real chip cut mid-page can leave a whole 256-byte page partly programmed.
  writeTwo();
  std::vector<uint8_t> page(256);
  for (uint32_t i = 0; i < page.size(); ++i) page[i] = static_cast<uint8_t>(0xFF ^ (i * 37));
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + 4096 + 100, page.data(), 256));
  reopen();
  EXPECT_EQ(2 * kSectorBytes, store.logUsed());
}

TEST(GlyphStoreRecoveryEdge, GarbageAtTheVeryEndLeavesTheLogFull) {
  SimNorFlash flash(kFlashBytes);
  FlashGlyphStore store(flash);
  const Layout layout = smallLayout(1024, 512, 8192);
  ASSERT_TRUE(install(store, layout));
  writeRawRecord(flash, layout, 0, makeKey('a', 0, 0), 9, 12);
  const uint8_t junk = 0x00;
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + 8190, &junk, 1));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(8192u, store.logUsed());
  EXPECT_EQ(0u, flash.violations());
}
```

- [x] **Step 2: Run them to verify they fail**

```bash
cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest
```

Expected: a compile error, `FlashGlyphStore.h: No such file or directory`.

- [x] **Step 3: Create `lib/EpdFont/ttf/FlashGlyphStore.h`**

```cpp
#pragma once

#include <cstdint>
#include <memory>

#include "EpdFontData.h"
#include "GlyphStoreFormat.h"
#include "NorFlash.h"

/// Glyph records staged in a caller-owned buffer exactly as they will sit in the log, so
/// FlashGlyphStore::commit() writes a whole batch with ONE program call. On the X3 each write call costs
/// about 100 µs on top of its bytes (spec Appendix A), and one call per batch also programs each flash
/// page once rather than once per record.
class GlyphBatch {
 public:
  GlyphBatch(uint8_t* buffer, const uint32_t capacity) : buffer_(buffer), capacity_(capacity) {}

  /// Stages a record for `key` with `metrics` (its dataOffset is set at commit). Returns where the caller
  /// writes the metrics.dataLength bitmap bytes, or nullptr when the buffer has no room (commit, then add
  /// again) or the bitmap is over glyphstore::kMaxBitmapBytes. Keys in one batch must be distinct and not
  /// yet stored.
  uint8_t* add(uint32_t key, const EpdGlyph& metrics);

  uint16_t count() const { return count_; }
  uint32_t bytes() const { return used_; }
  void clear() {
    used_ = 0;
    count_ = 0;
  }

 private:
  friend class FlashGlyphStore;

  uint8_t* buffer_;
  uint32_t capacity_;
  uint32_t used_ = 0;
  uint16_t count_ = 0;
};

/// The persistent TTF glyph store in the `spiffs` partition (spec section 4):
/// - a write-once hash index and an append-only log of rendered glyphs;
/// - a kerning pair index and a size table;
/// - the font's own bytes, in style blocks.
///
/// Rules it keeps:
/// - Outside the header sector, every byte is programmed at most once between erases.
/// - A record is written before the slot that points at it, so a power cut leaves at worst bytes that
///   nothing references.
/// - Erases happen only at install and wipe, and only on sectors that hold data.
/// - A lookup reads mapped flash in place: a hit is a pointer to the EpdGlyph inside the log.
///
/// Not thread-safe: TtfFont calls it only under RenderLock (spec 3.4).
class FlashGlyphStore {
 public:
  enum class OpenResult : uint8_t { Ready, NotInstalled, OtherFamily, MapFailed, NoMemory };
  enum class Lookup : uint8_t { Absent, Missing, Found };
  enum class Commit : uint8_t { Stored, Full, WriteFailed };

  explicit FlashGlyphStore(NorFlash& flash) : flash_(flash) {}
  ~FlashGlyphStore() { close(); }
  FlashGlyphStore(const FlashGlyphStore&) = delete;
  FlashGlyphStore& operator=(const FlashGlyphStore&) = delete;

  // --- Install (4.1, 7.1) ---

  /// Erases the header first, so a power cut from here on reads as "not installed". Then erases every
  /// sector of the new layout that holds data. `sdInfo` (SD mode, may be null) supplies each style's file
  /// hash, size and table map; offsets and sizes come from `layout`.
  bool beginInstall(const glyphstore::Layout& layout, const uint8_t identity[16],
                    const glyphstore::StyleInfo* sdInfo);
  /// Copies style data into a style block, each byte once. Only between beginInstall() and finishInstall().
  bool writeStyleData(uint8_t style, uint32_t offset, const void* data, uint32_t len);
  /// Writes the header. Only now does the store open.
  bool finishInstall();

  // --- Session ---

  OpenResult open(const uint8_t identity[16]);
  void close();
  bool isOpen() const { return open_; }
  uint8_t mode() const { return geometry_.mode; }
  uint8_t styleCount() const { return geometry_.styleCount; }
  /// EpdFontData::bitmap for every TTF face: EpdGlyph::dataOffset is relative to it.
  const uint8_t* logBase() const { return log_; }
  /// A style's data, mapped. Flash mode maps one style at a time: the pointer stays valid until
  /// mapStyle() is called for another style, or the store closes. SD mode maps all styles together.
  const uint8_t* mapStyle(uint8_t style);
  /// A style's header record (SD mode: its table map). False when not open.
  bool styleInfo(uint8_t style, glyphstore::StyleInfo& out) const;

  // --- Glyphs (4.3-4.5) ---

  Lookup find(uint32_t key, const EpdGlyph** glyph) const;
  /// Writes the staged records with one program call, then one slot per record, and clears the batch.
  /// - Full: nothing new was stored, and wipe is now pending (4.6).
  /// - WriteFailed: a write failed, and wipe is now pending (6.2).
  Commit commit(GlyphBatch& batch);
  /// Records that the font has no glyph for this key (6.1): a slot with no record.
  Commit markMissing(uint32_t key);

  // --- Sizes (4.2) ---

  /// The size-table index for a pixels-per-em (26.6), claiming a free entry on first use. -1 when the
  /// table is full (wipe is then pending), on a write error, or for a new size while wipe is pending.
  int sizeIndex(uint32_t ppem26_6, uint8_t flags);

  // --- Kerning pairs (4.3) ---

  bool findPair(uint16_t left, uint16_t right, uint8_t style, int16_t* fontUnits) const;
  /// Stores a pair's kerning in font units. Returns false when it was not stored: the pair index is full
  /// (pairs then come from the GPOS reader every time; never a reason to wipe), wipe is pending, or a
  /// write failed.
  bool storePair(uint16_t left, uint16_t right, uint8_t style, int16_t fontUnits);

  // --- A full store, and the wipe (4.6, 7.5) ---

  bool wipePending() const { return wipePending_; }
  /// Erases the index, the pair index and the log (only sectors that hold data), then rewrites the header
  /// with an empty size table and no wipe pending. Call it only where no EpdGlyph pointer into the log can
  /// be live: at book open, or before a section build.
  bool wipe();

  // --- Stats ---

  uint32_t logUsed() const { return logUsed_; }
  uint32_t usedSlots() const { return usedSlots_; }
  uint32_t usedPairSlots() const { return usedPairs_; }
  const glyphstore::Geometry& geometry() const { return geometry_; }

 private:
  enum class Insert : uint8_t { Done, ChainFull, WriteFailed };

  bool plausibleRecord(uint32_t offset) const;
  const EpdGlyph* validRecord(uint32_t key, uint32_t logOffset) const;
  uint32_t scanLog() const;
  Insert insert(uint32_t tableOffset, const uint8_t* table, uint32_t slots, uint32_t key, uint32_t value);
  bool eraseWritten(uint32_t offset, uint32_t len);
  bool isBlank(uint32_t sector);
  void setWipePending();

  NorFlash& flash_;
  glyphstore::Geometry geometry_{};
  uint32_t styleOffset_[glyphstore::kMaxStyles] = {};
  uint32_t styleSize_[glyphstore::kMaxStyles] = {};
  const uint8_t* region_ = nullptr;  // one mapping: index, pair index, log
  const uint8_t* index_ = nullptr;
  const uint8_t* pairs_ = nullptr;
  const uint8_t* log_ = nullptr;
  const uint8_t* styleMap_ = nullptr;
  uint8_t mappedStyle_ = 0xFF;
  uint32_t logUsed_ = 0;
  uint32_t usedSlots_ = 0;
  uint32_t usedPairs_ = 0;
  bool open_ = false;
  bool wipePending_ = false;
  std::unique_ptr<glyphstore::Header> installing_;  // written by finishInstall()
};
```

- [x] **Step 4: Create `lib/EpdFont/ttf/FlashGlyphStore.cpp`**

```cpp
#include "FlashGlyphStore.h"

#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstring>

using namespace glyphstore;

static_assert(sizeof(EpdGlyph) + 4 == kRecordHeaderBytes, "a record header is the key plus one EpdGlyph");
static_assert(offsetof(EpdGlyph, dataOffset) % 4 == 0, "EpdGlyph::dataOffset must be readable in place");

namespace {

uint32_t loadU32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof value);
  return value;
}

EpdGlyph loadGlyph(const uint8_t* bytes) {
  EpdGlyph glyph;
  std::memcpy(&glyph, bytes, sizeof glyph);
  return glyph;
}

Slot loadSlot(const uint8_t* table, const uint32_t i) {
  Slot slot;
  std::memcpy(&slot, table + static_cast<size_t>(i) * kSlotBytes, sizeof slot);
  return slot;
}

bool slotErased(const Slot& slot) { return slot.key == kErased && slot.value == kErased; }

uint32_t recordBytes(const uint16_t dataLength) { return align4(kRecordHeaderBytes + dataLength); }

uint32_t tableBytes(const uint32_t slots) { return alignUp(slots * kSlotBytes, kSectorBytes); }

uint32_t countUsed(const uint8_t* table, const uint32_t slots) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < slots; ++i) {
    if (!slotErased(loadSlot(table, i))) ++used;
  }
  return used;
}

// The regions are sector-aligned, in the order index, pairs, log, and inside the flash.
bool validGeometry(const Geometry& g, const uint32_t flashSize) {
  if (g.mode > kModeSd || g.styleCount == 0 || g.styleCount > kMaxStyles) return false;
  if (g.indexSlots == 0 || g.pairSlots == 0 || g.logSize == 0 || g.logSize % kSectorBytes != 0) return false;
  if (g.indexOffset % kSectorBytes != 0 || g.pairOffset % kSectorBytes != 0 || g.logOffset % kSectorBytes != 0) {
    return false;
  }
  const uint64_t indexEnd = uint64_t{g.indexOffset} + uint64_t{g.indexSlots} * kSlotBytes;
  const uint64_t pairEnd = uint64_t{g.pairOffset} + uint64_t{g.pairSlots} * kSlotBytes;
  return g.indexOffset >= kSectorBytes && indexEnd <= g.pairOffset && pairEnd <= g.logOffset &&
         uint64_t{g.logOffset} + g.logSize <= flashSize;
}

// The style blocks are sector-aligned, in order, between the header sector and the index.
bool validStyles(const uint32_t* offsets, const uint32_t* sizes, const Geometry& g) {
  uint32_t floor = kSectorBytes;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    if (offsets[s] < floor || offsets[s] % kSectorBytes != 0 || sizes[s] == 0) return false;
    if (uint64_t{offsets[s]} + sizes[s] > g.indexOffset) return false;
    floor = offsets[s] + sizes[s];
  }
  return true;
}

}  // namespace

// --- Install ---

bool FlashGlyphStore::beginInstall(const Layout& layout, const uint8_t identity[16], const StyleInfo* sdInfo) {
  close();
  installing_.reset();
  const Geometry& g = layout.geometry;
  if (!validGeometry(g, flash_.size()) || !validStyles(layout.styleOffset, layout.styleSize, g)) {
    LOG_ERR("GST", "install: the layout does not fit the flash");
    return false;
  }
  for (uint8_t s = 0; sdInfo && s < g.styleCount; ++s) {
    if (sdInfo[s].tableCount > kMaxTables) {
      LOG_ERR("GST", "install: style %u lists %u tables (max %u)", s, static_cast<unsigned>(sdInfo[s].tableCount),
              static_cast<unsigned>(kMaxTables));
      return false;
    }
  }
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "install: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return false;
  }
  header->magic = kMagic;
  header->version = kFormatVersion;
  header->headerBytes = sizeof(Header);
  std::memcpy(header->identity, identity, sizeof header->identity);
  header->geometry = g;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    if (sdInfo) header->styles[s] = sdInfo[s];
    header->styles[s].offset = layout.styleOffset[s];
    header->styles[s].size = layout.styleSize[s];
  }

  // The header sector goes first: from here until finishInstall() the store reads as not installed.
  bool erased = eraseWritten(0, kSectorBytes);
  for (uint8_t s = 0; erased && s < g.styleCount; ++s) {
    erased = eraseWritten(layout.styleOffset[s], alignUp(layout.styleSize[s], kSectorBytes));
  }
  erased = erased && eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) &&
           eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) && eraseWritten(g.logOffset, g.logSize);
  if (!erased) {
    LOG_ERR("GST", "install: erase failed");
    return false;
  }
  installing_ = std::move(header);
  return true;
}

bool FlashGlyphStore::writeStyleData(const uint8_t style, const uint32_t offset, const void* data,
                                     const uint32_t len) {
  if (!installing_ || style >= installing_->geometry.styleCount) return false;
  const StyleInfo& info = installing_->styles[style];
  if (offset > info.size || len > info.size - offset) return false;
  return flash_.program(info.offset + offset, data, len);
}

bool FlashGlyphStore::finishInstall() {
  if (!installing_) return false;
  installing_->check = fnv1a(installing_.get(), offsetof(Header, check));
  const bool written = flash_.program(0, installing_.get(), sizeof(Header));
  installing_.reset();
  if (!written) {
    LOG_ERR("GST", "install: header write failed");
  }
  return written;
}

// --- Session ---

FlashGlyphStore::OpenResult FlashGlyphStore::open(const uint8_t identity[16]) {
  close();
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "open: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return OpenResult::NoMemory;
  }
  if (!flash_.read(0, header.get(), sizeof(Header)) || header->magic != kMagic ||
      header->version != kFormatVersion || header->headerBytes != sizeof(Header) ||
      header->check != fnv1a(header.get(), offsetof(Header, check))) {
    return OpenResult::NotInstalled;
  }
  const Geometry& g = header->geometry;
  uint32_t offsets[kMaxStyles] = {};
  uint32_t sizes[kMaxStyles] = {};
  for (uint8_t s = 0; s < kMaxStyles; ++s) {
    offsets[s] = header->styles[s].offset;
    sizes[s] = header->styles[s].size;
  }
  if (!validGeometry(g, flash_.size()) || !validStyles(offsets, sizes, g)) return OpenResult::NotInstalled;
  if (std::memcmp(header->identity, identity, sizeof header->identity) != 0) return OpenResult::OtherFamily;

  region_ = flash_.map(g.indexOffset, g.logOffset + g.logSize - g.indexOffset);
  if (!region_) {
    LOG_ERR("GST", "open: cannot map the index and log (%u B, %u map pages free)",
            static_cast<unsigned>(g.logOffset + g.logSize - g.indexOffset), static_cast<unsigned>(flash_.freeMapPages()));
    return OpenResult::MapFailed;
  }
  geometry_ = g;
  std::memcpy(styleOffset_, offsets, sizeof styleOffset_);
  std::memcpy(styleSize_, sizes, sizeof styleSize_);
  index_ = region_;
  pairs_ = region_ + (g.pairOffset - g.indexOffset);
  log_ = region_ + (g.logOffset - g.indexOffset);
  uint32_t wipeWord = kErased;
  if (!flash_.read(kWipeWordOffset, &wipeWord, sizeof wipeWord)) wipeWord = 0;  // unreadable: assume pending
  wipePending_ = wipeWord != kErased;
  logUsed_ = scanLog();
  usedSlots_ = countUsed(index_, g.indexSlots);
  usedPairs_ = countUsed(pairs_, g.pairSlots);
  open_ = true;
  LOG_INF("GST", "open: log %u of %u B, %u of %u slots, %u pairs%s", static_cast<unsigned>(logUsed_),
          static_cast<unsigned>(g.logSize), static_cast<unsigned>(usedSlots_), static_cast<unsigned>(g.indexSlots),
          static_cast<unsigned>(usedPairs_), wipePending_ ? ", wipe pending" : "");
  return OpenResult::Ready;
}

void FlashGlyphStore::close() {
  if (styleMap_) flash_.unmap(styleMap_);
  if (region_) flash_.unmap(region_);
  styleMap_ = nullptr;
  mappedStyle_ = 0xFF;
  region_ = index_ = pairs_ = log_ = nullptr;
  geometry_ = Geometry{};
  logUsed_ = usedSlots_ = usedPairs_ = 0;
  open_ = false;
  wipePending_ = false;
}

const uint8_t* FlashGlyphStore::mapStyle(const uint8_t style) {
  if (!open_ || style >= geometry_.styleCount) return nullptr;
  if (geometry_.mode == kModeSd) {
    // All styles' tables map together: the budget counted them all (4.1).
    if (!styleMap_) {
      const uint8_t last = geometry_.styleCount - 1;
      styleMap_ = flash_.map(styleOffset_[0], styleOffset_[last] + styleSize_[last] - styleOffset_[0]);
      if (!styleMap_) {
        LOG_ERR("GST", "cannot map the copied tables");
        return nullptr;
      }
    }
    return styleMap_ + (styleOffset_[style] - styleOffset_[0]);
  }
  // Flash mode: one style at a time, so the budget holds only the largest.
  if (styleMap_ && mappedStyle_ == style) return styleMap_;
  if (styleMap_) flash_.unmap(styleMap_);
  styleMap_ = flash_.map(styleOffset_[style], styleSize_[style]);
  mappedStyle_ = styleMap_ ? style : 0xFF;
  if (!styleMap_) {
    LOG_ERR("GST", "cannot map style %u (%u B)", style, static_cast<unsigned>(styleSize_[style]));
  }
  return styleMap_;
}

bool FlashGlyphStore::styleInfo(const uint8_t style, StyleInfo& out) const {
  if (!open_ || style >= geometry_.styleCount) return false;
  return flash_.read(static_cast<uint32_t>(offsetof(Header, styles) + style * sizeof(StyleInfo)), &out, sizeof out);
}

// --- The log ---

// A record the scan can step over: a glyph key, a bitmap length that matches its size and the cap, and a
// dataOffset that points just past its own header. A header torn by a power cut fails one of these.
bool FlashGlyphStore::plausibleRecord(const uint32_t offset) const {
  if (offset % 4 != 0 || geometry_.logSize - offset < kRecordHeaderBytes) return false;
  if (!isGlyphKey(loadU32(log_ + offset))) return false;
  const EpdGlyph glyph = loadGlyph(log_ + offset + 4);
  return glyph.dataLength <= kMaxBitmapBytes && glyph.dataLength == glyphDataBytes(glyph.width, glyph.height, true) &&
         glyph.dataOffset == offset + kRecordHeaderBytes && recordBytes(glyph.dataLength) <= geometry_.logSize - offset;
}

uint32_t FlashGlyphStore::scanLog() const {
  const uint32_t size = geometry_.logSize;
  uint32_t offset = 0;
  while (size - offset >= kRecordHeaderBytes && loadU32(log_ + offset) != kErased && plausibleRecord(offset)) {
    offset += recordBytes(loadGlyph(log_ + offset + 4).dataLength);
  }
  // Whatever stopped the scan, a cut write can have left programmed bytes after it, even behind a key that
  // still reads erased. A torn write spans at most one record, so look that far; resume past the last
  // programmed byte, at the next sector boundary, so no byte is ever programmed twice.
  const uint32_t end = size - offset < kMaxRecordBytes ? size : offset + kMaxRecordBytes;
  for (uint32_t i = end; i > offset; --i) {
    if (log_[i - 1] != 0xFF) {
      const uint32_t resume = alignUp(i, kSectorBytes);
      LOG_INF("GST", "log: torn write after %u; resuming at %u", static_cast<unsigned>(offset),
              static_cast<unsigned>(resume < size ? resume : size));
      return resume < size ? resume : size;
    }
  }
  return offset;
}

// --- Erasing ---

// Erases are what wear the chip, and one costs 4-6 ms even on a blank sector (spec Appendix A), so only
// sectors that hold data are erased; neighbouring ones go in one call.
bool FlashGlyphStore::eraseWritten(const uint32_t offset, const uint32_t len) {
  uint32_t runStart = 0;
  uint32_t runLen = 0;
  for (uint32_t sector = offset; sector < offset + len; sector += kSectorBytes) {
    if (!isBlank(sector)) {
      if (runLen == 0) runStart = sector;
      runLen += kSectorBytes;
      continue;
    }
    if (runLen != 0 && !flash_.erase(runStart, runLen)) return false;
    runLen = 0;
  }
  return runLen == 0 || flash_.erase(runStart, runLen);
}

// Through the mapping when the sector is in it (a wipe), else by reading small chunks (an install).
bool FlashGlyphStore::isBlank(const uint32_t sector) {
  if (region_ && sector >= geometry_.indexOffset && sector + kSectorBytes <= geometry_.logOffset + geometry_.logSize) {
    const uint8_t* bytes = region_ + (sector - geometry_.indexOffset);
    for (uint32_t i = 0; i < kSectorBytes; i += 4) {
      if (loadU32(bytes + i) != kErased) return false;
    }
    return true;
  }
  uint8_t chunk[256];
  for (uint32_t at = 0; at < kSectorBytes; at += sizeof chunk) {
    if (!flash_.read(sector + at, chunk, sizeof chunk)) return false;  // unreadable: erase it
    for (const uint8_t b : chunk) {
      if (b != 0xFF) return false;
    }
  }
  return true;
}
```

- [x] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest
```

Expected: `[  PASSED  ] 41 tests.` (24 earlier, 11 install, 6 recovery)

- [x] **Step 6: Commit**

```bash
git add lib/EpdFont/ttf/FlashGlyphStore.h lib/EpdFont/ttf/FlashGlyphStore.cpp test/glyph_store
git commit -m "ttf: FlashGlyphStore install, open and power-cut recovery scan"
```

---
### Task 4: Glyphs: batched commits, lookups and the full store

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.cpp`: add `GlyphBatch::add`, `validRecord`, `find`, `insert`,
  `commit`, `markMissing` and `setWipePending`
- Modify: `test/glyph_store/GlyphStoreTestUtil.h`: add `stage`, `storedIntact` and `absentOrIntact`
- Create: `test/glyph_store/GlyphStoreGlyphTest.cpp`
- Modify: `test/glyph_store/CMakeLists.txt`: add `GlyphStoreGlyphTest.cpp` after `GlyphStoreRecoveryTest.cpp`

**Interfaces:**
- Consumes: the declarations from Task 3.
- Produces: working `GlyphBatch::add`, `FlashGlyphStore::find`, `commit` and `markMissing`. In `gst_test`:
  - `stage(GlyphBatch&, uint32_t key, uint8_t w, uint8_t h)`
  - `storedIntact(const FlashGlyphStore&, uint32_t key, uint8_t w, uint8_t h)`
  - `absentOrIntact(...)`, with the same parameters

- [ ] **Step 1: Write the failing tests**

Add these three functions to `test/glyph_store/GlyphStoreTestUtil.h`, just above the comment
`/// Every store test runs on its own simulated chip`:

```cpp
/// Stages `key` as a width x height glyph whose bitmap is pattern(key, i).
inline bool stage(GlyphBatch& batch, const uint32_t key, const uint8_t width, const uint8_t height) {
  const EpdGlyph glyph = metrics(width, height);
  uint8_t* bitmap = batch.add(key, glyph);
  if (!bitmap) return false;
  for (uint32_t i = 0; i < glyph.dataLength; ++i) bitmap[i] = pattern(key, i);
  return true;
}

/// True when `key` is stored with exactly the metrics and bitmap stage() gave it.
inline bool storedIntact(const FlashGlyphStore& store, const uint32_t key, const uint8_t width,
                         const uint8_t height) {
  const EpdGlyph* glyph = nullptr;
  if (store.find(key, &glyph) != FlashGlyphStore::Lookup::Found || glyph == nullptr) return false;
  const EpdGlyph want = metrics(width, height);
  if (glyph->width != want.width || glyph->height != want.height || glyph->advanceX != want.advanceX ||
      glyph->left != want.left || glyph->top != want.top || glyph->dataLength != want.dataLength) {
    return false;
  }
  const uint8_t* bitmap = store.logBase() + glyph->dataOffset;
  for (uint32_t i = 0; i < glyph->dataLength; ++i) {
    if (bitmap[i] != pattern(key, i)) return false;
  }
  return true;
}

/// Absent, or stored intact: a lookup may miss a glyph whose write was cut, but never serve wrong bytes.
inline bool absentOrIntact(const FlashGlyphStore& store, const uint32_t key, const uint8_t width,
                           const uint8_t height) {
  return store.find(key, nullptr) == FlashGlyphStore::Lookup::Absent || storedIntact(store, key, width, height);
}
```

Create `test/glyph_store/GlyphStoreGlyphTest.cpp`:

```cpp
// Storing and finding rendered glyphs (spec 4.3-4.6).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

class GlyphStoreGlyphs : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, layout)); }

  void reinstall(const Layout& other) {
    store.close();
    layout = other;
    ASSERT_TRUE(installAndOpen(store, layout));
  }

  Layout layout = smallLayout();
  alignas(4) uint8_t buffer[16384] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

// Keys with the same home slot, found by search: a probe-chain test needs real collisions.
std::vector<uint32_t> collidingKeys(const uint32_t slots, const size_t count) {
  std::vector<uint32_t> keys;
  const uint32_t home = homeSlot(makeKey(0x4E00, 0, 0), slots);
  for (uint32_t cp = 0x4E00; keys.size() < count && cp < 0x10FFFF; ++cp) {
    if (homeSlot(makeKey(cp, 0, 0), slots) == home) keys.push_back(makeKey(cp, 0, 0));
  }
  return keys;
}

uint8_t widthOf(const uint32_t cp) { return static_cast<uint8_t>(6 + cp % 9); }
uint8_t heightOf(const uint32_t cp) { return static_cast<uint8_t>(9 + cp % 7); }

}  // namespace

TEST_F(GlyphStoreGlyphs, ABatchIsStoredAndFound) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_TRUE(stage(batch, makeKey('g', 1, 0), 8, 14));
  ASSERT_TRUE(stage(batch, makeKey(0x4E2D, 0, 2), 24, 26));
  EXPECT_EQ(3, batch.count());
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_EQ(0, batch.count());  // ready to be filled again
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, 0), 9, 12));
  EXPECT_TRUE(storedIntact(store, makeKey('g', 1, 0), 8, 14));
  EXPECT_TRUE(storedIntact(store, makeKey(0x4E2D, 0, 2), 24, 26));
  EXPECT_EQ(3u, store.usedSlots());
}

TEST_F(GlyphStoreGlyphs, ABatchIsOneLogWritePlusOneSlotWritePerGlyph) {
  for (uint32_t cp = 'a'; cp < 'a' + 10; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  const uint32_t before = flash.programCalls();
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_EQ(before + 1 + 10, flash.programCalls());
}

TEST_F(GlyphStoreGlyphs, RecordsAreLaidOutAsTheSpecSays) {
  const uint32_t first = makeKey('A', 0, 0);
  ASSERT_TRUE(stage(batch, first, 9, 12));
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 10, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  const uint8_t* log = flash.raw() + layout.geometry.logOffset;
  uint32_t key = 0;
  std::memcpy(&key, log, sizeof key);
  EXPECT_EQ(first, key);
  const EpdGlyph* found = nullptr;
  ASSERT_EQ(Lookup::Found, store.find(first, &found));
  EXPECT_EQ(store.logBase() + 4, reinterpret_cast<const uint8_t*>(found));  // the EpdGlyph sits at offset 4
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(found) % 4);
  EXPECT_EQ(kRecordHeaderBytes, found->dataOffset);  // relative to the log base
  std::memcpy(&key, log + recordSize(9, 12), sizeof key);
  EXPECT_EQ(makeKey('B', 0, 0), key);  // the next record starts 4-byte aligned
  EXPECT_EQ(recordSize(9, 12) + recordSize(10, 12), store.logUsed());
}

TEST_F(GlyphStoreGlyphs, AGlyphWithNoPixelsIsStored) {
  ASSERT_TRUE(stage(batch, makeKey(' ', 0, 0), 0, 0));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_TRUE(storedIntact(store, makeKey(' ', 0, 0), 0, 0));
}

TEST_F(GlyphStoreGlyphs, AnUnknownKeyIsAbsent) {
  EXPECT_EQ(Lookup::Absent, store.find(makeKey('Q', 0, 0), nullptr));
  EXPECT_EQ(Lookup::Absent, store.find(kErased, nullptr));
}

TEST_F(GlyphStoreGlyphs, MissingGlyphsAreRemembered) {
  const uint32_t key = makeKey(0x2603, 0, 0);
  ASSERT_EQ(Commit::Stored, store.markMissing(key));
  EXPECT_EQ(Lookup::Missing, store.find(key, nullptr));
  EXPECT_EQ(0u, store.logUsed());  // a missing glyph has no record
  EXPECT_EQ(1u, store.usedSlots());
}

TEST_F(GlyphStoreGlyphs, EverythingSurvivesAReopen) {
  for (uint32_t cp = 0x21; cp < 0x21 + 50; ++cp) {
    ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), widthOf(cp), heightOf(cp)));
    if (cp % 10 == 0) {
      ASSERT_EQ(Commit::Stored, store.commit(batch));
    }
  }
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (uint32_t cp = 0x3000; cp < 0x3005; ++cp) ASSERT_EQ(Commit::Stored, store.markMissing(makeKey(cp, 0, 0)));
  const uint32_t logUsed = store.logUsed();
  const uint32_t slots = store.usedSlots();
  store.close();
  FlashGlyphStore reopened(flash);
  ASSERT_EQ(OpenResult::Ready, reopened.open(kIdentityA));
  EXPECT_EQ(logUsed, reopened.logUsed());
  EXPECT_EQ(slots, reopened.usedSlots());
  for (uint32_t cp = 0x21; cp < 0x21 + 50; ++cp) {
    EXPECT_TRUE(storedIntact(reopened, makeKey(cp, 0, 0), widthOf(cp), heightOf(cp))) << cp;
  }
  for (uint32_t cp = 0x3000; cp < 0x3005; ++cp) {
    EXPECT_EQ(Lookup::Missing, reopened.find(makeKey(cp, 0, 0), nullptr)) << cp;
  }
}

TEST_F(GlyphStoreGlyphs, CollidingKeysProbeOn) {
  const std::vector<uint32_t> keys = collidingKeys(layout.geometry.indexSlots, 5);
  ASSERT_EQ(5u, keys.size());
  for (const uint32_t key : keys) ASSERT_TRUE(stage(batch, key, 7, 9));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  for (const uint32_t key : keys) EXPECT_TRUE(storedIntact(store, key, 7, 9));
}

TEST_F(GlyphStoreGlyphs, ALongProbeChainCountsAsFull) {
  const std::vector<uint32_t> keys = collidingKeys(layout.geometry.indexSlots, kMaxProbe + 1);
  ASSERT_EQ(kMaxProbe + 1, keys.size());
  for (uint32_t i = 0; i < kMaxProbe; ++i) ASSERT_EQ(Commit::Stored, store.markMissing(keys[i])) << i;
  EXPECT_EQ(Commit::Full, store.markMissing(keys[kMaxProbe]));
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, SeventyPercentOfTheIndexCountsAsFull) {
  reinstall(smallLayout(1000));
  uint32_t stored = 0;
  for (uint32_t cp = 0x4E00; cp < 0x4E00 + 1000; ++cp) {
    const Commit result = store.markMissing(makeKey(cp, 0, 0));
    if (result != Commit::Stored) {
      EXPECT_EQ(Commit::Full, result);
      break;
    }
    ++stored;
  }
  EXPECT_EQ(700u, stored);
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, ANearlyFullLogCountsAsFull) {
  // A 64 KB log, filled with 2,020-byte records (100 x 80 bitmaps), one per batch.
  uint32_t stored = 0;
  for (uint32_t cp = 0x41;; ++cp) {
    ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 100, 80));
    const Commit result = store.commit(batch);
    if (result != Commit::Stored) {
      EXPECT_EQ(Commit::Full, result);
      break;
    }
    ++stored;
  }
  const uint64_t limit = uint64_t{layout.geometry.logSize} * kLogFullPercent;
  EXPECT_LE(uint64_t{store.logUsed()} * 100, limit);
  EXPECT_GT((uint64_t{store.logUsed()} + recordSize(100, 80)) * 100, limit);
  EXPECT_EQ(store.logUsed() / recordSize(100, 80), stored);
  EXPECT_TRUE(store.wipePending());
}

TEST_F(GlyphStoreGlyphs, OversizedOrInconsistentBitmapsAreRefusedAtStaging) {
  EXPECT_EQ(nullptr, batch.add(makeKey('W', 0, 0), metrics(255, 255)));  // 16,257 B, over the 8 KB cap
  EpdGlyph wrong = metrics(10, 10);
  wrong.dataLength = 7;  // not the size of a 10 x 10 bitmap: the scan would take it for a torn record
  EXPECT_EQ(nullptr, batch.add(makeKey('X', 0, 0), wrong));
  EXPECT_NE(nullptr, batch.add(makeKey('V', 0, 0), metrics(181, 181)));  // 8,191 B
  EXPECT_EQ(1, batch.count());
}

TEST_F(GlyphStoreGlyphs, ABatchThatDoesNotFitTheBufferIsRefused) {
  uint8_t small[64];
  GlyphBatch tiny(small, sizeof small);
  EXPECT_EQ(nullptr, tiny.add(makeKey('M', 0, 0), metrics(30, 30)));  // 248 B with its header
  EXPECT_NE(nullptr, tiny.add(makeKey('i', 0, 0), metrics(4, 12)));   // 32 B
  EXPECT_EQ(1, tiny.count());
}

TEST_F(GlyphStoreGlyphs, AFailedLogWriteSetsWipePending) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  flash.failNextProgram();
  EXPECT_EQ(Commit::WriteFailed, store.commit(batch));
  EXPECT_TRUE(store.wipePending());
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.wipePending());  // recorded in flash, not only for this session
}

TEST_F(GlyphStoreGlyphs, StoredGlyphsStayReadableWhileWipeIsPending) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 9, 12));
  flash.failNextProgram();
  ASSERT_EQ(Commit::WriteFailed, store.commit(batch));
  ASSERT_TRUE(stage(batch, makeKey('C', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Full, store.commit(batch));  // nothing new is stored...
  EXPECT_EQ(Commit::Full, store.markMissing(makeKey('D', 0, 0)));
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, 0), 9, 12));  // ...but what is stored is still served
  EXPECT_EQ(Lookup::Absent, store.find(makeKey('B', 0, 0), nullptr));
}

TEST_F(GlyphStoreGlyphs, ReinstallingErasesOnlySectorsThatHoldData) {
  for (uint32_t cp = 'a'; cp < 'a' + 3; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  store.close();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(install(store, layout, kIdentityB));
  // The header, the style, the log's first sector, and at most three index sectors (three slots).
  EXPECT_LE(flash.totalSectorErases() - before, 6u);
  const uint32_t logSector = layout.geometry.logOffset / kSectorBytes;
  EXPECT_EQ(1u, flash.sectorErases(logSector));
  EXPECT_EQ(0u, flash.sectorErases(logSector + 1));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityB));
  EXPECT_EQ(Lookup::Absent, store.find(makeKey('a', 0, 0), nullptr));
}
```

- [ ] **Step 2: Run them to verify they fail**

```bash
cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest
```

Expected: link errors, undefined references to `GlyphBatch::add`, `FlashGlyphStore::commit`,
`FlashGlyphStore::find` and `FlashGlyphStore::markMissing`.

- [ ] **Step 3: Implement them**

Append to `lib/EpdFont/ttf/FlashGlyphStore.cpp`:

```cpp
// --- Glyphs ---

uint8_t* GlyphBatch::add(const uint32_t key, const EpdGlyph& metrics) {
  // A record whose length does not match its size would read as torn to the log scan.
  if (metrics.dataLength > kMaxBitmapBytes ||
      metrics.dataLength != glyphDataBytes(metrics.width, metrics.height, true)) {
    return nullptr;
  }
  const uint32_t bytes = recordBytes(metrics.dataLength);
  if (bytes > capacity_ - used_) return nullptr;
  uint8_t* record = buffer_ + used_;
  std::memset(record, 0xFF, bytes);  // the padding stays erased
  std::memcpy(record, &key, sizeof key);
  EpdGlyph glyph = metrics;
  glyph.dataOffset = 0;  // known at commit, when the record's place in the log is
  std::memcpy(record + 4, &glyph, sizeof glyph);
  used_ += bytes;
  ++count_;
  return record + kRecordHeaderBytes;
}

// A record a slot may point at: inside the written log, with the slot's key, and well formed.
const EpdGlyph* FlashGlyphStore::validRecord(const uint32_t key, const uint32_t logOffset) const {
  if (logOffset >= logUsed_ || logUsed_ - logOffset < kRecordHeaderBytes) return nullptr;
  if (loadU32(log_ + logOffset) != key || !plausibleRecord(logOffset)) return nullptr;
  const auto* glyph = reinterpret_cast<const EpdGlyph*>(log_ + logOffset + 4);
  if (recordBytes(glyph->dataLength) > logUsed_ - logOffset) return nullptr;
  return glyph;
}

FlashGlyphStore::Lookup FlashGlyphStore::find(const uint32_t key, const EpdGlyph** glyph) const {
  if (!open_ || !isGlyphKey(key)) return Lookup::Absent;
  const uint32_t slots = geometry_.indexSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = (i + 1 == slots) ? 0 : i + 1) {
    const Slot slot = loadSlot(index_, i);
    if (slotErased(slot)) return Lookup::Absent;
    if (slot.key != key) continue;
    if (slot.value == kMissing) return Lookup::Missing;
    if (const EpdGlyph* found = validRecord(key, slot.value)) {
      if (glyph) *glyph = found;
      return Lookup::Found;
    }
    // A slot cut short, or one whose record never completed: keep probing.
  }
  return Lookup::Absent;
}

FlashGlyphStore::Insert FlashGlyphStore::insert(const uint32_t tableOffset, const uint8_t* table,
                                                const uint32_t slots, const uint32_t key, const uint32_t value) {
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe && probe < slots; ++probe, i = (i + 1 == slots) ? 0 : i + 1) {
    // A slot in use, or one cut short (some bits programmed), is never programmed again.
    if (!slotErased(loadSlot(table, i))) continue;
    const Slot slot{key, value};
    return flash_.program(tableOffset + i * kSlotBytes, &slot, sizeof slot) ? Insert::Done : Insert::WriteFailed;
  }
  return Insert::ChainFull;
}

FlashGlyphStore::Commit FlashGlyphStore::commit(GlyphBatch& batch) {
  if (batch.count_ == 0) return Commit::Stored;
  if (!open_) {
    batch.clear();
    return Commit::WriteFailed;
  }
  if (wipePending_) {
    batch.clear();
    return Commit::Full;
  }
  // 4.6: full when the batch would take the log past 95 % or the index past 70 %.
  if ((uint64_t{logUsed_} + batch.used_) * 100 > uint64_t{geometry_.logSize} * kLogFullPercent ||
      (uint64_t{usedSlots_} + batch.count_) * 100 > uint64_t{geometry_.indexSlots} * kIndexLoadPercent) {
    LOG_INF("GST", "full: log %u of %u B, %u of %u slots; wipe pending", static_cast<unsigned>(logUsed_),
            static_cast<unsigned>(geometry_.logSize), static_cast<unsigned>(usedSlots_),
            static_cast<unsigned>(geometry_.indexSlots));
    setWipePending();
    batch.clear();
    return Commit::Full;
  }
  // Each bitmap's offset is relative to the log base, known now.
  const uint32_t base = logUsed_;
  for (uint32_t at = 0; at < batch.used_;) {
    EpdGlyph glyph = loadGlyph(batch.buffer_ + at + 4);
    glyph.dataOffset = base + at + kRecordHeaderBytes;
    std::memcpy(batch.buffer_ + at + 4, &glyph, sizeof glyph);
    at += recordBytes(glyph.dataLength);
  }
  // Every record in one program call...
  if (!flash_.program(geometry_.logOffset + base, batch.buffer_, batch.used_)) {
    LOG_ERR("GST", "log write of %u B at %u failed; wipe pending", static_cast<unsigned>(batch.used_),
            static_cast<unsigned>(base));
    setWipePending();
    batch.clear();
    return Commit::WriteFailed;
  }
  logUsed_ = base + batch.used_;
  // ...then the slots, so a probe never finds a slot whose record is incomplete.
  Commit result = Commit::Stored;
  for (uint32_t at = 0; at < batch.used_;) {
    const Insert inserted =
        insert(geometry_.indexOffset, index_, geometry_.indexSlots, loadU32(batch.buffer_ + at), base + at);
    if (inserted != Insert::Done) {
      if (inserted == Insert::ChainFull) {
        LOG_INF("GST", "full: a probe chain passed %u slots; wipe pending", static_cast<unsigned>(kMaxProbe));
      } else {
        LOG_ERR("GST", "slot write failed; wipe pending");
      }
      result = inserted == Insert::ChainFull ? Commit::Full : Commit::WriteFailed;
      setWipePending();
      break;
    }
    ++usedSlots_;
    at += recordBytes(loadGlyph(batch.buffer_ + at + 4).dataLength);
  }
  batch.clear();
  return result;
}

FlashGlyphStore::Commit FlashGlyphStore::markMissing(const uint32_t key) {
  if (!open_ || !isGlyphKey(key)) return Commit::WriteFailed;
  if (wipePending_) return Commit::Full;
  if ((uint64_t{usedSlots_} + 1) * 100 > uint64_t{geometry_.indexSlots} * kIndexLoadPercent) {
    setWipePending();
    return Commit::Full;
  }
  switch (insert(geometry_.indexOffset, index_, geometry_.indexSlots, key, kMissing)) {
    case Insert::Done:
      ++usedSlots_;
      return Commit::Stored;
    case Insert::ChainFull:
      setWipePending();
      return Commit::Full;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "slot write failed; wipe pending");
  setWipePending();
  return Commit::WriteFailed;
}

// The wipe word goes from erased to zero once; if even that write fails, wipe stays pending for the session.
void FlashGlyphStore::setWipePending() {
  if (wipePending_) return;
  wipePending_ = true;
  const uint32_t zero = 0;
  if (!flash_.program(kWipeWordOffset, &zero, sizeof zero)) {
    LOG_ERR("GST", "could not record wipe pending in flash");
  }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest
```

Expected: `[  PASSED  ] 57 tests.` (41 earlier, 16 new)

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/FlashGlyphStore.cpp test/glyph_store
git commit -m "ttf: glyph store commits batches in one write; lookups, missing glyphs, full store"
```

---

### Task 5: Sizes, kerning pairs and the wipe

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.cpp`: add `sizeIndex`, `findPair`, `storePair` and `wipe`
- Create: `test/glyph_store/GlyphStoreSizesPairsWipeTest.cpp`
- Modify: `test/glyph_store/CMakeLists.txt`: add `GlyphStoreSizesPairsWipeTest.cpp` after
  `GlyphStoreGlyphTest.cpp`

**Interfaces:**
- Consumes: Tasks 3 and 4, including `setWipePending()`, `insert()` and `eraseWritten()`.
- Produces: working `sizeIndex`, `findPair`, `storePair` and `wipe`.

- [ ] **Step 1: Write the failing tests**

Create `test/glyph_store/GlyphStoreSizesPairsWipeTest.cpp`:

```cpp
// The size table, kerning pairs and the wipe (spec 4.2, 4.3, 4.6, 4.7, 7.5).
#include <gtest/gtest.h>

#include <cstdint>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

class GlyphStoreSizes : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, smallLayout())); }
};

class GlyphStorePairs : public GlyphStoreSizes {};

class GlyphStoreWipe : public GlyphStoreSizes {
 protected:
  // Claims all 16 sizes and one more: the store is then full, with wipe pending.
  void fillSizeTable() {
    for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_EQ(static_cast<int>(i), store.sizeIndex((20 + i) << 6, 0));
    ASSERT_EQ(-1, store.sizeIndex(99 << 6, 0));
    ASSERT_TRUE(store.wipePending());
  }

  alignas(4) uint8_t buffer[4096] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

}  // namespace

TEST_F(GlyphStoreSizes, TheFirstSizeGetsIndexZeroAndKeepsIt) {
  EXPECT_EQ(0, store.sizeIndex(29 << 6, 0));
  EXPECT_EQ(1, store.sizeIndex(54 << 6, 1));
  EXPECT_EQ(0, store.sizeIndex(29 << 6, 0));
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, store.sizeIndex(54 << 6, 1));
  EXPECT_EQ(2, store.sizeIndex(33 << 6, 0));
}

TEST_F(GlyphStoreSizes, TheSeventeenthSizeFillsTheStore) {
  for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_EQ(static_cast<int>(i), store.sizeIndex((20 + i) << 6, 0));
  EXPECT_EQ(-1, store.sizeIndex(99 << 6, 0));
  EXPECT_TRUE(store.wipePending());
  EXPECT_EQ(3, store.sizeIndex(23 << 6, 0));  // known sizes are still answered
}

TEST_F(GlyphStoreSizes, ASizeEntryCutShortIsNeverReused) {
  flash.cutAfterSteps(5);  // the ppem and the flags byte land; the pad does not
  EXPECT_EQ(-1, store.sizeIndex(29 << 6, 0));
  flash.powerCycle();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(1, store.sizeIndex(29 << 6, 0));  // entry 0 is spent, not programmed again
}

TEST_F(GlyphStorePairs, PairsAreStoredPerStyle) {
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  ASSERT_TRUE(store.storePair('A', 'V', 1, -60));
  int16_t kern = 0;
  ASSERT_TRUE(store.findPair('A', 'V', 0, &kern));
  EXPECT_EQ(-74, kern);
  ASSERT_TRUE(store.findPair('A', 'V', 1, &kern));
  EXPECT_EQ(-60, kern);
  EXPECT_FALSE(store.findPair('A', 'V', 2, &kern));
  EXPECT_FALSE(store.findPair('V', 'A', 0, &kern));
  EXPECT_EQ(2u, store.usedPairSlots());
}

TEST_F(GlyphStorePairs, ZeroIsAStoredAnswer) {
  // "No kerning" is worth storing as well: the GPOS reader is not asked again.
  ASSERT_TRUE(store.storePair('o', 'o', 0, 0));
  int16_t kern = 99;
  ASSERT_TRUE(store.findPair('o', 'o', 0, &kern));
  EXPECT_EQ(0, kern);
}

TEST_F(GlyphStorePairs, PairsSurviveAReopen) {
  for (uint16_t right = 'a'; right <= 'z'; ++right) {
    ASSERT_TRUE(store.storePair('T', right, 0, static_cast<int16_t>('a' - right)));
  }
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(26u, store.usedPairSlots());
  for (uint16_t right = 'a'; right <= 'z'; ++right) {
    int16_t kern = 1;
    ASSERT_TRUE(store.findPair('T', right, 0, &kern));
    EXPECT_EQ('a' - right, kern);
  }
}

TEST_F(GlyphStorePairs, ImpossiblePairsAreRefused) {
  EXPECT_FALSE(store.storePair(0xFFFF, 0xFFFF, 0, 1));  // would read as an erased slot
  EXPECT_FALSE(store.storePair('A', 'V', 4, 1));        // styles are 0-3
  EXPECT_EQ(0u, store.usedPairSlots());
}

TEST(GlyphStorePairsFull, AFullPairIndexStopsStoringButNeverWipes) {
  SimNorFlash flash(kFlashBytes);
  FlashGlyphStore store(flash);
  ASSERT_TRUE(installAndOpen(store, smallLayout(1024, 100)));
  uint32_t stored = 0;
  for (uint16_t right = 0x100; right < 0x200 && store.storePair(0x41, right, 0, -1); ++right) ++stored;
  EXPECT_EQ(70u, stored);
  EXPECT_FALSE(store.wipePending());
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));  // glyphs are not affected
  EXPECT_EQ(0u, flash.violations());
}

TEST_F(GlyphStoreWipe, WipeEmptiesTheStoreAndKeepsTheFamily) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  ASSERT_EQ(Commit::Stored, store.markMissing(makeKey(0x2603, 0, 0)));
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  fillSizeTable();
  ASSERT_TRUE(store.wipe());
  for (int pass = 0; pass < 2; ++pass) {  // as wiped, then again after a reopen
    EXPECT_FALSE(store.wipePending()) << pass;
    EXPECT_EQ(Lookup::Absent, store.find(makeKey('A', 0, 0), nullptr)) << pass;
    EXPECT_EQ(Lookup::Absent, store.find(makeKey(0x2603, 0, 0), nullptr)) << pass;
    EXPECT_FALSE(store.findPair('A', 'V', 0, nullptr)) << pass;
    EXPECT_EQ(0u, store.logUsed()) << pass;
    EXPECT_EQ(0u, store.usedSlots()) << pass;
    EXPECT_EQ(0u, store.usedPairSlots()) << pass;
    const uint8_t* style = store.mapStyle(0);  // the font itself is kept
    ASSERT_NE(nullptr, style);
    EXPECT_EQ(styleByte(0, 7), style[7]);
    store.close();
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }
  EXPECT_EQ(0, store.sizeIndex(54 << 6, 0));  // the size table starts over
  ASSERT_TRUE(stage(batch, makeKey('B', 0, 0), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));
}

TEST_F(GlyphStoreWipe, WipeErasesOnlySectorsThatHoldData) {
  for (uint32_t cp = 'a'; cp < 'a' + 3; ++cp) ASSERT_TRUE(stage(batch, makeKey(cp, 0, 0), 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  fillSizeTable();
  const Geometry g = store.geometry();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(store.wipe());
  // The header, the log's first sector and at most three index sectors; no pair sector (no pair stored).
  EXPECT_LE(flash.totalSectorErases() - before, 5u);
  EXPECT_EQ(1u, flash.sectorErases(g.logOffset / kSectorBytes));
  EXPECT_EQ(0u, flash.sectorErases(g.logOffset / kSectorBytes + 1));
  EXPECT_EQ(0u, flash.sectorErases(g.pairOffset / kSectorBytes));
}

TEST_F(GlyphStoreWipe, WipingAnEmptyStoreErasesOnlyTheHeader) {
  fillSizeTable();
  const uint32_t before = flash.totalSectorErases();
  ASSERT_TRUE(store.wipe());
  EXPECT_EQ(1u, flash.totalSectorErases() - before);
  EXPECT_EQ(1u, flash.sectorErases(0));
}
```

- [ ] **Step 2: Run them to verify they fail**

```bash
cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest
```

Expected: link errors, undefined references to `FlashGlyphStore::sizeIndex`, `findPair`, `storePair` and
`wipe`.

- [ ] **Step 3: Implement them**

Append to `lib/EpdFont/ttf/FlashGlyphStore.cpp`:

```cpp
// --- Sizes ---

int FlashGlyphStore::sizeIndex(const uint32_t ppem26_6, const uint8_t flags) {
  if (!open_ || ppem26_6 == kErased) return -1;
  SizeEntry entries[kSizeEntries];  // 128 B
  if (!flash_.read(kSizeTableOffset, entries, sizeof entries)) return -1;
  for (int i = 0; i < kSizeEntries; ++i) {
    const SizeEntry& e = entries[i];
    if (e.ppem26_6 == ppem26_6 && e.pad[0] == 0 && e.pad[1] == 0 && e.pad[2] == 0) return i;
  }
  if (wipePending_) return -1;
  for (int i = 0; i < kSizeEntries; ++i) {
    const SizeEntry& e = entries[i];
    // Free only if every byte is erased: an entry cut short is never programmed again.
    if (e.ppem26_6 != kErased || e.flags != 0xFF || e.pad[0] != 0xFF || e.pad[1] != 0xFF || e.pad[2] != 0xFF) {
      continue;
    }
    const SizeEntry claim{ppem26_6, flags, {0, 0, 0}};  // the zero pad lands last and marks it in use
    if (!flash_.program(kSizeTableOffset + static_cast<uint32_t>(i) * sizeof(SizeEntry), &claim, sizeof claim)) {
      LOG_ERR("GST", "size table write failed; wipe pending");
      setWipePending();
      return -1;
    }
    return i;
  }
  LOG_INF("GST", "full: all %u sizes in use; wipe pending", static_cast<unsigned>(kSizeEntries));
  setWipePending();
  return -1;
}

// --- Kerning pairs ---

bool FlashGlyphStore::findPair(const uint16_t left, const uint16_t right, const uint8_t style,
                               int16_t* fontUnits) const {
  const uint32_t key = pairKey(left, right);
  if (!open_ || key == kErased) return false;
  const uint32_t slots = geometry_.pairSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = (i + 1 == slots) ? 0 : i + 1) {
    const Slot slot = loadSlot(pairs_, i);
    if (slotErased(slot)) return false;
    // The value's top bits are written as zero: a value cut short reads as some other style.
    if (slot.key != key || (slot.value >> 16) != style) continue;
    if (fontUnits) *fontUnits = static_cast<int16_t>(slot.value & 0xFFFF);
    return true;
  }
  return false;
}

bool FlashGlyphStore::storePair(const uint16_t left, const uint16_t right, const uint8_t style,
                                const int16_t fontUnits) {
  const uint32_t key = pairKey(left, right);
  if (!open_ || wipePending_ || key == kErased || style > 3) return false;
  // A full pair index only stops storing pairs; they come from the GPOS reader instead (never a wipe).
  if ((uint64_t{usedPairs_} + 1) * 100 > uint64_t{geometry_.pairSlots} * kIndexLoadPercent) return false;
  const uint32_t value = (static_cast<uint32_t>(style) << 16) | static_cast<uint16_t>(fontUnits);
  switch (insert(geometry_.pairOffset, pairs_, geometry_.pairSlots, key, value)) {
    case Insert::Done:
      ++usedPairs_;
      return true;
    case Insert::ChainFull:
      return false;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "pair write failed; wipe pending");
  setWipePending();
  return false;
}

// --- Wipe ---

bool FlashGlyphStore::wipe() {
  if (!open_) return false;
  const Geometry g = geometry_;
  // Everything a lookup can reach goes first. The header, which holds the size table and the wipe word, is
  // rewritten last: a cut before that leaves wipe pending, and the next wipe() finishes the job.
  if (!eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) || !eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) ||
      !eraseWritten(g.logOffset, g.logSize)) {
    LOG_ERR("GST", "wipe: erase failed");
    return false;
  }
  logUsed_ = usedSlots_ = usedPairs_ = 0;
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "wipe: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return false;
  }
  if (!flash_.read(0, header.get(), sizeof(Header)) || !flash_.erase(0, kSectorBytes) ||
      !flash_.program(0, header.get(), sizeof(Header))) {
    LOG_ERR("GST", "wipe: header rewrite failed; the family will be installed again");
    close();
    return false;
  }
  wipePending_ = false;
  return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest
```

Expected: `[  PASSED  ] 68 tests.` (57 earlier, 11 new)

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/FlashGlyphStore.cpp test/glyph_store
git commit -m "ttf: glyph store size table, kerning pairs and wipe (erasing only written sectors)"
```

---
### Task 6: Power cuts at every write step

**Files:**
- Create: `test/glyph_store/GlyphStorePowerCutTest.cpp`
- Modify: `test/glyph_store/CMakeLists.txt`: add `GlyphStorePowerCutTest.cpp` after
  `GlyphStoreSizesPairsWipeTest.cpp`

**Interfaces:**
- Consumes: the whole store (Tasks 3-5) and `gst_test` helpers.
- Produces: no new code. If a cut breaks a rule, that is a store bug:
  - find it with superpowers:systematic-debugging;
  - fix it in `FlashGlyphStore.cpp`;
  - add a focused regression test to the test file of the task that owns the code.

This task tests behaviour that Tasks 3-5 built, so a sweep that passes at once is expected. Step 3 proves
the sweep can fail, by breaking the store on purpose.

- [ ] **Step 1: Write the sweeps**

Create `test/glyph_store/GlyphStorePowerCutTest.cpp`:

```cpp
// A power cut at every step of every write (spec 4.5, 9.3). Whatever the store reported as stored is intact
// after the reboot, nothing is served torn, and writing carries on without programming any byte twice.
#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <string>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

struct GlyphSpec {
  uint32_t key;
  uint8_t width;
  uint8_t height;
};

const GlyphSpec kBatchA[] = {{makeKey('A', 0, 0), 9, 12}, {makeKey('B', 0, 0), 10, 12}, {makeKey(' ', 0, 0), 0, 0}};
const GlyphSpec kBatchB[] = {{makeKey('C', 0, 0), 11, 13}, {makeKey('d', 1, 0), 8, 12}, {makeKey(0x4E2D, 0, 1), 24, 26}};
const GlyphSpec kAfter[] = {{makeKey('E', 0, 0), 9, 13}, {makeKey('F', 0, 0), 9, 12}};
constexpr uint32_t kMissingKey = makeKey(0x2603, 0, 0);
constexpr uint32_t kPpem = 29 << 6;

// What each step of a session reported.
struct Outcome {
  bool installed = false;
  int sizeIndex = -1;
  bool batchA = false;
  bool missing = false;
  bool batchB = false;
  bool pair = false;
};

Layout sweepLayout() {
  Layout layout = smallLayout();
  layout.styleSize[0] = 256;  // a short style keeps the sweep fast
  return layout;
}

bool commitAll(FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count) {
  alignas(4) uint8_t buffer[2048];
  GlyphBatch batch(buffer, sizeof buffer);
  for (size_t i = 0; i < count; ++i) {
    if (!stage(batch, glyphs[i].key, glyphs[i].width, glyphs[i].height)) return false;
  }
  return store.commit(batch) == Commit::Stored;
}

// Every kind of write a session makes, from the install on.
Outcome runSession(SimNorFlash& flash) {
  Outcome out;
  FlashGlyphStore store(flash);
  out.installed = install(store, sweepLayout());
  if (!out.installed || store.open(kIdentityA) != OpenResult::Ready) return out;
  out.sizeIndex = store.sizeIndex(kPpem, 0);
  out.batchA = commitAll(store, kBatchA, std::size(kBatchA));
  out.missing = store.markMissing(kMissingKey) == Commit::Stored;
  out.batchB = commitAll(store, kBatchB, std::size(kBatchB));
  out.pair = store.storePair('A', 'V', 0, -74);
  return out;
}

void checkGlyphs(const FlashGlyphStore& store, const GlyphSpec* glyphs, const size_t count, const bool reported,
                 const std::string& where) {
  for (size_t i = 0; i < count; ++i) {
    const GlyphSpec& g = glyphs[i];
    if (reported) {
      EXPECT_TRUE(storedIntact(store, g.key, g.width, g.height)) << where << " key " << g.key;
    } else {
      EXPECT_TRUE(absentOrIntact(store, g.key, g.width, g.height)) << where << " key " << g.key;
    }
  }
}

void checkAfterReboot(SimNorFlash& flash, const Outcome& out, const std::string& where) {
  FlashGlyphStore store(flash);
  const OpenResult opened = store.open(kIdentityA);
  if (out.installed) {
    ASSERT_EQ(OpenResult::Ready, opened) << where;
  } else if (opened != OpenResult::Ready) {
    EXPECT_EQ(OpenResult::NotInstalled, opened) << where;
    return;  // the next install starts from scratch
  }
  checkGlyphs(store, kBatchA, std::size(kBatchA), out.batchA, where);
  checkGlyphs(store, kBatchB, std::size(kBatchB), out.batchB, where);
  const Lookup missing = store.find(kMissingKey, nullptr);
  if (out.missing) {
    EXPECT_EQ(Lookup::Missing, missing) << where;
  } else {
    EXPECT_NE(Lookup::Found, missing) << where;
  }
  int16_t kern = 0;
  if (store.findPair('A', 'V', 0, &kern)) {
    EXPECT_EQ(-74, kern) << where;
  } else {
    EXPECT_FALSE(out.pair) << where;
  }
  if (out.sizeIndex >= 0) {
    EXPECT_EQ(out.sizeIndex, store.sizeIndex(kPpem, 0)) << where;
  }
  // The store carries on: a new batch lands, and everything is still there after another reboot.
  ASSERT_FALSE(store.wipePending()) << where;
  ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter))) << where;
  store.close();
  FlashGlyphStore again(flash);
  ASSERT_EQ(OpenResult::Ready, again.open(kIdentityA)) << where;
  checkGlyphs(again, kAfter, std::size(kAfter), true, where);
  EXPECT_EQ(0u, flash.violations()) << where;
}

// A store with data and wipe pending, as the full path leaves it.
void populate(SimNorFlash& flash) {
  FlashGlyphStore store(flash);
  ASSERT_TRUE(install(store, sweepLayout()));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_TRUE(commitAll(store, kBatchA, std::size(kBatchA)));
  ASSERT_TRUE(store.storePair('A', 'V', 0, -74));
  for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_GE(store.sizeIndex((20 + i) << 6, 0), 0);
  ASSERT_EQ(-1, store.sizeIndex(99 << 6, 0));
  ASSERT_TRUE(store.wipePending());
}

}  // namespace

TEST(GlyphStorePowerCut, EveryCutDuringASessionLeavesAConsistentStore) {
  SimNorFlash dry(kFlashBytes);
  const Outcome all = runSession(dry);
  ASSERT_TRUE(all.installed && all.batchA && all.missing && all.batchB && all.pair && all.sizeIndex == 0);
  const uint64_t steps = dry.steps();
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < steps; ++cut) {
      SimNorFlash flash(kFlashBytes);
      flash.partialCut(partial);
      flash.cutAfterSteps(static_cast<int64_t>(cut));
      const Outcome out = runSession(flash);
      const std::string where = "cut " + std::to_string(cut) + (partial ? " partial" : "");
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      checkAfterReboot(flash, out, where);
      if (HasFailure()) return;  // one cut's report is enough to debug from
    }
  }
}

TEST(GlyphStorePowerCut, EveryCutDuringAWipeIsRecoverable) {
  SimNorFlash dry(kFlashBytes);
  populate(dry);
  if (HasFatalFailure()) return;
  const uint64_t before = dry.steps();
  {
    FlashGlyphStore store(dry);
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
    ASSERT_TRUE(store.wipe());
  }
  const uint64_t wipeSteps = dry.steps() - before;
  ASSERT_GT(wipeSteps, 0u);
  for (const bool partial : {false, true}) {
    for (uint64_t cut = 0; cut < wipeSteps; ++cut) {
      const std::string where = "wipe cut " + std::to_string(cut) + (partial ? " partial" : "");
      SimNorFlash flash(kFlashBytes);
      populate(flash);
      if (HasFatalFailure()) return;
      {
        FlashGlyphStore store(flash);
        ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA)) << where;
        flash.partialCut(partial);
        flash.cutAfterSteps(static_cast<int64_t>(cut));
        EXPECT_FALSE(store.wipe()) << where;
      }
      ASSERT_TRUE(flash.dead()) << where;
      flash.powerCycle();
      FlashGlyphStore store(flash);
      const OpenResult opened = store.open(kIdentityA);
      if (opened == OpenResult::NotInstalled) {
        // The cut hit the header rewrite: the family is installed again from scratch.
        ASSERT_TRUE(installAndOpen(store, sweepLayout())) << where;
      } else {
        ASSERT_EQ(OpenResult::Ready, opened) << where;
        checkGlyphs(store, kBatchA, std::size(kBatchA), false, where);
        if (store.wipePending()) {
          ASSERT_TRUE(store.wipe()) << where;
        }
      }
      // Either way the store is now empty and usable.
      for (const GlyphSpec& g : kBatchA) EXPECT_EQ(Lookup::Absent, store.find(g.key, nullptr)) << where;
      EXPECT_FALSE(store.findPair('A', 'V', 0, nullptr)) << where;
      EXPECT_EQ(0, store.sizeIndex(kPpem, 0)) << where;
      ASSERT_TRUE(commitAll(store, kAfter, std::size(kAfter))) << where;
      EXPECT_EQ(0u, flash.violations()) << where;
      if (HasFailure()) return;
    }
  }
}
```

- [ ] **Step 2: Run the sweeps**

```bash
cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest --gtest_filter='GlyphStorePowerCut.*'
```

Expected: `[  PASSED  ] 2 tests.` A few thousand reboots in all; it should take seconds, not minutes.

- [ ] **Step 3: Prove the sweep bites**

Break recovery on purpose: in `FlashGlyphStore::scanLog()` add `return offset;` as the first statement after
the `while` loop, so the check of the bytes after the last record is skipped. Then run:

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest --gtest_filter='GlyphStorePowerCut.*'
```

Expected: FAIL in `EveryCutDuringASessionLeavesAConsistentStore`. A cut in a record's header makes the next
batch program over the torn bytes, so `violations()` is not 0. The recovery tests from Task 3 fail too.

Remove the `return offset;` line again, rebuild, and confirm `[  PASSED  ]` for the whole executable:

```bash
cmake --build build/test --target GlyphStoreTest && build/test/glyph_store/GlyphStoreTest
```

Expected: `[  PASSED  ] 70 tests.` Write the failing output of the broken run into your report.

- [ ] **Step 4: Commit**

```bash
git add test/glyph_store
git commit -m "test: glyph store power-cut sweeps over every write step of a session and of a wipe"
```

---

### Task 7: `EspNorFlash`, the device backend

**Files:**
- Create: `lib/hal/EspNorFlash.h`, `lib/hal/EspNorFlash.cpp`

**Interfaces:**
- Consumes: `NorFlash` (Task 1); `FlashFontPartition::fontUsableSize()` and `FlashFontPartition::isMapped()`
  (`lib/hal/FlashFontPartition.h`).
- Produces: `class EspNorFlash final : public NorFlash` with `bool begin()`. Task 8's bench uses it, and
  Stage 2c will hand it to `FlashGlyphStore`.

No host test can reach ESP-IDF. The checks are the firmware builds plus Task 8's run on the device.

- [ ] **Step 1: Create `lib/hal/EspNorFlash.h`**

```cpp
#pragma once

#include <esp_partition.h>

#include "NorFlash.h"

/// NorFlash over the font area of the `spiffs` partition: everything below the language slot
/// (FlashFontPartition::fontUsableSize()). The TTF glyph store reaches flash only through this class, and
/// it is the only code of that store that calls the ESP-IDF partition API.
///
/// The partition is shared with FlashFontPartition (.cpfont families), so program() and erase() are refused
/// while a .cpfont is mapped from it: its reader would see the bytes change underneath. Not thread-safe;
/// callers serialise (RenderLock).
class EspNorFlash final : public NorFlash {
 public:
  /// Finds the partition. False when it is missing or not larger than the language slot.
  bool begin();

  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* dst, uint32_t len) override;
  bool program(uint32_t offset, const void* src, uint32_t len) override;
  bool erase(uint32_t offset, uint32_t len) override;
  const uint8_t* map(uint32_t offset, uint32_t len) override;
  void unmap(const uint8_t* mapped) override;
  uint32_t freeMapPages() const override;

 private:
  static constexpr int kMaxMaps = 4;  // the glyph regions, a style (or the copied tables), and spares

  struct Mapping {
    const uint8_t* ptr = nullptr;
    esp_partition_mmap_handle_t handle = 0;
  };

  bool inRange(const uint32_t offset, const uint32_t len) const {
    return part_ != nullptr && offset <= size_ && len <= size_ - offset;
  }
  bool writable(const char* what, uint32_t offset, uint32_t len) const;

  const esp_partition_t* part_ = nullptr;
  uint32_t size_ = 0;
  Mapping maps_[kMaxMaps];
};
```

- [ ] **Step 2: Create `lib/hal/EspNorFlash.cpp`**

```cpp
#include "EspNorFlash.h"

#include <Logging.h>
#include <spi_flash_mmap.h>

#include "FlashFontPartition.h"

bool EspNorFlash::begin() {
  part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part_) {
    LOG_ERR("ENF", "spiffs partition not found");
    return false;
  }
  size_ = static_cast<uint32_t>(FlashFontPartition::fontUsableSize());
  return size_ > 0;
}

bool EspNorFlash::read(const uint32_t offset, void* dst, const uint32_t len) {
  if (!inRange(offset, len)) return false;
  return esp_partition_read(part_, offset, dst, len) == ESP_OK;
}

bool EspNorFlash::writable(const char* what, const uint32_t offset, const uint32_t len) const {
  if (!inRange(offset, len)) {
    LOG_ERR("ENF", "%s of %u B at %u is outside the font area (%u B)", what, static_cast<unsigned>(len),
            static_cast<unsigned>(offset), static_cast<unsigned>(size_));
    return false;
  }
  if (FlashFontPartition::isMapped()) {
    LOG_ERR("ENF", "%s refused: a .cpfont is mapped from this partition", what);
    return false;
  }
  return true;
}

bool EspNorFlash::program(const uint32_t offset, const void* src, const uint32_t len) {
  if (!writable("program", offset, len)) return false;
  const esp_err_t err = esp_partition_write(part_, offset, src, len);
  if (err != ESP_OK) {
    LOG_ERR("ENF", "program of %u B at %u failed: %d", static_cast<unsigned>(len), static_cast<unsigned>(offset), err);
    return false;
  }
  return true;
}

bool EspNorFlash::erase(const uint32_t offset, const uint32_t len) {
  if (!writable("erase", offset, len)) return false;
  if (offset % kSectorBytes != 0 || len % kSectorBytes != 0) {
    LOG_ERR("ENF", "erase of %u B at %u is not sector-aligned", static_cast<unsigned>(len), static_cast<unsigned>(offset));
    return false;
  }
  const esp_err_t err = esp_partition_erase_range(part_, offset, len);
  if (err != ESP_OK) {
    LOG_ERR("ENF", "erase of %u B at %u failed: %d", static_cast<unsigned>(len), static_cast<unsigned>(offset), err);
    return false;
  }
  return true;
}

const uint8_t* EspNorFlash::map(const uint32_t offset, const uint32_t len) {
  if (len == 0 || !inRange(offset, len)) return nullptr;
  for (Mapping& m : maps_) {
    if (m.ptr != nullptr) continue;
    const void* ptr = nullptr;
    const esp_err_t err = esp_partition_mmap(part_, offset, len, ESP_PARTITION_MMAP_DATA, &ptr, &m.handle);
    if (err != ESP_OK) {
      LOG_ERR("ENF", "map of %u B at %u failed: %d (%u pages free)", static_cast<unsigned>(len),
              static_cast<unsigned>(offset), err, static_cast<unsigned>(freeMapPages()));
      m = Mapping{};
      return nullptr;
    }
    m.ptr = static_cast<const uint8_t*>(ptr);
    return m.ptr;
  }
  LOG_ERR("ENF", "map refused: all %d mappings are in use", kMaxMaps);
  return nullptr;
}

void EspNorFlash::unmap(const uint8_t* mapped) {
  if (mapped == nullptr) return;
  for (Mapping& m : maps_) {
    if (m.ptr == mapped) {
      esp_partition_munmap(m.handle);
      m = Mapping{};
      return;
    }
  }
}

uint32_t EspNorFlash::freeMapPages() const {
  return static_cast<uint32_t>(spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA));
}
```

- [ ] **Step 3: Build both firmware boards**

Run each in the background and wait for it to finish. Run them one after the other, never in parallel:

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /c/tmp/stage2a-default.log 2>&1; grep -a "RAM:\|Flash:\|\[SUCCESS\]\|FAILED" /c/tmp/stage2a-default.log | tail -3
"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro > /c/tmp/stage2a-x4pro.log 2>&1; grep -a "RAM:\|Flash:\|\[SUCCESS\]\|FAILED" /c/tmp/stage2a-x4pro.log | tail -3
find .pio/build/default -name "EspNorFlash.cpp.o" -o -name "FlashGlyphStore.cpp.o" -o -name "GlyphStoreFormat.cpp.o"
```

Expected:
- `[SUCCESS]` for both.
- The `find` lists all three objects: the new code compiles for RISC-V even though nothing links it yet.
- Flash within a few hundred bytes of master. Master `default` at `fe8f2757d` was 6,275,193 B flash and
  54,728 B RAM; if master has moved, compare against a fresh master build.

If a link fails with "file format not recognized" or truncated objects, delete only the zero-byte `.o`
files under that env's `.pio/build/<env>`, then rebuild. Never run `pio run -t clean`.

- [ ] **Step 4: Commit**

```bash
git add lib/hal/EspNorFlash.h lib/hal/EspNorFlash.cpp
git commit -m "hal: EspNorFlash, the glyph store's NorFlash over the spiffs font area"
```

---

### Task 8: Device self-test, format, pull request

**Files:**
- Create: `bench/glyph_store_main.cpp`
- Modify: `platformio.ini` (add `[env:bench_glyph_store]` right after the `[env:bench_font]` block)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Write the device self-test**

Create `bench/glyph_store_main.cpp`:

```cpp
// Device self-test for the TTF glyph store (Stage 2a): FlashGlyphStore through EspNorFlash on the real
// `spiffs` partition. Prints timings and checks, and ends with "=== done ===".
//
// It ERASES the partition's font area, which holds the .cpfont flash cache: the reader copies a .cpfont
// family back from the SD card the next time it needs one. The language slot at the end of the partition
// must come through untouched, which the test checks.
//
// Build & flash: pio run -e bench_glyph_store -t upload
#include <Arduino.h>
#include <EspNorFlash.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <ttf/FlashGlyphStore.h>
#include <ttf/GlyphStoreFormat.h>

using namespace glyphstore;

namespace {

constexpr uint8_t kIdentity[16] = {'g', 'l', 'y', 'p', 'h', '-', 'b', 'e', 'n', 'c', 'h'};
constexpr uint32_t kStyleBytes = 300000;
constexpr uint32_t kBatched = 3000;
constexpr uint32_t kSingles = 200;
constexpr uint32_t kPairs = 500;
alignas(4) uint8_t gBatch[4096];
alignas(4) uint8_t gChunk[4096];

uint32_t langSlotHash() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) return 0;
  uint32_t hash = 2166136261u;
  for (uint32_t at = part->size - 65536; at < part->size; at += sizeof gChunk) {
    if (esp_partition_read(part, at, gChunk, sizeof gChunk) != ESP_OK) return 0;
    for (const uint8_t b : gChunk) {
      hash ^= b;
      hash *= 16777619u;
    }
  }
  return hash;
}

EpdGlyph shapeOf(const uint32_t i) {
  EpdGlyph glyph{};
  glyph.width = static_cast<uint8_t>(6 + i % 19);
  glyph.height = static_cast<uint8_t>(9 + i % 23);
  glyph.advanceX = static_cast<uint16_t>((glyph.width + 1) << 4);
  glyph.left = 1;
  glyph.top = glyph.height;
  glyph.dataLength = glyphDataBytes(glyph.width, glyph.height, true);
  return glyph;
}

uint8_t patternOf(const uint32_t key, const uint32_t j) { return static_cast<uint8_t>((key * 31u + j * 7u) ^ 0x5Au); }

uint32_t keyOf(const uint32_t i, const int body, const int heading) {
  return makeKey(0x21 + i, static_cast<uint8_t>(i % 2), static_cast<uint8_t>(i % 3 == 0 ? heading : body));
}

bool stage(GlyphBatch& batch, const uint32_t i, const int body, const int heading) {
  const uint32_t key = keyOf(i, body, heading);
  const EpdGlyph glyph = shapeOf(i);
  uint8_t* bitmap = batch.add(key, glyph);
  if (!bitmap) return false;
  for (uint32_t j = 0; j < glyph.dataLength; ++j) bitmap[j] = patternOf(key, j);
  return true;
}

uint32_t countBad(const FlashGlyphStore& store, const uint32_t count, const int body, const int heading) {
  uint32_t bad = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t key = keyOf(i, body, heading);
    const EpdGlyph want = shapeOf(i);
    const EpdGlyph* glyph = nullptr;
    if (store.find(key, &glyph) != FlashGlyphStore::Lookup::Found || glyph->width != want.width ||
        glyph->height != want.height || glyph->dataLength != want.dataLength) {
      ++bad;
      continue;
    }
    const uint8_t* bitmap = store.logBase() + glyph->dataOffset;
    for (uint32_t j = 0; j < glyph->dataLength; ++j) {
      if (bitmap[j] != patternOf(key, j)) {
        ++bad;
        break;
      }
    }
  }
  return bad;
}

unsigned msSince(const int64_t start) { return static_cast<unsigned>((esp_timer_get_time() - start) / 1000); }

void finish(const char* fatal) {
  if (fatal) Serial.printf("FATAL: %s\n", fatal);
  Serial.println("=== done ===");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Glyph store device self-test (TTF Stage 2a) ===");
  const uint32_t langBefore = langSlotHash();

  EspNorFlash flash;
  if (!flash.begin()) return finish("no spiffs partition");
  Serial.printf("font area %u B, free map pages %u, free heap %u B\n", static_cast<unsigned>(flash.size()),
                static_cast<unsigned>(flash.freeMapPages()), static_cast<unsigned>(esp_get_free_heap_size()));

  PlanInput in{};
  in.mode = kModeFlash;
  in.styleCount = 2;
  in.styleBytes[0] = in.styleBytes[1] = kStyleBytes;
  in.usableBytes = flash.size();
  in.freeMapPages = flash.freeMapPages();
  Layout layout{};
  if (!planLayout(in, layout)) return finish("planLayout refused the budget");
  const Geometry& g = layout.geometry;
  Serial.printf("layout: index %u slots at %u, log %u KB at %u\n", static_cast<unsigned>(g.indexSlots),
                static_cast<unsigned>(g.indexOffset), static_cast<unsigned>(g.logSize / 1024),
                static_cast<unsigned>(g.logOffset));

  FlashGlyphStore store(flash);
  int64_t t = esp_timer_get_time();
  bool ok = store.beginInstall(layout, kIdentity, nullptr);
  Serial.printf("BENCH install_erase     %u ms (only sectors that hold data)\n", msSince(t));
  t = esp_timer_get_time();
  for (uint8_t s = 0; ok && s < 2; ++s) {
    for (uint32_t at = 0; ok && at < kStyleBytes; at += sizeof gChunk) {
      const uint32_t len = kStyleBytes - at < sizeof gChunk ? kStyleBytes - at : static_cast<uint32_t>(sizeof gChunk);
      for (uint32_t j = 0; j < len; ++j) gChunk[j] = static_cast<uint8_t>(at + j + s);
      ok = store.writeStyleData(s, at, gChunk, len);
    }
  }
  ok = ok && store.finishInstall();
  Serial.printf("BENCH install_copy      %u ms for %u KB\n", msSince(t), static_cast<unsigned>(2 * kStyleBytes / 1024));
  t = esp_timer_get_time();
  if (!ok || store.open(kIdentity) != FlashGlyphStore::OpenResult::Ready) return finish("install or open failed");
  Serial.printf("BENCH open_empty        %u ms\n", msSince(t));

  const int body = store.sizeIndex(29 << 6, 0);
  const int heading = store.sizeIndex(54 << 6, 1);

  // Batched, as prefetch will commit a text run's glyphs.
  GlyphBatch batch(gBatch, sizeof gBatch);
  unsigned batches = 0;
  t = esp_timer_get_time();
  for (uint32_t i = 0; i < kBatched; ++i) {
    if (stage(batch, i, body, heading)) continue;
    if (store.commit(batch) != FlashGlyphStore::Commit::Stored) return finish("batched commit failed");
    ++batches;
    if (!stage(batch, i, body, heading)) return finish("a glyph did not fit an empty batch");
  }
  if (store.commit(batch) != FlashGlyphStore::Commit::Stored) return finish("last batched commit failed");
  ++batches;
  const int64_t batchedUs = esp_timer_get_time() - t;

  // One at a time, as the per-glyph miss path will.
  t = esp_timer_get_time();
  for (uint32_t i = kBatched; i < kBatched + kSingles; ++i) {
    GlyphBatch one(gBatch, sizeof gBatch);
    stage(one, i, body, heading);
    if (store.commit(one) != FlashGlyphStore::Commit::Stored) return finish("single commit failed");
  }
  const int64_t singleUs = esp_timer_get_time() - t;
  Serial.printf("BENCH commit_batched    per_glyph=%lldus  glyphs=%u  batches=%u\n", batchedUs / kBatched,
                static_cast<unsigned>(kBatched), batches);
  Serial.printf("BENCH commit_single     per_glyph=%lldus  glyphs=%u\n", singleUs / kSingles,
                static_cast<unsigned>(kSingles));
  Serial.printf("verify after writes:    %u bad of %u\n",
                static_cast<unsigned>(countBad(store, kBatched + kSingles, body, heading)),
                static_cast<unsigned>(kBatched + kSingles));

  // Hits, read in place through the mapping.
  t = esp_timer_get_time();
  unsigned found = 0;
  for (uint32_t r = 0; r < 10000; ++r) {
    if (store.find(keyOf(r % kBatched, body, heading), nullptr) == FlashGlyphStore::Lookup::Found) ++found;
  }
  Serial.printf("BENCH find_hit          per_lookup=%lldns  found=%u/10000\n", (esp_timer_get_time() - t) / 10, found);

  // Kerning pairs.
  t = esp_timer_get_time();
  unsigned pairsStored = 0;
  for (uint32_t p = 0; p < kPairs; ++p) {
    if (store.storePair(0x41 + p % 26, 0x61 + p / 26, p % 2, static_cast<int16_t>(-static_cast<int>(p % 90)))) {
      ++pairsStored;
    }
  }
  const int64_t pairWriteUs = esp_timer_get_time() - t;
  t = esp_timer_get_time();
  unsigned pairsRight = 0;
  for (uint32_t p = 0; p < kPairs; ++p) {
    int16_t kern = 1;
    if (store.findPair(0x41 + p % 26, 0x61 + p / 26, p % 2, &kern) && kern == -static_cast<int>(p % 90)) {
      ++pairsRight;
    }
  }
  Serial.printf("BENCH pair_store        per_pair=%lldus  stored=%u/%u\n", pairWriteUs / kPairs, pairsStored,
                static_cast<unsigned>(kPairs));
  Serial.printf("BENCH pair_find         per_pair=%lldns  right=%u/%u\n", (esp_timer_get_time() - t) * 1000 / kPairs,
                pairsRight, static_cast<unsigned>(kPairs));

  // A reboot: open scans the log to find where writing resumes.
  const uint32_t logUsed = store.logUsed();
  store.close();
  t = esp_timer_get_time();
  const bool reopened = store.open(kIdentity) == FlashGlyphStore::OpenResult::Ready;
  Serial.printf("BENCH open_scan         %u ms  log=%u B  slots=%u  resume point %s\n", msSince(t),
                static_cast<unsigned>(store.logUsed()), static_cast<unsigned>(store.usedSlots()),
                reopened && store.logUsed() == logUsed ? "matches" : "MISMATCH");
  Serial.printf("verify after reopen:    %u bad\n",
                static_cast<unsigned>(countBad(store, kBatched + kSingles, body, heading)));

  // The wipe erases only the sectors that hold data.
  t = esp_timer_get_time();
  const bool wiped = store.wipe();
  Serial.printf("BENCH wipe              %u ms  %s\n", msSince(t), wiped ? "ok" : "FAILED");
  Serial.printf("after wipe:             first glyph %s, sizes restart at %d\n",
                store.find(keyOf(0, body, heading), nullptr) == FlashGlyphStore::Lookup::Absent ? "absent"
                                                                                               : "STILL THERE",
                store.sizeIndex(29 << 6, 0));

  // Leave the partition without a store header, as the reader found it.
  store.close();
  flash.erase(0, NorFlash::kSectorBytes);
  Serial.printf("language slot untouched: %s\n", langSlotHash() == langBefore ? "yes" : "NO");
  finish(nullptr);
}

void loop() { delay(30000); }
```

Add to `platformio.ini`, right after the `[env:bench_font]` block (keep a blank line between blocks):

```ini
[env:bench_glyph_store]
extends = base, firmware_tuned
; Same source filter reasoning as env:bench_font: HalSystem.cpp already brings the panic wraps.
build_src_filter = -<*> +<../bench/glyph_store_main.cpp> +<platform/skip_efuse_blk_check.c>
build_flags =
  ${c3.build_flags}
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=1
  ; scripts/git_branch.py only injects this for `default` and the *gh_release_rc envs, and
  ; HalSystem.cpp pastes it straight into a string literal.
  -DCROSSPOINT_VERSION='"bench"'
```

Build it in the background:

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_glyph_store > /c/tmp/stage2a-bench-build.log 2>&1; grep -a "\[SUCCESS\]\|FAILED\|error:" /c/tmp/stage2a-bench-build.log | tail -3
```

Expected: `[SUCCESS]`.

- [ ] **Step 2: Run it on the X3**

USER ACTION: confirm that erasing the `.cpfont` flash cache is fine. The reader copies a family back from
the SD card the next time it needs one. The X3 must be on COM6, with no serial monitor holding the port.

The capture script opens the port with DTR and RTS held low, because toggling them after `open()` resets
the C3. It then pulses RTS once to reset the device, so the run is captured from its first line with no
button press. Write it with the Write tool as `/c/tmp/capture_reset.py`, unless it already exists:

```python
"""Capture a run from the X3 (ESP32-C3 USB-Serial-JTAG) from its first line.
usage: capture_reset.py PORT OUT [SECONDS] [--no-reset] [--append]"""
import sys
import time

import serial  # pyserial, from the PlatformIO penv

port, out_path = sys.argv[1], sys.argv[2]
args = [a for a in sys.argv[3:] if not a.startswith("--")]
flags = [a for a in sys.argv[3:] if a.startswith("--")]
deadline = time.time() + (int(args[0]) if args else 600)


def open_port():
    while time.time() < deadline:
        try:
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout = port, 115200, 1
            ser.dtr = False
            ser.rts = False
            ser.open()
            return ser
        except serial.SerialException:
            time.sleep(0.5)
    sys.exit("port never opened")


ser = open_port()
if "--no-reset" not in flags:
    ser.rts = True  # reset pulse
    time.sleep(0.1)
    ser.rts = False
last = time.time()
done = False
with open(out_path, "a" if "--append" in flags else "w", encoding="utf-8", errors="replace") as out:
    while time.time() < deadline and not done:
        try:
            line = ser.readline().decode("utf-8", errors="replace")
        except serial.SerialException:
            line = ""
            ser.close()
            ser = open_port()
        if line:
            last = time.time()
            out.write(line)
            out.flush()
            done = "=== done ===" in line
        elif time.time() - last > 20:
            ser.close()
            ser = open_port()
            last = time.time()
ser.close()
print("done" if done else "deadline reached")
```

```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_glyph_store -t upload --upload-port COM6 > /c/tmp/stage2a-bench-upload.log 2>&1; grep -a "\[SUCCESS\]\|FAILED" /c/tmp/stage2a-bench-upload.log | tail -1
/c/pio/penv/Scripts/python.exe /c/tmp/capture_reset.py COM6 /c/tmp/stage2a-bench-run.txt 900
grep -a "BENCH\|verify\|resume\|after wipe\|language\|FATAL\|=== done" /c/tmp/stage2a-bench-run.txt
```

Expected:
- `verify after writes: 0 bad of 3200` and `verify after reopen: 0 bad`;
- `resume point matches`;
- `found=10000/10000`;
- `right=500/500` (with `stored=500/500`);
- `wipe ... ok` and `first glyph absent, sizes restart at 0`;
- `language slot untouched: yes`;
- no `FATAL`.

Record every `BENCH` line for the PR. Plausible ranges from step 0 (Appendix A):
- `find_hit` about 1.5 µs;
- `commit_single` about 0.4 ms;
- `commit_batched` below it, since it saves about 100 µs of call overhead per batched record;
- `open_scan` well under 100 ms.

A value far outside these ranges is a finding to report, not to tune away.

- [ ] **Step 3: Restore the reader firmware**

```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6 > /c/tmp/stage2a-restore.log 2>&1; grep -a "\[SUCCESS\]\|FAILED" /c/tmp/stage2a-restore.log | tail -1
```

Expected: `[SUCCESS]`. USER ACTION: check that the reader boots and opens a book.

- [ ] **Step 4: Format, full host suite, commit**

CI formats with clang-format **21.1.8**. If `/c/tmp/cf21/bin/clang-format-21.exe` does not exist yet, install it:

```bash
python -m pip install --target /c/tmp/cf21/pkg clang-format==21.1.8
mkdir -p /c/tmp/cf21/bin
cp /c/tmp/cf21/pkg/clang_format/data/bin/clang-format.exe /c/tmp/cf21/bin/clang-format-21.exe
```

```bash
F=$(git diff --name-only master...HEAD | grep -E '\.(h|cpp|c)$'; git ls-files --others --exclude-standard | grep -E '\.(h|cpp|c)$')
/c/tmp/cf21/bin/clang-format-21.exe -i $F
/c/tmp/cf21/bin/clang-format-21.exe --dry-run -Werror $F && echo "format clean"
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/test -j 8 -- -k 0 > /c/tmp/stage2a-host-build.log 2>&1; tail -3 /c/tmp/stage2a-host-build.log
ctest --test-dir build/test -j 8 --output-on-failure > /c/tmp/stage2a-ctest.log 2>&1; tail -4 /c/tmp/stage2a-ctest.log
git add bench/glyph_store_main.cpp platformio.ini
git commit -am "bench: glyph store device self-test (env bench_glyph_store); clang-format"
```

Expected: `format clean`; the only build failure is `epub_build_inventory` (`<dlfcn.h>`); `100% tests passed`.

- [ ] **Step 5: Push and open the PR**

```bash
git push -u origin feat/ttf-glyph-store
gh pr create --base master --head feat/ttf-glyph-store --title "TTF Stage 2a: FlashGlyphStore, a write-once glyph store in flash" --body-file /c/tmp/stage2a-pr-body.md
```

Write `/c/tmp/stage2a-pr-body.md` first, with the Write tool:

```markdown
Stage 2a of on-device TTF fonts (design: `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` on
`docs/ttf-fonts-design`, section 4). **Nothing calls it yet**; Stage 2c wires it to `TtfFont`.

- **`FlashGlyphStore`** (`lib/EpdFont/ttf/`) is a persistent store of rendered glyphs in the `spiffs` partition:
  - a write-once hash index and an append-only log;
  - a kerning pair index and a size table;
  - the font's own bytes, in style blocks.
  - A lookup is a pointer into mapped flash.
- **Power cuts:** a record is written before its slot. A torn header fails a check, a torn slot is skipped,
  and recovery resumes past any programmed byte. Host sweeps cut the power at every write step of a session
  and of a wipe: N reboots, 0 bytes programmed twice.
- **Fewer, smaller writes and erases:**
  - glyph records go to flash in one write per batch: `commit_batched` X µs/glyph against `commit_single`
    Y µs;
  - install and wipe erase only sectors that hold data;
  - the index is sized to the log, so the store is not wiped while half the log is empty.
- **`NorFlash`** (`lib/hal`) is a thin interface. `EspNorFlash` is the only code calling `esp_partition_*`;
  it stops below the language slot and refuses writes while a `.cpfont` is mapped. Host tests use a
  simulated NOR chip.

Tests: GlyphStoreTest 70/70; full host suite <N>/<N>. Builds: default (flash ±<d> B against master), x4pro.
X3 self-test (`env:bench_glyph_store`):

<BENCH lines>
```

Before running `gh pr create`, replace X, Y, N, <d> and `<BENCH lines>` with the measured values.
