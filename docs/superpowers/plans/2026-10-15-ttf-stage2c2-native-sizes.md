# TTF Stage 2c-2: native sizes. Implementation plan

> **Status (2026-10-15): done, branch `feat/ttf-native-sizes`.**
> - **Tasks 1-7** executed inline, each matching its stated test counts. The host suite was 2,255 rather than 2,246,
>   because master gained PR #473's 9 tests.
> - **Three-lane review** (memory and lifetimes; store and glyph paths; layout and integration): no Critical, five
>   Important, all fixed test-first:
>   - a stand-in face is scaled to the size it stands in for, so cached layouts stay valid (`ResolvedFont`,
>     `resolvedBaseScale`);
>   - a per-family ceiling (`TtfFont::maxNativePx()`, Bookerly 119 px) keeps every glyph's top inside `int8`;
>   - the ring keeps within 8 KB and is freed with the sizes;
>   - taking the last size entry makes the wipe pending;
>   - the ladder clamps a size as a float.
> - **After the fixes:** host suite 2,261.
> - **Task 8** ran on the X3; the results are in spec Appendix A, "Stage 2c-2".
> - **Measured:** the scratch block grows by 1.1 KB, not the 2.6 KB estimated below, and a size costs 540 B.
> - **Where the code differs from the task code below:** where those fixes went (`FontLookup.h`, `GfxRenderer.h`,
>   `TtfFont`, `FlashGlyphStore::sizeIndex`, `FontSizeLadder::resolveNative`).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A TTF family draws headings, CSS font sizes and drop caps from its outlines at their own pixel size,
instead of resampling the body face. The 2c-1 device run showed why: a drop cap was "clearly bitmap scaled up", and on
anti-aliased pages a native size is also 2.3-4.6x faster to draw than a resampled one (2c-1 plan, "Measured").

**Architecture:**

- **Every size is its own face, at a whole number of pixels per em,** from 10 to 120 px. A size's font ID is
  `base | px`: the family gets a window of 256 IDs whose high bits come from its identity, so a section laid out with
  them finds them again in the next session. Above 120 px the largest face is drawn scaled (a 4x drop cap on a 26 pt
  body); below 10 px the smallest.
- **Layout picks the size,** as pure data. `FontSizeLadder::setNative(base, bodyPpem, minPx, maxPx)` replaces the
  rungs for a TTF family: `resolve(pct)` returns `base | round(bodyPx * pct / 100)`. The leftover scale (under 1.5%
  from 16 pt up) falls inside the ladder's 3% dead zone, so nothing is resampled. `ChapterHtmlSlimParser` drops its
  one-auxiliary-face budget for such a ladder (the budget is the decompressor's four page slots; a native face is read
  from the glyph store), and a drop cap resolves its own size instead of the largest rung.
- **Faces are made on first use,** through the font resolver Stage 1 added to `GfxRenderer`:
  `SdCardFontManager::resolveTtfSize` decodes the ID and asks `TtfFont::nativeFonts(px)`. That builds a `Size`, the
  four styles' `EpdFontData` and providers, with nothing to render yet: glyphs are rendered and stored as they are
  looked up, as the body's are. Sizes live until the next book open; at most 16 are held, and when all are taken
  the nearest one serves.
- **`TtfFont` becomes a family plus sizes:** the body is one `Size`, each native size another. Each has its own entry
  in the store's size table and its own claim on it.
- **A full size table no longer stops the family:** it makes the wipe pending, but the sizes already claimed go on
  storing; the size the table refused is drawn from the ring until the wipe at the next book open. Today the whole
  family drops into ring mode, and native sizes would fill the 32 entries much sooner.
- **Prewarm reaches dormant sizes:** `FontCacheManager::prewarmCache` looks fonts up through the renderer (and so its
  resolver), not `fontMap` alone. A page laid out in an earlier session and drawn first in this one still renders its
  heading's glyphs in one batch.
- **The scratch block is planned for 120 px:** its output region reaches the 4 KB cap (about 2.6 KB more than 2c-1's
  26 pt plan), so any glyph of a native size fits, and its top stays inside `EpdGlyphRef`'s `int8`.

**Tech Stack:** C++20 host tests (gtest via CMake, UCRT64 gcc; ASan and UBSan where they link); firmware gnu++2a
(PlatformIO, ESP32-C3/S3); clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (branch `docs/ttf-fonts-design`): sections 3.3 (the
resolver), 5.5 (sizes), 6.1, 7.2, 7.4, 4.6 (a full store) and the 2c-2 obligations in 8.1 (the resolver's concurrency
and allocation; `prewarmCache` and dormant sizes; "For 2c-2, from the 2c-1 device run"). This plan departs from
5.5 in three points, recorded as decisions below and in the spec by Task 8.

