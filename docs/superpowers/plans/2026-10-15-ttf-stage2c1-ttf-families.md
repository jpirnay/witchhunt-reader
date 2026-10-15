# TTF Stage 2c-1: TrueType families in the reader. Implementation plan

> **Status (2026-10-15): done, branch `feat/ttf-families`.** Tasks 1-7 executed inline, each matching its stated
> test counts; then a three-lane review (memory and lifetimes, store and flash, integration): no Critical, four
> findings fixed test-first, 26 minors deferred (listed in the PR). Task 8 ran on the X3; results in spec
> Appendix A, "Stage 2c-1".
> - **The code differs from the task code below in these places:**
>   - `TtfFont::load()` reinstalls once when a style no longer maps (`openStyles()`, `StylesResult`);
>   - `ttf::probeFile()` and `FileProbe` (weight from `OS/2.usWeightClass`, variable fonts spotted by `fvar`);
>     `assignStyles()` takes probes and prefers static files at the weight nearest 400 or 700;
>   - `ttf::fitsFlashMode()` and `SdCardFontRegistry::setTtfFits()`: a family too large for flash mode is not
>     listed;
>   - `SdCardFontFamilyInfo::ttf()` is derived from the files instead of a field (512 B of permanent heap).
> - **Test totals:** TtfFontTest 58, ReaderFontSizesTest 22, GlyphStoreTest 91, GlyphProviderTest 9; host suite
>   2,224.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A folder of `.ttf` files on the SD card becomes a reader font family. The device draws it at the reader's
body size, 8 to 26 pt, from the glyph store: each glyph is rendered on first use, stored in flash, and read from there
on every later page and boot.

**Architecture:**

- **Stage 2c is split into four plans,** each shipping working software (the order of the middle two as proposed
  after the measurement below):
  - **2c-1 (this plan):** TTF families at the body size, in flash mode (every style's whole TTF copied into the
    partition);
  - **2c-2:** native sizes: headings and CSS sizes drawn from the outlines at their own pixel size (dormant faces,
    the resolver, the TTF ladder branch), and font previews;
  - **2c-3:** kerning (the pre-resolved GPOS reader and the pair index) and ligatures (GSUB);
  - **2c-4:** SD mode (FontBytes' page cache, copied tables, the 8 KB pool), for CJK fonts too large for flash.
- **`GlyphProvider::prefetch(text)`** (no-op by default) and **`EpdFontFamily::prefetch(text, styleMask)`** let
  `GfxRenderer::ensureFontReady` (before layout measures a run) and `FontCacheManager::prewarmCache` (before a page
  is drawn) ask a TTF face to render a text's missing glyphs in one batch. Neither needs to know about TTF.
- **`ttf::probeStyle` / `ttf::familyIdentity`** (`lib/EpdFont/ttf/TtfFamily.{h,cpp}`): a file's style from
  `head.macStyle`, read in three small reads at discovery; the family identity from each file's name, size and
  modification time (spec 4.2).
- **`TtfFont`** (`lib/EpdFont/ttf/TtfFont.{h,cpp}`) owns one family:
  - the `FlashGlyphStore` (installing the family when the partition holds something else);
  - one `ttf::Face` per style and one `ttf::Rasterizer`, whose output region is also the `GlyphBatch` buffer;
  - the body face: four `EpdFontData` with no tables, each answering through a per-style `GlyphProvider` adapter;
  - an 8-slot heap ring for glyphs drawn without being stored (store full, write failed, oversized).
- **The registry** turns a directory of `.ttf` files into a family (`SdCardFontFamilyInfo::ttf`); **the manager**
  loads it through `EspNorFlash` + `TtfFont` and registers the body face; **`SdCardFontSystem`** offers it every
  size from 8 pt (`ReaderSizeList::native()`) and changes its size without a reload.

**Tech Stack:** C++20 host tests (gtest via CMake, UCRT64 gcc; ASan and UBSan where they link); firmware gnu++2a
(PlatformIO, ESP32-C3/S3); clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (branch `docs/ttf-fonts-design`): sections 3.1-3.4,
4.1-4.2, 4.6, 5.3, 5.5 (body face only), 6.1-6.3, 7.1-7.5 and the Stage 2c obligations in 8.1. Task 8 records this
plan's decisions there.

**Pre-validated:** every task's code was built and tested in a prototype (`C:/tmp/wt-2c1-proto`), and the plan
was replayed task by task on a fresh tree at `b9e65f9e9`: each "see it fail" step fails as stated, each test count
matches, and every file the plan writes is identical to the prototype. `default` and `x4pro` build. It has not run
on the X3 yet (Task 8).

## Global Constraints

- **Branch:** `feat/ttf-families`, stacked on `feat/ttf-rasterizer` (Stage 2b, PR #470); if #470 has merged,
  branch from `master` instead. The PR targets `feat/ttf-rasterizer` until #470 merges, then `master`.
- **Under RenderLock:** every `TtfFont` call runs under `RenderLock` (spec 3.4). The app checks it once per boot
  through `TtfFont::Hooks::renderLockHeld` and logs a violation; it does not assert.
- **Write once:** every flash byte is programmed at most once between erases. Every `TtfFont` test ends with
  `flash.violations() == 0` and `flash.reprograms() == 0` (its `TearDown`).
- **Heap:** every allocation is `makeUniqueNoThrow`, null-checked and logged with its size (`heap-discipline`
  skill); no bare `new`, no allocation per glyph on the store path.
- **No floating point** in `lib/EpdFont/ttf` (`scripts/check_no_float.py` runs after every firmware link).
- **Includes:** code outside `lib/EpdFont/ttf` includes its headers as `<ttf/TtfFont.h>` (the firmware's include
  path has `lib/EpdFont`, not `lib/EpdFont/ttf`).
- **Sizes:** the reader ladder is 10-26 pt (`CrossPointSettings::FONT_SIZE_RUNGS`); a TTF family also offers 8 pt
  (`ReaderSizeList::native()`, `kNativeSmallestPoints = 8`), on every board. `TtfFont::kMaxPointSize` is 26 and a
  `static_assert` in `SdCardFontSystem.cpp` keeps it in step with the ladder.
- **Host test commands** (Git Bash, from the worktree root):
  `export PATH=/c/msys64/ucrt64/bin:$PATH` once; configure with
  `cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src`
  (the generator matters: the full suite needs `cmake --build build/test -j 8 -- -k` to get past
  `epub_build_inventory`, which does not build on Windows (`dlfcn.h`), and Ninja wants `-k 0` instead).
- **Firmware builds:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
  `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`; run one env at a time (a second env wipes the first's
  build directory).
- **Formatting:** clang-format 21 (`/c/tmp/cf21/bin/clang-format-21.exe -i <files>` locally; CI checks with 21).
- **Commits:** semantic prefixes (`feat:`, `fix:`, `test:`, `docs:`, `refactor:`); no `Co-Authored-By` trailer.

## Review Focus

The five situations the spec implies that no host test reaches, most likely first. Each is checked on the X3 in
Task 8 (step numbers there):

1. **Switching between a TTF family and a `.cpfont` family.** They share the partition (spec 4.1): each switch
   erases the other's data. Expected: the "Loading font" popup and a reinstall (about 10 s for TTF), never a
   garbled glyph or a mapping refused for the wrong owner. Task 8, step 7.
2. **Changing the size from the reader menu mid-book.** Expected: a new font ID and a re-layout, no install and
   no reload (`SdCardFontSystem` calls `setTtfBodySize`). Task 8, step 5.
3. **A TTF file removed or the card pulled after discovery.** Expected: the load fails, the reader falls back to a
   built-in font, nothing is installed. Task 8, step 8.
4. **Headings and other scaled text.** The ladder for an SD family is empty, so headings draw the body face
   scaled (as `.cpfont` families do until 2c-3). Expected: headings render, from stored glyphs. Task 8, step 4.
5. **Long renders under RenderLock** (a hostile glyph can take most of a second; an install takes about 10 s).
   Expected: no watchdog reset; the app's yield hook sleeps a tick every 200 ms. Task 8, step 3 (the install).

## File Structure

| File | Task | Responsibility |
| --- | --- | --- |
| `lib/EpdFont/ttf/FlashGlyphStore.{h,cpp}`, `GlyphStoreFormat.h` | 1 | `GlyphBatch::dropLast()`; `glyphstore::recordBytes()` public |
| `lib/EpdFont/GlyphProvider.h`, `EpdFontFamily.{h,cpp}` | 2 | the `prefetch` hook |
| `lib/EpdFont/ttf/TtfFamily.{h,cpp}` | 3 | style probe, family identity, `.ttf` names, style assignment |
| `lib/EpdFont/ttf/TtfFont.{h,cpp}` | 4, 5 | the family: install, body face, glyph lookups, ring, wipe; prefetch |
| `lib/EpdFont/SdCardFontRegistry.{h,cpp}` | 6 | TTF directories become families |
| `lib/EpdFont/SdCardFontManager.{h,cpp}` | 7 | load, size change, wipe, unload of a TTF family |
| `src/SdCardFontSystem.cpp`, `src/ReaderFontSizes.h`, `src/activities/RenderLock.h`, `ActivityManager.cpp` | 7 | sizes from 8 pt, reader-entry paths, app hooks |
| `lib/GfxRenderer/GfxRenderer.cpp`, `FontCacheManager.cpp` | 7 | the two prefetch call sites |
| `test/ttf_font/*` | 3-6 | new test target `TtfFontTest` |

## Decisions this plan makes (record them in the spec, Task 8)

- **A family is a directory**, as `.cpfont` families are: `/.crosspoint/fonts/<Family>/*.ttf`. The spec's
  name-table family (7.1) is not used: the directory name is what the font list shows today, and it needs no
  `name` table parsing. A directory with any `.cpfont` file stays a `.cpfont` family.
- **Style from `head.macStyle`;** the first file of each style, by name, serves it; a regular style is required.
- **TTF families start at 8 pt** on every board (8, then the 10-26 pt ladder). Points convert at the converter's
  150 dpi everywhere, so 8 pt is about 17 px per em on each panel and looks smaller the denser the panel. Built-in
  families keep 10-26 pt (they have no 8 pt face, and a resampled 10 pt face would lose its anti-aliasing: see
  Measured); `.cpfont` families keep the sizes they ship.
- **A staged batch record can be taken back** (`GlyphBatch::dropLast()`): when `rasterize()` fails after `add()`,
  nothing fills the slot (spec 8.1, "add then rasterize").
- **The output region holds a whole record:** its size grows by the 20-byte record header plus alignment (capped
  at `kMaxOutputBytes`), so any glyph that loads fits an empty batch; a record that still does not fit is drawn
  from the ring, unstored.
- **Glyphs whose left or top do not fit `int8`** (EpdGlyphRef) are marked missing (spec 8.1, field widths).
- **A store that cannot be mapped** reinstalls with a fresh plan (`MapFailed`); if it still cannot open, the
  family is refused and the reader falls back to a built-in font. Spec 6.2's "ring mode for the session" does not
  apply in flash mode: the font's own bytes are in the store.
- **Prefetch:** at most 64 distinct missing glyphs per call, in glyph-ID order; ASCII repeats skipped with a
  bitmap; layout prefetches the regular style only (it passes no style).
- **The wipe runs at family load and at book open** (the explicit-family `ensureLoaded`, with the "Loading font"
  popup); not before a section build (deferred).
- **Deferred to later 2c plans:** native sizes, the resolver, previews (2c-2); kerning and ligatures (2c-3); SD
  mode, the tricky-font check, the record-size planner input, S3 internal-RAM scratch (2c-4).

## Measured: native sizes against resampled ones (X3, 2026-10-15)

**The question:** a size the family has no face for (a heading, a CSS font-size) is resampled today from the body
face (`renderCharAtScale` and its 80-entry mask cache). Is drawing it natively, rendered once, stored, then read
back, faster as well as sharper?

**The bench** (throwaway, `bench/ttf_scale_main.cpp` in the prototype tree, env `bench_ttf_scale`): one page of prose,
417 glyphs (30 distinct), drawn through `GfxRenderer` into the framebuffer from Bookerly Regular with a 14 pt body.
Each target size is drawn resampled from the 14 pt face and drawn natively. Times per page; "AA" is the BW pass plus
the two grey passes of an anti-aliased page:

| Target (scale) | Resampled BW warm | Native BW warm | Resampled AA warm | Native AA warm | First page BW: resampled / native |
| --- | --- | --- | --- | --- | --- |
| 8 pt (0.57) | 15.6 ms | 15.0 ms | 46.2 ms | 43.5 ms | 16.7 / 30.3 ms |
| 12 pt (0.86) | 17.8 ms | 20.3 ms | 53.0 ms | 58.7 ms | 19.7 / 39.7 ms |
| 16 pt (1.14) | 20.2 ms | 24.7 ms | 162.9 ms | 71.0 ms | 50.3 / 48.6 ms |
| 20 pt (1.43) | 22.1 ms | 28.1 ms | 274.1 ms | 80.4 ms | 67.7 / 57.7 ms |
| 26 pt (1.86) | 150.8 ms | 34.0 ms | 449.0 ms | 97.1 ms | 150.9 / 74.4 ms |
| 14 pt body | | 22.8 ms | | 65.7 ms | 45.7 ms (native, with its renders) |

A native first page includes rendering and storing each glyph: 0.50 ms per glyph at 8 pt up to 1.34 ms at 26 pt,
paid once per glyph and size for as long as the store holds it, across pages and boots.

**Findings:**
- **Enlarged sizes: native is faster, even on the first page.** It is equal at 16 pt, 15% faster at 20 pt and 2x
  faster at 26 pt on first contact. Once stored, on an anti-aliased page, it is 2.3x faster at 16 pt, 3.4x at 20 pt
  and 4.6x at 26 pt. An AA page resamples each glyph up to three times (the BW, LSB and MSB masks), which overflows
  the 80-entry mask cache, so resampled text is resampled again on every page.
  - In BW mode a small enlargement (16-20 pt) is about 20% cheaper resampled once cached: a 1-bit mask blit against
    a 2-bit glyph.
  - At 26 pt the masks outgrow the 3.5 KB arena, and resampling is 4.4x slower than native.
- **Reduced sizes are equally fast either way once warm.** Resampling is nearest-neighbour and cached, but it drops
  the anti-aliasing. Native costs 0.5-0.65 ms per glyph once.
- **Quality:** a native glyph is the outline drawn at that size, as FreeType draws it; a resampled one is the 14 pt
  bitmap enlarged or thinned.

**Consequences:**
- **Draw a TTF family's non-body sizes natively** (proposed as plan 2c-2, ahead of kerning). It differs from spec 5.5
  in one point: each size is drawn at its own pixel size, pixels per em rounded to whole pixels. The leftover
  residual (under 1.5% from 16 pt up) then falls inside the ladder's 3% dead zone and nothing is resampled. The
  spec's nearest 2 pt rung plus a residual of a few percent would still go through the resampler and its cache.
  The store's 32-entry size table becomes that design's budget: a book uses its body size plus a handful of CSS
  sizes, and a full table means a wipe at the next book open.
- **This plan is unchanged by it.** Its TTF families resample non-body text exactly as `.cpfont` families do today.

---

### Task 1: The store takes back a staged record

`TtfFont` renders straight into `GlyphBatch::add()`'s pointer. `rasterize()` can still fail after `add()` (an
ftgrays overflow), and a staged record used to be final: its bitmap would have been committed as 0xFF, solid ink.
`dropLast()` takes it back. `recordBytes()` moves into the format header, because `TtfFont` must know whether a
glyph's record fits an empty batch before it stages it.

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp`,
  `lib/EpdFont/ttf/GlyphStoreFormat.h`
- Test: `test/glyph_store/GlyphStoreGlyphTest.cpp`, `test/glyph_store/GlyphStoreFormatTest.cpp`

**Interfaces:**
- Produces:
  - `void GlyphBatch::dropLast()`: un-stages the record of the last `add()`. Only once per `add()`; it does
    nothing on an empty batch.
  - `constexpr uint32_t glyphstore::recordBytes(uint32_t dataLength)`: `align4(20 + dataLength)`, the bytes
    a record takes in the log and in a batch.

- [ ] **Step 1: Branch**

```bash
git fetch origin
git switch -c feat/ttf-families origin/feat/ttf-rasterizer   # origin/master once PR #470 has merged
git submodule update --init freeink-sdk
```

- [ ] **Step 2: Write the failing tests**

In `test/glyph_store/GlyphStoreGlyphTest.cpp`, add before `KeysOfAnUnclaimedSizeAreRefused`:

<!-- plan-edit: test/glyph_store/GlyphStoreGlyphTest.cpp -->
```cpp
TEST_F(GlyphStoreGlyphs, KeysOfAnUnclaimedSizeAreRefused) {
```
with:

```cpp
TEST_F(GlyphStoreGlyphs, DropLastTakesBackOnlyTheLastRecord) {
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 0), 9, 12));
  ASSERT_NE(nullptr, batch.add(makeKey('B', 0, 0), metrics(10, 12)));  // its bitmap is never drawn
  batch.dropLast();
  batch.dropLast();  // once per add(): A stays
  EXPECT_EQ(1, batch.count());
  ASSERT_TRUE(stage(batch, makeKey('C', 0, 0), 8, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_TRUE(storedIntact(store, makeKey('A', 0, 0), 9, 12));
  EXPECT_EQ(Lookup::NotStored, store.find(makeKey('B', 0, 0), nullptr));
  EXPECT_TRUE(storedIntact(store, makeKey('C', 0, 0), 8, 12));
  const EpdGlyph* c = nullptr;
  ASSERT_EQ(Lookup::Found, store.find(makeKey('C', 0, 0), &c));
  EXPECT_EQ(recordSize(9, 12) + kRecordHeaderBytes, c->dataOffset);  // staged where B was
}

TEST_F(GlyphStoreGlyphs, KeysOfAnUnclaimedSizeAreRefused) {
```

In `test/glyph_store/GlyphStoreFormatTest.cpp`, add before the first planner test:

<!-- plan-edit: test/glyph_store/GlyphStoreFormatTest.cpp -->
```cpp
TEST(GlyphStorePlan, LatinFlashModeFillsThePartition) {
```
with:

```cpp
TEST(GlyphStoreFormat, ARecordIsItsHeaderAndBitmapRoundedToFourBytes) {
  EXPECT_EQ(20u, recordBytes(0));  // a space: the header alone
  EXPECT_EQ(24u, recordBytes(1));
  EXPECT_EQ(24u, recordBytes(4));
  EXPECT_EQ(28u, recordBytes(5));
}

TEST(GlyphStorePlan, LatinFlashModeFillsThePartition) {
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_brief=1`
Expected: compile errors, `'class GlyphBatch' has no member named 'dropLast'` and `'recordBytes' was not declared
in this scope`.

- [ ] **Step 4: Implement**

In `lib/EpdFont/ttf/GlyphStoreFormat.h`:

<!-- plan-edit: lib/EpdFont/ttf/GlyphStoreFormat.h -->
```cpp
constexpr uint32_t align4(const uint32_t n) { return (n + 3u) & ~3u; }
```
with:

```cpp
constexpr uint32_t align4(const uint32_t n) { return (n + 3u) & ~3u; }
/// Bytes a record with a `dataLength`-byte bitmap takes in the log, and in a GlyphBatch.
constexpr uint32_t recordBytes(const uint32_t dataLength) { return align4(kRecordHeaderBytes + dataLength); }
```

In `lib/EpdFont/ttf/FlashGlyphStore.h`, the class `GlyphBatch`:

<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  /// or is over glyphstore::kMaxBitmapBytes, or the buffer has no room left (commit, then add again). A
  /// staged record cannot be taken back. Keys in one batch must be distinct and not yet stored.
  uint8_t* add(uint32_t key, const EpdGlyph& metrics);
```
with:

```cpp
  /// or is over glyphstore::kMaxBitmapBytes, or the buffer has no room left (commit, then add again). Keys in
  /// one batch must be distinct and not yet stored.
  uint8_t* add(uint32_t key, const EpdGlyph& metrics);
  /// Takes back the record of the last add() (its bitmap could not be drawn). Only once per add().
  void dropLast();
```

<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
    used_ = 0;
    count_ = 0;
  }
```
with:

```cpp
    used_ = 0;
    count_ = 0;
    lastUsed_ = 0;
  }
```

<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  uint32_t used_ = 0;
  uint16_t count_ = 0;
};
```
with:

```cpp
  uint32_t used_ = 0;
  uint32_t lastUsed_ = 0;  // used_ before the last add()
  uint16_t count_ = 0;
};
```

In `lib/EpdFont/ttf/FlashGlyphStore.cpp`, remove the file-local `recordBytes` (the header's replaces it):

<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
uint32_t recordBytes(const uint16_t dataLength) { return align4(kRecordHeaderBytes + dataLength); }

uint32_t tableBytes(const uint32_t slots) { return alignUp(slots * kSlotBytes, kSectorBytes); }
```
with:

```cpp
uint32_t tableBytes(const uint32_t slots) { return alignUp(slots * kSlotBytes, kSectorBytes); }
```

Then remember where each record starts, and add `dropLast()` after `add()`:

<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  std::memcpy(record + 4, &glyph, sizeof glyph);
  used_ += bytes;
  ++count_;
  return record + kRecordHeaderBytes;
}
```
with:

```cpp
  std::memcpy(record + 4, &glyph, sizeof glyph);
  lastUsed_ = used_;
  used_ += bytes;
  ++count_;
  return record + kRecordHeaderBytes;
}

void GlyphBatch::dropLast() {
  if (count_ == 0 || used_ == lastUsed_) return;
  used_ = lastUsed_;
  --count_;
}
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 91 tests.`

- [ ] **Step 6: Commit**

```bash
git add lib/EpdFont/ttf/FlashGlyphStore.h lib/EpdFont/ttf/FlashGlyphStore.cpp lib/EpdFont/ttf/GlyphStoreFormat.h \
        test/glyph_store/GlyphStoreGlyphTest.cpp test/glyph_store/GlyphStoreFormatTest.cpp
git commit -m "feat(ttf): GlyphBatch::dropLast takes back a record whose bitmap could not be drawn"
```

### Task 2: A prefetch hook on GlyphProvider

Layout and drawing ask a font to get a run of text ready before they measure or draw it. Today that is an SD-font
special case in two places. A no-op `GlyphProvider::prefetch` reaches any provider through the font family, so the
renderer's two call sites (Task 7) need not know about TTF.

**Files:**
- Modify: `lib/EpdFont/GlyphProvider.h`, `lib/EpdFont/EpdFontFamily.h`, `lib/EpdFont/EpdFontFamily.cpp`
- Test: `test/glyph_provider/GlyphProviderTest.cpp`, `test/glyph_provider/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `virtual void GlyphProvider::prefetch(const char* utf8Text)`: no-op by default.
  - `void EpdFontFamily::prefetch(const char* utf8Text, uint8_t styleMask) const`: bit `i` of `styleMask` is
    `EpdFontFamily::Style` `i`. Each style's font (after `getFont()`'s fallback) is asked once; fonts without a
    provider are skipped.

- [ ] **Step 1: Write the failing tests**

In `test/glyph_provider/GlyphProviderTest.cpp`, include the family and give the fake provider a prefetch log:

<!-- plan-edit: test/glyph_provider/GlyphProviderTest.cpp -->
```cpp
#include <map>
#include <utility>
#include <vector>

#include "EpdFont.h"
#include "GlyphProvider.h"
```
with:

```cpp
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "GlyphProvider.h"
```

<!-- plan-edit: test/glyph_provider/GlyphProviderTest.cpp -->
```cpp
    kernAsked.emplace_back(leftCp, rightCp);
    return kernValue;
  }
};
```
with:

```cpp
    kernAsked.emplace_back(leftCp, rightCp);
    return kernValue;
  }
  std::vector<std::string> prefetched;
  void prefetch(const char* utf8Text) override { prefetched.emplace_back(utf8Text); }
};
```

Append to the same file:

<!-- plan-append: test/glyph_provider/GlyphProviderTest.cpp -->
```cpp
TEST(GlyphProvider, PrefetchAsksEachStylesFontOnceAfterFallback) {
  FakeProvider regularProvider;
  FakeProvider boldProvider;
  const EpdFontData regularData = providerOnly(&regularProvider);
  const EpdFontData boldData = providerOnly(&boldProvider);
  const EpdFont regular(&regularData);
  const EpdFont bold(&boldData);
  const EpdFontFamily family(&regular, &bold);  // italic falls back to regular, bold italic to bold

  family.prefetch("Wolf", 0x0F);

  EXPECT_EQ(std::vector<std::string>{"Wolf"}, regularProvider.prefetched);
  EXPECT_EQ(std::vector<std::string>{"Wolf"}, boldProvider.prefetched);
}

