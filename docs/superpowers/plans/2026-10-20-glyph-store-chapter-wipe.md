# Glyph store emptied at the next chapter — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A TrueType glyph store that fills up while a chapter is read is emptied when the next chapter opens, not only at the next book open, so a CJK reader stops paying a full render for every new glyph on every page.

**Architecture:** `TtfFontSet::wipeIfPending` refuses to run unless RenderLock is held (the app's `renderLockHeld` hook), so it can only run where no glyph record of the log is in use: every lookup, draw and pre-render takes RenderLock. The EPUB reader calls it at the start of `EpubReaderActivity::buildSection()`, which runs in the render pass under RenderLock before the chapter is laid out or drawn. Book open keeps calling it as before.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), host tests with GoogleTest/CMake (`test/`), Python for a device test book.

**Spec:** `docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md`, section 8 (the glyph store), with sections 9 (validation) and 10 (documentation). The plan was cut from a prototype (`proto/chapter-wipe`, worktree `C:/tmp/wt-wipe-proto`): every code block below is taken from it, and it passes every test this plan names.

## Global Constraints

- Base: master at 67bb1e428 or later. The reader anchor in Task 2 was checked against it.
- No format change: `glyphstore::kFormatVersion`, `ttf::kRenderVersion` and `SECTION_FILE_VERSION` stay as they are.
- The wipe erases only the sectors holding data (`FlashGlyphStore::wipe`, unchanged); the busy popup is the one book open shows (`STR_LOADING_FONT`). No new strings.
- Comments say what the code does and why; no history narration.
- clang-format 21 is what CI runs: `bin/clang-format-fix` with clang-format 21.1.8 on the PATH (version 22 differs). Run it before pushing.
- Host tests build and run from Git Bash: configure with `-DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1`, build with `cmake --build build/test -j 8 -- -k 0`.
- Firmware: `default`, `x4pro` and `lilygo_t5s3` must build, one `pio run` at a time, with `PLATFORMIO_CORE_DIR=C:/pio`.
- Devices: never pulse RTS or run esptool to reset an X3. Read each setting with `CMD:GET` before changing it and restore it afterwards (the X4 runs in German).

## Review Focus

1. **A chapter turn onto the spine Background-B built, or is building.** The wipe runs before `buildSection()` adopts B's section. B's partial build keeps words and measured widths, never a glyph record, and the glyphs drawn again after the wipe have the same metrics, so the adopted layout stays right. The reviewer reads `buildSection()` against this; Task 3 turns into a chapter B built.
2. **A load without RenderLock** (the boot-time `ensureLoaded`, a load from the settings screens): the wipe waits for the next chapter or book open instead of running under a draw. Pinned in Task 1: `TtfFontTest.AWipeWaitsForRenderLock`.
3. **A wipe that fails** (a flash error): the store stays pending and the next chapter tries again (`FlashGlyphStore::wipe` returns false, unchanged). The reviewer checks `wipeIfPending` leaves `ringMode_` set on that path.
4. **The TXT and Markdown readers** have no chapters: there the wipe still waits for the next book open. Nothing to change; the reviewer confirms they never call `buildSection()`.
5. **A store that fills again within one chapter** (a large size on a dense book): the ring serves that chapter's new glyphs, and the next chapter wipes once more. Task 3 runs at 26 pt, where the store fills within two chapters.

---

### Task 1: The wipe waits for RenderLock

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFont.h` (`TtfFontSet::wipeIfPending` comment)
- Modify: `lib/EpdFont/ttf/TtfFont.cpp` (`TtfFontSet::wipeIfPending`, `TtfFontSet::noteCommit`)
- Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Produces: `bool TtfFontSet::wipeIfPending(const std::function<void()>& onBusy = {})`, unchanged in signature: false, without calling `onBusy`, when the app's `TtfFont::Hooks::renderLockHeld` hook is set and returns false.

- [ ] **Step 1: Write the failing test.** In `test/ttf_font/TtfFontTest.cpp`, after `TEST_F(TtfFontTest, AWipeAtTheNextBookOpenEndsTheRing)`:

```cpp
TEST_F(TtfFontTest, AWipeWaitsForRenderLock) {
  // The reader asks at every chapter open (spec 8.2): only under RenderLock is no glyph record of the log in use.
  const auto font = open();
  ASSERT_NE(nullptr, font);
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'B'));
  ASSERT_TRUE(font->ringMode());
  static bool held = false;
  TtfFont::setHooks({[] { return held; }});
  busy = 0;
  EXPECT_FALSE(font.set->wipeIfPending([this] { ++busy; }));
  EXPECT_EQ(0, busy);
  EXPECT_TRUE(font->ringMode());  // still pending
  held = true;
  EXPECT_TRUE(font.set->wipeIfPending([this] { ++busy; }));
  EXPECT_EQ(1, busy);
  EXPECT_FALSE(font->ringMode());
}
```

`TearDown()` already resets the hooks (`TtfFont::setHooks({})`).

- [ ] **Step 2: Run it to see it fail.**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_filter='*AWipeWaitsForRenderLock*'`
Expected: FAIL at `EXPECT_FALSE(font.set->wipeIfPending(...))`: the wipe runs without the lock, `busy` is 1.