**Pre-validated:** every task's code was built and tested in a prototype (`C:/tmp/wt-2c2-proto`), and the plan was
replayed task by task on a fresh tree at `0dba89ac8` (master with #473): each "see it fail" step fails as stated, each test count
matches, and every file the plan writes is identical to the prototype. `default` builds (flash 96.3%, +1.7 KB). It
has not run on the X3 yet (Task 8).

## Global Constraints

- **Branch:** `feat/ttf-native-sizes` from `origin/master` (2c-1 is PR #472; validated on `0dba89ac8`).
- **Under RenderLock:** every `TtfFont` call runs under `RenderLock` (spec 3.4), and so does the resolver: layout and
  drawing both hold it. `TtfFont` logs a violation once (`Hooks::renderLockHeld`); it does not assert.
- **Write once:** every flash byte is programmed at most once between erases. Every `TtfFont` test ends with
  `flash.violations() == 0` and `flash.reprograms() == 0` (its `TearDown`).
- **Heap:** every allocation is `makeUniqueNoThrow`, null-checked and logged (`heap-discipline` skill); a size that
  cannot be made falls back (the nearest size, else the body face) instead of failing the lookup, because layout
  already chose its font ID.
- **No floating point** in `lib/EpdFont/ttf` (`scripts/check_no_float.py` runs after every firmware link).
  `FontSizeLadder` (lib/Epub) already computes in float and keeps doing so.
- **Includes:** code outside `lib/EpdFont/ttf` includes its headers as `<ttf/TtfFont.h>`.
- **Section cache:** a change to how a block's font is chosen is a cache format change: this plan bumps
  `SECTION_FILE_VERSION` to 82 (Task 6). The ladder stays out of the property hash: it is derived from the body font
  ID, which carries the family identity.
- **Host test commands** (Git Bash, from the worktree root): `export PATH=/c/msys64/ucrt64/bin:$PATH` once; configure
  with
  `cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src`
  (the generator matters: the full suite needs `cmake --build build/test -j 8 -- -k` to get past
  `epub_build_inventory`, which does not build on Windows (`dlfcn.h`)). The steps below write `cmake -S test -B
  build/test` for short: once configured, that reuses the generator.
- **Firmware builds:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
  `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`; one env at a time.
- **Formatting:** clang-format 21 (`/c/tmp/cf21/bin/clang-format-21.exe -i <files>`; CI checks with 21). The code
  below is already formatted.
- **Commits:** semantic prefixes (`feat:`, `fix:`, `test:`, `refactor:`); no `Co-Authored-By` trailer. PR title
  `feat(ttf): ...` (the Title Check needs the prefix).

## Review Focus

The five situations this plan's host tests do not reach that are most likely to bite, most likely first. Each is
checked on the X3 in Task 8 (step numbers there), or by the final reviewer:

1. **A book with more distinct sizes than 16.** Expected: the nearest size held serves the rest (a pixel off in a
   gradient like Alice's mouse tale, more when the sizes are far apart); layout and drawing agree, because one font
   ID maps to one family for the session. Task 8, step 6.
2. **Little heap when a size is first used** (it is made in the middle of layout or a draw). Expected: no abort: the
   `Size` is `makeUniqueNoThrow` and a failure falls back to the nearest size or the body face. The renderer's
   `fontMap` emplace (about 48 B) cannot be made nothrow; the reviewer checks nothing else allocates on that path.
3. **A font change, or the reader closing, with sizes registered.** Expected: every native ID leaves the renderer
   before its `Size` is freed (`dropTtfNativeSizes` before `ttf_.reset()`), and the resolver is uninstalled at unload,
   so no lookup reaches a freed font. Task 8, step 7.
4. **A chapter laid out in an earlier session, drawn first.** Expected: the same IDs (the window is derived from the
   family identity), faces made on the first lookup, glyphs from the store; the prewarm reaches them through the
   lookup. Task 8, step 5.
5. **The store's size table filling over many books.** Expected: the new size draws from the ring for the rest of the
   book, the others keep storing, and the next book open wipes and starts the table over. Host-tested in `TtfFont`;
   on the device only the log shows it (Task 8, step 8).

## File Structure

| File | Task | Responsibility |
| --- | --- | --- |
| `lib/EpdFont/ttf/FlashGlyphStore.{h,cpp}` | 1 | `writesStopped()`; a full size table stops only new sizes |
| `lib/EpdFont/ttf/TtfFont.{h,cpp}` | 2 | `Size`; the body and native sizes; `nativeFonts`, `dropNativeSizes`; scratch for 120 px |
| `lib/EpdFont/ttf/TtfFamily.h` | 3 | the native ID window: `isNativeId`, `nativeIdPixels`, `nativeBaseFrom` |
| `lib/EpdFont/SdCardFontManager.{h,cpp}`, `src/SdCardFontSystem.{h,cpp}` | 3 | the resolver, the window, dropping sizes at book open |
| `lib/GfxRenderer/FontCacheManager.{h,cpp}`, `GfxRenderer.{h,cpp}` | 4 | prewarm through the renderer's lookup |
| `lib/Epub/Epub/FontSizeLadder.h` | 5 | `setNative()`, `resolveNative()` |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}`, `Section.cpp` | 6 | no auxiliary-face budget for native sizes; drop caps; version 82 |
| `src/activities/reader/EpubReaderActivity.cpp` | 7 | the reader's ladder for a TTF family |
| `test/glyph_store`, `test/ttf_font`, `test/font_cache_manager` (new), `test/word_size_layout`, `test/epub_pipeline` | 1-6 | tests |

## Decisions this plan makes (record them in the spec, Task 8)

- **Whole-pixel sizes instead of rungs** (spec 5.5 planned the 2 pt ladder's rungs plus a resampled residual). A
  size is drawn at `round(bodyPx * pct / 100)` px, so the residual stays inside the 3% dead zone; the spec's nearest
  rung would leave several percent to resample.
- **No dormant table:** the font ID carries the size (`base | px`), so the resolver decodes it; nothing is allocated
  or listed until a size is used.
- **Sizes are dropped at book open,** not kept to family unload: a book's sizes are a handful, and the next book
  wants others. At most 16 at once (about 0.6 KB each; Task 8 logs the size); beyond that the nearest serves.
- **10-120 px.** 120 px is a 4x drop cap on a 14 pt body; above it the largest face is scaled, as the spec's 8.1
  note asks ("a resampled fallback above what the region and `EpdGlyphRef` hold").
- **A full size table stops only new sizes** (spec 4.6 made the whole store wait for the wipe).
- **Previews move to 2c-4.** A family that is not installed can only be previewed from the SD card, which needs
  `FontBytes`' SD mode (2c-4). Today a TTF family is previewed in the built-in font (2c-1 behaviour, unchanged).
- **Inline sizes stay scaled:** a word whose own size differs from its block's (a `<small>` or `<sup>` inside a
  paragraph) is still the block's face scaled. Native per-word sizes would need a font ID per word in the section
  format. Deferred.

---

### Task 1: A full size table stops only new sizes

Today `FlashGlyphStore::sizeIndex()` answers a full table by setting wipe pending, and wipe pending refuses every
commit: the whole family falls back to the ring until the next book open. Native sizes make a full table far more
likely (a book adds its handful of sizes to the table), so this splits the two meanings of wipe pending:

- **`wipePending()`:** a wipe is due at the next book open (the flag reaches flash, so a reboot finishes it);
- **`writesStopped()`:** nothing is written until then: the log or the index filled, a write failed, or the store
  opened with a wipe pending (what filled it last session is not known).

A full size table sets the first only. The renamed `stopWrites()` sets both, for every other reason.

**Files:**
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h`, `lib/EpdFont/ttf/FlashGlyphStore.cpp`
- Test: `test/glyph_store/GlyphStoreSizesPairsWipeTest.cpp`

**Interfaces:**
- Produces:
  - `bool FlashGlyphStore::writesStopped() const`.
  - `sizeIndex()` on a full table: false, `wipePending()` true, `writesStopped()` unchanged; commits, `markNoGlyph`
    and `storePair` under claimed sizes go on working until `wipe()`.

- [ ] **Step 1: Branch**

```bash
git fetch origin
git switch -c feat/ttf-native-sizes origin/master
git submodule update --init freeink-sdk
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src
```

- [ ] **Step 2: Write the failing tests**

A new test pins the new behaviour; the pair test that used a full size table to stop writes now needs a store that
opened with a wipe pending.

<!-- plan-edit: test/glyph_store/GlyphStoreSizesPairsWipeTest.cpp -->
```cpp
TEST_F(GlyphStorePairs, PairsAreNotStoredWhileWipeIsPending) {
  fillSizeTable(store);
  EXPECT_FALSE(store.storePair('A', 'V', 0, -74));
  EXPECT_EQ(0u, store.usedPairSlots());
}
```
with:

```cpp
TEST_F(GlyphStorePairs, PairsAreNotStoredWhileWritesAreStopped) {
  fillSizeTable(store);
  store.close();  // a store that opens with wipe pending writes nothing until the wipe
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_TRUE(store.writesStopped());
  EXPECT_FALSE(store.storePair('A', 'V', 0, -74));
  EXPECT_EQ(0u, store.usedPairSlots());
}
```
<!-- plan-edit: test/glyph_store/GlyphStoreSizesPairsWipeTest.cpp -->
```cpp
TEST_F(GlyphStoreSizes, ASizeEntryCutShortIsNeverReused) {
```
with:

```cpp
TEST_F(GlyphStoreSizes, AFullSizeTableStopsOnlyNewSizes) {
  for (uint32_t i = 0; i < kSizeEntries; ++i) ASSERT_EQ(static_cast<int>(i), claim((20 + i) << 6));
  EXPECT_EQ(-1, claim(99 << 6));
  ASSERT_TRUE(store.wipePending());
  EXPECT_FALSE(store.writesStopped());
  // The claimed sizes still store their glyphs, missing glyphs and pairs until the wipe.
  alignas(4) uint8_t buffer[256];
  GlyphBatch batch(buffer, sizeof buffer);
  ASSERT_TRUE(stage(batch, makeKey('A', 0, 5), 9, 12));
  EXPECT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_EQ(Lookup::Found, store.find(makeKey('A', 0, 5), nullptr));
  EXPECT_EQ(Commit::Stored, store.markNoGlyph(makeKey(0x2603, 0, 5)));
  EXPECT_TRUE(store.storePair('A', 'V', 0, -74));
  // The next session wipes first.
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.wipePending());
  EXPECT_TRUE(store.writesStopped());
}

TEST_F(GlyphStoreSizes, ASizeEntryCutShortIsNeverReused) {
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_brief=1`
Expected: compile errors, `'class FlashGlyphStore' has no member named 'writesStopped'`.

- [ ] **Step 4: Implement**

<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  /// The size-table index for a pixels-per-em (26.6), claiming a free entry (a flash write) on first use.
  /// False when all entries are taken (wipe is then pending), on a write error, or for a new size while wipe
  /// is pending. A wipe empties the table: indices claimed before it are void, and commits under them are
  /// refused until they are claimed again.
```
with:

```cpp
  /// The size-table index for a pixels-per-em (26.6), claiming a free entry (a flash write) on first use.
  /// False when all entries are taken, on a write error, or for a new size while wipe is pending. A full table
  /// makes the wipe pending but stops nothing else: the claimed sizes go on storing their glyphs until the wipe.
  /// A wipe empties the table: indices claimed before it are void, and commits under them are refused until
  /// they are claimed again.
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  /// Stores a pair's kerning in font units. False when it was not stored: the pair index is full (pairs then
  /// come from the GPOS reader every time; never a reason to wipe), wipe is pending, or a write failed (which
  /// sets wipe pending).
```
with:

```cpp
  /// Stores a pair's kerning in font units. False when it was not stored: the pair index is full (pairs then
  /// come from the GPOS reader every time; never a reason to wipe), writes are stopped, or a write failed (which
  /// stops them).
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  bool wipePending() const { return wipePending_; }
```
with:

```cpp
  bool wipePending() const { return wipePending_; }
  /// Nothing is written until the next wipe: the log or the index filled, a write failed, or the store opened
  /// with a wipe pending. A full size table alone does not stop writes.
  bool writesStopped() const { return writesStopped_; }
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  void setWipePending();
  bool recordWipePending();
```
with:

```cpp
  void stopWrites();
  bool recordWipePending();
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.h -->
```cpp
  bool wipePending_ = false;
  std::unique_ptr
```
with:

```cpp
  bool wipePending_ = false;
  bool writesStopped_ = false;
  std::unique_ptr
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  wipePending_ = wipeWord != kErased;
  for (uint8_t i = 0;
```
with:

```cpp
  wipePending_ = wipeWord != kErased;
  writesStopped_ = wipePending_;  // what filled the store, or failed, last session is not known
  for (uint8_t i = 0;
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  open_ = false;
  wipePending_ = false;
  installing_.reset();
```
with:

```cpp
  open_ = false;
  wipePending_ = false;
  writesStopped_ = false;
  installing_.reset();
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  if (wipePending_) return Commit::Full;
  if (logUsed_ + batch.used_
```
with:

```cpp
  if (writesStopped_) return Commit::Full;
  if (logUsed_ + batch.used_
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
      LOG_INF("GST", "full: a probe chain passed %u slots; wipe pending", static_cast<unsigned>(kMaxProbe));
      setWipePending();
```
with:

```cpp
      LOG_INF("GST", "full: a probe chain passed %u slots; wipe pending", static_cast<unsigned>(kMaxProbe));
      stopWrites();
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  LOG_ERR("GST", "slot write failed; wipe pending");
  setWipePending();
```
with:

```cpp
  LOG_ERR("GST", "slot write failed; wipe pending");
  stopWrites();
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
            static_cast<unsigned>(geometry_.indexSlots));
    setWipePending();
    return Commit::Full;
```
with:

```cpp
            static_cast<unsigned>(geometry_.indexSlots));
    stopWrites();
    return Commit::Full;
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
            static_cast<unsigned>(base));
    setWipePending();
    return Commit::WriteFailed;
```
with:

```cpp
            static_cast<unsigned>(base));
    stopWrites();
    return Commit::WriteFailed;
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  if (wipePending_) return Commit::Full;
  if (usedSlots_ + 1 > limit(geometry_.indexSlots, kIndexLoadPercent)) {
    setWipePending();
    return Commit::Full;
  }
```
with:

```cpp
  if (writesStopped_) return Commit::Full;
  if (usedSlots_ + 1 > limit(geometry_.indexSlots, kIndexLoadPercent)) {
    stopWrites();
    return Commit::Full;
  }
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  if (free < 0) {
    LOG_INF("GST", "full: all %u sizes in use; wipe pending", static_cast<unsigned>(kSizeEntries));
    setWipePending();
    return false;
  }
```
with:

```cpp
  if (free < 0) {
    // Only a new size is refused: the claimed ones go on storing, and the wipe at the next book open frees the
    // table.
    LOG_INF("GST", "full: all %u sizes in use; wipe pending", static_cast<unsigned>(kSizeEntries));
    wipePending_ = true;
    recordWipePending();
    return false;
  }
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
    LOG_ERR("GST", "size table write failed; wipe pending");
    setWipePending();
```
with:

```cpp
    LOG_ERR("GST", "size table write failed; wipe pending");
    stopWrites();
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  if (!open_ || wipePending_ || key == kErased || style > 3) return false;
```
with:

```cpp
  if (!open_ || writesStopped_ || key == kErased || style > 3) return false;
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  LOG_ERR("GST", "pair write failed; wipe pending");
  setWipePending();
```
with:

```cpp
  LOG_ERR("GST", "pair write failed; wipe pending");
  stopWrites();
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
void FlashGlyphStore::setWipePending() {
  if (wipePending_) return;
  wipePending_ = true;  // holds for the session even if the flash write below fails
  recordWipePending();
}
```
with:

```cpp
void FlashGlyphStore::stopWrites() {
  writesStopped_ = true;
  if (wipePending_) return;
  wipePending_ = true;  // holds for the session even if the flash write below fails
  recordWipePending();
}
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  wipePending_ = true;
  if (!recordWipePending()) return false;
```
with:

```cpp
  wipePending_ = true;
  writesStopped_ = true;
  if (!recordWipePending()) return false;
```
<!-- plan-edit: lib/EpdFont/ttf/FlashGlyphStore.cpp -->
```cpp
  ++generation_;
  wipePending_ = false;
  return true;
```
with:

```cpp
  ++generation_;
  wipePending_ = false;
  writesStopped_ = false;
  return true;
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build build/test --target GlyphStoreTest -j 8 && ./build/test/glyph_store/GlyphStoreTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 92 tests.` (91 before; the power-cut sweeps still pass: a wipe sets both flags first.)

- [ ] **Step 6: Commit**

```bash
git add lib/EpdFont/ttf/FlashGlyphStore.h lib/EpdFont/ttf/FlashGlyphStore.cpp test/glyph_store/GlyphStoreSizesPairsWipeTest.cpp
git commit -m "feat(ttf): a full size table stops only new sizes, not the store's writes"
```

---

### Task 2: `TtfFont` draws native sizes

`TtfFont` held one size, the body's, in member fields. It becomes a family plus sizes:

- **`Size`:** a pixel size, 26.6. It holds the four styles' `EpdFontData`, `EpdFont` and `StyleProvider`, plus its own
  claim on the store's size table. The claim is made at the first glyph and asked again after a wipe; a claim the
  full table refused is not retried before the wipe, and that size's glyphs come from the ring meanwhile.
- **The body:** a `Size` member, set by `setBodySize()` as before.
- **Native sizes:** `nativeFonts(px)` makes one on first use (`makeUniqueNoThrow`); at most `kMaxNativeSizes`, after
  which the nearest held one serves. `dropNativeSizes()` frees them all.
- **Shared by every size:** each style's `ttf::Face`, the rasterizer, the ring and the store.
- **The scratch block is planned for `kMaxNativePx`** (120 px), the largest size drawn, so the output region holds
  any glyph of a native size; the `int8` placement guard stays.

`StyleProvider` gets a `Size*`, so a lookup knows which size it serves. The body's public `glyph()` and `prefetch()`
keep their signatures (the tests and the body's providers use them).

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`
- Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: Task 1 (a size the full table refuses leaves the other sizes storing).
- Produces:
  - `static constexpr uint8_t TtfFont::kMinNativePx = 10`, `kMaxNativePx = 120`, `kMaxNativeSizes = 16`.
  - `bool TtfFont::nativeFonts(uint8_t px, const EpdFont* (&fonts)[4])`: the four fonts of a native size (nullptr
    for a style the family lacks); the nearest held size when all are taken or memory is short; false when `px` is
    out of range, a line height does not fit, or no size can serve.
  - `void TtfFont::dropNativeSizes()`, `uint8_t TtfFont::nativeSizeCount() const`.
  - `uint32_t TtfFont::bodyPpem() const`: the body size's pixels per em (26.6).
  - The "family loaded" log line also gives `sizeof(Size)` (Task 8 reads it on the device).

- [ ] **Step 1: Write the failing tests**

`reference()` gains a pixel-size twin, `referenceAt()`, planned for the largest native size; the new tests ask a
size's glyphs through its provider, as the renderer does.

<!-- plan-edit: test/ttf_font/TtfFontTest.cpp -->
```cpp
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
```
with:

```cpp
/// The same at a pixels-per-em (26.6).
Reference referenceAt(const std::vector<uint8_t>& file, const uint32_t cp, const uint32_t ppem) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(
      ttf::planScratch(face, static_cast<uint32_t>(TtfFont::kMaxNativePx) << 6, ttf::kPoolBytesFlash)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.render(bytes, face, ttf::glyphIndex(bytes, face, cp), ppem, &ref.metrics));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

Reference reference(const std::vector<uint8_t>& file, const uint32_t cp, const uint8_t points) {
  return referenceAt(file, cp, ttf::ppemFromPoints(points));
}

constexpr uint32_t px(const uint32_t pixels) { return pixels << 6; }
```
<!-- plan-append: test/ttf_font/TtfFontTest.cpp -->
```cpp
// --- Native sizes: headings, CSS sizes and drop caps at their own pixel size ---

namespace {

/// A native size's glyph, as the renderer asks for it: through the style's provider.
const EpdGlyph* nativeGlyph(const EpdFont* font, const uint32_t cp) {
  return font ? font->data->provider->missGlyph(cp) : nullptr;
}

}  // namespace

TEST_F(TtfFontTest, ANativeSizeIsDrawnAtItsOwnPixelSize) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(40, fonts));
  for (const uint8_t style : {kRegular, kBold, kItalic}) {
    ASSERT_NE(nullptr, fonts[style]);
    expectDrawnAs(referenceAt(files[style].bytes, 'M', px(40)), *font, style, nativeGlyph(fonts[style], 'M'));
  }
  // Its line metrics are the face's at 40 px.
  FontBytes bytes(files[kRegular].bytes.data(), static_cast<uint32_t>(files[kRegular].bytes.size()));
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  const ttf::LineMetrics expected = ttf::lineMetrics(face, px(40));
  EXPECT_EQ(expected.advanceY, fonts[kRegular]->data->advanceY);
  EXPECT_EQ(expected.ascender, fonts[kRegular]->data->ascender);
  EXPECT_EQ(expected.descender, fonts[kRegular]->data->descender);
}

TEST_F(TtfFontTest, ANativeSizeIsMadeOnceAndStoredApartFromTheBody) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* first[4] = {};
  const EpdFont* again[4] = {};
  const EpdFont* other[4] = {};
  ASSERT_TRUE(font->nativeFonts(40, first));
  ASSERT_TRUE(font->nativeFonts(40, again));
  ASSERT_TRUE(font->nativeFonts(41, other));
  EXPECT_EQ(first[kRegular], again[kRegular]);
  EXPECT_NE(first[kRegular], other[kRegular]);
  EXPECT_EQ(2u, font->nativeSizeCount());
  const EpdGlyph* body = font->glyph(kRegular, 'M');
  const EpdGlyph* at40 = nativeGlyph(first[kRegular], 'M');
  ASSERT_NE(nullptr, at40);
  EXPECT_NE(body->width, at40->width);
  EXPECT_EQ(at40, nativeGlyph(again[kRegular], 'M'));  // read from the store the second time
  EXPECT_EQ(2u, font->renders());
}