TEST(GlyphProvider, PrefetchAsksOnlyTheStylesInTheMask) {
  FakeProvider regularProvider;
  FakeProvider boldProvider;
  const EpdFontData regularData = providerOnly(&regularProvider);
  const EpdFontData boldData = providerOnly(&boldProvider);
  const EpdFont regular(&regularData);
  const EpdFont bold(&boldData);
  const EpdFontFamily family(&regular, &bold);

  family.prefetch("Wolf", 1u << EpdFontFamily::BOLD);

  EXPECT_TRUE(regularProvider.prefetched.empty());
  EXPECT_EQ(std::vector<std::string>{"Wolf"}, boldProvider.prefetched);
}

TEST(GlyphProvider, PrefetchSkipsFontsWithoutAProvider) {
  FakeProvider boldProvider;
  const EpdFontData builtIn{};  // a built-in font has no provider
  const EpdFontData boldData = providerOnly(&boldProvider);
  const EpdFont regular(&builtIn);
  const EpdFont bold(&boldData);
  const EpdFontFamily family(&regular, &bold);

  family.prefetch("Wolf", 0x0F);

  EXPECT_EQ(std::vector<std::string>{"Wolf"}, boldProvider.prefetched);
}
```

In `test/glyph_provider/CMakeLists.txt`, build the family too:

<!-- plan-edit: test/glyph_provider/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
```
with:

```cmake
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target GlyphProviderTest -j 8 && ./build/test/glyph_provider/GlyphProviderTest.exe --gtest_brief=1`
Expected: compile errors, `marked 'override', but does not override` and `'const class EpdFontFamily' has no member
named 'prefetch'`.

- [ ] **Step 3: Implement**

In `lib/EpdFont/GlyphProvider.h`, the class comment and the new virtual:

<!-- plan-edit: lib/EpdFont/GlyphProvider.h -->
```cpp
/// carries no kern tables. GfxRenderer asks it whether a glyph's bitmap is one it keeps itself.
/// SdCardFont implements it for its glyph-miss ring; a font with no tables at all can answer every
/// lookup through it.
```
with:

```cpp
/// carries no kern tables. GfxRenderer asks it whether a glyph's bitmap is one it keeps itself, and
/// lets it prepare a run of text before measuring or drawing it. SdCardFont implements it for its
/// glyph-miss ring; a font with no tables at all (TtfFont) answers every lookup through it.
```

<!-- plan-edit: lib/EpdFont/GlyphProvider.h -->
```cpp
    (void)rightCp;
    return 0;
  }
};
```
with:

```cpp
    (void)rightCp;
    return 0;
  }

  /// `utf8Text` is about to be measured or drawn in this provider's style: a provider that renders glyphs
  /// can prepare the ones it lacks in one batch. Lookups must still work for anything it skipped.
  virtual void prefetch(const char* utf8Text) { (void)utf8Text; }
};
```

In `lib/EpdFont/EpdFontFamily.h`:

<!-- plan-edit: lib/EpdFont/EpdFontFamily.h -->
```cpp
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
```
with:

```cpp
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
  /// Asks the provider of each style in `styleMask` (bit i = Style i) to prepare `utf8Text`; each font
  /// once, after the usual fallback to another style. Nothing for fonts without a provider.
  void prefetch(const char* utf8Text, uint8_t styleMask) const;
```

In `lib/EpdFont/EpdFontFamily.cpp`:

<!-- plan-edit: lib/EpdFont/EpdFontFamily.cpp -->
```cpp
#include "EpdFontFamily.h"

```
with:

```cpp
#include "EpdFontFamily.h"

#include <algorithm>

#include "GlyphProvider.h"

```

<!-- plan-edit: lib/EpdFont/EpdFontFamily.cpp -->
```cpp
uint32_t EpdFontFamily::applyLigatures(
```
with:

```cpp
void EpdFontFamily::prefetch(const char* utf8Text, const uint8_t styleMask) const {
  const EpdFont* asked[4] = {};
  uint8_t count = 0;
  for (uint8_t style = 0; style < 4; ++style) {
    if (!(styleMask & (1u << style))) continue;
    const EpdFont* font = getFont(static_cast<Style>(style));
    if (!font || !font->data->provider || std::find(asked, asked + count, font) != asked + count) continue;
    asked[count++] = font;
    font->data->provider->prefetch(utf8Text);
  }
}

uint32_t EpdFontFamily::applyLigatures(
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target GlyphProviderTest -j 8 && ./build/test/glyph_provider/GlyphProviderTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 9 tests.`

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/GlyphProvider.h lib/EpdFont/EpdFontFamily.h lib/EpdFont/EpdFontFamily.cpp \
        test/glyph_provider/GlyphProviderTest.cpp test/glyph_provider/CMakeLists.txt
git commit -m "feat(fonts): GlyphProvider::prefetch, reached through EpdFontFamily::prefetch"
```

### Task 3: What a TTF file is, and which family it belongs to

The registry needs each `.ttf` file's style without loading the font; the manager needs an identity that changes
whenever any file of the family changes, so a stale store is never reused (spec 4.2). Both are pure functions over a
`FileSource` (a size and a read callback), so the host tests run them on bundled and hand-built fonts.

**Files:**
- Create: `lib/EpdFont/ttf/TtfFamily.h`, `lib/EpdFont/ttf/TtfFamily.cpp`
- Create: `test/ttf_font/CMakeLists.txt`, `test/ttf_font/TtfFontTestUtil.h`, `test/ttf_font/TtfFamilyTest.cpp`
- Modify: `test/CMakeLists.txt` (one line), `test/ttf_rasterizer/TtfBuilder.h` (a `macStyle` field),
  `test/ttf_rasterizer/CMakeLists.txt` (shares its sanitizer flags)

**Interfaces:**
- Produces (all in `namespace ttf`, header `TtfFamily.h`):
  - `struct FileSource { uint32_t size; bool (*read)(void* ctx, uint32_t offset, void* dst, uint32_t len); void* ctx; }`
  - `bool isTtfFileName(const char* name)`: ends in `.ttf`, any case.
  - `template <typename File> FileSource fileSource(File& file, uint32_t size)`: a source over any open file
    with `seekSet()` and `read()` (`HalFile` on the device).
  - `int probeStyle(const FileSource& file)`: `head.macStyle & 3`, or -1 (not TrueType with `glyf`, a needed
    table missing or outside the file, a read error).
  - `bool assignStyles(const int* styles, size_t count, int (&out)[4])`: first file of each style; false without a
    regular style.
  - `constexpr uint8_t kRenderVersion = 1;`
  - `struct FileStamp { const char* name; uint32_t size; uint16_t date; uint16_t time; }` (`name` null: no file for
    that style)
  - `void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes])`
  - `uint32_t identityHash(const uint8_t identity[glyphstore::kIdentityBytes])`: FNV-1a of the 16 bytes.
  - Test helper `ttf_test::MemoryFile { std::vector<uint8_t> bytes; bool failReads; uint32_t reads; FileSource source(); }`.
  - Test target `TtfFontTest` (`test/ttf_font/CMakeLists.txt`), which Tasks 4-6 extend.

- [ ] **Step 1: Write the failing tests**

Give `TtfBuilder`'s fonts a `macStyle`:

<!-- plan-edit: test/ttf_rasterizer/TtfBuilder.h -->
```cpp
  uint16_t unitsPerEm = 1000;
  int16_t ascender = 800;
```
with:

```cpp
  uint16_t unitsPerEm = 1000;
  uint16_t macStyle = 0;  ///< head.macStyle: bit 0 bold, bit 1 italic
  int16_t ascender = 800;
```

<!-- plan-edit: test/ttf_rasterizer/TtfBuilder.h -->
```cpp
  set16(head, 42, static_cast<uint16_t>(yMax));
```
with:

```cpp
  set16(head, 42, static_cast<uint16_t>(yMax));
  set16(head, 44, font.macStyle);
```

Let the new target reuse the rasterizer tests' sanitizer flags. In `test/ttf_rasterizer/CMakeLists.txt`:

<!-- plan-edit: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")
```
with:

```cmake
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")
set(TTF_SANITIZERS ${TTF_SANITIZERS} PARENT_SCOPE)  # ttf_font uses them too
```

In `test/CMakeLists.txt`, after the rasterizer's directory (the order matters: it sets `TTF_SANITIZERS`):

<!-- plan-edit: test/CMakeLists.txt -->
```cmake
add_subdirectory(ttf_rasterizer)
```
with:

```cmake
add_subdirectory(ttf_rasterizer)
add_subdirectory(ttf_font)
```

Create `test/ttf_font/CMakeLists.txt`:

<!-- plan-file: test/ttf_font/CMakeLists.txt -->
```cmake
# TtfFont (lib/EpdFont/ttf): TrueType families on the glyph store, from the file probe to drawn pages.

add_executable(TtfFontTest
  TtfFamilyTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFamily.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

# test/shims before lib/hal: the host HalStorage, but the real NorFlash.h.
target_include_directories(TtfFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/ttf_rasterizer
  ${REPO_ROOT}/test/glyph_store
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(TtfFontTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfFontTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# The sanitizers ttf_rasterizer found for this toolchain (ASan + UBSan on CI's Linux GCC).
target_compile_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})

gtest_discover_tests(TtfFontTest DISCOVERY_TIMEOUT 60)
```

Create `test/ttf_font/TtfFontTestUtil.h`:

<!-- plan-file: test/ttf_font/TtfFontTestUtil.h -->
```cpp
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "TtfFamily.h"

namespace ttf_test {

/// A font file read through ttf::FileSource, as the SD card serves it, with an I/O error on demand.
struct MemoryFile {
  std::vector<uint8_t> bytes;
  bool failReads = false;
  uint32_t reads = 0;

  ttf::FileSource source() { return {static_cast<uint32_t>(bytes.size()), &MemoryFile::read, this}; }

  static bool read(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
    auto* file = static_cast<MemoryFile*>(ctx);
    ++file->reads;
    if (file->failReads || offset > file->bytes.size() || len > file->bytes.size() - offset) return false;
    std::memcpy(dst, file->bytes.data() + offset, len);
    return true;
  }
};

}  // namespace ttf_test
```