- [ ] **Step 3: Make the wipe wait for RenderLock.** In `TtfFontSet::wipeIfPending` (`TtfFont.cpp`), after the `isOpen()`/`wipePending()` check and before `onBusy`:

```cpp
  if (hooks.renderLockHeld && !hooks.renderLockHeld()) {
    // A lookup or a draw (a pre-render among them) may hold a record of the log: they all run under RenderLock.
    LOG_INF("TTF", "glyph store wipe deferred: RenderLock is not held");
    return false;
  }
```

In the same function the comment on `if (!store_.wipe()) return false;` becomes `// stays pending: the next chapter or book open tries again`. In `TtfFontSet::noteCommit` the log line becomes `"glyph store %s: new glyphs are not stored until the next chapter or book opens"`. In `TtfFont.h` the doc comment of `wipeIfPending` becomes:

```cpp
  /// Runs a pending wipe (the store filled up, or a write failed), calling `onBusy` first. Only where no glyph
  /// record of the set is in use: at book or chapter open, under RenderLock, which every lookup and draw holds; without
  /// it (the app's renderLockHeld hook) the wipe waits for the next call. True when a wipe ran.
```

- [ ] **Step 4: Run the whole TtfFontTest.**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
Expected: PASS, every test (142 on the prototype's base; the count grows by one).

- [ ] **Step 5: Commit.**

```bash
git add lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp test/ttf_font/TtfFontTest.cpp
git commit -m "fix(ttf): the glyph store is wiped only under RenderLock"
```

### Task 2: The reader empties a full store when a chapter opens

**Files:**
- Modify: `src/SdCardFontSystem.h` (a passthrough)
- Modify: `lib/EpdFont/SdCardFontManager.h` (comment of `wipeTtfIfPending`)
- Modify: `src/activities/reader/EpubReaderActivity.cpp` (`buildSection()`)
- Create: `tools/make_cjk_store_fill_epub.py`
- Modify: `docs/file-formats.md` (the wipe word), `docs/future_work/cjk.md` (selective eviction)

**Interfaces:**
- Consumes: Task 1's `TtfFontSet::wipeIfPending`, through the existing `SdCardFontManager::wipeTtfIfPending(const std::function<void()>&)`.
- Produces: `void SdCardFontSystem::wipeTtfIfPending(const std::function<void()>& onBusy)`.

No host test reaches `EpubReaderActivity`; Task 1 pins the engine's half and Task 3 proves this one on the devices.

- [ ] **Step 1: The passthrough.** In `src/SdCardFontSystem.h`, after `releaseTtfMissMemory()`:

```cpp
  /// Empties a full glyph store (SdCardFontManager::wipeTtfIfPending), calling onBusy first: the reader's chapter
  /// open, where no glyph record is in use.
  void wipeTtfIfPending(const std::function<void()>& onBusy) { manager_.wipeTtfIfPending(onBusy); }
```

In `lib/EpdFont/SdCardFontManager.h` the comment of `wipeTtfIfPending` becomes:

```cpp
  // Runs the TTF glyph store's pending wipe (it filled up), calling onBusy first. Only at book or chapter open, under
  // RenderLock (TtfFontSet::wipeIfPending).
```

- [ ] **Step 2: The call at chapter open.** In `EpubReaderActivity::buildSection()`, after the invalid-spine early return and before `const uint16_t viewportWidth = layout.viewportWidth;`:

```cpp
  // A TrueType glyph store that filled up during the last chapter is emptied before this one is laid out or drawn
  // (spec 8.2), or every glyph new since would be drawn again on every page until the next book. No glyph record is in
  // use here: the render pass holds RenderLock, which every lookup, draw and pre-render takes. About 2 s, once per
  // fill, behind the same popup as at book open.
  sdFontSystem.wipeTtfIfPending([this] { GUI.drawPopup(renderer, tr(STR_LOADING_FONT)); });
```

- [ ] **Step 3: The device test book.** Create `tools/make_cjk_store_fill_epub.py`: a Traditional Chinese EPUB (`dc:language` `zh-Hant`) of eight chapters, `第1章` to `第8章`, that together hold every Big5 ideograph (U+4E00-U+9FFF decoded from lead bytes 0xA4-0xF9, trail bytes 0x40-0x7E and 0xA1-0xFE) once, in Big5 order, about 1,630 a chapter, in `<p>` paragraphs of 50; a `nav.xhtml` TOC; every zip entry dated 2026-10-20 so the output is the same bytes each run; the output path is `sys.argv[1]`, default `cjk_store_fill.epub`. Its docstring says why it is not in `test/epubs`: every book there becomes a golden case (`EpubPipelineTest.cpp`'s `corpusCases()`), and this one is for the device only. The prototype's file (`C:/tmp/wt-wipe-proto/tools/make_cjk_store_fill_epub.py`, 108 lines) can be copied as it is.

Run: `python tools/make_cjk_store_fill_epub.py /c/tmp/cjk_store_fill.epub`
Expected: `Wrote /c/tmp/cjk_store_fill.epub: 13051 characters, 1632 a chapter`

- [ ] **Step 4: Docs.**
  - `docs/file-formats.md`, the glyph store's "wipe word" bullet: "... and the state sector last, at the next book open." becomes "... and the state sector last, when the next chapter or book opens."
  - `docs/future_work/cjk.md`: a new section "## Glyph store: selective eviction" with spec section 8.3 in this file's style: **Open.** The store is emptied whole when it fills, at the next chapter (about 2 s, X3: 1.9 s at the 32-page budget). **The sketch** (64 KB pages of the log; a RAM "used since the last pass" bitmap, 2 KB for 16,384 slots; when full, the oldest page's glyphs still in use are copied forward, the others' index slots cleared by the key each record carries, the page erased, 50-100 ms). **Why not now** (the index needs periodic rebuilds; the copies add flash wear; emptying at the next chapter already removes most of the cost).

- [ ] **Step 5: Build the firmware.** One at a time, each in the background:

```bash
export PLATFORMIO_CORE_DIR=C:/pio PYTHONIOENCODING=utf-8 PYTHONUTF8=1
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default
"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro
"$HOME/.platformio/penv/Scripts/pio.exe" run -e lilygo_t5s3
```

Expected: `SUCCESS` for each; `check_no_float: ... TTF engine objects, no floating point`.

- [ ] **Step 6: Format and commit.**

```bash
PATH="<clang-format 21 dir>:$PATH" bash bin/clang-format-fix
git add src/SdCardFontSystem.h lib/EpdFont/SdCardFontManager.h src/activities/reader/EpubReaderActivity.cpp \
  tools/make_cjk_store_fill_epub.py docs/file-formats.md docs/future_work/cjk.md
git commit -m "feat(reader): a full glyph store is emptied when the next chapter opens"
```

### Task 3: On the X4 and the X3

**Files:** none changed; the figures go into the PR description.

What the prototype did on the X4 (2026-10-20, Noto Sans TC 26 pt, `proto/cjk-all`): opening the book wiped a full store at book open; the store filled again during chapter 1 (Background-B's build of chapter 2 measured its glyphs); turning into chapter 2 logged `glyph store wiped` about 1.3 s after the last page of chapter 1 (a 1.83 MB log), then adopted Background-B's section (`Cache found, skipping build`) and drew its first page in 1,060 ms, the glyphs drawn again. Expect the same shape on both boards.

A CJK TrueType family must be on the card. If the CJK fonts plan has merged, download Noto Sans TC from the Font Manager; otherwise copy any CJK `.ttf` into `/.crosspoint/fonts/<Family>/` over USB mass storage (`docs/usb-mass-storage.md`). Upload the test book with `python tools/serial_transfer.py` or over USB mass storage to `/test/cjk_store_fill.epub`.

- [ ] **Step 1: Flash.** The X4: `pio run -e default -t upload --upload-port <its port>` (stop any serial logger first). The X3: the maintainer flashes it; never reset it from the host.

- [ ] **Step 2: Fill the store at 26 pt.** With `tools/devctl.py --port <port> --log /c/tmp/wipe-<board>.log`: `cmd:GET:fontFamily`, `cmd:GET:fontSize` and `cmd:GET:sleepTimeoutMinutes` (write them down); select the CJK family (`cmd:SET:fontFamily=<its index>`), `cmd:SET:fontSize=26`, `cmd:SET:sleepTimeoutMinutes=30`; `cmd:ACT:READER:/test/cjk_store_fill.epub@1:1`, `idle:120000`; turn pages with `cmd:BTN:RIGHT` and `idle:30000` until the log shows `glyph store full: new glyphs are not stored until the next chapter or book opens`.
Expected: the line appears within the first two chapters.

- [ ] **Step 3: The next chapter empties it.** Turn on into the next chapter (or `cmd:ACT:READER:/test/cjk_store_fill.epub@<next spine>:1`).
Expected, in this order: the popup, `glyph store wiped` in the log (note its time from the line before), then the chapter's first page; `CMD:STATE` keeps the expected `chapter`; the next pages show store hits again (no `glyph store full` until the store has filled once more).

- [ ] **Step 4: Heap and a chapter Background-B built.** Read `heap=`/`maxblock=` from `CMD:STATE` before and after the wipe. Then go back two chapters, read to the last page of a chapter and wait (`idle:60000`) so Background-B builds the next one, fill the store again, and turn into that chapter.
Expected: the wipe runs once, the adopted chapter's pages draw correctly (a screenshot, `shot:/c/tmp/wipe-<board>-adopted.png`), and the free heap is within 2 KB of before.

- [ ] **Step 5: Restore and record.** Restore `fontFamily`, `fontSize` and `sleepTimeoutMinutes`. Record per board for the PR: the wipe's duration, the chapter the store filled in, and the heap before/after.