TEST_F(TtfFontTest, NativeSizesOutsideTheRangeAreRefused) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  EXPECT_FALSE(font->nativeFonts(TtfFont::kMinNativePx - 1, fonts));
  EXPECT_FALSE(font->nativeFonts(TtfFont::kMaxNativePx + 1, fonts));
  EXPECT_EQ(nullptr, fonts[kRegular]);
  EXPECT_EQ(0u, font->nativeSizeCount());
}

TEST_F(TtfFontTest, TheLargestNativeSizeDrawsEveryGlyphOfTheFamily) {
  // Planned for: the output region holds each glyph, and its top fits EpdGlyphRef.
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(TtfFont::kMaxNativePx, fonts));
  for (const uint32_t cp : {uint32_t{'W'}, uint32_t{'@'}, uint32_t{'Q'}, uint32_t{0xC5}}) {
    for (const uint8_t style : {kRegular, kBold, kItalic}) {
      const EpdGlyph* glyph = nativeGlyph(fonts[style], cp);
      ASSERT_NE(nullptr, glyph) << std::hex << cp << " style " << int{style};
      expectDrawnAs(referenceAt(files[style].bytes, cp, px(TtfFont::kMaxNativePx)), *font, style, glyph);
    }
  }
}

TEST_F(TtfFontTest, WhenEveryNativeSizeIsHeldTheNearestServesUntilTheyAreDropped) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  for (uint8_t i = 0; i < TtfFont::kMaxNativeSizes; ++i) {
    ASSERT_TRUE(font->nativeFonts(static_cast<uint8_t>(TtfFont::kMinNativePx + i), fonts)) << int{i};
  }
  const EpdFont* largestHeld[4] = {};
  ASSERT_TRUE(font->nativeFonts(TtfFont::kMinNativePx + TtfFont::kMaxNativeSizes - 1, largestHeld));
  ASSERT_TRUE(font->nativeFonts(TtfFont::kMaxNativePx, fonts));
  EXPECT_EQ(largestHeld[kRegular], fonts[kRegular]);
  EXPECT_EQ(TtfFont::kMaxNativeSizes, font->nativeSizeCount());
  font->dropNativeSizes();
  EXPECT_EQ(0u, font->nativeSizeCount());
  ASSERT_TRUE(font->nativeFonts(TtfFont::kMaxNativePx, fonts));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(TtfFont::kMaxNativePx)), *font, kRegular,
                nativeGlyph(fonts[kRegular], 'M'));
}

TEST_F(TtfFontTest, ADroppedSizeMadeAgainFindsItsStoredGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(40, fonts));
  ASSERT_NE(nullptr, nativeGlyph(fonts[kRegular], 'M'));
  font->dropNativeSizes();
  ASSERT_TRUE(font->nativeFonts(40, fonts));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(40)), *font, kRegular, nativeGlyph(fonts[kRegular], 'M'));
  EXPECT_EQ(1u, font->renders());
}

TEST_F(TtfFontTest, ASizeTheFullTableRefusesIsDrawnFromTheRingWhileTheOthersStore) {
  const auto font = open();  // the body takes the first size entry
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  // Every native size takes an entry at its first glyph: 16, dropped, then 15 more fill the 32 entries.
  for (uint8_t pixels = 20; pixels < 20 + 31; ++pixels) {
    if (font->nativeSizeCount() == TtfFont::kMaxNativeSizes) font->dropNativeSizes();
    ASSERT_TRUE(font->nativeFonts(pixels, fonts));
    ASSERT_NE(nullptr, nativeGlyph(fonts[kRegular], 'a')) << int{pixels};
  }
  font->dropNativeSizes();
  ASSERT_TRUE(font->nativeFonts(60, fonts));
  const EpdGlyph* refused = nativeGlyph(fonts[kRegular], 'M');
  const uint8_t* bitmap = nullptr;
  EXPECT_TRUE(font->ownsBitmap(refused, bitmap));  // drawn without being stored
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(60)), *font, kRegular, refused);
  EXPECT_FALSE(font->ringMode());
  EXPECT_FALSE(font->ownsBitmap(font->glyph(kRegular, 'Q'), bitmap));  // the body still stores its glyphs
  // The wipe at the next book open frees the table.
  EXPECT_TRUE(font->wipeIfPending());
  EXPECT_FALSE(font->ownsBitmap(nativeGlyph(fonts[kRegular], 'M'), bitmap));
  expectDrawnAs(referenceAt(files[kRegular].bytes, 'M', px(60)), *font, kRegular, nativeGlyph(fonts[kRegular], 'M'));
}

TEST_F(TtfFontTest, PrefetchStoresANativeSizesGlyphs) {
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(48, fonts));
  fonts[kRegular]->data->provider->prefetch("Chapter One");
  const uint32_t renders = font->renders();
  EXPECT_EQ(10u, renders);  // C h a p t e r O n and the space (no ink, but a glyph): each once
  for (const char c : std::string("Chapter One")) {
    expectDrawnAs(referenceAt(files[kRegular].bytes, static_cast<uint8_t>(c), px(48)), *font, kRegular,
                  nativeGlyph(fonts[kRegular], static_cast<uint8_t>(c)));
  }
  EXPECT_EQ(renders, font->renders());
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: compile errors, `'kMaxNativePx' is not a member of 'TtfFont'` and `'class TtfFont' has no member named
'nativeFonts'`.

- [ ] **Step 3: Implement**

The header: constants, the native-size API, `Size`, and the private functions that take a `Size&`:

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
/// It serves one face, the body size: four EpdFontData without tables, so every lookup goes to a per-style
/// GlyphProvider adapter. Kerning, ligatures and the size-ladder rungs come in later stages.
```
with:

```cpp
/// It serves the body size and, on demand, native sizes (headings, CSS sizes, drop caps at their own pixel size):
/// each size is four EpdFontData without tables, so every lookup goes to a per-style GlyphProvider adapter, and has
/// its own entry in the store's size table. Kerning and ligatures come in later stages.
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  static constexpr uint8_t kMaxPointSize = 26;
  static constexpr uint8_t kRingSlots = 8;
```
with:

```cpp
  static constexpr uint8_t kMaxPointSize = 26;
  /// Native sizes, in whole pixels per em. The scratch block is planned for the largest: its output region then
  /// holds any glyph up to 4 KB, and a glyph's top stays inside EpdGlyphRef's int8. Larger text is drawn from the
  /// largest size scaled.
  static constexpr uint8_t kMinNativePx = 10;
  static constexpr uint8_t kMaxNativePx = 120;
  /// Native sizes held at once, until dropNativeSizes().
  static constexpr uint8_t kMaxNativeSizes = 16;
  static constexpr uint8_t kRingSlots = 8;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  /// The body face's font for `style`; nullptr for a style the family lacks (EpdFontFamily then falls back).
  const EpdFont* font(uint8_t style) const;