Create `test/ttf_font/TtfFamilyTest.cpp`:

<!-- plan-file: test/ttf_font/TtfFamilyTest.cpp -->
```cpp
// What the registry learns from a TTF file before loading it (its style), and the family identity.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "TtfBuilder.h"
#include "TtfFamily.h"
#include "TtfFontTestUtil.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

int probe(const Bytes& bytes) {
  MemoryFile file{bytes};
  return ttf::probeStyle(file.source());
}

Bytes builtFont(const uint16_t macStyle = 0) {
  Font font;
  font.macStyle = macStyle;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  return buildFont(font);
}

std::array<uint8_t, glyphstore::kIdentityBytes> identityOf(const ttf::FileStamp (&styles)[4]) {
  std::array<uint8_t, glyphstore::kIdentityBytes> out{};
  ttf::familyIdentity(styles, out.data());
  return out;
}

}  // namespace

TEST(TtfFamily, TheBundledBookerlyFilesDeclareTheirStyles) {
  EXPECT_EQ(0, probe(readFont("Bookerly/Bookerly-Regular.ttf")));
  EXPECT_EQ(1, probe(readFont("Bookerly/Bookerly-Bold.ttf")));
  EXPECT_EQ(2, probe(readFont("Bookerly/Bookerly-Italic.ttf")));
  EXPECT_EQ(3, probe(readFont("Bookerly/Bookerly-BoldItalic.ttf")));
}

TEST(TtfFamily, TheStyleIsTheLowTwoBitsOfMacStyle) {
  EXPECT_EQ(0, probe(builtFont(0)));
  EXPECT_EQ(1, probe(builtFont(1)));
  EXPECT_EQ(2, probe(builtFont(2)));
  EXPECT_EQ(3, probe(builtFont(3 | 0x40)));  // other macStyle bits (condensed, ...) do not matter
}

TEST(TtfFamily, CffFontsAndCollectionsAreNotTtfFamilies) {
  EXPECT_EQ(-1, probe(readFont("OpenDyslexic/OpenDyslexic-Regular.otf")));
  Bytes collection = builtFont();
  set32(collection, 0, 0x74746366);  // 'ttcf'
  EXPECT_EQ(-1, probe(collection));
}

TEST(TtfFamily, AFontMissingATableTheEngineNeedsIsRefused) {
  for (const char* table : {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"}) {
    Bytes font = builtFont();
    const size_t record = findRecord(font, table);
    ASSERT_NE(0u, record) << table;
    font[record] = 'X';  // the table is no longer in the directory
    EXPECT_EQ(-1, probe(font)) << table;
  }
}

TEST(TtfFamily, ATableOutsideTheFileIsRefused) {
  Bytes font = builtFont();
  const size_t record = findRecord(font, "glyf");
  set32(font, record + 12, static_cast<uint32_t>(font.size()));  // its length runs past the end
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, ATruncatedFileIsRefused) {
  const Bytes font = builtFont();
  for (const size_t keep : {size_t{0}, size_t{11}, size_t{20}, font.size() / 2, font.size() - 1}) {
    EXPECT_EQ(-1, probe(Bytes(font.begin(), font.begin() + static_cast<std::ptrdiff_t>(keep)))) << keep;
  }
}

TEST(TtfFamily, AHeadWithoutItsMagicIsRefused) {
  Bytes font = builtFont();
  const size_t record = findRecord(font, "head");
  const uint32_t head = static_cast<uint32_t>(font[record + 8] << 24 | font[record + 9] << 16 | font[record + 10] << 8 |
                                              font[record + 11]);
  set32(font, head + 12, 0x12345678);
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, AReadErrorIsRefused) {
  MemoryFile file{readFont("Bookerly/Bookerly-Bold.ttf")};
  file.failReads = true;
  EXPECT_EQ(-1, ttf::probeStyle(file.source()));
}

TEST(TtfFamily, TheProbeReadsOnlyTheDirectoryAndHead) {
  MemoryFile file{readFont("NotoSans/NotoSans-Regular.ttf")};
  ASSERT_EQ(0, ttf::probeStyle(file.source()));
  EXPECT_LE(file.reads, 6u);  // the header, the directory 8 records at a time, head: cheap at discovery
}

TEST(TtfFamily, AFileSourceReadsThroughSeekAndRead) {
  struct SeekReadFile {  // HalFile's two calls
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    bool seekSet(const size_t to) {
      if (to > bytes.size()) return false;
      pos = to;
      return true;
    }
    int read(void* dst, size_t n) {
      n = std::min(n, bytes.size() - pos);
      std::memcpy(dst, bytes.data() + pos, n);
      pos += n;
      return static_cast<int>(n);
    }
  };
  SeekReadFile file{readFont("Bookerly/Bookerly-Italic.ttf")};
  EXPECT_EQ(2, ttf::probeStyle(ttf::fileSource(file, static_cast<uint32_t>(file.bytes.size()))));
  const auto size = static_cast<uint32_t>(file.bytes.size());
  file.bytes.resize(100);  // the file shrank under the source: short reads fail
  EXPECT_EQ(-1, ttf::probeStyle(ttf::fileSource(file, size)));
}

TEST(TtfFamily, TheIdentityChangesWithAnyFileDetail) {
  const ttf::FileStamp base[4] = {{"Lit-Regular.ttf", 450048, 0x5A21, 0x6400},
                                  {"Lit-Bold.ttf", 452000, 0x5A21, 0x6400}};
  const auto identity = identityOf(base);
  EXPECT_EQ(identity, identityOf(base));

  ttf::FileStamp renamed[4] = {base[0], base[1]};
  renamed[1].name = "Lit-Heavy.ttf";
  ttf::FileStamp resized[4] = {base[0], base[1]};
  resized[0].size += 1;
  ttf::FileStamp redated[4] = {base[0], base[1]};
  redated[0].date += 1;
  ttf::FileStamp retimed[4] = {base[0], base[1]};
  retimed[1].time += 2;
  ttf::FileStamp moved[4] = {base[0], {}, base[1]};  // the second file now serves italic, not bold
  ttf::FileStamp fewer[4] = {base[0]};
  for (const auto* other : {&renamed, &resized, &redated, &retimed, &moved, &fewer}) {
    EXPECT_NE(identity, identityOf(*other));
  }
}

TEST(TtfFamily, TheIdentityIsSixteenIndependentBytes) {
  const ttf::FileStamp styles[4] = {{"A.ttf", 1, 2, 3}};
  const auto identity = identityOf(styles);
  // Four lanes with different seeds: no lane repeats another.
  for (int a = 0; a < 4; ++a) {
    for (int b = a + 1; b < 4; ++b) {
      EXPECT_NE(0, std::memcmp(identity.data() + 4 * a, identity.data() + 4 * b, 4)) << a << " " << b;
    }
  }
  EXPECT_EQ(glyphstore::fnv1a(identity.data(), identity.size()), ttf::identityHash(identity.data()));
}

TEST(TtfFamily, TtfFileNamesAreRecognisedInAnyCase) {
  EXPECT_TRUE(ttf::isTtfFileName("Literata-Regular.ttf"));
  EXPECT_TRUE(ttf::isTtfFileName("LITERATA.TTF"));
  EXPECT_TRUE(ttf::isTtfFileName("x.TtF"));
  EXPECT_FALSE(ttf::isTtfFileName(".ttf"));              // no name
  EXPECT_FALSE(ttf::isTtfFileName("Literata.ttf.tmp"));  // an upload in progress
  EXPECT_FALSE(ttf::isTtfFileName("Literata.otf"));      // CFF: not drawn
  EXPECT_FALSE(ttf::isTtfFileName("Literata_14.cpfont"));
  EXPECT_FALSE(ttf::isTtfFileName("Literatattf"));
}

TEST(TtfFamily, TheFirstFileOfEachStyleServesIt) {
  // Sorted by name: Bold, BoldCondensed (also bold), Italic, Regular.
  const int styles[] = {1, 1, 2, 0};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(styles, 4, chosen));
  EXPECT_EQ(3, chosen[0]);
  EXPECT_EQ(0, chosen[1]);  // not the condensed one that sorts after it
  EXPECT_EQ(2, chosen[2]);
  EXPECT_EQ(-1, chosen[3]);  // no bold italic: it falls back
}

TEST(TtfFamily, FilesTheEngineCannotDrawAreIgnored) {
  const int styles[] = {-1, 0, -1};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(styles, 3, chosen));
  EXPECT_EQ(1, chosen[0]);
}

TEST(TtfFamily, ADirectoryWithoutARegularStyleIsNoFamily) {
  const int styles[] = {1, 2, 3, -1};
  int chosen[4];
  EXPECT_FALSE(ttf::assignStyles(styles, 4, chosen));
  EXPECT_FALSE(ttf::assignStyles(nullptr, 0, chosen));
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: CMake fails with `Cannot find source file: .../lib/EpdFont/ttf/TtfFamily.cpp`.

- [ ] **Step 3: Implement**

Create `lib/EpdFont/ttf/TtfFamily.h`:

<!-- plan-file: lib/EpdFont/ttf/TtfFamily.h -->
```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include "GlyphStoreFormat.h"

/// What the registry and the manager need to know about a TrueType family before loading it: which style a
/// file is, and the identity that tells an installed glyph store whether it holds this family.
namespace ttf {

/// A font file read in pieces (on the SD card), not mapped.
struct FileSource {
  uint32_t size = 0;
  /// Copies `len` bytes at `offset` to `dst`. False on an I/O error.
  bool (*read)(void* ctx, uint32_t offset, void* dst, uint32_t len) = nullptr;
  void* ctx = nullptr;
};

/// A FileSource over an open file with seekSet() and read() (HalFile on the device), `size` bytes long. The file
/// must outlive the source.
template <typename File>
FileSource fileSource(File& file, const uint32_t size) {
  return {size,
          [](void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
            auto* f = static_cast<File*>(ctx);
            return f->seekSet(offset) && f->read(dst, len) == static_cast<int>(len);
          },
          &file};
}

/// True for a name ending in ".ttf", in any case. In-progress copies ("x.ttf.tmp") do not count.
bool isTtfFileName(const char* name);

/// The style a TrueType file declares in head.macStyle: bit 0 bold, bit 1 italic, as EpdFontFamily::Style.
/// -1 when it is not a font the engine draws: not TrueType (CFF, a collection), a table it needs missing or
/// outside the file, or a read error.
int probeStyle(const FileSource& file);

/// Which file serves each style of a directory's TrueType files. `styles[i]` is probeStyle() of file i, the
/// files sorted by name; the first file of each style wins, so a family does not change with the card's listing
/// order. `out[style]` is that file's index, or -1. False when no file is regular: not a family the reader can
/// use, since every missing style falls back to regular.
bool assignStyles(const int* styles, size_t count, int (&out)[4]);

/// Bump whenever the engine's output changes for the same font (the rasterizer, the 150 dpi sizing, the 2-bit
/// thresholds): the identity changes with it, so every installed store is rebuilt.
constexpr uint8_t kRenderVersion = 1;

/// One style's file as the card lists it.
struct FileStamp {
  const char* name = nullptr;  ///< nullptr: the family has no file for this style
  uint32_t size = 0;
  uint16_t date = 0;  ///< FAT modification date and time
  uint16_t time = 0;
};

/// The family identity (spec 4.2): FNV-1a over each style's file name, size and modification time, and
/// kRenderVersion, in four lanes with different seeds. Any change to a file gives a new identity.
void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes]);

/// The identity folded to 32 bits, to seed the family's font IDs.
inline uint32_t identityHash(const uint8_t identity[glyphstore::kIdentityBytes]) {
  return glyphstore::fnv1a(identity, glyphstore::kIdentityBytes);
}

}  // namespace ttf
```

Create `lib/EpdFont/ttf/TtfFamily.cpp`:

<!-- plan-file: lib/EpdFont/ttf/TtfFamily.cpp -->
```cpp
#include "TtfFamily.h"

#include <algorithm>
#include <cstring>

namespace ttf {
namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
         p[3];
}
constexpr uint32_t tag(const char (&t)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(t[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(t[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(t[2])) << 8 | static_cast<uint8_t>(t[3]);
}

// The tables parseFace() needs; a file without one of them can never draw a glyph.
constexpr uint32_t kRequired[] = {tag("cmap"), tag("glyf"), tag("head"), tag("hhea"),
                                  tag("hmtx"), tag("loca"), tag("maxp")};
constexpr uint32_t kAllRequired = (1u << (sizeof kRequired / sizeof kRequired[0])) - 1;
constexpr uint16_t kMaxTables = 256;  // real fonts have under 40; this bounds a hostile directory's reads
constexpr uint32_t kHeadBytes = 54;
constexpr uint32_t kHeadMagic = 0x5F0F3CF5;
constexpr uint32_t kMacStyleOffset = 44;

void putLe(uint8_t* out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

}  // namespace

bool isTtfFileName(const char* name) {
  const size_t len = std::strlen(name);
  if (len <= 4 || name[len - 4] != '.') return false;
  const char* ext = name + len - 3;
  return (ext[0] | 0x20) == 't' && (ext[1] | 0x20) == 't' && (ext[2] | 0x20) == 'f';
}

bool assignStyles(const int* styles, const size_t count, int (&out)[4]) {
  for (int& file : out) file = -1;
  for (size_t i = 0; i < count; ++i) {
    if (styles[i] >= 0 && styles[i] < 4 && out[styles[i]] < 0) out[styles[i]] = static_cast<int>(i);
  }
  return out[0] >= 0;
}

int probeStyle(const FileSource& file) {
  uint8_t header[12];
  if (!file.read || file.size < sizeof header || !file.read(file.ctx, 0, header, sizeof header)) return -1;
  const uint32_t version = be32(header);
  if (version != 0x00010000 && version != tag("true")) return -1;  // CFF ('OTTO') and collections ('ttcf')
  const uint16_t numTables = be16(header + 4);
  if (numTables == 0 || numTables > kMaxTables || sizeof header + 16u * numTables > file.size) return -1;

  uint32_t found = 0;
  uint32_t headOffset = 0;
  uint8_t records[8 * 16];
  for (uint16_t first = 0; first < numTables; first += 8) {
    const uint16_t n = std::min<uint16_t>(8, static_cast<uint16_t>(numTables - first));
    if (!file.read(file.ctx, sizeof header + 16u * first, records, 16u * n)) return -1;
    for (uint16_t i = 0; i < n; ++i) {
      const uint8_t* record = records + 16 * i;
      const uint32_t tableTag = be32(record);
      const uint32_t offset = be32(record + 8);
      const uint32_t length = be32(record + 12);
      for (uint32_t k = 0; k < sizeof kRequired / sizeof kRequired[0]; ++k) {
        if (tableTag != kRequired[k]) continue;
        if (offset > file.size || length > file.size - offset) return -1;
        if (tableTag == tag("head")) {
          if (length < kHeadBytes) return -1;
          headOffset = offset;
        }
        found |= 1u << k;
      }
    }
  }
  if (found != kAllRequired) return -1;

  uint8_t head[kHeadBytes];
  if (!file.read(file.ctx, headOffset, head, sizeof head) || be32(head + 12) != kHeadMagic) return -1;
  return be16(head + kMacStyleOffset) & 3;
}

void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes]) {
  for (uint32_t lane = 0; lane < glyphstore::kIdentityBytes / 4; ++lane) {
    uint32_t hash = 2166136261u ^ (lane * 0x9E3779B9u);
    for (uint8_t style = 0; style < 4; ++style) {
      const FileStamp& file = styles[style];
      const uint8_t marker = file.name ? style : static_cast<uint8_t>(0x80 | style);
      hash = glyphstore::fnv1a(&marker, 1, hash);
      if (!file.name) continue;
      hash = glyphstore::fnv1a(file.name, std::strlen(file.name) + 1, hash);
      uint8_t fields[8];
      putLe(fields, file.size, 4);
      putLe(fields + 4, file.date, 2);
      putLe(fields + 6, file.time, 2);
      hash = glyphstore::fnv1a(fields, sizeof fields, hash);
    }
    hash = glyphstore::fnv1a(&kRenderVersion, 1, hash);
    putLe(out + 4 * lane, hash, 4);
  }
}

}  // namespace ttf
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 16 tests.`

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/TtfFamily.h lib/EpdFont/ttf/TtfFamily.cpp test/ttf_font test/CMakeLists.txt \
        test/ttf_rasterizer/TtfBuilder.h test/ttf_rasterizer/CMakeLists.txt
git commit -m "feat(ttf): a TTF file's style from macStyle, and the family identity"
```

### Task 4: TtfFont, a TrueType family on the glyph store

The family object (spec 3.2). Read the whole task before writing; these are the rules the tests pin:

- **`load()`** opens the store. `NotInstalled`, `OtherFamily` and `MapFailed` mean install first: plan the layout
  from the flash's size and free mapping pages (nothing of the store mapped), call `onBusy`, then copy each present
  style in 4 KB chunks into its style block. Present styles take blocks 0, 1, ... in style order, at install and at
  every load, so the mapping never needs storing. A family too large for flash mode is refused before anything is
  erased. Each style is then parsed from its mapping; one that does not parse is left out; regular is required.
- **The scratch block** is planned for `kMaxPointSize` over every present style (`mergePlans`). Its output region
  doubles as the `GlyphBatch` buffer, so it grows by the 20-byte record header plus 3 bytes of alignment (capped
  at `ttf::kMaxOutputBytes`): any glyph that loads then fits an empty batch.
- **`setBodySize()`** computes each style's line metrics and refuses the size if any `advanceY` is outside
  1-255 (`EpdFontData::advanceY` is a `uint8_t`). It fills the four `EpdFontData` (no intervals, `is2Bit`, `bitmap`
  = the log base, `provider` = the style's adapter) and claims the size index.
- **`glyph()`**, the miss path every lookup takes:
  1. A default-ignorable codepoint gets a static empty glyph (no advance, no ink), never stored.
  2. With a store key (store open, size index claimed, re-claimed after a wipe): `find()`. Found returns the
     record in flash; NoGlyph returns nullptr.
  3. Otherwise render: map the style (flash mode maps one style at a time, so the pointer is fetched for every
     render); `glyphIndex()` 0 means the font has none: mark it missing (unless `ioError()`); `load()`; a
     permanent failure (and a glyph whose left or top does not fit `int8`) is marked missing and logged (at
     most 16 lines per session); a transient one is just retried next time.
  4. Stage a one-record batch in the output region, `rasterize()` into `add()`'s pointer (on failure:
     `dropLast()`), `commit()`, and return the stored record.
  5. A commit that reports Full or WriteFailed switches to ring mode: new glyphs are drawn into the 8-slot heap
     ring, unstored, until the next wipe. Glyphs already stored are still read from the store.
- **`ownsBitmap()`** is an address-range check against the ring (every draw from the store pays it), plus the
  empty glyph.
- **`wipeIfPending()`** (book open) wipes, frees the ring buffers, leaves ring mode, refreshes each face's
  `bitmap` and claims the size again.

**Files:**
- Create: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`
- Create: `test/ttf_font/TtfFontTest.cpp`
- Modify: `test/ttf_font/CMakeLists.txt`

**Interfaces:**
- Consumes: `GlyphBatch::dropLast()`, `glyphstore::recordBytes()` (Task 1); `ttf::FileSource` (Task 3);
  `FlashGlyphStore`, `glyphstore::planLayout`, `ttf::parseFace`, `ttf::glyphIndex`, `ttf::lineMetrics`,
  `ttf::planScratch`, `ttf::mergePlans`, `ttf::Rasterizer` (Stages 2a/2b).
- Produces (`class TtfFont`, header `ttf/TtfFont.h`):
  - `static constexpr uint8_t kMaxPointSize = 26;` and `kRingSlots = 8;`
  - `struct Hooks { bool (*renderLockHeld)(); void (*yield)(); }` and `static void setHooks(const Hooks&)`
  - `explicit TtfFont(NorFlash& flash)`
  - `bool load(const ttf::FileSource (&files)[4], const uint8_t identity[16], const std::function<void()>& onBusy = {})`
  - `bool setBodySize(uint8_t pointSize)`, `uint8_t bodySize() const`
  - `const EpdFont* font(uint8_t style) const` (nullptr for a style the family lacks)
  - `bool wipeIfPending(const std::function<void()>& onBusy = {})`, `bool ringMode() const`
  - `uint32_t renders() const`, `uint32_t storeHits() const`
  - `const EpdGlyph* glyph(uint8_t style, uint32_t codepoint)`,
    `bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const`

- [ ] **Step 1: Write the failing tests**

Replace `test/ttf_font/CMakeLists.txt` (the engine, the store and the font family join the target):

<!-- plan-file: test/ttf_font/CMakeLists.txt -->
```cmake
# TtfFont (lib/EpdFont/ttf): TrueType families on the glyph store, from the file probe to drawn pages.

add_executable(TtfFontTest
  TtfFamilyTest.cpp
  TtfFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/FlashGlyphStore.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

# test/shims before lib/hal: the host HalStorage, but the real NorFlash.h.
target_include_directories(TtfFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/ttf_rasterizer
  ${REPO_ROOT}/test/glyph_store
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(TtfFontTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfFontTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# The sanitizers ttf_rasterizer found for this toolchain (ASan + UBSan on CI's Linux GCC).
target_compile_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})

gtest_discover_tests(TtfFontTest DISCOVERY_TIMEOUT 60)
```

Create `test/ttf_font/TtfFontTest.cpp`:

<!-- plan-file: test/ttf_font/TtfFontTest.cpp -->
```cpp
// TtfFont: a TrueType family installed into the glyph store, its glyphs rendered once and then read from flash.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpdFontFamily.h"
#include "SimNorFlash.h"
#include "TtfBuilder.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

constexpr uint32_t kFlashBytes = 3538944 - 65536;  // the X3's font area: spiffs below the language slot
constexpr uint8_t kRegular = 0;
constexpr uint8_t kBold = 1;
constexpr uint8_t kItalic = 2;
constexpr uint8_t kIdentityA[glyphstore::kIdentityBytes] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'A'};
constexpr uint8_t kIdentityB[glyphstore::kIdentityBytes] = {'f', 'a', 'm', 'i', 'l', 'y', '-', 'B'};

/// What the rasterizer draws for `cp` at `points`, straight from the file: what a stored glyph must equal.
struct Reference {
  EpdGlyph metrics{};
  std::vector<uint8_t> bitmap;
};

Reference reference(const std::vector<uint8_t>& file, const uint32_t cp, const uint8_t points) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(ttf::planScratch(face, ttf::ppemFromPoints(26), ttf::kPoolBytesFlash)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.render(bytes, face, ttf::glyphIndex(bytes, face, cp),
                                               ttf::ppemFromPoints(points), &ref.metrics));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

/// A glyph's bitmap, wherever TtfFont keeps it: the ring, or the store's log.
std::vector<uint8_t> bitmapOf(const TtfFont& font, const uint8_t style, const EpdGlyph* glyph) {
  const uint8_t* bitmap = nullptr;
  if (!font.ownsBitmap(glyph, bitmap)) bitmap = font.font(style)->data->bitmap + glyph->dataOffset;
  return bitmap ? std::vector<uint8_t>(bitmap, bitmap + glyph->dataLength) : std::vector<uint8_t>{};
}

void expectDrawnAs(const Reference& ref, const TtfFont& font, const uint8_t style, const EpdGlyph* glyph) {
  ASSERT_NE(nullptr, glyph);
  EXPECT_EQ(ref.metrics.width, glyph->width);
  EXPECT_EQ(ref.metrics.height, glyph->height);
  EXPECT_EQ(ref.metrics.left, glyph->left);
  EXPECT_EQ(ref.metrics.top, glyph->top);
  EXPECT_EQ(ref.metrics.advanceX, glyph->advanceX);
  EXPECT_EQ(ref.bitmap, bitmapOf(font, style, glyph));
}

int yields = 0;
int lockChecks = 0;

class TtfFontTest : public ::testing::Test {
 protected:
  void SetUp() override {
    files[0].bytes = readFont("Bookerly/Bookerly-Regular.ttf");
    files[1].bytes = readFont("Bookerly/Bookerly-Bold.ttf");
    files[2].bytes = readFont("Bookerly/Bookerly-Italic.ttf");
    files[3].bytes = readFont("Bookerly/Bookerly-BoldItalic.ttf");
    yields = lockChecks = 0;
  }
  void TearDown() override {
    TtfFont::setHooks({});
    EXPECT_EQ(0u, flash.violations());
    EXPECT_EQ(0u, flash.reprograms());
  }

  /// A TtfFont over the test's flash, loaded and set to `points`: what each boot sees. Null when it fails.
  std::unique_ptr<TtfFont> open(const uint8_t* identity = kIdentityA, const uint8_t points = 14) {
    auto font = std::make_unique<TtfFont>(flash);
    ttf::FileSource sources[4];
    for (int s = 0; s < 4; ++s) {
      if (!files[s].bytes.empty()) sources[s] = files[s].source();
    }
    if (!font->load(sources, identity, [this] { ++busy; })) return nullptr;
    if (points != 0 && !font->setBodySize(points)) return nullptr;
    return font;
  }

  /// A one-style family built in memory, so a test can give a glyph any outline.
  void useBuiltFont(const Font& font) {
    files[0].bytes = buildFont(font);
    for (int s = 1; s < 4; ++s) files[s].bytes.clear();
  }

  SimNorFlash flash{kFlashBytes};
  MemoryFile files[4];
  int busy = 0;
};

}  // namespace

TEST_F(TtfFontTest, TheFirstLoadInstallsTheFamilyAndTheNextReusesIt) {
  ASSERT_NE(nullptr, open());
  EXPECT_EQ(1, busy);
  const uint32_t erases = flash.totalSectorErases();
  const auto again = open();
  ASSERT_NE(nullptr, again);
  EXPECT_EQ(1, busy);  // not installed twice
  EXPECT_EQ(erases, flash.totalSectorErases());
  EXPECT_NE(nullptr, again->font(kBold));
}

TEST_F(TtfFontTest, AnotherFamilyIsInstalledOverTheStore) {
  ASSERT_NE(nullptr, open(kIdentityA));
  ASSERT_NE(nullptr, open(kIdentityB));
  EXPECT_EQ(2, busy);
}

TEST_F(TtfFontTest, AStoreThatNoLongerMapsIsInstalledAgain) {
  ASSERT_NE(nullptr, open());
  flash.setFreePages(20);  // a firmware update left fewer mapping pages than the install planned for
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(2, busy);
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *font, kRegular, font->glyph(kRegular, 'A'));
}

TEST_F(TtfFontTest, AFamilyWithoutARegularStyleIsRefused) {
  files[0].bytes.clear();
  EXPECT_EQ(nullptr, open());
  EXPECT_EQ(0u, flash.totalSectorErases());
}

TEST_F(TtfFontTest, AFamilyTooLargeForThePartitionIsRefusedBeforeAnyErase) {
  SimNorFlash small{1024 * 1024};
  TtfFont font(small);
  ttf::FileSource sources[4];
  for (int s = 0; s < 4; ++s) sources[s] = files[s].source();
  EXPECT_FALSE(font.load(sources, kIdentityA, [this] { ++busy; }));
  EXPECT_EQ(0, busy);
  EXPECT_EQ(0u, small.totalSectorErases());
}

TEST_F(TtfFontTest, AStyleTheEngineCannotReadIsLeftOut) {
  files[kItalic].bytes.assign(5000, 0x41);  // the file changed after discovery: not a font any more
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->font(kItalic));
  EXPECT_NE(nullptr, font->font(kBold));
}

TEST_F(TtfFontTest, StoredGlyphsAreWhatTheRasterizerDraws) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'g'}, uint32_t{'W'}, uint32_t{0xE9}, uint32_t{' '}}) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 14), *font, kRegular, font->glyph(kRegular, cp));
    expectDrawnAs(reference(files[kBold].bytes, cp, 14), *font, kBold, font->glyph(kBold, cp));
  }
}

TEST_F(TtfFontTest, AGlyphIsRenderedOnceAndThenReadFromTheStore) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdGlyph* first = font->glyph(kRegular, 'A');
  const uint32_t writes = flash.programCalls();
  const EpdGlyph* second = font->glyph(kRegular, 'A');
  EXPECT_EQ(first, second);  // the record in the log, in place
  EXPECT_EQ(1u, font->renders());
  EXPECT_EQ(1u, font->storeHits());
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_FALSE(font->ringMode());
}

TEST_F(TtfFontTest, StoredGlyphsSurviveTheNextBoot) {
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    ASSERT_NE(nullptr, font->glyph(kBold, 'Q'));
  }
  const auto font = open();
  ASSERT_NE(nullptr, font);
  expectDrawnAs(reference(files[kBold].bytes, 'Q', 14), *font, kBold, font->glyph(kBold, 'Q'));
  EXPECT_EQ(0u, font->renders());
  EXPECT_EQ(1u, font->storeHits());
}

TEST_F(TtfFontTest, ACodepointTheFontLacksIsAskedOnlyOnce) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->glyph(kRegular, 0x4E2D));  // Bookerly has no CJK
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 0x4E2D));
  EXPECT_EQ(writes, flash.programCalls());  // the "missing" slot answered
}

TEST_F(TtfFontTest, DefaultIgnorableCodepointsHaveNoAdvanceAndNoInk) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const uint32_t writes = flash.programCalls();
  const EpdGlyph* zwsp = font->glyph(kRegular, 0x200B);
  ASSERT_NE(nullptr, zwsp);
  EXPECT_EQ(0, zwsp->advanceX);
  EXPECT_EQ(0, zwsp->width);
  EXPECT_EQ(0, zwsp->height);
  const uint8_t* bitmap = &kIdentityA[0];
  EXPECT_TRUE(font->ownsBitmap(zwsp, bitmap));
  EXPECT_EQ(nullptr, bitmap);
  EXPECT_EQ(writes, flash.programCalls());  // nothing stored for it
}

TEST_F(TtfFontTest, EachBodySizeHasItsOwnGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdGlyph* at14 = font->glyph(kRegular, 'M');
  ASSERT_TRUE(font->setBodySize(20));
  expectDrawnAs(reference(files[kRegular].bytes, 'M', 20), *font, kRegular, font->glyph(kRegular, 'M'));
  ASSERT_TRUE(font->setBodySize(14));
  EXPECT_EQ(at14, font->glyph(kRegular, 'M'));  // still stored under its own size
  EXPECT_EQ(2u, font->renders());
}

TEST_F(TtfFontTest, TheSmallestReaderSizeIsDrawnNatively) {
  const auto font = open(kIdentityA, 8);
  ASSERT_NE(nullptr, font);
  for (const uint32_t cp : {uint32_t{'a'}, uint32_t{'W'}, uint32_t{0xE9}}) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 8), *font, kRegular, font->glyph(kRegular, cp));
  }
}

TEST_F(TtfFontTest, TheLineMetricsAreTheFacesAtTheBodySize) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  FontBytes bytes(files[kRegular].bytes.data(), static_cast<uint32_t>(files[kRegular].bytes.size()));
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  const ttf::LineMetrics expected = ttf::lineMetrics(face, ttf::ppemFromPoints(14));
  const EpdFontData* data = font->font(kRegular)->data;
  EXPECT_EQ(expected.advanceY, data->advanceY);
  EXPECT_EQ(expected.ascender, data->ascender);
  EXPECT_EQ(expected.descender, data->descender);
  EXPECT_TRUE(data->is2Bit);
  EXPECT_EQ(0u, data->intervalCount);  // every lookup goes to the provider
  EXPECT_NE(nullptr, data->provider);
}

TEST_F(TtfFontTest, SizesOutsideTheReaderRangeAreRefused) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_FALSE(font->setBodySize(0));
  EXPECT_FALSE(font->setBodySize(TtfFont::kMaxPointSize + 1));
  EXPECT_EQ(14, font->bodySize());  // unchanged
  EXPECT_TRUE(font->setBodySize(TtfFont::kMaxPointSize));
}

TEST_F(TtfFontTest, AFaceWhoseLineHeightDoesNotFitIsRefused) {
  Font built;
  built.ascender = 5000;  // a 282 px line at the largest size
  built.glyphs.resize(2);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 700)};
  built.cmap = {{'A', 1}};
  useBuiltFont(built);
  const auto font = open(kIdentityA, 0);
  ASSERT_NE(nullptr, font);
  EXPECT_TRUE(font->setBodySize(10));
  EXPECT_FALSE(font->setBodySize(TtfFont::kMaxPointSize));
  EXPECT_EQ(10, font->bodySize());
}

TEST_F(TtfFontTest, AWriteFailureSwitchesToTheRingWithTheSameGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));  // stored before the failure
  flash.failNextProgram();
  const EpdGlyph* b = font->glyph(kRegular, 'B');
  EXPECT_TRUE(font->ringMode());
  expectDrawnAs(reference(files[kRegular].bytes, 'B', 14), *font, kRegular, b);
  const uint8_t* bitmap = nullptr;
  EXPECT_TRUE(font->ownsBitmap(b, bitmap));
  // Stored glyphs are still read from the store; new ones are drawn without a write.
  const uint32_t writes = flash.programCalls();
  EXPECT_FALSE(font->ownsBitmap(font->glyph(kRegular, 'A'), bitmap));
  expectDrawnAs(reference(files[kRegular].bytes, 'C', 14), *font, kRegular, font->glyph(kRegular, 'C'));
  EXPECT_EQ(writes, flash.programCalls());
}

TEST_F(TtfFontTest, RingRecordsStayValidForTheNextSevenLookups) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  flash.failNextProgram();
  const std::string letters = "BCDEFGHI";
  std::vector<const EpdGlyph*> glyphs;
  for (const char c : letters) glyphs.push_back(font->glyph(kRegular, static_cast<uint8_t>(c)));
  ASSERT_TRUE(font->ringMode());
  for (size_t i = 0; i < letters.size(); ++i) {
    expectDrawnAs(reference(files[kRegular].bytes, static_cast<uint8_t>(letters[i]), 14), *font, kRegular, glyphs[i]);
  }
}