```
with:

```cpp
  /// The body face's font for `style`; nullptr for a style the family lacks (EpdFontFamily then falls back).
  const EpdFont* font(uint8_t style) const;
  /// The body face's pixels per em (26.6), which native sizes are measured against; 0 before setBodySize().
  uint32_t bodyPpem() const { return body_.ppem; }

  /// The four fonts of the native size `px` (whole pixels per em), made on first use; nullptr for a style the
  /// family lacks. False, with `fonts` untouched, when `px` is outside kMinNativePx..kMaxNativePx, a line height
  /// does not fit EpdFontData, all kMaxNativeSizes are in use, or memory is short. They live until
  /// dropNativeSizes() or the font's end.
  bool nativeFonts(uint8_t px, const EpdFont* (&fonts)[4]);
  /// Frees every native size. Only once nothing refers to their fonts any more (their font IDs removed).
  void dropNativeSizes();
  uint8_t nativeSizeCount() const;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  // The provider side; the per-style adapters forward here.
  const EpdGlyph* glyph(uint8_t style, uint32_t codepoint);
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
  void prefetch(uint8_t style, const char* utf8Text);

 private:
  class StyleProvider final : public GlyphProvider {
   public:
    TtfFont* owner = nullptr;
    uint8_t style = 0;
    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return owner->glyph(style, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      return owner->ownsBitmap(glyph, bitmap);
    }
    void prefetch(const char* utf8Text) override { owner->prefetch(style, utf8Text); }
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
```
with:

```cpp
  // The body size's glyphs, as its per-style adapters see them.
  const EpdGlyph* glyph(uint8_t style, uint32_t codepoint) { return glyph(body_, style, codepoint); }
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
  void prefetch(uint8_t style, const char* utf8Text) { prefetch(body_, style, utf8Text); }

 private:
  struct Size;

  class StyleProvider final : public GlyphProvider {
   public:
    TtfFont* owner = nullptr;
    Size* size = nullptr;
    uint8_t style = 0;
    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return owner->glyph(*size, style, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      return owner->ownsBitmap(glyph, bitmap);
    }
    void prefetch(const char* utf8Text) override { owner->prefetch(*size, style, utf8Text); }
  };

  /// One pixel size: its index in the store's size table and the four styles' fonts. Never copied or moved: each
  /// font points at its own data, and each provider at its size.
  struct Size {
    struct StyleFont {
      EpdFontData data{};
      EpdFont font{&data};
      StyleProvider provider;
    };
    Size() = default;
    Size(const Size&) = delete;
    Size& operator=(const Size&) = delete;

    uint32_t ppem = 0;  // 26.6; 0 until configured
    uint8_t index = 0;
    bool indexValid = false;
    bool claimTried = false;  // in store generation `generation`
    uint32_t generation = 0;
    StyleFont styles[4];
  };

  // A style's file, shared by every size.
  struct Style {
    bool present = false;
    uint8_t block = 0;  // the store's style block
    uint32_t bytes = 0;
    ttf::Face face;
  };
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  bool fontBytes(uint8_t style, FontBytes* bytes);
  bool storeKey(uint8_t style, uint32_t codepoint, uint32_t* key);
  void claimSize();
  const EpdGlyph* render(uint8_t style, uint32_t codepoint, bool haveKey, uint32_t key);
```
with:

```cpp
  bool fontBytes(uint8_t style, FontBytes* bytes);
  bool configure(Size& size, uint32_t ppem);
  Size* nativeSize(uint8_t px);
  bool claimSize(Size& size);
  bool storeKey(Size& size, uint8_t style, uint32_t codepoint, uint32_t* key);
  const EpdGlyph* glyph(Size& size, uint8_t style, uint32_t codepoint);
  void prefetch(Size& size, uint8_t style, const char* utf8Text);
  const EpdGlyph* render(const Size& size, uint8_t style, uint32_t codepoint, bool haveKey, uint32_t key);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  Style styles_[4];
  uint32_t ppem_ = 0;
  uint8_t pointSize_ = 0;
  uint8_t sizeIndex_ = 0;
  bool sizeValid_ = false;
  uint32_t sizeGeneration_ = 0;
  bool ringMode_ = false;
```
with:

```cpp
  Style styles_[4];
  Size body_;
  uint8_t pointSize_ = 0;
  std::unique_ptr<Size> native_[kMaxNativeSizes];
  bool ringMode_ = false;
```

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  /// The four fonts of the native size `px` (whole pixels per em), made on first use; nullptr for a style the
  /// family lacks. False, with `fonts` untouched, when `px` is outside kMinNativePx..kMaxNativePx, a line height
  /// does not fit EpdFontData, all kMaxNativeSizes are in use, or memory is short. They live until
  /// dropNativeSizes() or the font's end.
```
with:

```cpp
  /// The four fonts of the native size `px` (whole pixels per em), made on first use; nullptr for a style the
  /// family lacks. When all kMaxNativeSizes are held, or memory is short, the nearest size held serves instead, a
  /// few percent off. False, with `fonts` untouched, when `px` is outside kMinNativePx..kMaxNativePx, a line
  /// height does not fit EpdFontData, or no size can serve. They live until dropNativeSizes() or the font's end.
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  Size* nativeSize(uint8_t px);
```
with:

```cpp
  Size* nativeSize(uint8_t px);
  Size* nearestNative(uint32_t ppem);
```

The source: the log line, the provider wiring moved into `configure()`, the plan for 120 px, the size functions,
and every glyph path taking its `Size`:

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  LOG_INF("TTF", "family loaded: %u B scratch, log %lu of %lu B used",
          static_cast<unsigned>(rasterizer_.scratchBytes()), static_cast<unsigned long>(store_.logUsed()),
          static_cast<unsigned long>(store_.geometry().logSize));
```
with:

```cpp
  LOG_INF("TTF", "family loaded: %u B scratch, %u B per native size, log %lu of %lu B used",
          static_cast<unsigned>(rasterizer_.scratchBytes()), static_cast<unsigned>(sizeof(Size)),
          static_cast<unsigned long>(store_.logUsed()), static_cast<unsigned long>(store_.geometry().logSize));
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
TtfFont::TtfFont(NorFlash& flash) : flash_(flash), store_(flash) {
  for (uint8_t s = 0; s < 4; ++s) {
    styles_[s].provider.owner = this;
    styles_[s].provider.style = s;
  }
}
```
with:

```cpp
TtfFont::TtfFont(NorFlash& flash) : flash_(flash), store_(flash) {}
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    // Field by field: assigning a fresh Style would point its EpdFont at the temporary's data.
    style.present = false;
    style.face = {};
    style.data = EpdFontData{};
    if (files[s].size == 0) continue;
```
with:

```cpp
    style.present = false;
    style.face = {};
    if (files[s].size == 0) continue;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    style.present = true;
    *plan =
        ttf::mergePlans(*plan, ttf::planScratch(style.face, ttf::ppemFromPoints(kMaxPointSize), ttf::kPoolBytesFlash));
```
with:

```cpp
    style.present = true;
    // Planned for the largest size drawn: a native size (5 x 26 pt's pixels per em, about) above the body sizes.
    const uint32_t largest = std::max(ttf::ppemFromPoints(kMaxPointSize), static_cast<uint32_t>(kMaxNativePx) << 6);
    *plan = ttf::mergePlans(*plan, ttf::planScratch(style.face, largest, ttf::kPoolBytesFlash));
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
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
```
with:

```cpp
bool TtfFont::configure(Size& size, const uint32_t ppem) {
  ttf::LineMetrics metrics[4];
  for (uint8_t s = 0; s < 4; ++s) {
    if (!styles_[s].present) continue;
    metrics[s] = ttf::lineMetrics(styles_[s].face, ppem);
    if (metrics[s].advanceY <= 0 || metrics[s].advanceY > 255) {
      LOG_ERR("TTF", "style %u at %lu/64 px: line height %ld does not fit", s, static_cast<unsigned long>(ppem),
              static_cast<long>(metrics[s].advanceY));
      return false;
    }
  }
  size.ppem = ppem;
  size.claimTried = false;
  for (uint8_t s = 0; s < 4; ++s) {
    Size::StyleFont& style = size.styles[s];
    style.provider.owner = this;
    style.provider.size = &size;
    style.provider.style = s;
    style.data = EpdFontData{};
    if (!styles_[s].present) continue;
    style.data.advanceY = static_cast<uint8_t>(metrics[s].advanceY);
    style.data.ascender = metrics[s].ascender;
    style.data.descender = metrics[s].descender;
    style.data.is2Bit = true;
    style.data.bitmap = store_.logBase();
    style.data.provider = &style.provider;
  }
  return true;
}

bool TtfFont::setBodySize(const uint8_t pointSize) {
  if (pointSize == 0 || pointSize > kMaxPointSize || !styles_[0].present) return false;
  if (!configure(body_, ttf::ppemFromPoints(pointSize))) return false;
  pointSize_ = pointSize;
  claimSize(body_);
  return true;
}

const EpdFont* TtfFont::font(const uint8_t style) const {
  return style < 4 && styles_[style].present && body_.ppem != 0 ? &body_.styles[style].font : nullptr;
}

TtfFont::Size* TtfFont::nativeSize(const uint8_t px) {
  if (px < kMinNativePx || px > kMaxNativePx || !styles_[0].present) return nullptr;
  const uint32_t ppem = static_cast<uint32_t>(px) << 6;
  std::unique_ptr<Size>* free = nullptr;
  for (auto& size : native_) {
    if (size && size->ppem == ppem) return size.get();
    if (!size && !free) free = &size;
  }
  if (!free) {
    LOG_ERR("TTF", "%u px: all %u native sizes in use", px, kMaxNativeSizes);
    return nullptr;
  }
  checkLock();
  auto size = makeUniqueNoThrow<Size>();
  if (!size) {
    LOG_ERR("TTF", "%u px: no memory for a native size", px);
    return nullptr;
  }
  if (!configure(*size, ppem)) return nullptr;
  *free = std::move(size);
  return free->get();
}

bool TtfFont::nativeFonts(const uint8_t px, const EpdFont* (&fonts)[4]) {
  Size* size = nativeSize(px);
  if (!size) return false;
  for (uint8_t s = 0; s < 4; ++s) fonts[s] = styles_[s].present ? &size->styles[s].font : nullptr;
  return true;
}

void TtfFont::dropNativeSizes() {
  for (auto& size : native_) size.reset();
}

uint8_t TtfFont::nativeSizeCount() const {
  uint8_t count = 0;
  for (const auto& size : native_) count += size ? 1 : 0;
  return count;
}

bool TtfFont::claimSize(Size& size) {
  // A wipe voids every size index: claimed again once the store's generation moved. A claim the full table
  // refused is not asked again before then; that size's glyphs come from the ring.
  if (size.claimTried && size.generation == store_.generation()) return size.indexValid;
  size.claimTried = true;
  size.generation = store_.generation();
  size.indexValid = store_.sizeIndex(size.ppem, &size.index);
  return size.indexValid;
}

bool TtfFont::storeKey(Size& size, const uint8_t style, const uint32_t codepoint, uint32_t* key) {
  if (!store_.isOpen() || size.ppem == 0 || !claimSize(size)) return false;
  *key = glyphstore::makeKey(codepoint, style, size.index);
  return true;
}
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
const EpdGlyph* TtfFont::glyph(const uint8_t style, const uint32_t codepoint) {
  if (style >= 4 || !styles_[style].present || ppem_ == 0) return nullptr;
  if (utf8IsDefaultIgnorable(codepoint)) return &kIgnorable;
  uint32_t key = 0;
  const bool haveKey = storeKey(style, codepoint, &key);
```
with:

```cpp
const EpdGlyph* TtfFont::glyph(Size& size, const uint8_t style, const uint32_t codepoint) {
  if (style >= 4 || !styles_[style].present || size.ppem == 0) return nullptr;
  if (utf8IsDefaultIgnorable(codepoint)) return &kIgnorable;
  uint32_t key = 0;
  const bool haveKey = storeKey(size, style, codepoint, &key);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  return render(style, codepoint, haveKey, key);
}

const EpdGlyph* TtfFont::render(const uint8_t style, const uint32_t codepoint, const bool haveKey, const uint32_t key) {
```
with:

```cpp
  return render(size, style, codepoint, haveKey, key);
}

const EpdGlyph* TtfFont::render(const Size& size, const uint8_t style, const uint32_t codepoint, const bool haveKey,
                                const uint32_t key) {
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, index, ppem_, &metrics), metrics);
  if (loaded != ttf::Status::Ok) {
    failed(style, codepoint, loaded, haveKey, key);
    return nullptr;
  }
  ++renders_;
```
with:

```cpp
  const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, index, size.ppem, &metrics), metrics);
  if (loaded != ttf::Status::Ok) {
    failed(style, codepoint, loaded, haveKey, key);
    return nullptr;
  }
  ++renders_;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
void TtfFont::prefetch(const uint8_t style, const char* utf8Text) {
  if (!utf8Text || style >= 4 || !styles_[style].present || ringMode_) return;
  uint32_t key = 0;
  if (!storeKey(style, 0, &key)) return;
```
with:

```cpp
void TtfFont::prefetch(Size& size, const uint8_t style, const char* utf8Text) {
  if (!utf8Text || style >= 4 || !styles_[style].present || ringMode_) return;
  uint32_t key = 0;
  if (!storeKey(size, style, 0, &key)) return;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    key = glyphstore::makeKey(cp, style, sizeIndex_);
    if (store_.find(key, nullptr) != Lookup::NotStored) continue;
```
with:

```cpp
    key = glyphstore::makeKey(cp, style, size.index);
    if (store_.find(key, nullptr) != Lookup::NotStored) continue;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    key = glyphstore::makeKey(p.codepoint, style, sizeIndex_);
    EpdGlyph metrics{};
    const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, p.glyph, ppem_, &metrics), metrics);
```
with:

```cpp
    key = glyphstore::makeKey(p.codepoint, style, size.index);
    EpdGlyph metrics{};
    const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, p.glyph, size.ppem, &metrics), metrics);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  for (Style& style : styles_) {
    if (style.present && ppem_ != 0) style.data.bitmap = store_.logBase();
  }
  if (ppem_ != 0) claimSize();
  return true;
```
with:

```cpp
  // The log may map elsewhere now; every size claims its index again at its next lookup.
  const auto rebase = [this](Size& size) {
    if (size.ppem == 0) return;
    for (uint8_t s = 0; s < 4; ++s) {
      if (styles_[s].present) size.styles[s].data.bitmap = store_.logBase();
    }
  };
  rebase(body_);
  for (auto& size : native_) {
    if (size) rebase(*size);
  }
  return true;
```

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  if (!free) {
    LOG_ERR("TTF", "%u px: all %u native sizes in use", px, kMaxNativeSizes);
    return nullptr;
  }
  checkLock();
  auto size = makeUniqueNoThrow<Size>();
  if (!size) {
    LOG_ERR("TTF", "%u px: no memory for a native size", px);
    return nullptr;
  }
```
with:

```cpp
  if (!free) {
    LOG_ERR("TTF", "%u px: all %u native sizes in use; the nearest serves", px, kMaxNativeSizes);
    return nearestNative(ppem);
  }
  checkLock();
  auto size = makeUniqueNoThrow<Size>();
  if (!size) {
    LOG_ERR("TTF", "%u px: no memory for a native size; the nearest serves", px);
    return nearestNative(ppem);
  }
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
bool TtfFont::nativeFonts(const uint8_t px, const EpdFont* (&fonts)[4]) {
```
with:

```cpp
TtfFont::Size* TtfFont::nearestNative(const uint32_t ppem) {
  Size* nearest = nullptr;
  uint32_t distance = UINT32_MAX;
  for (auto& size : native_) {
    if (!size) continue;
    const uint32_t d = size->ppem > ppem ? size->ppem - ppem : ppem - size->ppem;
    if (d < distance) {
      distance = d;
      nearest = size.get();
    }
  }
  return nearest;
}

bool TtfFont::nativeFonts(const uint8_t px, const EpdFont* (&fonts)[4]) {
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 66 tests.` (58 before.)

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp test/ttf_font/TtfFontTest.cpp
git commit -m "feat(ttf): TtfFont draws native sizes, each with its own size index"
```

---

### Task 3: Native font IDs and the resolver

A native size's font ID is `base | px`. The window's base is a multiple of 256, taken from bits 8-30 of a hash of
the family's identity and name. That gives three properties:

- the top bit is clear, so a negative ID is never inside it;
- the same family gets the same window every session, so cached sections find their sizes again;
- a window that already holds a registered font ID moves on to the next salt, so a size never shadows a font.

The pure part (the window's arithmetic) is in `TtfFamily.h` with host tests. The manager, which only the firmware
builds, owns the rest:

- **at load:** picks the window and installs `resolveTtfSize` as the renderer's font resolver;
- **on a lookup of an unknown ID:** `resolveTtfSize` declines in one comparison when the ID is not in the window.
  Otherwise it returns the size's four fonts, or the body's if no size can serve: layout already chose the ID, so the
  lookup must not fail;
- **`dropTtfNativeSizes()`:** removes every native ID from the renderer (that also drops the scaled-glyph cache,
  keyed on font data), then frees the sizes. It runs at book open, with the wipe, and at unload, before the
  resolver goes.

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFamily.h`, `lib/EpdFont/SdCardFontManager.h`, `lib/EpdFont/SdCardFontManager.cpp`,
  `src/SdCardFontSystem.h`, `src/SdCardFontSystem.cpp`
- Test: `test/ttf_font/TtfFamilyTest.cpp`

**Interfaces:**
- Consumes: Task 2 (`TtfFont::nativeFonts`, `dropNativeSizes`, `bodyPpem`, `kMinNativePx`, `kMaxNativePx`).
- Produces:
  - `ttf::kNativeIdMask`, `bool ttf::isNativeId(int32_t base, int fontId)`, `uint8_t ttf::nativeIdPixels(int fontId)`,
    `int32_t ttf::nativeBaseFrom(uint32_t hash)`.
  - `struct SdCardFontManager::NativeSizes { int32_t base; uint32_t bodyPpem; uint8_t minPx, maxPx; }`;
    `std::optional<NativeSizes> SdCardFontManager::ttfNativeSizes(int bodyFontId) const`;
    `void SdCardFontManager::dropTtfNativeSizes(GfxRenderer&)`.
  - `std::optional<SdCardFontManager::NativeSizes> SdCardFontSystem::ttfNativeSizes(int bodyFontId) const` (Task 7's
    ladder reads it).

- [ ] **Step 1: Write the failing tests**

<!-- plan-append: test/ttf_font/TtfFamilyTest.cpp -->
```cpp
// --- Native size font IDs ---

TEST(TtfNativeIds, AWindowHoldsItsSizesAndNothingElse) {
  const int32_t base = ttf::nativeBaseFrom(0xDEADBEEFu);
  EXPECT_EQ(0x5EADBE00, base);  // bits 8-30: positive, a multiple of 256
  EXPECT_TRUE(ttf::isNativeId(base, base | 47));
  EXPECT_EQ(47, ttf::nativeIdPixels(base | 47));
  EXPECT_FALSE(ttf::isNativeId(base, (base + 0x100) | 47));
  EXPECT_FALSE(ttf::isNativeId(base, static_cast<int>(0xDEADBE2Fu)));  // the same bits with the sign set
  EXPECT_FALSE(ttf::isNativeId(0, 47));                                // no window
}

TEST(TtfNativeIds, AHashWithoutWindowBitsGivesNoWindow) { EXPECT_EQ(0, ttf::nativeBaseFrom(0x800000FFu)); }
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: compile errors, `'nativeBaseFrom' is not a member of 'ttf'` and `'isNativeId' is not a member of 'ttf'`.

- [ ] **Step 3: Implement the window**

<!-- plan-edit: lib/EpdFont/ttf/TtfFamily.h -->
```cpp
namespace ttf {
```
with:

```cpp
namespace ttf {

/// A loaded family's native sizes have font IDs `base | px`: a window of 256 IDs, its base a multiple of 256 with
/// the top bit clear, so no negative ID is ever in it. FontSizeLadder makes the same IDs (its setNative()).
constexpr uint32_t kNativeIdMask = 0xFFFFFF00u;
inline bool isNativeId(const int32_t base, const int fontId) {
  return base != 0 && (static_cast<uint32_t>(fontId) & kNativeIdMask) == static_cast<uint32_t>(base);
}
inline uint8_t nativeIdPixels(const int fontId) { return static_cast<uint8_t>(fontId & 0xFF); }
/// A window base from `hash`: its bits 8-30. 0 (no window) when they are all clear.
inline int32_t nativeBaseFrom(const uint32_t hash) { return static_cast<int32_t>(hash & 0x7FFFFF00u); }
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 68 tests.`

- [ ] **Step 5: The resolver in the manager**

<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
```
with:

```cpp
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class EpdFontFamily;
class GfxRenderer;
```
<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  // Runs the TTF glyph store's pending wipe (it filled up), calling onBusy first. Only at book open.
  void wipeTtfIfPending(const std::function<void()>& onBusy);
  bool ttfLoaded() const { return ttf_ != nullptr; }
```
with:

```cpp
  // Runs the TTF glyph store's pending wipe (it filled up), calling onBusy first. Only at book open.
  void wipeTtfIfPending(const std::function<void()>& onBusy);
  bool ttfLoaded() const { return ttf_ != nullptr; }