TEST_F(TtfFontTest, AWipeAtTheNextBookOpenEndsTheRing) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'B'));
  ASSERT_TRUE(font->ringMode());
  busy = 0;
  EXPECT_TRUE(font->wipeIfPending([this] { ++busy; }));
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(font->ringMode());
  EXPECT_FALSE(font->wipeIfPending());  // nothing pending now
  const uint32_t renders = font->renders();
  const EpdGlyph* a = font->glyph(kRegular, 'A');  // the wipe emptied the store: rendered and stored again
  EXPECT_EQ(renders + 1, font->renders());
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(font->ownsBitmap(a, bitmap));
  expectDrawnAs(reference(files[kRegular].bytes, 'A', 14), *font, kRegular, a);
}

TEST_F(TtfFontTest, AFullStoreSwitchesToTheRingAndTheLoadAfterItWipes) {
  flash.setFreePages(16);  // the smallest log the planner makes: 4 pages
  {
    const auto font = open(kIdentityA, TtfFont::kMaxPointSize);
    ASSERT_NE(nullptr, font);
    for (uint32_t cp = 0x21; cp < 0x3000 && !font->ringMode(); ++cp) font->glyph(kRegular, cp);
    ASSERT_TRUE(font->ringMode());
    expectDrawnAs(reference(files[kRegular].bytes, 0x2026, TtfFont::kMaxPointSize), *font, kRegular,
                  font->glyph(kRegular, 0x2026));
  }
  busy = 0;
  const auto font = open(kIdentityA, TtfFont::kMaxPointSize);  // the next boot finishes the pending wipe
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(font->ringMode());
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(font->ownsBitmap(font->glyph(kRegular, 'A'), bitmap));
  EXPECT_EQ(1u, font->renders());
}

TEST_F(TtfFontTest, ABrokenGlyphIsMarkedMissingAndNotTriedAgain) {
  Font built;
  built.glyphs.resize(3);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 700)};
  built.glyphs[2].raw = {0x00, 0x01};  // one contour, then nothing: Malformed
  built.cmap = {{'A', 1}, {'B', 2}};
  useBuiltFont(built);
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'B'));
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'B'));
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_NE(nullptr, font->glyph(kRegular, 'A'));
  EXPECT_EQ(nullptr, font->font(kBold));  // a one-style family
}

TEST_F(TtfFontTest, AGlyphReachingTooFarFromThePenIsMissing) {
  Font built;
  built.glyphs.resize(3);
  built.glyphs[0].contours = {rect(0, 0, 400, 600)};
  built.glyphs[1].contours = {rect(50, 0, 450, 700)};
  built.glyphs[2].contours = {rect(0, 2900, 100, 3000)};  // 163 px above the baseline at 26 pt: past int8
  built.cmap = {{'A', 1}, {'T', 2}};
  useBuiltFont(built);
  const auto font = open(kIdentityA, TtfFont::kMaxPointSize);
  ASSERT_NE(nullptr, font);
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'T'));
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 'T'));
  EXPECT_EQ(writes, flash.programCalls());  // marked missing for good
  ASSERT_TRUE(font->setBodySize(10));       // 63 px: drawn
  EXPECT_NE(nullptr, font->glyph(kRegular, 'T'));
}

TEST_F(TtfFontTest, TextMeasuresTheSameColdWarmAndFromTheRing) {
  const char* text = "Sphinx of black quartz, judge my vow";
  int coldWidth = 0, coldHeight = 0;
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    font->font(kRegular)->getTextDimensions(text, &coldWidth, &coldHeight);
    ASSERT_GT(coldWidth, 0);
  }
  int warmWidth = 0, warmHeight = 0;
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    font->font(kRegular)->getTextDimensions(text, &warmWidth, &warmHeight);
    EXPECT_EQ(0u, font->renders());
  }
  int ringWidth = 0, ringHeight = 0;
  {
    const auto font = open(kIdentityB);  // a fresh store...
    ASSERT_NE(nullptr, font);
    flash.failNextProgram();  // ...that fails at its first write
    font->font(kRegular)->getTextDimensions(text, &ringWidth, &ringHeight);
    EXPECT_TRUE(font->ringMode());
  }
  EXPECT_EQ(coldWidth, warmWidth);
  EXPECT_EQ(coldHeight, warmHeight);
  EXPECT_EQ(coldWidth, ringWidth);
  EXPECT_EQ(coldHeight, ringHeight);
}

TEST_F(TtfFontTest, TheFamilyServesEpdFontFamilyStyles) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFontFamily family(font->font(0), font->font(1), font->font(2), font->font(3));
  const EpdGlyphRef bold = family.getGlyph('W', EpdFontFamily::BOLD);
  ASSERT_TRUE(bold);
  EXPECT_EQ(reference(files[kBold].bytes, 'W', 14).metrics.advanceX, bold.advanceX);
  EXPECT_NE(family.getGlyph('W', EpdFontFamily::REGULAR).advanceX, bold.advanceX);
}

TEST_F(TtfFontTest, TheAppHooksAreCalled) {
  TtfFont::setHooks({[] {
                       ++lockChecks;
                       return true;
                     },
                     [] { ++yields; }});
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_GT(yields, 0);  // between install chunks
  EXPECT_EQ(0, lockChecks);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  EXPECT_EQ(1, lockChecks);  // a render checks the lock; a store hit does not need it
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  EXPECT_EQ(1, lockChecks);
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: CMake fails with `Cannot find source file: .../lib/EpdFont/ttf/TtfFont.cpp`.

- [ ] **Step 3: Implement**

Create `lib/EpdFont/ttf/TtfFont.h`:

<!-- plan-file: lib/EpdFont/ttf/TtfFont.h -->
```cpp
#pragma once

#include <NorFlash.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "FlashGlyphStore.h"
#include "GlyphProvider.h"
#include "TtfFace.h"
#include "TtfFamily.h"
#include "TtfRasterizer.h"

/// One TrueType family drawn from the glyph store (spec 3.2), in flash mode: every style's whole TTF is copied
/// into the `spiffs` partition when the family is installed, and read there in place. A glyph is rendered the
/// first time it is looked up and stored, so later lookups, in this session and the next, read it from flash.
///
/// It serves one face, the body size: four EpdFontData without tables, so every lookup goes to a per-style
/// GlyphProvider adapter. Kerning, ligatures and the size-ladder rungs come in later stages.
///
/// Not thread-safe: every call runs under RenderLock (spec 3.4).
class TtfFont {
 public:
  /// The largest reader size: the scratch block is planned for it, and setBodySize() refuses anything larger.
  static constexpr uint8_t kMaxPointSize = 26;
  static constexpr uint8_t kRingSlots = 8;

  /// Supplied by the app; both may stay null (host tests).
  struct Hooks {
    bool (*renderLockHeld)() = nullptr;  ///< debug check of spec 3.4, logged once when false
    void (*yield)() = nullptr;           ///< called between glyphs and between install chunks
  };
  static void setHooks(const Hooks& hooks);

  explicit TtfFont(NorFlash& flash);
  TtfFont(const TtfFont&) = delete;
  TtfFont& operator=(const TtfFont&) = delete;

  /// Opens the store for this family. When the partition holds no store, another family's, or one whose
  /// mappings no longer fit, installs it first: calls `onBusy`, then copies every style from `files` (size 0:
  /// the family has no such style). Finishes a pending wipe, behind `onBusy` too. False when the family cannot
  /// be drawn: no regular style, one the engine cannot read, more than the partition holds, or out of memory.
  bool load(const ttf::FileSource (&files)[4], const uint8_t identity[glyphstore::kIdentityBytes],
            const std::function<void()>& onBusy = {});

  /// Serves `pointSize`: the face metrics at that size, and its size index in the store. False when it is 0
  /// or over kMaxPointSize, or its line height does not fit EpdFontData (advanceY over 255).
  bool setBodySize(uint8_t pointSize);
  uint8_t bodySize() const { return pointSize_; }
  /// The body face's font for `style`; nullptr for a style the family lacks (EpdFontFamily then falls back).
  const EpdFont* font(uint8_t style) const;

  /// Runs a pending wipe (the store filled up, or a write failed), calling `onBusy` first. Only where no glyph
  /// record of this font is in use: at book open. True when a wipe ran.
  bool wipeIfPending(const std::function<void()>& onBusy = {});
  /// New glyphs are drawn without being stored until the next wipe.
  bool ringMode() const { return ringMode_; }

  uint32_t renders() const { return renders_; }
  uint32_t storeHits() const { return hits_; }

  // The provider side; the per-style adapters forward here.
  const EpdGlyph* glyph(uint8_t style, uint32_t codepoint);
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;

 private:
  class StyleProvider final : public GlyphProvider {
   public:
    TtfFont* owner = nullptr;
    uint8_t style = 0;
    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return owner->glyph(style, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      return owner->ownsBitmap(glyph, bitmap);
    }
  };

  struct Style {
    bool present = false;
    uint8_t block = 0;  // the store's style block
    uint32_t bytes = 0;
    ttf::Face face;
    EpdFontData data{};
    EpdFont font{&data};
    StyleProvider provider;
  };

  bool install(const ttf::FileSource (&files)[4], const uint8_t* identity, const std::function<void()>& onBusy);
  bool fontBytes(uint8_t style, FontBytes* bytes);
  bool storeKey(uint8_t style, uint32_t codepoint, uint32_t* key);
  void claimSize();
  const EpdGlyph* render(uint8_t style, uint32_t codepoint, bool haveKey, uint32_t key);
  const EpdGlyph* drawToRing(const EpdGlyph& metrics);
  void failed(uint8_t style, uint32_t codepoint, ttf::Status status, bool haveKey, uint32_t key);
  void noteCommit(FlashGlyphStore::Commit result);
  uint32_t batchCapacity() const;
  void checkLock() const;

  NorFlash& flash_;
  FlashGlyphStore store_;
  ttf::Rasterizer rasterizer_;
  Style styles_[4];
  uint32_t ppem_ = 0;
  uint8_t pointSize_ = 0;
  uint8_t sizeIndex_ = 0;
  bool sizeValid_ = false;
  uint32_t sizeGeneration_ = 0;
  bool ringMode_ = false;

  // The ring: glyphs drawn without being stored. Their records live until the slot is reused, 8 lookups later.
  EpdGlyph ringGlyphs_[kRingSlots] = {};
  std::unique_ptr<uint8_t[]> ringBitmaps_[kRingSlots];
  uint16_t ringCapacity_[kRingSlots] = {};
  uint8_t ringNext_ = 0;

  uint32_t renders_ = 0;
  uint32_t hits_ = 0;
  uint8_t failuresLogged_ = 0;
};
```

Create `lib/EpdFont/ttf/TtfFont.cpp`:

<!-- plan-file: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
#include "TtfFont.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdint>

using glyphstore::kIdentityBytes;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

TtfFont::Hooks hooks;

// What a default-ignorable codepoint draws as: no advance and no ink, as the converter makes them (spec 8.1).
const EpdGlyph kIgnorable{};

constexpr uint32_t kCopyBytes = 4096;  // install: one SD read and one flash write per chunk
constexpr uint8_t kMaxFailuresLogged = 16;

// EpdGlyphRef carries left and top as int8 (spec 8.1): a glyph reaching further from the pen cannot be placed.
ttf::Status placeable(const ttf::Status loaded, const EpdGlyph& metrics) {
  if (loaded != ttf::Status::Ok) return loaded;
  const bool fits =
      metrics.left >= INT8_MIN && metrics.left <= INT8_MAX && metrics.top >= INT8_MIN && metrics.top <= INT8_MAX;
  return fits ? loaded : ttf::Status::Unsupported;
}

}  // namespace

void TtfFont::setHooks(const Hooks& newHooks) { hooks = newHooks; }

TtfFont::TtfFont(NorFlash& flash) : flash_(flash), store_(flash) {
  for (uint8_t s = 0; s < 4; ++s) {
    styles_[s].provider.owner = this;
    styles_[s].provider.style = s;
  }
}

void TtfFont::checkLock() const {
  static bool logged = false;
  if (hooks.renderLockHeld && !logged && !hooks.renderLockHeld()) {
    logged = true;
    LOG_ERR("TTF", "glyph rendered without RenderLock (spec 3.4)");
  }
}

bool TtfFont::load(const ttf::FileSource (&files)[4], const uint8_t identity[kIdentityBytes],
                   const std::function<void()>& onBusy) {
  if (files[0].size == 0) {
    LOG_ERR("TTF", "family has no regular style");
    return false;
  }
  OpenResult opened = store_.open(identity);
  if (opened == OpenResult::NotInstalled || opened == OpenResult::OtherFamily || opened == OpenResult::MapFailed) {
    if (!install(files, identity, onBusy)) return false;
    opened = store_.open(identity);
  }
  if (opened != OpenResult::Ready) {
    LOG_ERR("TTF", "glyph store did not open (%u)", static_cast<unsigned>(opened));
    return false;
  }

  // Present styles take the store's style blocks in order, at install as here.
  ttf::ScratchPlan plan;
  uint8_t block = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    Style& style = styles_[s];
    // Field by field: assigning a fresh Style would point its EpdFont at the temporary's data.
    style.present = false;
    style.face = {};
    style.data = EpdFontData{};
    if (files[s].size == 0) continue;
    style.block = block++;
    glyphstore::StyleInfo info{};
    if (!store_.styleInfo(style.block, info)) return false;
    style.bytes = info.size;
    FontBytes bytes;
    if (!fontBytes(s, &bytes) || ttf::parseFace(bytes, &style.face) != ttf::FaceStatus::Ok) {
      LOG_ERR("TTF", "style %u is not a font the engine draws", s);
      continue;
    }
    style.present = true;
    plan =
        ttf::mergePlans(plan, ttf::planScratch(style.face, ttf::ppemFromPoints(kMaxPointSize), ttf::kPoolBytesFlash));
  }
  if (!styles_[0].present) return false;

  // The output region doubles as the batch buffer, so it also holds the record header of its largest bitmap.
  plan.outputBytes = std::min(plan.outputBytes + glyphstore::kRecordHeaderBytes + 3, ttf::kMaxOutputBytes);
  if (!rasterizer_.allocate(plan)) return false;
  if (store_.wipePending()) wipeIfPending(onBusy);
  LOG_INF("TTF", "family loaded: %u B scratch, log %lu of %lu B used",
          static_cast<unsigned>(rasterizer_.scratchBytes()), static_cast<unsigned long>(store_.logUsed()),
          static_cast<unsigned long>(store_.geometry().logSize));
  return true;
}

bool TtfFont::install(const ttf::FileSource (&files)[4], const uint8_t* identity, const std::function<void()>& onBusy) {
  store_.close();  // the plan needs the free mapping pages with nothing of the store mapped
  glyphstore::PlanInput in{};
  in.mode = glyphstore::kModeFlash;
  for (uint8_t s = 0; s < 4; ++s) {
    if (files[s].size != 0) in.styleBytes[in.styleCount++] = files[s].size;
  }
  in.usableBytes = flash_.size();
  in.freeMapPages = flash_.freeMapPages();
  glyphstore::Layout layout{};
  if (!glyphstore::planLayout(in, layout)) {
    LOG_ERR("TTF", "family does not fit the font partition (%u styles, %lu free mapping pages)", in.styleCount,
            static_cast<unsigned long>(in.freeMapPages));
    return false;
  }
  if (onBusy) onBusy();
  // Freed when the install ends; a 4 KB chunk keeps the copy at one SD read and one flash write per call.
  const auto buffer = makeUniqueNoThrow<uint8_t[]>(kCopyBytes);
  if (!buffer) {
    LOG_ERR("TTF", "install: no %lu B copy buffer", static_cast<unsigned long>(kCopyBytes));
    return false;
  }
  if (!store_.beginInstall(layout, identity, nullptr)) return false;
  uint8_t block = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    const ttf::FileSource& file = files[s];
    if (file.size == 0) continue;
    for (uint32_t offset = 0; offset < file.size; offset += kCopyBytes) {
      const uint32_t len = std::min(kCopyBytes, file.size - offset);
      if (!file.read(file.ctx, offset, buffer.get(), len) || !store_.writeStyleData(block, offset, buffer.get(), len)) {
        LOG_ERR("TTF", "install: copying style %u failed at %lu", s, static_cast<unsigned long>(offset));
        return false;  // the header is still erased: the next load installs again
      }
      if (hooks.yield) hooks.yield();
    }
    ++block;
  }
  if (!store_.finishInstall()) return false;
  LOG_INF("TTF", "family installed: %u styles, log %lu B", in.styleCount,
          static_cast<unsigned long>(layout.geometry.logSize));
  return true;
}

bool TtfFont::setBodySize(const uint8_t pointSize) {
  if (pointSize == 0 || pointSize > kMaxPointSize || !styles_[0].present) return false;
  const uint32_t ppem = ttf::ppemFromPoints(pointSize);
  ttf::LineMetrics metrics[4];
  for (uint8_t s = 0; s < 4; ++s) {
    if (!styles_[s].present) continue;
    metrics[s] = ttf::lineMetrics(styles_[s].face, ppem);
    if (metrics[s].advanceY <= 0 || metrics[s].advanceY > 255) {
      LOG_ERR("TTF", "style %u at %u pt: line height %ld does not fit", s, pointSize,
              static_cast<long>(metrics[s].advanceY));
      return false;
    }
  }
  ppem_ = ppem;
  pointSize_ = pointSize;
  for (uint8_t s = 0; s < 4; ++s) {
    Style& style = styles_[s];
    if (!style.present) continue;
    style.data = EpdFontData{};
    style.data.advanceY = static_cast<uint8_t>(metrics[s].advanceY);
    style.data.ascender = metrics[s].ascender;
    style.data.descender = metrics[s].descender;
    style.data.is2Bit = true;
    style.data.bitmap = store_.logBase();
    style.data.provider = &style.provider;
  }
  sizeValid_ = false;
  claimSize();
  return true;
}

const EpdFont* TtfFont::font(const uint8_t style) const {
  return style < 4 && styles_[style].present && ppem_ != 0 ? &styles_[style].font : nullptr;
}

void TtfFont::claimSize() {
  // A wipe voids every size index; claim it again once the store's generation moved.
  if (sizeValid_ && sizeGeneration_ == store_.generation()) return;
  sizeGeneration_ = store_.generation();
  sizeValid_ = store_.sizeIndex(ppem_, &sizeIndex_);
}

bool TtfFont::storeKey(const uint8_t style, const uint32_t codepoint, uint32_t* key) {
  if (!store_.isOpen() || ppem_ == 0) return false;
  claimSize();
  if (!sizeValid_) return false;
  *key = glyphstore::makeKey(codepoint, style, sizeIndex_);
  return true;
}

bool TtfFont::fontBytes(const uint8_t style, FontBytes* bytes) {
  // Flash mode maps one style at a time, so the pointer is fetched again for every render.
  const uint8_t* mapped = store_.mapStyle(styles_[style].block);
  if (!mapped) {
    LOG_ERR("TTF", "style %u could not be mapped", style);
    return false;
  }
  *bytes = FontBytes(mapped, styles_[style].bytes);
  return true;
}

uint32_t TtfFont::batchCapacity() const { return rasterizer_.plan().outputBytes; }

const EpdGlyph* TtfFont::glyph(const uint8_t style, const uint32_t codepoint) {
  if (style >= 4 || !styles_[style].present || ppem_ == 0) return nullptr;
  if (utf8IsDefaultIgnorable(codepoint)) return &kIgnorable;
  uint32_t key = 0;
  const bool haveKey = storeKey(style, codepoint, &key);
  if (haveKey) {
    const EpdGlyph* stored = nullptr;
    switch (store_.find(key, &stored)) {
      case Lookup::Found:
        ++hits_;
        return stored;
      case Lookup::NoGlyph:
        return nullptr;
      case Lookup::NotStored:
        break;
    }
  }
  return render(style, codepoint, haveKey, key);
}

const EpdGlyph* TtfFont::render(const uint8_t style, const uint32_t codepoint, const bool haveKey, const uint32_t key) {
  checkLock();
  FontBytes bytes;
  if (!fontBytes(style, &bytes)) return nullptr;
  const ttf::Face& face = styles_[style].face;
  const uint16_t index = ttf::glyphIndex(bytes, face, codepoint);
  if (index == 0) {
    // The font has no glyph for it. An I/O error is not an answer (SD mode): ask again next time.
    if (haveKey && !ringMode_ && !bytes.ioError()) noteCommit(store_.markNoGlyph(key));
    return nullptr;
  }
  EpdGlyph metrics{};
  const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, index, ppem_, &metrics), metrics);
  if (loaded != ttf::Status::Ok) {
    failed(style, codepoint, loaded, haveKey, key);
    return nullptr;
  }
  ++renders_;
  if (haveKey && !ringMode_ && glyphstore::recordBytes(metrics.dataLength) <= batchCapacity()) {
    // Drawn straight into a one-record batch in the output region, then stored.
    GlyphBatch batch(rasterizer_.output(), batchCapacity());
    uint8_t* bitmap = batch.add(key, metrics);
    if (bitmap) {
      const ttf::Status drawn = rasterizer_.rasterize(bitmap);
      if (drawn != ttf::Status::Ok) {
        batch.dropLast();
        failed(style, codepoint, drawn, haveKey, key);
        return nullptr;
      }
      const Commit committed = store_.commit(batch);
      const EpdGlyph* stored = nullptr;
      if (committed == Commit::Stored && store_.find(key, &stored) == Lookup::Found) return stored;
      noteCommit(committed);
    }
  }
  return drawToRing(metrics);
}

const EpdGlyph* TtfFont::drawToRing(const EpdGlyph& metrics) {
  const uint8_t slot = ringNext_;
  ringNext_ = static_cast<uint8_t>((ringNext_ + 1) % kRingSlots);
  if (metrics.dataLength > ringCapacity_[slot]) {
    // One heap block per slot, kept and reused while it is large enough: the ring exists only after the store
    // filled up, for oversized glyphs, or without a store.
    ringBitmaps_[slot] = makeUniqueNoThrow<uint8_t[]>(metrics.dataLength);
    ringCapacity_[slot] = ringBitmaps_[slot] ? metrics.dataLength : 0;
    if (!ringBitmaps_[slot]) {
      LOG_ERR("TTF", "ring: no %u B for a glyph", static_cast<unsigned>(metrics.dataLength));
      return nullptr;
    }
  }
  if (metrics.dataLength > 0 && rasterizer_.rasterize(ringBitmaps_[slot].get()) != ttf::Status::Ok) return nullptr;
  ringGlyphs_[slot] = metrics;
  ringGlyphs_[slot].dataOffset = 0;
  return &ringGlyphs_[slot];
}

bool TtfFont::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  // An address-range check: every glyph drawn from the store pays this call.
  const auto at = reinterpret_cast<uintptr_t>(glyph);
  const auto first = reinterpret_cast<uintptr_t>(ringGlyphs_);
  if (at >= first && at < first + sizeof ringGlyphs_) {
    const uint32_t slot = static_cast<uint32_t>((at - first) / sizeof(EpdGlyph));
    bitmap = ringGlyphs_[slot].dataLength > 0 ? ringBitmaps_[slot].get() : nullptr;
    return true;
  }
  if (glyph == &kIgnorable) {
    bitmap = nullptr;
    return true;
  }
  return false;
}

void TtfFont::failed(const uint8_t style, const uint32_t codepoint, const ttf::Status status, const bool haveKey,
                     const uint32_t key) {
  (void)style;  // both only logged, and logging may be compiled out
  (void)codepoint;
  // Transient (no scratch block, an SD read failed): drawn as missing this time, asked again at the next lookup.
  if (!ttf::isPermanent(status)) return;
  if (haveKey && !ringMode_) noteCommit(store_.markNoGlyph(key));
  if (failuresLogged_ < kMaxFailuresLogged) {
    ++failuresLogged_;
    LOG_INF("TTF", "U+%04lX style %u: %s, drawn as missing", static_cast<unsigned long>(codepoint), style,
            ttf::statusName(status));
  }
}

void TtfFont::noteCommit(const Commit result) {
  if (result == Commit::Full || result == Commit::WriteFailed) {
    if (!ringMode_) {
      LOG_INF("TTF", "glyph store %s: new glyphs are not stored until the next book open",
              result == Commit::Full ? "full" : "write failed");
    }
    ringMode_ = true;
  } else if (result == Commit::Refused) {
    LOG_ERR("TTF", "glyph store refused a write");
  }
}

bool TtfFont::wipeIfPending(const std::function<void()>& onBusy) {
  if (!store_.isOpen() || !store_.wipePending()) return false;
  if (onBusy) onBusy();
  if (!store_.wipe()) return false;  // stays pending: the next book open tries again
  LOG_INF("TTF", "glyph store wiped");
  ringMode_ = false;
  for (uint8_t i = 0; i < kRingSlots; ++i) {
    ringBitmaps_[i].reset();
    ringCapacity_[i] = 0;
  }
  for (Style& style : styles_) {
    if (style.present && ppem_ != 0) style.data.bitmap = store_.logBase();
  }
  if (ppem_ != 0) claimSize();
  return true;
}
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 41 tests.` The full-store test fills a 4-page log at 26 pt and takes about a second.

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp test/ttf_font
git commit -m "feat(ttf): TtfFont draws a TrueType family from the glyph store"
```

### Task 5: Prefetch, a page's missing glyphs in one batch

One glyph at a time costs two flash writes each (its record, then its slot) and a style remap whenever styles
alternate. `prefetch(style, text)` collects the text's missing codepoints (at most 64, each once; ASCII repeats
skipped with a 128-bit set), marks the ones the font lacks, sorts the rest by glyph ID (file order, which SD mode
needs in 2c-4) and renders them into one batch in the output region, committing when it fills and at the end.
Everything else keeps working without it: a glyph it skipped renders at its lookup.

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`
- Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: `EpdFontFamily::prefetch` and `GlyphProvider::prefetch` (Task 2); `TtfFont` (Task 4).
- Produces:
  - `void TtfFont::prefetch(uint8_t style, const char* utf8Text)`
  - `static constexpr uint8_t TtfFont::kPrefetchGlyphs = 64;`
  - the per-style adapter overrides `GlyphProvider::prefetch`, so `EpdFontFamily::prefetch` reaches it.

- [ ] **Step 1: Write the failing tests**

Append to `test/ttf_font/TtfFontTest.cpp`:

<!-- plan-append: test/ttf_font/TtfFontTest.cpp -->
```cpp
// --- prefetch() -------------------------------------------------------------------------------------------------

namespace {

std::string utf8(const std::vector<uint32_t>& codepoints) {
  std::string out;
  for (const uint32_t cp : codepoints) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

}  // namespace

TEST_F(TtfFontTest, PrefetchStoresATextsMissingGlyphsInBatches) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const uint32_t writes = flash.programCalls();
  font->prefetch(kRegular, "Hello world");
  EXPECT_EQ(8u, font->renders());  // H e l o space w r d
  // One log write per batch and one slot per glyph; one glyph at a time would be two writes each.
  EXPECT_GE(flash.programCalls() - writes, 9u);
  EXPECT_LE(flash.programCalls() - writes, 10u);
  for (const char c : std::string("Hello world")) {
    expectDrawnAs(reference(files[kRegular].bytes, static_cast<uint8_t>(c), 14), *font, kRegular,
                  font->glyph(kRegular, static_cast<uint8_t>(c)));
  }
  EXPECT_EQ(8u, font->renders());  // all found in the store
}

TEST_F(TtfFontTest, PrefetchSkipsWhatIsAlreadyStored) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  font->prefetch(kRegular, "Hello world");
  const uint32_t writes = flash.programCalls();
  font->prefetch(kRegular, "Hello world, world");
  EXPECT_EQ(9u, font->renders());               // only the comma is new
  EXPECT_EQ(writes + 2, flash.programCalls());  // one batch, one slot
}

TEST_F(TtfFontTest, PrefetchRendersAtMostItsLimitAndLookupsRenderTheRest) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  std::vector<uint32_t> codepoints;
  for (uint32_t cp = 'A'; cp <= 'Z'; ++cp) codepoints.push_back(cp);
  for (uint32_t cp = 'a'; cp <= 'z'; ++cp) codepoints.push_back(cp);
  for (uint32_t cp = '0'; cp <= '9'; ++cp) codepoints.push_back(cp);
  for (uint32_t cp = 0xC0; cp <= 0xD6; ++cp) codepoints.push_back(cp);
  font->prefetch(kRegular, utf8(codepoints).c_str());
  EXPECT_EQ(TtfFont::kPrefetchGlyphs, font->renders());
  for (const uint32_t cp : codepoints) {
    expectDrawnAs(reference(files[kRegular].bytes, cp, 14), *font, kRegular, font->glyph(kRegular, cp));
  }
  EXPECT_EQ(codepoints.size(), font->renders());
}

TEST_F(TtfFontTest, PrefetchMarksCodepointsTheFontLacks) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  font->prefetch(kRegular, "\xE4\xB8\xAD");  // U+4E2D
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(nullptr, font->glyph(kRegular, 0x4E2D));
  EXPECT_EQ(writes, flash.programCalls());
}

TEST_F(TtfFontTest, PrefetchInRingModeWritesNothing) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  ASSERT_TRUE(font->ringMode());
  const uint32_t writes = flash.programCalls();
  const uint32_t renders = font->renders();
  font->prefetch(kRegular, "DEF");
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_EQ(renders, font->renders());  // nothing to batch for: they are drawn as they are looked up
}

TEST_F(TtfFontTest, PrefetchYieldsBetweenGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  TtfFont::setHooks({[] {
                       ++lockChecks;
                       return true;
                     },
                     [] { ++yields; }});
  font->prefetch(kRegular, "abc");
  EXPECT_EQ(3, yields);
  EXPECT_EQ(1, lockChecks);
}

TEST_F(TtfFontTest, EpdFontFamilyPrefetchReachesTheStylesFont) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFontFamily family(font->font(0), font->font(1), font->font(2), font->font(3));
  family.prefetch("Wolf", 1u << EpdFontFamily::BOLD);
  EXPECT_EQ(4u, font->renders());
  expectDrawnAs(reference(files[kBold].bytes, 'W', 14), *font, kBold, font->glyph(kBold, 'W'));
  EXPECT_EQ(4u, font->renders());  // prefetched
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: compile errors, `'class TtfFont' has no member named 'prefetch'` and `'kPrefetchGlyphs' is not a member
of 'TtfFont'`.

- [ ] **Step 3: Implement**

In `lib/EpdFont/ttf/TtfFont.h`, the limit:

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  static constexpr uint8_t kRingSlots = 8;
```
with:

```cpp
  static constexpr uint8_t kRingSlots = 8;
  /// Missing glyphs one prefetch() call renders at most; the rest render as they are looked up.
  static constexpr uint8_t kPrefetchGlyphs = 64;
```

The public declaration:

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
```
with:

```cpp
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
  void prefetch(uint8_t style, const char* utf8Text);
```

The adapter forwards it:

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
      return owner->ownsBitmap(glyph, bitmap);
    }
```
with:

```cpp
      return owner->ownsBitmap(glyph, bitmap);
    }
    void prefetch(const char* utf8Text) override { owner->prefetch(style, utf8Text); }
```

The pending list (a member, 512 B inside the heap-allocated `TtfFont`, rather than on the loop task's stack):

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  bool install(const ttf::FileSource (&files)[4], const uint8_t* identity, const std::function<void()>& onBusy);
```
with:

```cpp
  struct Pending {
    uint32_t codepoint;
    uint16_t glyph;
  };

  bool install(const ttf::FileSource (&files)[4], const uint8_t* identity, const std::function<void()>& onBusy);
```

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  uint32_t renders_ = 0;
```
with:

```cpp
  Pending pending_[kPrefetchGlyphs] = {};
  uint32_t renders_ = 0;
```

In `lib/EpdFont/ttf/TtfFont.cpp`, before `wipeIfPending`:

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
bool TtfFont::wipeIfPending(const std::function<void()>& onBusy) {
```
with:

```cpp
void TtfFont::prefetch(const uint8_t style, const char* utf8Text) {
  if (!utf8Text || style >= 4 || !styles_[style].present || ringMode_) return;
  uint32_t key = 0;
  if (!storeKey(style, 0, &key)) return;
  checkLock();
  FontBytes bytes;
  if (!fontBytes(style, &bytes)) return;
  const ttf::Face& face = styles_[style].face;

  // The text's codepoints that are not stored yet, each once.
  uint8_t count = 0;
  uint32_t asciiSeen[4] = {};
  const auto* text = reinterpret_cast<const unsigned char*>(utf8Text);
  while (count < kPrefetchGlyphs) {
    const uint32_t cp = utf8NextCodepoint(&text);
    if (cp == 0) break;
    if (cp < 0x80) {
      if (asciiSeen[cp >> 5] & (1u << (cp & 31))) continue;
      asciiSeen[cp >> 5] |= 1u << (cp & 31);
    }
    if (utf8IsDefaultIgnorable(cp)) continue;
    key = glyphstore::makeKey(cp, style, sizeIndex_);
    if (store_.find(key, nullptr) != Lookup::NotStored) continue;
    if (std::any_of(pending_, pending_ + count, [cp](const Pending& p) { return p.codepoint == cp; })) continue;
    const uint16_t index = ttf::glyphIndex(bytes, face, cp);
    if (index == 0) {
      if (!bytes.ioError()) noteCommit(store_.markNoGlyph(key));
      if (ringMode_) return;
      continue;
    }
    pending_[count++] = {cp, index};
  }
  if (count == 0) return;

  // In glyph order, which is file order: in SD mode the outline reads then move forward through the file.
  std::sort(pending_, pending_ + count, [](const Pending& a, const Pending& b) { return a.glyph < b.glyph; });
  GlyphBatch batch(rasterizer_.output(), batchCapacity());
  for (uint8_t i = 0; i < count && !ringMode_; ++i) {
    const Pending& p = pending_[i];
    key = glyphstore::makeKey(p.codepoint, style, sizeIndex_);
    EpdGlyph metrics{};
    const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, p.glyph, ppem_, &metrics), metrics);
    if (loaded != ttf::Status::Ok) {
      failed(style, p.codepoint, loaded, true, key);
      continue;
    }
    if (glyphstore::recordBytes(metrics.dataLength) > batchCapacity()) continue;  // drawn from the ring when used
    uint8_t* bitmap = batch.add(key, metrics);
    if (!bitmap) {
      noteCommit(store_.commit(batch));
      if (ringMode_) break;
      bitmap = batch.add(key, metrics);
      if (!bitmap) continue;
    }
    const ttf::Status drawn = rasterizer_.rasterize(bitmap);
    if (drawn != ttf::Status::Ok) {
      batch.dropLast();
      failed(style, p.codepoint, drawn, true, key);
      continue;
    }
    ++renders_;
    if (hooks.yield) hooks.yield();
  }
  if (!ringMode_) noteCommit(store_.commit(batch));
}

bool TtfFont::wipeIfPending(const std::function<void()>& onBusy) {
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 48 tests.`

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp test/ttf_font/TtfFontTest.cpp
git commit -m "feat(ttf): prefetch renders a text's missing glyphs in one batch, in glyph order"
```

### Task 6: The registry finds TTF families

A directory under a fonts root whose files include `.ttf` (any case) and no `.cpfont` becomes a TTF family: each
file is probed (three small reads), the files are taken in name order, the first of each style serves it, and a
regular style is required. Its `SdCardFontFileInfo`s carry the style and `pointSize` 0. Callers that need a
per-size file (`pickClosestSize`: font previews, the `.cpfont` loader) get nullptr and skip the family, and
`availableSizes()` is empty (the web font list shows no sizes for it).

Directory scanning needs a card, so the host tests cover the decisions it makes; Task 8 checks the scan on the X3.

**Files:**
- Modify: `lib/EpdFont/SdCardFontRegistry.h`, `lib/EpdFont/SdCardFontRegistry.cpp`
- Create: `test/ttf_font/TtfRegistryTest.cpp`
- Modify: `test/ttf_font/CMakeLists.txt`

**Interfaces:**
- Consumes: `ttf::isTtfFileName`, `ttf::probeStyle`, `ttf::assignStyles`, `ttf::fileSource` (Task 3).
- Produces: `bool SdCardFontFamilyInfo::ttf` (default false); for a TTF family `pickClosestSize()` returns nullptr,
  `availableSizes()` is empty, and `findFile(0, style)` finds a style's file.

- [ ] **Step 1: Write the failing tests**

Create `test/ttf_font/TtfRegistryTest.cpp`:

<!-- plan-file: test/ttf_font/TtfRegistryTest.cpp -->
```cpp
// How the font registry lists a TrueType family. Directory scanning needs a card; these are the decisions it uses.
#include <gtest/gtest.h>