  // The loaded TTF family's native sizes (headings, CSS sizes, drop caps at their own pixel size). The renderer
  // asks for a size's font ID the first time something is laid out or drawn with it (the resolver installed at
  // load), and the face is made then. Their IDs come from a window FontSizeLadder::setNative() computes too.
  struct NativeSizes {
    int32_t base = 0;       // font ID base | px
    uint32_t bodyPpem = 0;  // the body face's pixels per em, 26.6
    uint8_t minPx = 0;
    uint8_t maxPx = 0;
  };
  // For a body font ID of the loaded TTF family; nothing for any other font, or when no window was free.
  std::optional<NativeSizes> ttfNativeSizes(int bodyFontId) const;
  // Forgets every native size: their font IDs leave the renderer and their faces are freed. Only at book open,
  // where nothing laid out with them is in use.
  void dropTtfNativeSizes(GfxRenderer& renderer);
```
<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  void registerTtfFace(GfxRenderer& renderer, int fontId);
```
with:

```cpp
  void registerTtfFace(GfxRenderer& renderer, int fontId);
  int32_t pickNativeBase(const GfxRenderer& renderer, uint32_t identityHash, const char* familyName) const;
  bool fontIdTaken(const GfxRenderer& renderer, int fontId) const;
  static std::optional<EpdFontFamily> resolveTtfSize(void* ctx, int fontId);
```
<!-- plan-edit: lib/EpdFont/SdCardFontManager.h -->
```cpp
  uint32_t ttfIdentityHash_ = 0;
  int ttfFontId_ = 0;
};
```
with:

```cpp
  uint32_t ttfIdentityHash_ = 0;
  int ttfFontId_ = 0;
  int32_t ttfNativeBase_ = 0;  // the native sizes' ID window; 0: none
};
```
<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
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
```
with:

```cpp
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
  ttfNativeBase_ = pickNativeBase(renderer, identityHash, family.name.c_str());
  if (ttfNativeBase_ != 0) renderer.setFontResolver(&SdCardFontManager::resolveTtfSize, this);
```
<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
bool SdCardFontManager::setTtfBodySize(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!ttf_) return false;
  if (pointSize == loadedPointSize_) return true;
  const int fontId = computeFontId(ttfIdentityHash_, loadedFamilyName_.c_str(), pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
```
with:

```cpp
int32_t SdCardFontManager::pickNativeBase(const GfxRenderer& renderer, const uint32_t identityHash,
                                          const char* familyName) const {
  // The window's high bits come from the family, so a section laid out with its sizes finds them again in the next
  // session; a window that holds a registered font ID moves on (another salt), so a size never shadows a font.
  for (uint8_t salt = 0; salt < 8; ++salt) {
    const int32_t base =
        ttf::nativeBaseFrom(static_cast<uint32_t>(computeFontId(identityHash, familyName, 0)) + salt * 0x100u);
    if (base == 0) continue;
    bool taken = false;
    for (const auto& entry : renderer.getFontMap()) {
      if (ttf::isNativeId(base, entry.first)) {
        taken = true;
        break;
      }
    }
    if (!taken) return base;
  }
  LOG_ERR("SDMGR", "%s: no free font ID window; sizes other than the body are scaled", familyName);
  return 0;
}

bool SdCardFontManager::fontIdTaken(const GfxRenderer& renderer, const int fontId) const {
  return renderer.getFontMap().count(fontId) != 0 || ttf::isNativeId(ttfNativeBase_, fontId);
}

std::optional<EpdFontFamily> SdCardFontManager::resolveTtfSize(void* ctx, const int fontId) {
  // Asked for every font ID the renderer does not know: declining is one comparison.
  auto* self = static_cast<SdCardFontManager*>(ctx);
  if (!self->ttf_ || !ttf::isNativeId(self->ttfNativeBase_, fontId)) return std::nullopt;
  const EpdFont* fonts[4] = {};
  if (!self->ttf_->nativeFonts(ttf::nativeIdPixels(fontId), fonts)) {
    // The text must still be laid out and drawn, the same way all session: with the body face.
    LOG_ERR("SDMGR", "%u px cannot be drawn natively; the body face serves", ttf::nativeIdPixels(fontId));
    for (uint8_t s = 0; s < 4; ++s) fonts[s] = self->ttf_->font(s);
  }
  return EpdFontFamily(fonts[0], fonts[1], fonts[2], fonts[3]);
}

std::optional<SdCardFontManager::NativeSizes> SdCardFontManager::ttfNativeSizes(const int bodyFontId) const {
  if (!ttf_ || ttfNativeBase_ == 0 || bodyFontId != ttfFontId_) return std::nullopt;
  NativeSizes sizes;
  sizes.base = ttfNativeBase_;
  sizes.bodyPpem = ttf_->bodyPpem();
  sizes.minPx = TtfFont::kMinNativePx;
  sizes.maxPx = TtfFont::kMaxNativePx;
  return sizes;
}

void SdCardFontManager::dropTtfNativeSizes(GfxRenderer& renderer) {
  if (!ttf_) return;
  // One at a time: removeFont() ends the walk over the map.
  for (bool found = true; found;) {
    found = false;
    for (const auto& entry : renderer.getFontMap()) {
      if (!ttf::isNativeId(ttfNativeBase_, entry.first)) continue;
      renderer.removeFont(entry.first);
      found = true;
      break;
    }
  }
  ttf_->dropNativeSizes();
}

bool SdCardFontManager::setTtfBodySize(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!ttf_) return false;
  if (pointSize == loadedPointSize_) return true;
  const int fontId = computeFontId(ttfIdentityHash_, loadedFamilyName_.c_str(), pointSize);
  if (fontIdTaken(renderer, fontId)) {
```
<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
  if (ttf_) {
    renderer.removeFont(ttfFontId_);
    LOG_INF("SDMGR", "TTF family %s unloaded
```
with:

```cpp
  if (ttf_) {
    dropTtfNativeSizes(renderer);
    renderer.setFontResolver(nullptr, nullptr);
    ttfNativeBase_ = 0;
    renderer.removeFont(ttfFontId_);
    LOG_INF("SDMGR", "TTF family %s unloaded
```

- [ ] **Step 6: Drop the last book's sizes at book open; expose the sizes to the reader**

<!-- plan-edit: src/SdCardFontSystem.cpp -->
```cpp
    if (family->ttf() && manager_.ttfLoaded()) {
      // Book open: no glyph record of the family is in use, so a full store can be emptied here.
      manager_.wipeTtfIfPending(onColdLoad);
      manager_.setTtfBodySize(renderer, targetPt);
      return;
    }
```
with:

```cpp
    if (family->ttf() && manager_.ttfLoaded()) {
      // Book open: no glyph record or face of the family is in use, so a full store can be emptied here, and the
      // last book's sizes go.
      manager_.dropTtfNativeSizes(renderer);
      manager_.wipeTtfIfPending(onColdLoad);
      manager_.setTtfBodySize(renderer, targetPt);
      return;
    }
```
<!-- plan-edit: src/SdCardFontSystem.h -->
```cpp
  void unload(GfxRenderer& renderer) { manager_.unloadAll(renderer); }
```
with:

```cpp
  void unload(GfxRenderer& renderer) { manager_.unloadAll(renderer); }
  // A TTF body font's native sizes, for the reader's size ladder (FontSizeLadder::setNative).
  std::optional<SdCardFontManager::NativeSizes> ttfNativeSizes(const int bodyFontId) const {
    return manager_.ttfNativeSizes(bodyFontId);
  }
```

- [ ] **Step 7: Build the firmware**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`
Expected: `[SUCCESS]`. Nothing uses the sizes yet (Task 7 gives the reader its ladder).

- [ ] **Step 8: Commit**

```bash
git add lib/EpdFont/ttf/TtfFamily.h lib/EpdFont/SdCardFontManager.h lib/EpdFont/SdCardFontManager.cpp src/SdCardFontSystem.h src/SdCardFontSystem.cpp test/ttf_font/TtfFamilyTest.cpp
git commit -m "feat(ttf): native size font IDs, made on first lookup through the font resolver"
```

---

### Task 4: The prewarm finds sizes through the renderer

`FontCacheManager::prewarmCache` held a reference to the font map and looked fonts up there. A native size is in
the map only after something looked it up. A page laid out in an earlier session and drawn first in this one reaches
the prewarm before that, so its heading's glyphs would render one at a time during the draw instead of in one batch.

The manager now asks the renderer's own lookup, which `GfxRenderer::setFontCacheManager` installs. That lookup knows
the resolver. Without one (a test that wires none), the map serves as before.

`FontCacheManager` had no host test. The new target links the real `SdCardFont` and decompressor sources,
because the manager's other paths use them; these tests never reach them.

**Files:**
- Create: `test/font_cache_manager/CMakeLists.txt`, `test/font_cache_manager/FontCacheManagerTest.cpp`
- Modify: `test/CMakeLists.txt`, `lib/GfxRenderer/FontCacheManager.h`, `lib/GfxRenderer/FontCacheManager.cpp`,
  `lib/GfxRenderer/GfxRenderer.h`, `lib/GfxRenderer/GfxRenderer.cpp`

**Interfaces:**
- Produces: `FontCacheManager::FontLookup` (`const EpdFontFamily* (*)(const void* ctx, int fontId)`),
  `void FontCacheManager::setFontLookup(FontLookup, const void* ctx)`; `GfxRenderer::setFontCacheManager` is
  defined in the .cpp and installs the lookup.

- [ ] **Step 1: Write the failing tests**

<!-- plan-file: test/font_cache_manager/CMakeLists.txt -->
```cmake
# FontCacheManager (lib/GfxRenderer): which font a page's text is prewarmed with. The real SdCardFont and
# FontDecompressor are linked because the manager's other paths use them; the tests here never reach them.
add_executable(FontCacheManagerTest
  FontCacheManagerTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)
target_include_directories(FontCacheManagerTest PRIVATE
  ${REPO_ROOT}/test/font_group_stream  # its Arduino.h adds micros() to the shared shim
  ${REPO_ROOT}/test/zip_entry_reader   # stdio-backed HalStorage.h; shadows test/shims stub
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/epub_pipeline  # last: only for its esp_heap_caps.h stub (HeapFit.h)
)
target_link_libraries(FontCacheManagerTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(FontCacheManagerTest)
```

<!-- plan-file: test/font_cache_manager/FontCacheManagerTest.cpp -->
```cpp
// FontCacheManager::prewarmCache finds a page's font through the renderer's lookup, so a face the font resolver
// makes on demand (a TTF family's native size) is prewarmed even before anything looked it up.
#include <gtest/gtest.h>

#include <map>
#include <string>

#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "FontCacheManager.h"
#include "GlyphProvider.h"

namespace {

struct CountingProvider final : GlyphProvider {
  int prefetches = 0;
  std::string text;
  const EpdGlyph* missGlyph(uint32_t) override { return nullptr; }
  void prefetch(const char* utf8Text) override {
    ++prefetches;
    text = utf8Text;
  }
};

struct Fixture : testing::Test {
  CountingProvider provider;
  EpdFontData data{};
  EpdFont font{&data};
  EpdFontFamily family{&font, nullptr, nullptr, nullptr};
  std::map<int, EpdFontFamily> fontMap;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager{fontMap, noSdFonts, noSdFonts};
  int asked = 0;

  void SetUp() override { data.provider = &provider; }
};

}  // namespace

TEST_F(Fixture, AFontOnlyTheLookupSuppliesIsPrewarmed) {
  manager.setFontLookup(
      [](const void* ctx, const int fontId) -> const EpdFontFamily* {
        auto* self = static_cast<Fixture*>(const_cast<void*>(ctx));
        ++self->asked;
        return fontId == 4711 ? &self->family : nullptr;
      },
      this);
  manager.prewarmCache(4711, "Chapter One", 0x01);
  EXPECT_EQ(1, asked);
  EXPECT_EQ(1, provider.prefetches);
  EXPECT_EQ("Chapter One", provider.text);
  manager.prewarmCache(4712, "nothing", 0x01);  // a font nobody knows: nothing to do
  EXPECT_EQ(1, provider.prefetches);
}

TEST_F(Fixture, WithoutALookupTheFontMapServes) {
  fontMap.emplace(7, family);
  manager.prewarmCache(7, "Body", 0x01);
  EXPECT_EQ(1, provider.prefetches);
  manager.prewarmCache(4711, "Chapter One", 0x01);
  EXPECT_EQ(1, provider.prefetches);
}
```

<!-- plan-edit: test/CMakeLists.txt -->
```cmake
add_subdirectory(font_group_stream)
```
with:

```cmake
add_subdirectory(font_group_stream)
add_subdirectory(font_cache_manager)  # after font_group_stream: links its uzlib_font_ring
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target FontCacheManagerTest -j 8 && ./build/test/font_cache_manager/FontCacheManagerTest.exe --gtest_brief=1`
Expected: a compile error, `'class FontCacheManager' has no member named 'setFontLookup'`.

- [ ] **Step 3: Implement**

<!-- plan-edit: lib/GfxRenderer/FontCacheManager.h -->
```cpp
  void setFontDecompressor(FontDecompressor* d);
```
with:

```cpp
  void setFontDecompressor(FontDecompressor* d);
  // How prewarmCache() turns a font ID into a family: the renderer's own lookup, which also makes the faces its
  // resolver supplies on demand (a TTF family's native sizes, perhaps not yet looked up when a page laid out in
  // an earlier session is prewarmed). Without one, the font map alone.
  using FontLookup = const EpdFontFamily* (*)(const void* ctx, int fontId);
  void setFontLookup(FontLookup lookup, const void* ctx) {
    lookup_ = lookup;
    lookupCtx_ = ctx;
  }
```
<!-- plan-edit: lib/GfxRenderer/FontCacheManager.h -->
```cpp
  const std::map<int, EpdFontFamily>& fontMap_;
```
with:

```cpp
  const std::map<int, EpdFontFamily>& fontMap_;
  FontLookup lookup_ = nullptr;
  const void* lookupCtx_ = nullptr;
  const EpdFontFamily* findFamily(int fontId) const;
```
<!-- plan-edit: lib/GfxRenderer/FontCacheManager.cpp -->
```cpp
void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
```
with:

```cpp
const EpdFontFamily* FontCacheManager::findFamily(const int fontId) const {
  if (lookup_) return lookup_(lookupCtx_, fontId);
  const auto it = fontMap_.find(fontId);
  return it != fontMap_.end() ? &it->second : nullptr;
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
```
<!-- plan-edit: lib/GfxRenderer/FontCacheManager.cpp -->
```cpp
  const auto family = fontMap_.find(fontId);
  if (family != fontMap_.end()) family->second.prefetch(utf8Text, styleMask);

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || family == fontMap_.end()) return;
```
with:

```cpp
  const EpdFontFamily* family = findFamily(fontId);
  if (family) family->prefetch(utf8Text, styleMask);

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || !family) return;
```
<!-- plan-edit: lib/GfxRenderer/FontCacheManager.cpp -->
```cpp
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
```
with:

```cpp
    const EpdFontData* data = family->getData(style);
```
<!-- plan-edit: lib/GfxRenderer/GfxRenderer.h -->
```cpp
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
```
with:

```cpp
  // The manager prewarms through this renderer's font lookup, so it reaches the faces the resolver makes.
  void setFontCacheManager(FontCacheManager* m);
```

<!-- plan-edit: lib/GfxRenderer/GfxRenderer.cpp -->
```cpp
bool GfxRenderer::releaseFontCaches() {
```
with:

```cpp
void GfxRenderer::setFontCacheManager(FontCacheManager* m) {
  fontCacheManager_ = m;
  if (!m) return;
  m->setFontLookup(
      [](const void* ctx, const int fontId) { return static_cast<const GfxRenderer*>(ctx)->findFontFamily(fontId); },
      this);
}

bool GfxRenderer::releaseFontCaches() {
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target FontCacheManagerTest -j 8 && ./build/test/font_cache_manager/FontCacheManagerTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 2 tests.` (`GfxRenderer.cpp` is not in any host test: Task 7's firmware build compiles it.)

- [ ] **Step 5: Commit**

```bash
git add test/CMakeLists.txt test/font_cache_manager lib/GfxRenderer/FontCacheManager.h lib/GfxRenderer/FontCacheManager.cpp lib/GfxRenderer/GfxRenderer.h lib/GfxRenderer/GfxRenderer.cpp
git commit -m "feat(fonts): prewarm looks fonts up through the renderer, so resolved sizes are found"
```

---

### Task 5: The ladder's native mode

`FontSizeLadder` is the pure data layout uses to turn a block's size (a percentage of the body) into a font ID and
a leftover scale. `setNative()` gives it the family's ID window, the body's pixels per em and the native range. In
native mode, `resolve(pct)` does this:

- within the 3% dead zone of the body size, returns the body face;
- otherwise returns `base | round(bodyPx * pct / 100)`, clamped to the range, with the leftover scale (snapped to
  1.0 inside the dead zone, as before).

**Files:**
- Modify: `lib/Epub/Epub/FontSizeLadder.h`
- Test: `test/word_size_layout/WordSizeLayoutTest.cpp`

**Interfaces:**
- Produces: `void FontSizeLadder::setNative(int32_t base, uint32_t bodyPpem26_6, uint8_t minPx, uint8_t maxPx)`
  (ignored unless all four are usable); `bool FontSizeLadder::native() const`; the public fields `nativeBase`,
  `nativeBodyPpem`, `nativeMinPx`, `nativeMaxPx`.

- [ ] **Step 1: Write the failing tests**

<!-- plan-edit: test/word_size_layout/WordSizeLayoutTest.cpp -->
```cpp
// ---------------------------------------------------------------------------
// ParsedText::foldUniformWordSizes
```
with:

```cpp
// A TTF family: every size its own face, at whole pixels per em.
namespace {
constexpr int32_t kNativeBase = 0x12345600;
constexpr uint32_t kBody14pt = (14 * 64 * 150 + 36) / 72;  // 29.17 px per em, 26.6

FontSizeLadder nativeLadder() {
  FontSizeLadder ladder;
  ladder.setNative(kNativeBase, kBody14pt, 10, 120);
  return ladder;
}
}  // namespace

TEST(FontSizeLadderTest, ANativeLadderGivesEachSizeItsOwnWholePixelFace) {
  const auto ladder = nativeLadder();
  // h1 160%: 46.7 px, drawn at 47 px; the 0.7% left over is inside the dead zone.
  const auto h1 = ladder.resolve(160.0f);
  EXPECT_EQ(kNativeBase | 47, h1.fontId);
  EXPECT_FLOAT_EQ(1.0f, h1.residual);
  const auto h2 = ladder.resolve(140.0f);
  EXPECT_EQ(kNativeBase | 41, h2.fontId);
  EXPECT_FLOAT_EQ(1.0f, h2.residual);
  // A footnote at 80%: 23.3 px at 23 px.
  const auto small = ladder.resolve(80.0f);
  EXPECT_EQ(kNativeBase | 23, small.fontId);
  EXPECT_FLOAT_EQ(1.0f, small.residual);
}

TEST(FontSizeLadderTest, ANativeLadderKeepsTheBodyFaceNearTheBodySize) {
  const auto ladder = nativeLadder();
  for (const float pct : {97.5f, 100.0f, 102.5f}) {
    const auto r = ladder.resolve(pct);
    EXPECT_EQ(0, r.fontId) << pct;
    EXPECT_FLOAT_EQ(1.0f, r.residual) << pct;
  }
  // 96%: 28.0 px is a face of its own, not the body's 29.17.
  EXPECT_EQ(kNativeBase | 28, ladder.resolve(96.0f).fontId);
}

TEST(FontSizeLadderTest, ANativeLadderScalesTheLargestFaceBeyondIt) {
  const auto ladder = nativeLadder();
  // A 5x drop cap: 145.8 px, the 120 px face scaled by the rest.
  const auto cap = ladder.resolve(500.0f);
  EXPECT_EQ(kNativeBase | 120, cap.fontId);
  EXPECT_NEAR(145.83f / 120.0f, cap.residual, 0.001f);
  // Tiny text: the smallest face, scaled down.
  const auto tiny = ladder.resolve(30.0f);
  EXPECT_EQ(kNativeBase | 10, tiny.fontId);
  EXPECT_NEAR(8.75f / 10.0f, tiny.residual, 0.001f);
}

TEST(FontSizeLadderTest, AnIncompleteNativeLadderIsIgnored) {
  FontSizeLadder ladder;
  ladder.setNative(0, kBody14pt, 10, 120);
  ladder.setNative(kNativeBase, 0, 10, 120);
  ladder.setNative(kNativeBase, kBody14pt, 20, 10);
  EXPECT_FALSE(ladder.native());
  EXPECT_FLOAT_EQ(1.6f, ladder.resolve(160.0f).residual);  // the scale fallback
}

// ---------------------------------------------------------------------------
// ParsedText::foldUniformWordSizes
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target WordSizeLayoutTest -j 8 && ./build/test/word_size_layout/WordSizeLayoutTest.exe --gtest_brief=1`
Expected: compile errors, `'struct FontSizeLadder' has no member named 'setNative'`.

- [ ] **Step 3: Implement**

<!-- plan-edit: lib/Epub/Epub/FontSizeLadder.h -->
```cpp
  Rung rungs[kMaxRungs] = {};
  int8_t count = 0;
```
with:

```cpp
  Rung rungs[kMaxRungs] = {};
  int8_t count = 0;

  // A family drawn natively at any size (a TTF family) has no rungs: every size is a face of its own at a whole
  // number of pixels per em, font ID nativeBase | px (SdCardFontManager hands out that window of 256 IDs and makes
  // a face on the first lookup of one). Whole pixels keep the leftover scale inside the dead zone from about 17 px
  // up, so nothing is resampled; above nativeMaxPx the largest face is drawn scaled. Also derived from the body
  // fontId alone (the family's identity is in both), so not hashed either.
  int32_t nativeBase = 0;       // 0: no native sizes
  uint32_t nativeBodyPpem = 0;  // the body face's pixels per em, 26.6
  uint8_t nativeMinPx = 0;
  uint8_t nativeMaxPx = 0;

  void setNative(const int32_t base, const uint32_t bodyPpem26_6, const uint8_t minPx, const uint8_t maxPx) {
    if (base == 0 || bodyPpem26_6 == 0 || minPx == 0 || maxPx < minPx) return;
    nativeBase = base;
    nativeBodyPpem = bodyPpem26_6;
    nativeMinPx = minPx;
    nativeMaxPx = maxPx;
  }
  bool native() const { return nativeBase != 0; }
```
<!-- plan-edit: lib/Epub/Epub/FontSizeLadder.h -->
```cpp
  Resolved resolve(const float desiredPct) const {
    Resolved r = resolveExact(desiredPct);
```
with:

```cpp
  Resolved resolve(const float desiredPct) const {
    Resolved r = native() ? resolveNative(desiredPct) : resolveExact(desiredPct);
```
<!-- plan-edit: lib/Epub/Epub/FontSizeLadder.h -->
```cpp
 private:
  Resolved resolveExact(const float desiredPct) const {
```
with:

```cpp
 private:
  Resolved resolveNative(const float desiredPct) const {
    Resolved r;
    r.residual = desiredPct / 100.0f;
    // Within the dead zone of the body size, the body face itself.
    if (desiredPct <= 0.0f || (r.residual > 1.0f - kResidualDeadZone && r.residual < 1.0f + kResidualDeadZone)) {
      return r;
    }
    const float target = static_cast<float>(nativeBodyPpem) / 64.0f * r.residual;
    int px = static_cast<int>(target + 0.5f);
    px = px < nativeMinPx ? nativeMinPx : px > nativeMaxPx ? nativeMaxPx : px;
    r.fontId = nativeBase | px;
    r.residual = target / static_cast<float>(px);
    return r;
  }

  Resolved resolveExact(const float desiredPct) const {
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target WordSizeLayoutTest -j 8 && ./build/test/word_size_layout/WordSizeLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 34 tests.` (30 before.)

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/FontSizeLadder.h test/word_size_layout/WordSizeLayoutTest.cpp
git commit -m "feat(layout): FontSizeLadder's native mode: every size its own whole-pixel face"
```

---

### Task 6: Layout gives every size its own face

Two places in `ChapterHtmlSlimParser` assume a built-in family's ladder:

- **`resolveBlockFont`** lets one size per section have a face of its own (the decompressor's fourth page slot);
  any other size is the body face scaled. A native ladder has no such budget.
- **`finalizePendingDropCap`** picks the largest rung for a drop cap and scales it the rest of the way. With native
  sizes the cap resolves its own size, scaled only above the largest face.

A section laid out before this change chose its fonts the old way, so `SECTION_FILE_VERSION` goes to 82.

**Files:**
- Create: `test/epub_pipeline/NativeSizeLayoutTest.cpp`
- Modify: `test/epub_pipeline/CMakeLists.txt`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`,
  `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`, `lib/Epub/Epub/Section.cpp`

**Interfaces:**
- Consumes: Task 5 (`FontSizeLadder::setNative`, `native()`).

- [ ] **Step 1: Write the failing tests**

The tests lay a synthetic chapter out with a native ladder (the fake renderer measures every font alike, so only
the chosen IDs and scales matter) and read the first page's lines back. One test pins the built-in ladder's budget,
which must not change.

<!-- plan-file: test/epub_pipeline/NativeSizeLayoutTest.cpp -->
```cpp
// A TTF family's sizes: every heading, CSS size and drop cap is laid out with a face of its own, at a whole number
// of pixels per em (FontSizeLadder::setNative), instead of the body face scaled.
//
// A built-in family has one auxiliary face per section (the decompressor's four page slots), so a second
// heading size is the body face scaled. A native face has no such budget: it is read from the glyph store.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/FontSizeLadder.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int32_t kNativeBase = 0x12345600;
constexpr uint32_t kBody14pt = (14 * 64 * 150 + 36) / 72;  // 29.17 px per em, 26.6

struct Line {
  std::string text;
  int32_t fontId;  // 0: the body face
  float mult;
};

struct NativeSizeLayoutFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_native_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The first page's text lines, laid out with `ladder`.
  std::vector<Line> layout(const std::string& body, const FontSizeLadder& ladder) {
    const auto epub = syntheticBook(work, 0, body);
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    params.fontSizeLadder = ladder;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<Line> lines;
    section.currentPage = 0;
    const auto page = section.loadPageFromSectionFile();
    if (!page) return lines;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
      if (block.wordCount() == 0) continue;
      Line line{{}, block.getRenderStyle().headingFontId, block.getRenderStyle().fontSizeMultiplier};
      for (uint16_t w = 0; w < block.wordCount(); ++w) {
        if (w > 0) line.text += ' ';
        line.text += block.wordText(w);
      }
      lines.push_back(line);
    }
    return lines;
  }

  static FontSizeLadder nativeLadder() {
    FontSizeLadder ladder;
    ladder.setNative(kNativeBase, kBody14pt, 10, 120);
    return ladder;
  }

  static const Line* find(const std::vector<Line>& lines, const std::string& text) {
    for (const auto& line : lines) {
      if (line.text == text) return &line;
    }
    return nullptr;
  }
};

}  // namespace

TEST_F(NativeSizeLayoutFixture, EachHeadingSizeHasAFaceOfItsOwn) {
  const auto lines = layout("<h1>Alpha</h1><h2>Bravo</h2><h3>Charlie</h3><p>Body text.</p>", nativeLadder());
  const Line* h1 = find(lines, "Alpha");
  const Line* h2 = find(lines, "Bravo");
  const Line* h3 = find(lines, "Charlie");
  const Line* p = find(lines, "Body text.");
  ASSERT_TRUE(h1 && h2 && h3 && p);
  // 1.6, 1.4 and 1.2 times 29.17 px: 47, 41 and 35 px, nothing left to scale.
  EXPECT_EQ(kNativeBase | 47, h1->fontId);
  EXPECT_EQ(kNativeBase | 41, h2->fontId);
  EXPECT_EQ(kNativeBase | 35, h3->fontId);
  for (const Line* heading : {h1, h2, h3}) EXPECT_FLOAT_EQ(1.0f, heading->mult) << heading->text;
  EXPECT_EQ(0, p->fontId);
  EXPECT_FLOAT_EQ(1.0f, p->mult);
}

TEST_F(NativeSizeLayoutFixture, ABuiltInLadderStillHasOneAuxiliaryFace) {
  FontSizeLadder rungs;  // Bookerly at 14 pt: 16 and 20 pt rungs
  rungs.addRung(104, 114);
  rungs.addRung(106, 142);
  const auto lines = layout("<h1>Alpha</h1><h3>Charlie</h3><p>Body text.</p>", rungs);
  const Line* h1 = find(lines, "Alpha");
  const Line* h3 = find(lines, "Charlie");
  ASSERT_TRUE(h1 && h3);
  EXPECT_EQ(106, h1->fontId);  // 160% on the 142% rung claims the section's one auxiliary face
  EXPECT_EQ(0, h3->fontId);    // 120% wants the 114% rung: taken, so the body face, scaled
  EXPECT_FLOAT_EQ(1.2f, h3->mult);
}

TEST_F(NativeSizeLayoutFixture, ACssSizeHasAFaceOfItsOwn) {
  const auto lines = layout("<p style=\"font-size: 0.8em\">Small print.</p><p>Body text.</p>", nativeLadder());
  const Line* small = find(lines, "Small print.");
  ASSERT_NE(nullptr, small);
  EXPECT_EQ(kNativeBase | 23, small->fontId);  // 23.3 px
  EXPECT_FLOAT_EQ(1.0f, small->mult);
}

TEST_F(NativeSizeLayoutFixture, ADropCapIsSetAtItsOwnSize) {
  const auto lines = layout(
      "<p><span style=\"float: left; font-size: 3em\">A</span>ll of this opening paragraph wraps beside a drop "
      "cap, line after line, until the cap is passed.</p>",
      nativeLadder());
  const Line* cap = find(lines, "A");
  ASSERT_NE(nullptr, cap);
  EXPECT_EQ(kNativeBase | 88, cap->fontId);  // 3 x 29.17 = 87.5 px
  EXPECT_FLOAT_EQ(1.0f, cap->mult);
}

TEST_F(NativeSizeLayoutFixture, ADropCapAboveTheLargestFaceIsItScaled) {
  FontSizeLadder ladder;
  ladder.setNative(kNativeBase, kBody14pt, 10, 60);
  const auto lines = layout(
      "<p><span style=\"float: left; font-size: 3em\">A</span>ll of this opening paragraph wraps beside a drop "
      "cap, line after line, until the cap is passed.</p>",
      ladder);
  const Line* cap = find(lines, "A");
  ASSERT_NE(nullptr, cap);
  EXPECT_EQ(kNativeBase | 60, cap->fontId);
  EXPECT_NEAR(87.52f / 60.0f, cap->mult, 0.002f);
}
```

<!-- plan-edit: test/epub_pipeline/CMakeLists.txt -->
```cmake
  HeadingBlockLayoutTest.cpp
```
with:

```cmake
  HeadingBlockLayoutTest.cpp
  NativeSizeLayoutTest.cpp
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter=NativeSize* --gtest_brief=1`
Expected: three failures:
- `EachHeadingSizeHasAFaceOfItsOwn`: h2 and h3 keep the body face, scaled;
- `ADropCapIsSetAtItsOwnSize` and `ADropCapAboveTheLargestFaceIsItScaled`: the cap is the body face scaled 3x.

`ACssSizeHasAFaceOfItsOwn` and `ABuiltInLadderStillHasOneAuxiliaryFace` pass already: the first block off the body
size claimed the budget before.

- [ ] **Step 3: Implement**

<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h -->
```cpp
  // One non-body font per section: body regular/bold/italic plus one auxiliary regular is
  // exactly the FontDecompressor's four page slots. The first block to resolve off-body
  // claims the slot; blocks that would need a different font keep the scale fallback.
  int32_t auxFontId_ = 0;
```
with:

```cpp
  // One non-body font per section: body regular/bold/italic plus one auxiliary regular is
  // exactly the FontDecompressor's four page slots. The first block to resolve off-body
  // claims the slot; blocks that would need a different font keep the scale fallback.
  // A native ladder (a TTF family) has no such budget: its faces are read from the glyph store.
  int32_t auxFontId_ = 0;
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
  int32_t capFontId = 0;  // 0 = body font
  float capScale = pendingDropCap_.multiplier;
  {
```
with:

```cpp
  int32_t capFontId = 0;  // 0 = body font
  float capScale = pendingDropCap_.multiplier;
  if (fontSizeLadder_.native()) {
    // Every size has a face of its own: the cap is drawn at its own size, scaled only above the largest face.
    const FontSizeLadder::Resolved r = fontSizeLadder_.resolve(pendingDropCap_.multiplier * 100.0f);
    capFontId = r.fontId;
    capScale = r.residual;
  } else {
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
  if (auxFontId_ == 0) auxFontId_ = r.fontId;
  if (r.fontId != auxFontId_) return;  // aux budget already claimed by another size — scale fallback
  bs.headingFontId = r.fontId;
```
with:

```cpp
  if (!fontSizeLadder_.native()) {
    if (auxFontId_ == 0) auxFontId_ = r.fontId;
    if (r.fontId != auxFontId_) return;  // aux budget already claimed by another size — scale fallback
  }
  bs.headingFontId = r.fontId;
```

<!-- plan-edit: lib/Epub/Epub/Section.cpp -->
```cpp
constexpr uint8_t SECTION_FILE_VERSION = 81;  // v81: a paragraph over 96 words gets its top margin and padding
```
with:

```cpp
constexpr uint8_t SECTION_FILE_VERSION = 82;  // v82: a TTF family draws every size with a face of its own
                                              // v81: a paragraph over 96 words gets its top margin and padding
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter=NativeSize* --gtest_brief=1`
Expected: `[  PASSED  ] 5 tests.`

- [ ] **Step 5: Commit**

```bash
git add test/epub_pipeline/NativeSizeLayoutTest.cpp test/epub_pipeline/CMakeLists.txt lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp lib/Epub/Epub/Section.cpp
git commit -m "feat(layout): with native sizes every heading, CSS size and drop cap gets its own face"
```

---

### Task 7: The reader's ladder for a TTF family

`buildReaderFontSizeLadder` builds the rungs of a built-in family and leaves the ladder empty for any other: an SD
family was the body face scaled. For the loaded TTF family's body font it now sets the native mode from
`SdCardFontSystem::ttfNativeSizes`. This is the step that switches the feature on.

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

**Interfaces:**
- Consumes: Task 3 (`sdFontSystem.ttfNativeSizes`), Task 5 (`FontSizeLadder::setNative`).

- [ ] **Step 1: Implement**

<!-- plan-edit: src/activities/reader/EpubReaderActivity.cpp -->
```cpp
  FontSizeLadder ladder;
  for (const uint8_t family : kFamilies) {
```
with:

```cpp
  FontSizeLadder ladder;
  // A TTF family draws every size with a face of its own.
  if (const auto native = sdFontSystem.ttfNativeSizes(bodyFontId)) {
    ladder.setNative(native->base, native->bodyPpem, native->minPx, native->maxPx);
    return ladder;
  }
  for (const uint8_t family : kFamilies) {
```

- [ ] **Step 2: Build the firmware, both boards**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`, then the same with `-e x4pro`.
Expected: `[SUCCESS]` for each. `default`: flash about 96.3% (2c-1 measured 96.2%), RAM unchanged.

- [ ] **Step 3: Run the whole host suite**

Run: `cmake --build build/test -j 8 -- -k; (cd build/test && ctest -j 8 2>&1 | tail -3)`
Expected: `100% tests passed`, 2,246 tests (2,224 before). The `-k` build reports one error,
`epub_build_inventory` (`dlfcn.h`), which never builds on Windows.

- [ ] **Step 4: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror $(git diff --name-only origin/master -- '*.h' '*.cpp')
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat(ttf): the reader draws a TTF family's headings and drop caps at their own size"
```

---

### Task 8: On the X3, and the record

The device run covers what no host test reaches: the resolver inside real layout and drawing, sections cached in an
earlier session, the heap, and the Review Focus situations. It needs the user for the on-device steps: ask for each
one, and capture the serial log meanwhile. BookerlyTTF is already on the X3's card (`/.crosspoint/fonts/BookerlyTTF`,
from the 2c-1 run).

**Files:**
- Modify (docs branch `docs/ttf-fonts-design`, worktree `C:/tmp/wt-ttf-docs`):
  `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, this plan

- [ ] **Step 1: A book with many sizes**

Save as `/c/tmp/make_sizes_epub.py` (scratch; not committed) and run it. It writes `sizes-test.epub`: a chapter with h1-h3,
a drop cap, and paragraphs from 50% to 300% of the body size. At a 14 pt body those are 20 distinct native sizes,
more than the 16 a `TtfFont` holds.

```python
import zipfile

sizes = [50, 60, 70, 80, 90, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 220, 240, 260, 280, 300]
body = ["<h1>Sizes</h1>", "<h2>Second level</h2>", "<h3>Third level</h3>",
        '<p><span style="float: left; font-size: 3em">O</span>nce upon a time this paragraph opened with a drop cap, '
        "and it goes on long enough to wrap beside it for a few lines.</p>"]
body += [f'<p style="font-size: {s}%">Size {s} percent: Quick brown fox.</p>' for s in sizes]
chapter = ('<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>S</title>'
           "</head><body>\n" + "\n".join(body) + "\n</body></html>\n")
opf = ('<?xml version="1.0" encoding="utf-8"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
       'unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">sizes-test'
       "</dc:identifier><dc:title>Sizes test</dc:title><dc:language>en</dc:language></metadata><manifest>"
       '<item id="c" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>'
       '<spine><itemref idref="c"/></spine></package>\n')
container = ('<?xml version="1.0"?>\n<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles>'
             "</container>\n")
with zipfile.ZipFile("sizes-test.epub", "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
    z.writestr("META-INF/container.xml", container, zipfile.ZIP_DEFLATED)
    z.writestr("content.opf", opf, zipfile.ZIP_DEFLATED)
    z.writestr("chapter.xhtml", chapter, zipfile.ZIP_DEFLATED)
print("wrote sizes-test.epub")
```

Run: `cd /c/tmp && /c/pio/penv/Scripts/python.exe make_sizes_epub.py`
Expected: `wrote sizes-test.epub`.

- [ ] **Step 2: Flash, copy the book, start the log**

Flash: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`.
Ask the user to open File Transfer → USB Transfer, then:
`MSYS_NO_PATHCONV=1 /c/pio/penv/Scripts/python.exe tools/serial_transfer.py --port COM6 put C:/tmp/sizes-test.epub /books/sizes-test.epub`
Ask the user to leave USB Transfer (the device reboots). Capture in the background, without a reset:
`/c/pio/penv/Scripts/python.exe /c/tmp/capture_reset.py COM6 /c/tmp/x3-2c2.log 1800 --no-reset`

- [ ] **Step 3: Headings and a drop cap**

Ask the user to make sure the font family is BookerlyTTF at 14 pt and the reader's embedded-style setting is on.
Then open the book from the 2c-1 run whose chapter opens with a large first letter, at that chapter.
Expected:
- the large letter and the headings are as sharp as the body text, no longer visibly scaled up;
- `family loaded: N B scratch, M B per native size` (note both; the scratch is about 2.6 KB more than 2c-1's);
- no `the nearest serves` and no `glyph rendered without RenderLock`.

Note the chapter's first page time.

- [ ] **Step 4: Read on**

Ask the user to turn 10-20 pages across a chapter start, then change the size from the reader menu (14 → 18 pt) and
back.
Expected: headings stay sharp at both sizes; the size change lays the chapter out again without a popup.

- [ ] **Step 5: A chapter cached in an earlier session (Review Focus 4)**

Ask the user to leave the book, reboot, and reopen it at the chapter start of Step 3.
Expected: the heading and the cap draw sharp at once; the reader's unload line after this visit shows few or no
glyphs rendered (they came from the store).

- [ ] **Step 6: More sizes than 16 (Review Focus 1)**

Ask the user to open "Sizes test" and page through it, then back.
Expected:
- 20 paragraphs, each larger than the one before, the drop cap, and the headings;
- four `all 16 native sizes in use; the nearest serves` lines: the 220-280% paragraphs come last and are drawn at
  the nearest size held (200% or the cap's 300%), visibly off;
- no crash, and pages identical going back.

- [ ] **Step 7: Another book, and leaving the reader (Review Focus 3)**

Ask the user to open another book, then leave the reader for the home screen.
Expected: no crash; the free heap at the home screen is back where it was before Step 6 (the sizes were dropped at
book open).

- [ ] **Step 8: The size table (Review Focus 5)**

From the log, count the sizes claimed so far. The table's 32 entries may have filled after Step 6 (20 sizes, plus
the body and the earlier book's).
Expected, if it filled: `full: all 32 sizes in use; wipe pending`, the size drawn anyway (from the ring), and
`glyph store wiped` at the next book open. Record which happened.

- [ ] **Step 9: Record**

In the spec (docs branch):
- 5.5 gets an "As built in 2c-2" block with the decisions above;
- 4.6, a note on the full size table;
- section 8's table: the 2c-2 status, and previews moved to 2c-4;
- 8.1: the resolver's concurrency and allocation, `prewarmCache` and dormant sizes, and the 2c-1 drop-cap note, all
  marked done;
- section 11: the deferred items (inline sizes; previews in 2c-4);
- Appendix A: the X3 numbers (bytes per size, scratch, first page with a heading, heap).

Add a status block to the top of this plan. Commit on `docs/ttf-fonts-design`:

```bash
git -C /c/tmp/wt-ttf-docs add docs/superpowers
git -C /c/tmp/wt-ttf-docs commit -m "docs(ttf): Stage 2c-2 as built: native sizes, X3 results"
```