#include "SdCardFontRegistry.h"

namespace {

SdCardFontFamilyInfo ttfFamily() {
  SdCardFontFamilyInfo family;
  family.name = "Literata";
  family.ttf = true;
  family.files.push_back({"/.crosspoint/fonts/Literata/Literata-Regular.ttf", 0, 0, 450048});
  family.files.push_back({"/.crosspoint/fonts/Literata/Literata-Bold.ttf", 0, 1, 452000});
  return family;
}

}  // namespace

TEST(TtfRegistry, ATtfFamilyHasNoPerSizeFiles) {
  const SdCardFontFamilyInfo family = ttfFamily();
  EXPECT_TRUE(family.availableSizes().empty());
  EXPECT_EQ(nullptr, family.pickClosestSize(14));  // previews and the .cpfont loader skip it
  const SdCardFontFileInfo* bold = family.findFile(0, 1);
  ASSERT_NE(nullptr, bold);
  EXPECT_EQ(452000u, bold->fileBytes);
  EXPECT_EQ(nullptr, family.findFile(0, 2));
}

TEST(TtfRegistry, ACpfontFamilyIsUnchanged) {
  SdCardFontFamilyInfo family;
  family.name = "Bookerly-SD";
  family.files.push_back({"/.crosspoint/fonts/Bookerly-SD/Bookerly-SD_12.cpfont", 12, 0, 1000});
  family.files.push_back({"/.crosspoint/fonts/Bookerly-SD/Bookerly-SD_16.cpfont", 16, 0, 1200});
  EXPECT_FALSE(family.ttf);
  EXPECT_EQ((std::vector<uint8_t>{12, 16}), family.availableSizes());
  ASSERT_NE(nullptr, family.pickClosestSize(15));
  EXPECT_EQ(16, family.pickClosestSize(15)->pointSize);
}
```

Replace `test/ttf_font/CMakeLists.txt` (the registry and its test join the target):

<!-- plan-file: test/ttf_font/CMakeLists.txt -->
```cmake
# TtfFont (lib/EpdFont/ttf): TrueType families on the glyph store, from the file probe to drawn pages.

add_executable(TtfFontTest
  TtfFamilyTest.cpp
  TtfFontTest.cpp
  TtfRegistryTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/FlashGlyphStore.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

# test/shims before lib/hal: the host HalStorage, but the real NorFlash.h.
target_include_directories(TtfFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/ttf_rasterizer
  ${REPO_ROOT}/test/glyph_store
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(TtfFontTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfFontTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# The sanitizers ttf_rasterizer found for this toolchain (ASan + UBSan on CI's Linux GCC).
target_compile_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})

gtest_discover_tests(TtfFontTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: compile errors, `'struct SdCardFontFamilyInfo' has no member named 'ttf'`.

- [ ] **Step 3: Implement**

In `lib/EpdFont/SdCardFontRegistry.h`:

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.h -->
```cpp
  uint8_t pointSize;  // parsed from filename: 14
  uint8_t style;      // always 0 in v4 (all 4 styles bundled in one file);
                      // kept for potential future formats
```
with:

```cpp
  uint8_t pointSize;  // parsed from filename: 14; 0 for a TTF file, which serves every size
  uint8_t style;      // always 0 in v4 (all 4 styles bundled in one file);
                      // a TTF file's head.macStyle: EpdFontFamily::Style
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.h -->
```cpp
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
```
with:

```cpp
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
  // A TrueType family: one .ttf file per style, regular among them, every size drawn from them on the
  // device (TtfFont). A directory with any .cpfont file is a .cpfont family, whatever else it holds.
  bool ttf = false;
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.h -->
```cpp
  // across SD card layouts. Returns nullptr when files is empty.
```
with:

```cpp
  // across SD card layouts. Returns nullptr when files is empty, and for a TTF
  // family, which has no per-size files (callers that need one skip it).
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.h -->
```cpp
  void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
```
with:

```cpp
  void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  static void addTtfFiles(const char* dirPath, const std::vector<std::string>& names,
                          const std::vector<uint32_t>& bytes, SdCardFontFamilyInfo& family);
```

In `lib/EpdFont/SdCardFontRegistry.cpp`, the includes:

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
```
with:

```cpp
#include <HalStorage.h>
#include <Logging.h>
#include <ttf/TtfFamily.h>

#include <algorithm>
#include <cstring>
#include <numeric>
```

The two helpers that need a size:

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
```
with:

```cpp
std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  if (ttf) return sizes;  // drawn at any size, from one file per style
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
const SdCardFontFileInfo* SdCardFontFamilyInfo::pickClosestSize(uint8_t targetPtSize) const {
```
with:

```cpp
const SdCardFontFileInfo* SdCardFontFamilyInfo::pickClosestSize(uint8_t targetPtSize) const {
  if (ttf) return nullptr;
```

The TTF pass, run after a directory's listing when it found no `.cpfont`:

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  char nameBuffer[128];
```
with:

```cpp
// A directory of .ttf files: each file's style comes from its head table, and the first file of each style (by
// name) serves it. Only a directory with a regular style becomes a family.
void SdCardFontRegistry::addTtfFiles(const char* dirPath, const std::vector<std::string>& names,
                                     const std::vector<uint32_t>& bytes, SdCardFontFamilyInfo& family) {
  std::vector<size_t> order(names.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return names[a] < names[b]; });
  std::vector<int> styles(order.size(), -1);
  for (size_t i = 0; i < order.size(); ++i) {
    const std::string path = std::string(dirPath) + "/" + names[order[i]];
    HalFile file;
    if (!Storage.openFileForRead("SDREG", path.c_str(), file)) continue;
    styles[i] = ttf::probeStyle(ttf::fileSource(file, bytes[order[i]]));
    if (styles[i] < 0) {
      LOG_DBG("SDREG", "Not a TrueType font the reader draws: %s", path.c_str());
    }
  }
  int chosen[4];
  if (!ttf::assignStyles(styles.data(), styles.size(), chosen)) {
    LOG_DBG("SDREG", "No regular TrueType style in %s", dirPath);
    return;
  }
  for (uint8_t style = 0; style < 4; ++style) {
    if (chosen[style] < 0) continue;
    const size_t file = order[static_cast<size_t>(chosen[style])];
    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + names[file];
    info.pointSize = 0;
    info.style = style;
    info.fileBytes = bytes[file];
    family.files.push_back(std::move(info));
  }
  family.ttf = true;
}

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  std::vector<std::string> ttfNames;
  std::vector<uint32_t> ttfBytes;
  char nameBuffer[128];
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
    if (!parseFilename(nameBuffer, size, style)) continue;
```
with:

```cpp
    if (!parseFilename(nameBuffer, size, style)) {
      if (ttf::isTtfFileName(nameBuffer)) {
        ttfNames.emplace_back(nameBuffer);
        ttfBytes.push_back(entryBytes);
      }
      continue;
    }
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
    family.files.push_back(std::move(info));
  }
  dir.close();
}
```
with:

```cpp
    family.files.push_back(std::move(info));
  }
  dir.close();
  if (family.files.empty() && !ttfNames.empty()) addTtfFiles(dirPath, ttfNames, ttfBytes, family);
}
```

<!-- plan-edit: lib/EpdFont/SdCardFontRegistry.cpp -->
```cpp
        LOG_DBG("SDREG", "Found family: %s (%d files) in %s", families_.back().name.c_str(),
                static_cast<int>(families_.back().files.size()), rootPath);
```
with:

```cpp
        LOG_DBG("SDREG", "Found %s family: %s (%d files) in %s", families_.back().ttf ? "TTF" : ".cpfont",
                families_.back().name.c_str(), static_cast<int>(families_.back().files.size()), rootPath);
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 50 tests.`

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/SdCardFontRegistry.h lib/EpdFont/SdCardFontRegistry.cpp test/ttf_font
git commit -m "feat(fonts): a directory of .ttf files is a TTF family"
```

### Task 7: Wire TTF families into the reader

The manager loads a TTF family through `EspNorFlash` and `TtfFont` and registers its body face. The font system
offers it every reader size from 8 pt (`ReaderSizeList::native()`: 8, then the 10-26 pt ladder, on every board) and
changes its size without a reload. The renderer's two prewarm points call
`EpdFontFamily::prefetch`. The app installs `TtfFont`'s hooks: the render-lock check and a yield that sleeps one
tick every 200 ms.

Only the size list has host tests. No host test builds `GfxRenderer`, `FontCacheManager`, the manager or the font
system (they need the device's HAL), so the rest of this task is checked by the firmware builds and the host suite;
Task 8 runs it on the X3.

**Files:**
- Modify: `lib/EpdFont/SdCardFontManager.h`, `lib/EpdFont/SdCardFontManager.cpp`
- Modify: `src/SdCardFontSystem.cpp`, `src/ReaderFontSizes.h`, `src/activities/RenderLock.h`,
  `src/activities/ActivityManager.cpp`
- Modify: `lib/GfxRenderer/GfxRenderer.cpp`, `lib/GfxRenderer/FontCacheManager.cpp`
- Test: `test/reader_font_sizes/ReaderFontSizesTest.cpp`

**Interfaces:**
- Consumes: `TtfFont` (Tasks 4-5), `ttf::familyIdentity`, `ttf::identityHash`, `ttf::fileSource` (Task 3),
  `SdCardFontFamilyInfo::ttf` (Task 6), `EpdFontFamily::prefetch` (Task 2), `EspNorFlash` (Stage 2a).
- Produces:
  - `bool SdCardFontManager::setTtfBodySize(GfxRenderer&, uint8_t pointSize)`,
    `void SdCardFontManager::wipeTtfIfPending(const std::function<void()>& onBusy)`,
    `bool SdCardFontManager::ttfLoaded() const`;
  - `static bool RenderLock::heldByCurrentTask()`;
  - `static ReaderSizeList ReaderSizeList::native()` and `ReaderSizeList::kNativeSmallestPoints = 8`.

- [ ] **Step 1: Write the failing size-list tests**

Append to `test/reader_font_sizes/ReaderFontSizesTest.cpp`:

<!-- plan-append: test/reader_font_sizes/ReaderFontSizesTest.cpp -->
```cpp
TEST(ReaderSizeListNative, OffersEightPointBelowTheLadder) {
  const ReaderSizeList sizes = ReaderSizeList::native();
  ASSERT_EQ(CrossPointSettings::FONT_SIZE_RUNG_COUNT + 1, sizes.count);
  EXPECT_EQ(8, sizes.points[0]);
  for (int i = 0; i < CrossPointSettings::FONT_SIZE_RUNG_COUNT; ++i) {
    EXPECT_EQ(CrossPointSettings::FONT_SIZE_RUNGS[i].points, sizes.points[i + 1]);
  }
}

TEST(ReaderSizeListNative, TheSmallSizesSnapAndStepToEight) {
  const ReaderSizeList sizes = ReaderSizeList::native();
  EXPECT_EQ(8, sizes.snap(9));  // a tie goes to the smaller size
  EXPECT_EQ(8, sizes.snap(1));
  EXPECT_EQ(8, sizes.step(10, -1));
  EXPECT_EQ(26, sizes.next(24));
  EXPECT_EQ(8, sizes.next(26));  // the size button wraps to the new smallest
}

TEST(ReaderSizeListNative, BuiltInFamiliesStillStartAtTheLadder) { EXPECT_EQ(10, ReaderSizeList::builtin().snap(8)); }
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/test --target ReaderFontSizesTest -j 8 && ./build/test/reader_font_sizes/ReaderFontSizesTest.exe --gtest_brief=1`
Expected: compile errors, `'native' is not a member of 'ReaderSizeList'`.

- [ ] **Step 3: The size list**

In `src/ReaderFontSizes.h`, before `forFamily()`:

<!-- plan-edit: src/ReaderFontSizes.h -->
```cpp
  /// What an SD family offers: every size it ships a file for, then the ladder sizes above its
```
with:

```cpp
  /// The smallest size a natively drawn family offers. Points convert to pixels at the converter's 150 dpi on
  /// every board, so 8 pt is about 17 px per em on all of them; the denser the panel, the smaller it looks.
  static constexpr uint8_t kNativeSmallestPoints = 8;

  /// What a TrueType family offers: its glyphs are drawn from the outlines at any size, so the ladder plus the
  /// sizes below it, in the ladder's 2 pt steps, down to kNativeSmallestPoints.
  static ReaderSizeList native() {
    ReaderSizeList list = builtin();
    for (uint8_t pt = kNativeSmallestPoints; pt < list.points[0]; pt += 2) list.insert(pt);
    return list;
  }

  /// What an SD family offers: every size it ships a file for, then the ladder sizes above its
```

- [ ] **Step 4: The manager**

In `lib/EpdFont/SdCardFontManager.h`:

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
#include <functional>
#include <string>
```
with:

```cpp
#include <functional>
#include <memory>
#include <string>
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
class SdCardFont;
struct SdCardFontFamilyInfo;
```
with:

```cpp
class SdCardFont;
class NorFlash;
class TtfFont;
struct SdCardFontFamilyInfo;
```

The constructor moves to the `.cpp`, where `TtfFont` is a complete type (the `unique_ptr` members need it):

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  SdCardFontManager() = default;
```
with:

```cpp
  SdCardFontManager();
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  // block and must unload this family before rewinding it. Only font previews pass one.
  bool loadFamily(
```
with:

```cpp
  // block and must unload this family before rewinding it. Only font previews pass one.
  //
  // A TTF family (SdCardFontFamilyInfo::ttf) is drawn by a TtfFont at targetPtSize itself, from the
  // glyph store in the same partition; onColdLoad fires before it is installed there. Read-only
  // (preview) loads of a TTF family are refused.
  bool loadFamily(
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  void unloadAll(GfxRenderer& renderer);
```
with:

```cpp
  void unloadAll(GfxRenderer& renderer);

  // The loaded TTF family's body size: a new font ID drawn natively at pointSize, never scaled.
  // False when no TTF family is loaded, the size is out of range, or its ID collides.
  bool setTtfBodySize(GfxRenderer& renderer, uint8_t pointSize);
  // Runs the TTF glyph store's pending wipe (it filled up), calling onBusy first. Only at book open.
  void wipeTtfIfPending(const std::function<void()>& onBusy);
  bool ttfLoaded() const { return ttf_ != nullptr; }
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  void dropSizeAlias(GfxRenderer& renderer);
```
with:

```cpp
  void dropSizeAlias(GfxRenderer& renderer);
  bool loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                     const std::function<void()>& onColdLoad);
  void registerTtfFace(GfxRenderer& renderer, int fontId);
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  int aliasFontId_ = 0;
  uint8_t aliasPointSize_ = 0;
};
```
with:

```cpp
  int aliasFontId_ = 0;
  uint8_t aliasPointSize_ = 0;
  // The loaded TTF family, if any: its font area (EspNorFlash) and the font drawn from it. The
  // font is destroyed first, so the store is closed before the flash goes.
  std::unique_ptr<NorFlash> ttfFlash_;
  std::unique_ptr<TtfFont> ttf_;
  uint32_t ttfIdentityHash_ = 0;
  int ttfFontId_ = 0;
};
```

In `lib/EpdFont/SdCardFontManager.cpp`, the includes, the constructor, and the destructor's no-renderer path:

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
#include <EpdFontFamily.h>
#include <FlashFontPartition.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstdlib>

```
with:

```cpp
#include <EpdFontFamily.h>
#include <EspNorFlash.h>
#include <FlashFontPartition.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>
#include <ttf/TtfFamily.h>
#include <ttf/TtfFont.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

SdCardFontManager::SdCardFontManager() = default;

```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
      delete lf.font;
    }
    loaded_.clear();
  }
}
```
with:

```cpp
      delete lf.font;
    }
    loaded_.clear();
    ttf_.reset();
    ttfFlash_.reset();
  }
}
```

The TTF functions, before `loadFamily()`. `loadTtfFamily` opens every style's file (they stay open while an install
copies them), stamps each with its size and FAT modification time for the identity, and registers the body face
under `computeFontId(identityHash, family, pt)`, so changing a file gives new IDs and the sections are laid out
again (spec 7.4):

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
```
with:

```cpp
bool SdCardFontManager::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                      const uint8_t targetPtSize, const std::function<void()>& onColdLoad) {
  // The partition has one owner (spec 4.1): a .cpfont mapping goes before the glyph store opens.
  if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();

  // The files stay open while the family loads: an install copies them into the store.
  HalFile files[4];
  ttf::FileSource sources[4];
  ttf::FileStamp stamps[4];
  for (const auto& file : family.files) {
    if (file.style >= 4) continue;
    HalFile& handle = files[file.style];
    if (!Storage.openFileForRead("SDMGR", file.path.c_str(), handle)) {
      LOG_ERR("SDMGR", "Cannot open %s", file.path.c_str());
      continue;
    }
    const auto size = static_cast<uint32_t>(handle.fileSize());
    sources[file.style] = ttf::fileSource(handle, size);
    uint16_t date = 0;
    uint16_t time = 0;
    handle.getModifyDateTime(&date, &time);
    const char* slash = std::strrchr(file.path.c_str(), '/');
    stamps[file.style] = {slash ? slash + 1 : file.path.c_str(), size, date, time};
  }
  uint8_t identity[glyphstore::kIdentityBytes];
  ttf::familyIdentity(stamps, identity);

  // Held while the family is loaded: the font area (a few words), and the font with its scratch block.
  auto flash = makeUniqueNoThrow<EspNorFlash>();
  if (!flash || !flash->begin()) {
    LOG_ERR("SDMGR", "No font partition for TTF family %s", family.name.c_str());
    return false;
  }
  auto font = makeUniqueNoThrow<TtfFont>(*flash);
  if (!font) {
    LOG_ERR("SDMGR", "OOM allocating TtfFont for %s", family.name.c_str());
    return false;
  }
  if (!font->load(sources, identity, onColdLoad) || !font->setBodySize(targetPtSize)) {
    LOG_ERR("SDMGR", "TTF family %s cannot be drawn at %u pt", family.name.c_str(), targetPtSize);
    return false;
  }
  const uint32_t identityHash = ttf::identityHash(identity);
  const int fontId = computeFontId(identityHash, family.name.c_str(), targetPtSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, family.name.c_str());
    return false;
  }
  ttfFlash_ = std::move(flash);
  ttf_ = std::move(font);
  ttfIdentityHash_ = identityHash;
  registerTtfFace(renderer, fontId);
  loadedFamilyName_ = family.name;
  loadedPointSize_ = targetPtSize;
  LOG_INF("SDMGR", "TTF family %s at %u pt (id=%d)", family.name.c_str(), targetPtSize, fontId);
  return true;
}

void SdCardFontManager::registerTtfFace(GfxRenderer& renderer, const int fontId) {
  // Not an SD font to the renderer: TtfFont keeps no per-page state for its maintenance loops.
  renderer.insertFont(fontId, EpdFontFamily(ttf_->font(0), ttf_->font(1), ttf_->font(2), ttf_->font(3)));
  ttfFontId_ = fontId;
}

bool SdCardFontManager::setTtfBodySize(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!ttf_) return false;
  if (pointSize == loadedPointSize_) return true;
  const int fontId = computeFontId(ttfIdentityHash_, loadedFamilyName_.c_str(), pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d for %s@%u collides with a registered font", fontId, loadedFamilyName_.c_str(),
            pointSize);
    return false;
  }
  if (!ttf_->setBodySize(pointSize)) return false;
  renderer.removeFont(ttfFontId_);
  registerTtfFace(renderer, fontId);
  loadedPointSize_ = pointSize;
  LOG_INF("SDMGR", "TTF family %s now at %u pt (id=%d)", loadedFamilyName_.c_str(), pointSize, fontId);
  return true;
}

void SdCardFontManager::wipeTtfIfPending(const std::function<void()>& onBusy) {
  if (ttf_) ttf_->wipeIfPending(onBusy);
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
```

`loadFamily()` hands TTF families over before the `.cpfont` path (it has already unloaded whatever was loaded):

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
  if (!loadedFamilyName_.empty()) unloadAll(renderer);

  const SdCardFontFileInfo* selected = family.pickClosestSize(targetPtSize);
```
with:

```cpp
  if (!loadedFamilyName_.empty()) unloadAll(renderer);

  if (family.ttf) {
    // A preview must never install over the partition; TTF previews come later (spec 7.6).
    if (policy == FlashCachePolicy::ReadOnly) return false;
    return loadTtfFamily(family, renderer, targetPtSize, onColdLoad);
  }

  const SdCardFontFileInfo* selected = family.pickClosestSize(targetPtSize);
```

`unloadAll()` closes the store before anything else touches the partition, and logs the session's counts (Task 8
reads them):

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  if (!renderer_) renderer_ = &renderer;
```
with:

```cpp
void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  if (!renderer_) renderer_ = &renderer;
  if (ttf_) {
    renderer.removeFont(ttfFontId_);
    LOG_INF("SDMGR", "TTF family %s unloaded: %lu glyphs rendered, %lu read from the store", loadedFamilyName_.c_str(),
            static_cast<unsigned long>(ttf_->renders()), static_cast<unsigned long>(ttf_->storeHits()));
    ttf_.reset();  // closes the store before its flash goes
    ttfFlash_.reset();
    ttfFontId_ = 0;
  }
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
int SdCardFontManager::getFontId(const std::string& familyName, const uint8_t pointSize) const {
```
with:

```cpp
int SdCardFontManager::getFontId(const std::string& familyName, const uint8_t pointSize) const {
  if (ttf_) return familyName == loadedFamilyName_ && pointSize == loadedPointSize_ ? ttfFontId_ : 0;
```

- [ ] **Step 5: The render-lock check**

In `src/activities/RenderLock.h`:

<!-- plan-edit: src/activities/RenderLock.h -->
```cpp
  static bool peek();
```
with:

```cpp
  static bool peek();
  // True when the calling task holds renderingMutex: the debug check for code that relies on it.
  static bool heldByCurrentTask();
```

In `src/activities/ActivityManager.cpp`:

<!-- plan-edit: src/activities/ActivityManager.cpp -->
```cpp
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; };
```
with:

```cpp
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; };

bool RenderLock::heldByCurrentTask() {
  return xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle();
}
```

- [ ] **Step 6: The font system**

In `src/SdCardFontSystem.cpp`, the includes, the size check and the yield hook:

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
#include <Logging.h>

```
with:

```cpp
#include <Logging.h>
#include <ttf/TtfFont.h>

```

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
#include "SdCardFontGlobals.h"
```
with:

```cpp
#include "SdCardFontGlobals.h"
#include "activities/RenderLock.h"

static_assert(CrossPointSettings::FONT_SIZE_RUNGS[CrossPointSettings::FONT_SIZE_RUNG_COUNT - 1].points <=
                  TtfFont::kMaxPointSize,
              "TtfFont plans its scratch block for the largest reader size");

namespace {

// Between glyphs and install chunks: a hostile glyph can take most of a second, and an install
// several seconds, all under RenderLock. A tick's sleep every 200 ms lets the idle task run.
void ttfYield() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 200) return;
  last = now;
  vTaskDelay(1);
}

}  // namespace
```

A TTF family offers every size from 8 pt:

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
    if (family.name != familyName) continue;
    uint8_t sizes[ReaderSizeList::kCapacity];
```
with:

```cpp
    if (family.name != familyName) continue;
    if (family.ttf) return ReaderSizeList::native();  // drawn natively at every size
    uint8_t sizes[ReaderSizeList::kCapacity];
```

The hooks, installed once at startup:

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
  (void)renderer;
  registry_.discover();
```
with:

```cpp
  (void)renderer;
  TtfFont::setHooks({&RenderLock::heldByCurrentTask, &ttfYield});
  registry_.discover();
```

When the loaded family is the wanted TTF family, a new size is a new body size, never a reload. In the settings
path:

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    const uint8_t bestPt = best ? best->pointSize : 0;
```
with:

```cpp
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    if (family->ttf && manager_.ttfLoaded()) {
      manager_.setTtfBodySize(renderer, targetPt);  // drawn natively: a new size, never a reload
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    const uint8_t bestPt = best ? best->pointSize : 0;
```

And at book open, which is also where a full store is wiped (behind the "Loading font" popup):

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
    if (!family) {
      manager_.unloadAll(renderer);
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
```
with:

```cpp
    if (!family) {
      manager_.unloadAll(renderer);
      return;
    }
    if (family->ttf && manager_.ttfLoaded()) {
      // Book open: no glyph record of the family is in use, so a full store can be emptied here.
      manager_.wipeTtfIfPending(onColdLoad);
      manager_.setTtfBodySize(renderer, targetPt);
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
```

- [ ] **Step 7: The two prefetch call sites**

In `lib/GfxRenderer/GfxRenderer.cpp`, `ensureFontReady()` (layout, before it measures a run):

<!-- plan-edit: lib/GfxRenderer/GfxRenderer.cpp -->
```cpp
  if (!font) return;                         // no-op for built-in fonts
```
with:

```cpp
  if (!font) {
    // A face whose glyphs come from a provider (TTF) renders the run's missing glyphs in one batch.
    // Layout passes no style, so the regular face it measures most with; built-in fonts do nothing.
    if (const EpdFontFamily* family = findFontFamily(fontId)) family->prefetch(utf8Text, 1u << EpdFontFamily::REGULAR);
    return;
  }
```

In `lib/GfxRenderer/FontCacheManager.cpp`, `prewarmCache()` (before a page is drawn), after the SD-font branch:

<!-- plan-edit: lib/GfxRenderer/FontCacheManager.cpp -->
```cpp
  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;
```
with:

```cpp
  // A face whose glyphs come from a provider (TTF): each requested style renders its missing glyphs in
  // one batch. Nothing for built-in fonts.
  const auto family = fontMap_.find(fontId);
  if (family != fontMap_.end()) family->second.prefetch(utf8Text, styleMask);

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || family == fontMap_.end()) return;
```

- [ ] **Step 8: Run the size-list tests to see them pass**

Run: `cmake --build build/test --target ReaderFontSizesTest -j 8 && ./build/test/reader_font_sizes/ReaderFontSizesTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 22 tests.`

- [ ] **Step 9: Build the firmware**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default 2>&1 | grep -E "error|check_no_float|RAM:|Flash:"`
Expected: no errors; `check_no_float: 7 TTF engine objects, no floating point`; RAM 54,808 B (+56 B static);
flash +30,032 B over the base, because the glyph store, the engine and `TtfFont` are linked for the first time.
The prototype measured: engine 9.7 KB, `TtfFont` 5.7 KB, store 5.3 KB, `EspNorFlash` 1.0 KB, family helpers
0.9 KB, manager and registry about 2 KB (`riscv32-esp-elf-nm --size-sort -S -C .pio/build/default/firmware.elf`).
Quote the measured numbers in the PR.

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro 2>&1 | grep -E "error|check_no_float|RAM:|Flash:"`
Expected: no errors (the S3 build; `EspNorFlash` bounces PSRAM sources there).

- [ ] **Step 10: Format and run the host suite**

Run: `/c/tmp/cf21/bin/clang-format-21.exe --dry-run -Werror $(git diff --name-only origin/feat/ttf-rasterizer -- '*.h' '*.cpp')`
Expected: no output.

Run: `cmake --build build/test -j 8 -- -k > /tmp/build.log 2>&1; (cd build/test && ctest -j 8 | tail -3)`
Expected: `100% tests passed out of 2216` (2,158 before this plan, plus 2 store, 3 provider, 50 `TtfFontTest` and
3 size-list tests).

- [ ] **Step 11: Commit**

```bash
git add lib/EpdFont/SdCardFontManager.h lib/EpdFont/SdCardFontManager.cpp src/SdCardFontSystem.cpp \
        src/ReaderFontSizes.h test/reader_font_sizes/ReaderFontSizesTest.cpp \
        src/activities/RenderLock.h src/activities/ActivityManager.cpp \
        lib/GfxRenderer/GfxRenderer.cpp lib/GfxRenderer/FontCacheManager.cpp
git commit -m "feat(fonts): TTF families load, draw and change size in the reader"
```

### Task 8: On the X3, and the record

The device run covers what no host test can: the directory scan, the install from the SD card, the renderer's
prefetch calls, RenderLock, and the five Review Focus situations. It needs the user for the on-device steps: ask
for each one, and capture the serial log meanwhile.

**Files:**
- Modify (docs branch `docs/ttf-fonts-design`): `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, this plan

- [ ] **Step 1: Flash and copy a family**

Flash: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`.
Ask the user to open File Transfer → USB Transfer on the device, then copy the bundled Bookerly TTFs:

```bash
PY=/c/pio/penv/Scripts/python.exe
$PY tools/serial_transfer.py --port COM6 mkdir /.crosspoint/fonts/BookerlyTTF
for f in Regular Bold Italic BoldItalic; do
  $PY tools/serial_transfer.py --port COM6 put lib/EpdFont/builtinFonts/source/Bookerly/Bookerly-$f.ttf \
      /.crosspoint/fonts/BookerlyTTF/Bookerly-$f.ttf
done
$PY tools/serial_transfer.py --port COM6 ls /.crosspoint/fonts/BookerlyTTF
```
Expected: four files of 450-460 KB each.

- [ ] **Step 2: Start the log**

Ask the user to leave USB Transfer (the device reboots). Capture in the background, without a reset:
`/c/pio/penv/Scripts/python.exe /c/tmp/capture_reset.py COM6 /c/tmp/x3-2c1.log 1800 --no-reset`
Expected at boot: `Found TTF family: BookerlyTTF (4 files)`.

- [ ] **Step 3: Install (Review Focus 5)**

Ask the user to choose Settings → Reader → Font family → BookerlyTTF and open a book.
Expected: the "Loading font" popup; `family installed: 4 styles`; `family loaded: ... B scratch`;
`TTF family BookerlyTTF at <pt> pt`; the page draws with bold and italic words; no watchdog reset; no
`glyph rendered without RenderLock`. Note the install time (log timestamps) and the first page's time.

- [ ] **Step 4: Read (Review Focus 4)**

Ask the user to turn 10-20 pages, including a chapter start with a heading.
Expected: headings draw (the body face, scaled); no missing glyphs in ordinary text.

- [ ] **Step 5: Size change (Review Focus 2)**

Ask the user to change the font size from the reader menu (for example 14 → 18 pt), then step down to the smallest
size.
Expected: `now at 18 pt`, pages laid out again, no popup and no `family installed`; the smallest size offered is
8 pt, and it is drawn sharp (native, not resampled).

- [ ] **Step 6: The next boot**

Ask the user to leave the book, reboot, and reopen it at the same place.
Expected: the first `unloaded:` line shows N rendered; after the reboot, reopening and turning the same pages logs
`unloaded: 0 glyphs rendered` or close to it, and many `read from the store`. Compare the first page's time with
Step 3 and with the built-in Bookerly at the same size (switch the family, reopen).

- [ ] **Step 7: Switching formats (Review Focus 1)**

If the card holds a `.cpfont` family, ask the user to select it and open a book, then select BookerlyTTF again.
Expected: each switch shows the popup; the TTF family installs again (about 10 s); no `refused` lines from
`EspNorFlash` or `FlashFontPartition`.

- [ ] **Step 8: A family that disappears (Review Focus 3)**

With BookerlyTTF selected and the reader closed, ask the user to open USB Transfer; rename the folder
(`$PY tools/serial_transfer.py --port COM6 mv /.crosspoint/fonts/BookerlyTTF /.crosspoint/fonts/BookerlyTTF.off`);
leave USB Transfer and open a book.
Expected: the book draws in a built-in font; no crash. Rename the folder back afterwards.

- [ ] **Step 9: Record**

In the spec (docs branch): section 8's table gets the 2c split (2c-1 to 2c-4) with this PR; 8.1 marks what 2c-1
did (field widths, batch records, add-then-rasterize, default-ignorables, RenderLock check, yields, partition
ownership, size re-claim) and leaves the rest for 2c-2 to 2c-4; the "Decisions this plan makes" list above goes
into sections 3.2, 7.1 and 7.5; Appendix A gets the X3 numbers (install time, first page cold and warm, heap after
opening a book, flash and RAM). Add a status block to the top of this plan. Commit on `docs/ttf-fonts-design`:

```bash
git -C /c/tmp/wt-ttf-docs add docs/superpowers
git -C /c/tmp/wt-ttf-docs commit -m "docs(ttf): Stage 2c-1 as built: TTF families in the reader, X3 results"
```
