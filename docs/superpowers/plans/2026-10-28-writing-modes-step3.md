# Writing modes step 3: vertical text through a turned frame — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Japanese, Chinese or Korean book set vertical-rl is laid out as today's horizontal page in a frame turned 90°
clockwise and drawn with its CJK characters upright and its Latin runs sideways; the book decides (CSS `writing-mode`
on `html`/`body`, then OPF `page-progression-direction`), a global setting and a per-book override decide over it, and
its pages turn right to left. Punctuation stays rough: no vertical forms, no placement of 、。 or small kana.

**Architecture:** The parser decides each section's mode at `<body>` and, for a vertical section, swaps its viewport;
composer, breakers, tables and ruby run unchanged. Everything a vertical section builds carries its frame: the page its
block extent, every line a column bit, every image a turn. `Page::render` and its siblings switch the renderer to the
turned frame (`setDrawOrientation`, one step from the reader's held orientation); `drawTextVertical` (lib/Epub, over
the renderer's existing glyph paths) draws sideways runs in that frame and each upright character in the reader's.
`FlowContext` holds the vertical decision and the page progression that the reader's buttons, taps, swipes, progress
bar and percent slider follow.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3; ESP32-S3 boards built by CI), host tests with GoogleTest/CMake
(`test/`), Python fixture and table generators, the serial test API (`tools/devctl.py`, `docs/serial-test-api.md`)
on the X4.

**Spec:** `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`: section 7 row 3, and 5.1, 5.2,
5.4, 5.5, 5.7, 7, 9, 10/10.1. Roadmap: `docs/future_work/writing-modes.md` ("Parsed in step 0, read later", "Left from
the spike, for step 3"). The prototype is the spike `8f28584a0` (`origin/spike/vertical-frame`): this plan cuts its
frame scope, its viewport swap and its column draw into real code; nothing is merged from it. Expected values are
derived by hand from the host shims (`test/word_size_layout/GfxRenderer.h`: every glyph 10 px, ascender 16;
`test/epub_pipeline/GfxRenderer.h`: full-width CJK 16 px, others `6 + cp % 7`, ascender 18, line 24, screen 480×800)
and from the code at master `c3c5a6c9e`, checked again on 2026-10-29 against master `39fba77f5` (A3b #575, the CJK
dictionary #569/#570/#574, JPEG #571/#572, LF goldens #567, the writing-modes roadmap #573); line references are
`39fba77f5`'s, and implementers find code by name. Each test's comment carries its derivation. A value that does not
reproduce is a finding to report, not a number to adjust.

## Rulings carried in (controller and maintainer, 2026-10-28)

1. Step 3 builds `FlowContext` and page progression; step 1 (bidi, not started) reuses them and adds the horizontal
   right-to-left branch.
2. Punctuation stays rough: Tr characters (brackets, dashes, ……, ー) are drawn sideways through the turned frame, Tu
   characters (、。, small kana, ！？) upright and unplaced. No font re-cut, no vertical-form lookup. The per-codepoint
   decision lives in one function (`drawsUpright`), where step 4 places Tu from `getGlyphInk` or a re-cut font.
3. Section version 104: master is 103 (A3b #575). Renumber at merge if master passes 103 first.
4. The X4 (COM5) validates. An X3 run is planned (portrait, NotoSansJP 14 pt, spec 10.1's figures on the branch) as
   a step of Task 14, not a gate. Claude flashes the X3 on COM6 with esptool (`write-flash 0x0` of the branch's
   `default` `firmware.factory.bin`) and drives it with devctl, never with `CMD:REBOOT` or an RTS pulse; if the X3 is
   silent, that step stops and is reported (maintainer, 2026-10-29: this supersedes "the maintainer flashes the X3,
   and it is never reset from here"). One touch-board session (X4 Pro or T5S3, whichever the maintainer attaches)
   closes Task 14: taps, swipes and link targets on vertical pages.
5. Page Turn Buttons defaults to Follow Book (maintainer, 2026-10-28).

## Rulings made while planning (each with its cost if wrong)

- **`drawTextVertical` lives in lib/Epub (`VerticalDraw.cpp`), not in `GfxRenderer`.** The spec names it a renderer
  member; every layout test replaces `GfxRenderer` wholesale with a shim, so a member could never run on the host, and
  the rule checker would test a copy. As a free function over the renderer's public calls (`drawText`,
  `setDrawOrientation`, ...) the real code runs against the recording shims. It adds no rotation code: upright glyphs
  use the reader frame's fast path, sideways runs the turned frame's. Cost if wrong: a move into the renderer later.
- **`GfxRenderer::setDrawOrientation` becomes `const`** (`orientation` `mutable`): `TextBlock::render` holds a `const
  GfxRenderer&`, like every draw call. The held orientation is untouched, so the input layer still reads the reader's.
- **The reader's frame is the renderer's held orientation** (`getHeldOrientation()`); the turned frame is one step
  from it (Portrait → LandscapeCounterClockwise, LandscapeClockwise → Portrait, PortraitInverted → LandscapeClockwise,
  LandscapeCounterClockwise → PortraitInverted, the spike's table). No static state, as the spike had.
- **The section decides at `<body>`**, from the writing mode in effect there (its own, else `html`'s), with the
  request and the book's ppd handed in before the parse; the viewport is swapped then, before any block is laid out.
  The header keeps the request (matched on load, like every render setting) and a status bit the outcome.
- **The decision rule:** Off → horizontal. On → vertical. Automatic → vertical when the root mode is vertical-rl;
  horizontal when it is horizontal-tb or vertical-lr; when unset, vertical when the ppd is rtl. The language gate (a
  book whose profile has no `verticalAllowed` gets Off) is the reader's, in `FlowContext::requestFor`, so a Latin
  book's cache key never depends on the setting. vertical-lr (Mongolian, practically never CJK) is set horizontally:
  only vertical-rl has a turned frame. Cost: a vertical-lr book reads horizontally.
- **Page progression:** right to left when the request is not Off and either the section is vertical or the book's
  ppd is rtl, so a horizontal colophon inside a vertical book keeps the book's direction; left to right otherwise.
  The spec's "OPF ppd" branch for horizontal books is gated on the request because a horizontal book's text still runs
  left to right until step 1 (a ppd-rtl Hebrew book, or a Japanese book the reader set Off, must not turn backwards).
  A vertical section turns right to left even under a contradictory ppd `ltr`.
- **Images:** in a vertical section the parser sizes an image with its native width and height swapped, so the
  unchanged sizing fits it turned into the content frame; the `ImageBlock` is then *turned*: it is decoded, cached
  (`.pxc`) and drawn at the reader's dimensions in the reader's frame, while `getWidth()`/`getHeight()` stay the box it
  takes on the page. Every image path (block, float, table cell, background, grayscale replay, warm passes) goes
  through the two `ImageBlock` entry points, so none is missed. A turned image is never cropped across pages (none is
  taller than a page: the sizing caps it). An image alone on a vertical page is not centred along the block axis.
  Cost: a small lone picture sits at the right edge.
- **Decorations:** in a column the underline is the Japanese side line, on the over side (right of the column, 2 px
  outside the em box), and line-through runs along the column's centre. `overline` is not parsed (no style bit is
  left, `EpdFontFamily.h:10-20`); out of scope.
- **Not read in step 3:** `text-orientation` and `text-combine-upright` (step 4 adds the per-word flag both need);
  every character takes its UAX #50 class (CSS `mixed`). Drop caps are not taken in a vertical section (their ink
  metrics are horizontal). Floats stay: a float is line-relative and wraps the columns in the turned frame.
- **Measurement seam:** in a vertical section a word's width is its column advance: an upright character one em (the
  advance of 一; its own advance in a font without CJK, CSS's 1 em vertical advance for fonts without vertical
  metrics), a sideways run its horizontal advance. Widths change only for upright characters whose advance is not an em
  (§, ×, ①, emoji). Measure and draw share `verticalTextAdvance`.
- **The dictionary is not available in vertical text yet** (guard, not mapping). Its word boxes and navigation would
  need rotated directions, and `DictionaryWordSelectActivity` has just been rewritten for CJK word lookup (#569, #570,
  #574: speculation, the span highlight, `resolveRun`). The guard sits in `EpubReaderActivity::openDictionary` and the
  menu, which those PRs left as they were (`openDictionary` :2645, the Dictionary row `EpubReaderMenuActivity.cpp:146`;
  `BTN_DICTIONARY` and the long-tap gesture reach `openDictionary` through `onButtonAction` :7562). Cost: vertical
  readers cannot look words up until the follow-up maps the selection through the frame.
- **Off the render task the reader reads the held frame.** A vertical page holds the turned frame for its whole draw,
  so `tapToLogical`, `wasSwipe`, the tap zones and `computeRenderLayout` read the held orientation (the GfxRenderer.h
  comment at `:385-389` already asks this of the input layer), and so do `logicalToTap` and the serial `CMD:TOUCH`
  injector that maps through it, or Task 14's touch session would inject through the turned frame.
- **Strings in `english.yaml` only** (the line-spacing precedent: the release round translates).
- **What Follow Book flips:** only the Left/Right buttons' built-in actions; an action the reader assigned
  explicitly, and the side buttons, do not. The built-in tap zones and page swipes mirror; the gesture bindings
  (ten-page flicks, corners, `TouchGestures::BINDINGS`) keep their directions. Cost: a flick in a vertical book skips
  the way it does in every other book.
- **The per-book override row shows only in a book whose language may be set vertically.**

## Global Constraints

From the brief and the spec, verbatim where they have it:
- Every step runs the host pipeline goldens: Latin and horizontal CJK books byte-identical. Every existing golden stays
  byte-identical; the new goldens are the vertical variants' and the new fixture's.
- A rule checker for vertical: every U character upright, every R run sideways, kinsoku at column ends (spec 7).
- No steady-state RAM; the vertical draw path allocates nothing (spec 8); stack locals under 256 B; comments say what
  and why, no history. What this step adds while reading: 4 B per `Page` object (at most three alive), 2 B per
  `Section`, 3 B in `EpubReaderActivity`; `TextBlock` stays 52 B (its pad byte, `TextBlock.cpp:20`), `ImageBlock`'s
  flag takes the padding after `largeImageCached_`. Nothing per word.
- Flash cost measured from the map file and stated in the last task. The UAX #50 table is 264 runs × 4 B = 1,056 B.
- Host tests from Git Bash only, never PowerShell. Fresh worktree: `git submodule update --init freeink-sdk` first.
  Configure: `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DGIT_EXECUTABLE="C:/Program Files/Git/cmd/git.exe"
  -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src
  -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-serial-api/build/test/_deps/arduinojson-src` (CMake reports
  `GIT_EXECUTABLE` unused while all three sources are given; passing it overrides a stale cache entry, which silently
  runs old binaries). Build `cmake --build build/test --target <T> -j 8`, run `ctest --test-dir build/test -R <pattern> --output-on-failure`
  (no `-j` beside other worktrees: shared temp fixtures). The whole suite before the PR:
  `cmake --build build/test -j 8 -- -k 0` (`epub_build_inventory` does not build on Windows) and `ctest --test-dir
  build/test`.
- Goldens are LF (`.gitattributes`, #567). A worktree checked out before #567 keeps CRLF copies of every golden its
  merges did not touch, and their `MatchesGolden` cases fail before any change: `git ls-files --eol
  test/epub_pipeline/goldens | grep w/crlf` must print nothing; if it does, delete those files and `git checkout --
  test/epub_pipeline/goldens` (done in `C:/tmp/wt-wm3` on 2026-10-29: 51 of 55 were CRLF).
- On Windows five `EpubPipelineTest` cases fail at master `39fba77f5` with `filesystem error: cannot remove all ...
  book.bin` (the test's cleanup runs while the book's `book.bin` is still open; Linux CI passes them):
  `FirstChapter.AGuideTextReferenceWithAFragmentFindsItsChapter`,
  `FirstChapter.AChaptersZipStatHoldsNoMemoryForTheBook`, `LongChapterPageTables.AChapterFarPastTheReserveBuildsWhole`,
  `LongChapterPageTables.ARefusedChunkStopsTheBuildWithBothTablesInStep`,
  `KeptReadingState.AReopenAfterAKeptClearLandsOnTheSavedPageAndBookmark`. "All of `EpubPipelineTest`: pass" means
  these five fail the same way and nothing else fails (at `39fba77f5`: 945 cases, 934 pass, 6 skip).
- Firmware: `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
  `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default` (and `-e gh_release`), one pio run at a time, in the
  background. `x4pro` and `lilygo_t5s3` are left to CI, except the one env Task 14's touch session needs.
- Devices: the X4 on COM5. The X3 on COM6: Claude flashes it with esptool (`write-flash 0x0` of the branch's
  `default` `firmware.factory.bin`, ruling 4) and drives it with devctl; nothing else resets it (no RTS pulse, no
  `CMD:REBOOT`, no `CMD:SLEEP`), and files reach it through its USB Transfer screen over a no-reset serial open. If the
  X3 is silent on USB, its step stops and is reported.
  The touch board: whichever the maintainer attaches. The acceptance book is
  `C:/_development/witchhunt-reader/.idea/annas-arch-91dbd462043f.epub` (1,663,588 B; also on the X3's `/test`),
  local only, never in the repository.
- clang-format 21, as CI: `PATH="/c/tmp/cf21/bin:$PATH" bin/clang-format-fix -g`, and `clang-format-21 -style=file -i`
  for new files.
- New user-facing strings through `tr()`: keys in `lib/I18n/translations/english.yaml` only, each used in code in the
  commit that adds it (the build's `scripts/gen_i18n.py` strips unused keys; never run it by hand). Settings follow
  `src/SettingsList.h`'s patterns; per-book overrides follow the line-spacing override (`4d0c4c526`).
- Caches: `SECTION_FILE_VERSION` 104 (`Section.cpp:36`; master is 103, A3b #575). `BOOK_CACHE_VERSION` stays 18: step
  0 already caches the ppd (`BookMetadataCache.h:30`, `Epub::pageProgression()`). `CSS_CACHE_VERSION` unchanged.
- The UAX #50 data is Unicode 16.0.0's `VerticalOrientation.txt`
  (https://www.unicode.org/Public/16.0.0/ucd/VerticalOrientation.txt, sha256
  `24ac1474554eb55c85ace5c16d551535a8c52cdc2f4c13cf68c3839214339bbe`), under the Unicode License v3. No code from
  matcha-reader (MIT, a second engine) or koreader-tategumi (GPL) is used.
- What landed while this plan waited (master `39fba77f5`): A3b (#575) changed the four breakers (each tests a line at
  its pushed-in width: `pushedInWidth`, `withEdgeTrims`, `gapShrinkBefore`), `extractLine` (push-in, the CJK-Latin
  gap `cjkLatinGapBefore`), `CjkPunctuation`, `CjkRulesTest.cpp`, `CjkTypographyTest.cpp`, `test_cjk_line_breaking`
  and the goldens of line_breaking, zh_honglou, zh_punctuation and test_writing_modes. This plan touches none of
  those; in `ParsedText.cpp` it edits `measureWordWidth` (`:104-121`), `calculateWordWidths` (`:890-900`), the split in
  `hyphenateWordAtIndex` (`:1669`, `:1730`) and `inkShift` in `extractLine` (`:1899`), all in Task 8. Its vertical
  goldens are new and are made on A3b's layout; no existing golden moves (test_writing_modes's included: the default
  profile is Off). The CJK dictionary (#569, #570, #574) changed `DictionaryWordSelectActivity`,
  `DictionaryDefinitionActivity`, `Dictionary`, `SerialTestApi.cpp` (`ACT:DICT`) and `Utf8.h` (`utf8CodepointLen`);
  this plan touches only `SerialTestApi.cpp`'s `injectContact` (Task 12), and the UAX #50 table is a new file, not in
  `Utf8.h`.

## Review Focus

1. **A screen geometry read off the render task while a vertical page draws.** The turned frame is held for the whole
   page draw; a tap, a swipe or a Background-B build's `computeRenderLayout` sampled then would see an 800-wide portrait
   screen. The reader must read the held frame. Pinned in Task 12 (the grep step, and no live-orientation read left on
   those paths) and Task 14 (the X4 log shows no `Parameters do not match` after Background-B builds while reading
   vertically; on the touch board, taps sent right behind a page turn land in the zone they were aimed at).
2. **A colophon:** a section whose `html`/`body` says `horizontal-tb` in a ppd-rtl book, under Automatic, stays
   horizontal and still turns right to left. Pinned in Task 3 (`FlowContext.AColophonKeepsTheBooksDirection`) and
   Task 9 (`VerticalLayout.AColophonStaysHorizontalAndTurnsRightToLeft`).
3. **Tokens that mix upright and sideways text:** `第13章`, a Latin word with a combining accent, a kana with a combining
   voiced mark, a URL longer than a run. Each piece keeps its marks and its codepoints, and the pens add up. Pinned in
   Task 1 (`VerticalOrientation.APieceKeepsItsMarksAndNeverSplitsACodepoint`) and Task 2
   (`VerticalDraw.ALongSidewaysRunIsDrawnInPiecesAtTheirPens`, `VerticalDraw.ACombiningMarkStaysOnItsBase`).
4. **Images in a vertical book:** portrait, landscape, larger than the page, in a table cell, alone on a page. Each is
   sized and drawn in the reader's frame and never rejected. Pinned in Task 7 (`VerticalLayout.*Image*`) and Task 12
   (the image-only page guard, checked on spine 22 of the acceptance book in Task 14).
5. **One cache key per book:** the reader, Background-B and the sleep screen's page compute the same request from the
   setting, the per-book override and the language, or the sleep path builds a duplicate variant (402cf5d67's lesson).
   Pinned in Task 3 (`FlowContext.TheRequestIsTheBooksThenTheSettingsThenTheLanguages`) and Task 10 (both paths call
   it; the X4 log in Task 14).

---

### Task 1: UAX #50 classes and the pieces of a column

**Files:**
- Create: `scripts/gen_vertical_orientation.py`, `lib/Epub/Epub/VerticalOrientationTable.h` (generated, committed),
  `lib/Epub/Epub/VerticalOrientation.h`, `lib/Epub/Epub/VerticalOrientation.cpp`
- Create: `test/vertical_text/CMakeLists.txt` (target `VerticalTextTest`: this file's sources plus
  `${REPO_ROOT}/lib/Utf8/Utf8.cpp`; include dirs `${REPO_ROOT}/test/word_size_layout` first, then `test/shims`,
  `lib/Epub`, `lib/EpdFont`, `lib/Utf8`, `lib/Logging`, `lib/Memory`, `lib/Serialization`, as `test/cjk_layout`),
  `test/vertical_text/VerticalOrientationTest.cpp`; append `add_subdirectory(vertical_text)` to `test/CMakeLists.txt`

**Interfaces:**
- Produces:
```cpp
// How a character is set in a vertical column: UAX #50's Vertical_Orientation (Unicode 16.0), a property of the
// character. R sideways, U upright, Tu and Tr upright or sideways with a vertical form of their own where the font
// has one.
enum class VerticalOrientation : uint8_t { R = 0, U = 1, Tu = 2, Tr = 3 };
VerticalOrientation verticalOrientationOf(uint32_t cp);
// The one place a class becomes a drawing: U and Tu upright in the reader's frame, R and Tr sideways through the
// turned frame. No vertical forms and no placement of Tu marks yet.
bool drawsUpright(uint32_t cp);
// The next piece the vertical draw path sets as one unit: a single upright character, or the longest run of sideways
// ones up to kMaxColumnRunBytes; either keeps the combining marks (utf8IsCombiningMark, U+3099, U+309A) and variation
// selectors (U+FE00-FE0F, U+E0100-E01EF) that follow a character, and a run never ends inside such a cluster. Returns
// the piece's length in bytes (0 at the end of `text`), and whether it is upright.
constexpr size_t kMaxColumnRunBytes = 64;
size_t nextColumnPiece(const char* text, bool& upright);
```
- The generated table: `constexpr uint32_t kVerticalOrientationRuns[]`, each entry `start << 2 | class` (class in
  `VerticalOrientation`'s numbering), sorted, the first starting at 0, adjacent runs of one class merged, R where the
  file lists nothing (its `@missing` line); `constexpr size_t kVerticalOrientationRunCount = 264;` (counted
  2026-10-28: 127 U, 89 R, 34 Tu, 14 Tr runs, 34 of them above the BMP). Lookup: the last run whose start is ≤ `cp`.

- [ ] **Step 1: Write the generator.** `python -I scripts/gen_vertical_orientation.py <VerticalOrientation.txt>
  [<out.h>]` (default out: `lib/Epub/Epub/VerticalOrientationTable.h`). It refuses a file whose first line is not
  `# VerticalOrientation-16.0.0.txt`, and writes a header naming the source file, its sha256, the Unicode License v3,
  and "generated by scripts/gen_vertical_orientation.py; do not edit". Download the data into the session scratchpad
  (never the repo) with Git Bash's `curl -sSfL`, check its sha256 against Global Constraints, run the generator.
- [ ] **Step 2: Write the failing tests** in `VerticalOrientationTest.cpp` (classes checked against the data file on
  2026-10-28):
```cpp
TEST(VerticalOrientation, TheTableIsUnicode16sInRuns) {
  EXPECT_EQ(264u, kVerticalOrientationRunCount);
  EXPECT_EQ(0u, kVerticalOrientationRuns[0] >> 2);
  for (size_t i = 1; i < kVerticalOrientationRunCount; ++i) {
    EXPECT_LT(kVerticalOrientationRuns[i - 1] >> 2, kVerticalOrientationRuns[i] >> 2);
    EXPECT_NE(kVerticalOrientationRuns[i - 1] & 3, kVerticalOrientationRuns[i] & 3) << "adjacent runs are merged";
  }
}

TEST(VerticalOrientation, ClassesFollowUax50) {
  using V = VerticalOrientation;
  for (const uint32_t cp : {0x4E00u, 0x3005u, 0x3000u, 0x30FBu, 0xAC00u, 0x1100u, 0xA7u, 0xA9u, 0xD7u, 0xF7u, 0x2160u,
                            0x2460u, 0x3099u, 0xFE45u, 0x20000u, 0x1F600u, 0xE000u, 0xFFFDu, 0x25CFu})
    EXPECT_EQ(V::U, verticalOrientationOf(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x3001u, 0x3002u, 0x3041u, 0x3063u, 0x30A1u, 0x30C3u, 0x31F0u, 0xFF01u, 0xFF0Cu, 0xFF0Eu,
                            0xFF1Fu, 0x309Bu, 0x309Cu})
    EXPECT_EQ(V::Tu, verticalOrientationOf(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x3008u, 0x300Cu, 0x300Du, 0x3014u, 0x301Cu, 0x301Du, 0x30FCu, 0x30A0u, 0xFF08u, 0xFF09u,
                            0xFF1Au, 0xFF1Bu, 0xFF5Eu, 0xFF5Bu, 0xFF5Du, 0xFF5Fu, 0xFF60u, 0xFF3Fu, 0xFF5Cu, 0x3030u})
    EXPECT_EQ(V::Tr, verticalOrientationOf(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x41u, 0x31u, 0x20u, 0x2014u, 0x2015u, 0x2025u, 0x2026u, 0x2E3Au, 0xB7u, 0x2010u, 0x2013u,
                            0x2190u, 0xFF0Du, 0xFF61u, 0xFF65u, 0xFF71u, 0xFF9Eu, 0x2022u, 0x10FFFFu})
    EXPECT_EQ(V::R, verticalOrientationOf(cp)) << std::hex << cp;
}

TEST(VerticalOrientation, UprightIsUAndTuSidewaysIsRAndTr) {
  EXPECT_TRUE(drawsUpright(0x4E00));   // 一 U
  EXPECT_TRUE(drawsUpright(0x3002));   // 。 Tu: upright, unplaced
  EXPECT_FALSE(drawsUpright(0x300C));  // 「 Tr: sideways
  EXPECT_FALSE(drawsUpright(0x30FC));  // ー Tr
  EXPECT_FALSE(drawsUpright('A'));
}
```
  and `VerticalOrientation.APieceKeepsItsMarksAndNeverSplitsACodepoint`, walking `nextColumnPiece` over each input and
  comparing the (length, upright) sequence: `"日本"` → (3, U) (3, U); `"Latin語"` → (5, S) (3, U); `"か\u3099"` → (6, U);
  `"e\u0301a"` → (4, S); `"\u3099"` alone → (3, U); 70 × `a` → (64, S) (6, S); 40 × `é` → (64, S) (16, S); 63 × `a` +
  `"e\u0301"` → (63, S) (3, S); `"😀\uFE0F"` → (7, U); `""` → 0 at once.
- [ ] **Step 3: Run; they fail to compile.** `cmake --build build/test --target VerticalTextTest -j 8`
- [ ] **Step 4: Implement** `VerticalOrientation.cpp`: a binary search over the table; `nextColumnPiece` with
  `utf8NextCodepoint`.
- [ ] **Step 5: Run** `ctest --test-dir build/test -R VerticalOrientation --output-on-failure`: pass.
- [ ] **Step 6: Commit** `feat(epub): UAX #50 vertical orientation and the pieces of a column`.

### Task 2: `drawTextVertical`, the column advance and the turned frame

**Files:**
- Create: `lib/Epub/Epub/TurnedFrame.h` (header-only, no includes but `<cstdint>`, plain-int orientations as
  `lib/GfxRenderer/TouchTransform.h` does), `lib/Epub/Epub/VerticalDraw.h`, `lib/Epub/Epub/VerticalDraw.cpp`
- Modify: `lib/GfxRenderer/GfxRenderer.h` (`:51` `orientation` becomes `mutable`; `:384` `setDrawOrientation` becomes
  `const`, its comment saying why: a vertical page switches frames from inside const draw calls)
- Modify: `test/word_size_layout/GfxRenderer.h` (the frame surface below; `DrawCall` gains `Orientation frame`, the live
  orientation at the call)
- Test: `test/vertical_text/VerticalDrawTest.cpp`, `test/vertical_text/AllocationCounter.{h,cpp}` (the global
  `operator new`/`delete` counter of `CjkTypographyTest.cpp:175-193`, as `long liveAllocations()` and `long
  allocations()`), added to `VerticalTextTest`; `test/touch_transform/TurnedFrameTest.cpp`, added to
  `TouchTransformTest` with `${REPO_ROOT}/lib/Epub` on its include path

**Interfaces:**
- Consumes: Task 1's `nextColumnPiece`, `drawsUpright`.
- Produces, `TurnedFrame.h`:
```cpp
// A vertical-rl page is laid out as a horizontal page in a frame one 90-degree step from the reader's (spec 5.2):
// inline runs down, block runs leftward, so a content point (x, y) is the reader point (readerWidth - 1 - y, x).
// Orientations in GfxRenderer::Orientation's order; Page.cpp static_asserts that they agree.
namespace turnedframe {
enum Orientation : int { Portrait = 0, LandscapeClockwise = 1, PortraitInverted = 2, LandscapeCounterClockwise = 3 };
constexpr int contentOf(int reader);   // the turned frame for the reader's
constexpr int readerOf(int content);   // its inverse
struct Rect { int x; int y; int width; int height; };
constexpr Rect toReader(int readerWidth, Rect content);  // {readerWidth - y - height, x, height, width}
struct Offsets { int x; int y; };
// Where a page drawn at reader offsets (readerX, readerY) puts its origin in the turned frame: block 0 is the right
// edge of a viewport blockExtent wide. {readerY, readerWidth - readerX - blockExtent}
constexpr Offsets contentOffsets(int readerWidth, int readerX, int readerY, int blockExtent);
}  // namespace turnedframe
```
- Produces, `VerticalDraw.h`:
```cpp
// The em of vertical drawing for fontId at scale: the advance of 一, else the ascender (a font without CJK).
int columnEm(const GfxRenderer& renderer, int fontId, EpdFontFamily::Style style, float scale);
// A column's centre line and its over edge (where the ruby band, emphasis and the side line lie: right of the column),
// from the line's baseline: the ideographic em box sits 880/1000 above the baseline and 120 below.
constexpr int columnAxis(int baseline, int em) { return baseline - (em * 38 + 50) / 100; }
constexpr int columnOverEdge(int baseline, int em) { return baseline - (em * 88 + 50) / 100; }
// How far `text` runs down a column at `scale` (spec 5.4): an upright piece one em (the advance of 一; its own advance
// in a font without 一), a sideways run its horizontal advance. Summed unscaled, rounded once (int(raw * scale + 0.5)),
// as ParsedText rounds a word.
int verticalTextAdvance(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style,
                        float scale);
// Draws `text` down a column. Called in the turned frame (Page::render's scope) with what drawText would get there:
// x the pen along the column, y the line's top. Allocates nothing; stack under 100 B.
void drawTextVertical(const GfxRenderer& renderer, int fontId, int x, int y, const char* text,
                      EpdFontFamily::Style style, float scale);
```
- `drawTextVertical`, exactly:
  - In the font cache's scan pass (`isFontCacheScanning()`) it hands the whole text to `drawText` (scale 1) or
    `drawTextScaled` and returns: the scan only collects glyphs.
  - `reader = getHeldOrientation()`, `content = getOrientation()`, `readerWidth = getScreenWidth(reader)`;
    `ascender` = `getFontAscenderSize` (scale 1) or `getFontAscenderSizeScaled`; `em = columnEm(...)`; `baseline = y +
    ascender`; `axis = columnAxis(baseline, em)`.
  - Each piece is copied NUL-terminated into a `char[kMaxColumnRunBytes + 1]`; `pen = x + scaled(rawBefore)`, where
    `rawBefore` sums the pieces' raw column advances and `scaled(v)` is `v` at scale 1, else `int(v * scale + 0.5)`.
  - A sideways piece is drawn at `(pen, y)` in `content`.
  - An upright piece with scaled advance `adv` (of its own glyphs) is drawn in `reader` at `(readerWidth - 1 - axis -
    adv / 2, pen + (em * 88 + 50) / 100 - ascender)`: centred across the column, its em box from the pen. The frame is
    set to `reader` for the draw and back to `content` after it.
- `GfxRenderer` shim additions (`test/word_size_layout`): `enum Orientation { Portrait, LandscapeClockwise,
  PortraitInverted, LandscapeCounterClockwise };`, `void setOrientation(Orientation)` (held and live), `void
  setDrawOrientation(Orientation) const` (live), `Orientation getOrientation() const`, `Orientation
  getHeldOrientation() const`, `int getScreenWidth(Orientation) const` (480 portrait, 800 landscape).

- [ ] **Step 1: Write the failing tests.** `TurnedFrameTest.cpp`:
  - `TurnedFrame.TheTurnedFrameIsOneStepFromTheReaders`: `contentOf` maps 0→3, 1→0, 2→1, 3→2; `readerOf` inverts all
    four.
  - `TurnedFrame.AgreesWithThePanel`: for each reader orientation `o` and each of (0.1, 0.2), (0.5, 0.5), (0.9, 0.7),
    `touchtransform::tapToLogical(o, 800, 480, nx, ny, rx, ry)` and the same into `contentOf(o)` (cx, cy) give `rx ==
    W - 1 - cy` and `ry == cx`, W = 480 for portrait and 800 for landscape. This ties the table to the panel transform
    `GfxRenderer::rotateCoordinates` inverts.
  - `TurnedFrame.ARectAndAPageOrigin`: `toReader(480, {10, 20, 30, 40})` is `{420, 10, 40, 30}`;
    `contentOffsets(480, 10, 20, 460)` is `{20, 10}`.

  `VerticalDrawTest.cpp` (shim: every glyph 10 px, so em 10, ascender 16; `kFontId = 1`; the renderer set to Portrait,
  then `setDrawOrientation(LandscapeCounterClockwise)`):
```cpp
// em 10, ascender 16: baseline 50 + 16 = 66, axis 66 - (380 + 50) / 100 = 62, upright x = 479 - 62 - 10 / 2 = 412,
// upright y = pen + (880 + 50) / 100 - 16 = pen - 7. Pens: 日 100, 本 110, Latin 120, 語 170.
TEST(VerticalDraw, UprightPiecesDrawInTheReadersFrameRunsInTheTurnedFrame) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  drawTextVertical(r, kFontId, 100, 50, "日本Latin語", EpdFontFamily::REGULAR, 1.0f);
  ASSERT_EQ(4u, r.drawCalls.size());
  expectCall(r.drawCalls[0], "日", GfxRenderer::Portrait, 412, 93);
  expectCall(r.drawCalls[1], "本", GfxRenderer::Portrait, 412, 103);
  expectCall(r.drawCalls[2], "Latin", GfxRenderer::LandscapeCounterClockwise, 120, 50);
  expectCall(r.drawCalls[3], "語", GfxRenderer::Portrait, 412, 163);
  EXPECT_EQ(GfxRenderer::LandscapeCounterClockwise, r.getOrientation());
  EXPECT_EQ(80, verticalTextAdvance(r, kFontId, "日本Latin語", EpdFontFamily::REGULAR, 1.0f));
}
```
  - `VerticalDraw.AScaledColumnRoundsOnceAsLayoutDoes` (scale 1.5, `drawTextScaled`): em `int(15.5)` = 15, ascender
    `int(24.5)` = 24, axis 74 - 6 = 68, adv 15, so x 479 - 68 - 7 = 404, y = pen + 13 - 24; pens 100, 115, 130, 205:
    日 (404, 89), 本 (404, 104), Latin (130, 50) in the turned frame, 語 (404, 194); the advance is `int(120.5)` = 120.
  - `VerticalDraw.ALongSidewaysRunIsDrawnInPiecesAtTheirPens`: 70 × `a` from x 100: two draws, 64 bytes at 100 and 6 at
    740; advance 700.
  - `VerticalDraw.ACombiningMarkStaysOnItsBase`: `"か\u3099"` is one upright draw of 6 bytes; advance 10.
  - `VerticalDraw.TheReadersFrameIsTheHeldOne`: for each of the four held orientations, with the live one
    `contentOf(held)`, 日 at (100, 50) is drawn in `held` at x = W - 1 - 62 - 5 (412 portrait, 732 landscape).
  - `VerticalDraw.DrawingAllocatesNothing`: `drawCalls.reserve(16)`, then no allocation, live or freed, across a draw
    of `"日本Latin語"` (`AllocationCounter`).
- [ ] **Step 2: Run; they fail to compile.** `cmake --build build/test --target VerticalTextTest TouchTransformTest -j 8`
- [ ] **Step 3: Implement** `TurnedFrame.h`, `VerticalDraw.cpp` (add it to `VerticalTextTest`), the shim, and the
  `GfxRenderer` change.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "VerticalDraw|TurnedFrame|VerticalOrientation|TouchTransform"
  --output-on-failure`, then `WordSizeLayoutTest` and `CjkLayoutTest` (they share the shim): pass.
- [ ] **Step 5: Commit** `feat(epub): drawTextVertical, the column advance and the turned frame`.

### Task 3: `FlowContext`: the request, the section's mode and the page progression

**Files:**
- Create: `lib/Epub/Epub/FlowContext.h` (forward-declares `enum class CssWritingMode : uint8_t;`, includes
  `PageProgression.h`), `lib/Epub/Epub/FlowContext.cpp`
- Modify: `lib/Epub/Epub/TypographyProfile.cpp:60-65` (the Korean branch: `verticalAllowed = true`, spec 9.5),
  `TypographyProfile.h:35` (the comment "Nothing reads it yet": FlowContext::requestFor reads it)
- Test: `test/typography/FlowContextTest.cpp` (added with `FlowContext.cpp` to `TypographyTest` in
  `test/typography/CMakeLists.txt`); `test/typography/TypographyTest.cpp:13` (the comment, stale on master since A3b:
  `breaking` and `justify` are not read by layout yet, `cjkLatinGap` is, since #575, and `verticalAllowed` is read by
  `FlowContext::requestFor`) and `:44` (Korean `EXPECT_TRUE`)

**Interfaces:**
```cpp
// The reader's choice for a book, in CrossPointSettings::VERTICAL_TEXT's order (stored by number).
enum class VerticalText : uint8_t { Automatic = 0, On = 1, Off = 2 };

// How a section runs and turns (spec 5.1, 5.5): resolved once per book and section, kept in BuildParams, the property
// hash and the section header. Step 1 adds a right-to-left base direction for horizontal text.
struct FlowContext {
  bool vertical = false;            // set vertical-rl through the turned frame
  bool pageProgressionRtl = false;  // its pages turn right to left

  // The request a book's sections are built with: the book's override (-1 none, else a VerticalText), else the
  // setting; Off for a language that is never set vertically (TypographyProfile::verticalAllowed).
  static VerticalText requestFor(VerticalText setting, int8_t bookOverride, bool verticalAllowed);
  // Whether a section is set vertically, from the writing mode in effect at its <body> and the book's ppd.
  static bool verticalFor(VerticalText request, CssWritingMode rootMode, PageProgression ppd);
  static bool pageProgressionRtlFor(VerticalText request, bool vertical, PageProgression ppd);
  static FlowContext of(VerticalText request, bool vertical, PageProgression ppd);
};
```

- [ ] **Step 1: Write the failing tests** (`FlowContextTest.cpp`):
  - `FlowContext.TheRequestIsTheBooksThenTheSettingsThenTheLanguages`: `requestFor(Automatic, -1, true)` Automatic;
    `(Automatic, 1, true)` On; `(On, 2, true)` Off; `(Off, 0, true)` Automatic; `(On, -1, false)` Off; `(Automatic, 1,
    false)` Off; an override outside 0-2 (`7`) reads as none.
  - `FlowContext.OnAndOffDecideOverTheBook`: `verticalFor(Off, VerticalRl, Rtl)` false; `(On, HorizontalTb, Default)`
    true; `(On, Unset, Ltr)` true.
  - `FlowContext.AutomaticFollowsTheCssThenThePpd`: `(Automatic, VerticalRl, Default)` true; `(Automatic, HorizontalTb,
    Rtl)` false; `(Automatic, Unset, Rtl)` true; `(Automatic, Unset, Default)` false; `(Automatic, Unset, Ltr)` false;
    `(Automatic, VerticalLr, Rtl)` false (only vertical-rl has a turned frame).
  - `FlowContext.AColophonKeepsTheBooksDirection`: `pageProgressionRtlFor(Automatic, false, Rtl)` true;
    `(Automatic, true, Default)` true; `(On, true, Ltr)` true (columns advance right to left); `(Automatic, false,
    Default)` false; `(Off, false, Rtl)` false (text still runs left to right); `of(Automatic, true, Default)` is
    `{true, true}`.
  - In `TypographyTest.cpp`, `KoreanKeepsWholeWordsWithoutGridOrGap` expects `verticalAllowed` true (KLREQ; spec 9.5),
    and `TypographyProfile.OnlyChineseJapaneseAndKoreanMayBeSetVertically`: `ja`, `zh`, `zh-Hant`, `ko-KR` true; `en`,
    `he`, `""` false.
- [ ] **Step 2: Run; they fail.** `cmake --build build/test --target TypographyTest -j 8`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "FlowContext|TypographyProfile" --output-on-failure` and
  `EpubPipelineTest` (no golden moves: nothing reads the flag yet).
- [ ] **Step 5: Commit** `feat(epub): FlowContext, the vertical decision and the page progression`.

### Task 4: Lines in columns

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h` (`setVertical`/`isVertical`, the member after `wordFontCount`),
  `TextBlock.cpp` (`render` :470-682, `wordBox` :426-468, `serialize` :684-720, `deserialize` :722-816)
- Modify: `test/word_size_layout/GfxRenderer.h` (records `drawLine` calls: x1, y1, x2, y2, width, frame);
  `test/text_block_range/GfxRenderer.h` and `test/epub_pipeline/GfxRenderer.h` (the Task 2 frame surface; the pipeline
  stub's `getOrientation()` returns the live value and `getScreenWidth()`/`getScreenHeight()` follow it)
- Modify: `test/cjk_layout/CMakeLists.txt`, `test/word_size_layout/CMakeLists.txt`,
  `test/text_block_range/CMakeLists.txt`, `test/epub_pipeline/CMakeLists.txt` (`PIPELINE_SOURCES`): add
  `VerticalDraw.cpp` and `VerticalOrientation.cpp` (and `FlowContext.cpp` to `PIPELINE_SOURCES`)
- Test: `test/vertical_text/VerticalTextBlockTest.cpp` (`VerticalTextTest` gains `TextBlock.cpp` and
  `CjkPunctuation.cpp`, compiled as `test/cjk_layout/CMakeLists.txt` compiles them)

**Interfaces:**
- Consumes: Task 2's `drawTextVertical`, `verticalTextAdvance`, `columnEm`, `columnAxis`, `columnOverEdge`.
- Produces: `void TextBlock::setVertical(bool vertical)`, `bool TextBlock::isVertical() const` ("a line of a
  vertical-rl page: drawn down its column in the turned frame"). Stored in the pad byte (`static_assert` 52 holds);
  serialized as bit 2 of the flags byte (`TextBlock.cpp:698`), which `deserialize` stops rejecting (the mask at :742
  becomes `0x08`).
- A vertical line's `render`: each word, ruby character and emphasis mark goes through `drawTextVertical` at the
  position the horizontal code computes; the emphasis loop's per-character advance is the character's column advance
  (`verticalTextAdvance` at scale 1); the decoration and guide-dot widths are `verticalTextAdvance`. With `baseline =
  baseY + lineAscender` and `blockEm = columnEm(renderer, effFontId, REGULAR, blockScale)`: the underline at
  `columnOverEdge(baseline, blockEm) - 2`, the line-through at `columnAxis(wordY + ascender, columnEm(renderer, font,
  style, scale))` (the word's own font, style and scale), a guide dot centred on `columnAxis(baseline, blockEm)`.
  `wordBox`'s width is the column advance. Horizontal lines are unchanged.

- [ ] **Step 1: Write the failing tests** (`VerticalTextBlockTest.cpp`; a helper builds a line from words, x positions
  and annotations through `TextBlock::WordRange`, as `CjkRubyTest.cpp:20-44` does, then `setVertical(true)`; the shim in
  the turned frame of Portrait):
  - `VerticalTextBlock.EveryWordRunsDownItsColumn`: words 日, 本, Latin at x 0, 10, 20, `render(r, kFontId, 50, 100)`:
    日 (Portrait, 362, 43), 本 (Portrait, 362, 53), Latin (turned, 70, 100). (axis 100 + 16 - 4 = 112, x 479 - 112 - 5.)
  - `VerticalTextBlock.RubyAndMarksSitRightOfTheColumn`: 漢字 with ruby かんじ over both, then with a sesame run: every
    ruby and mark draw is in Portrait at an x greater than every base character's; ruby x 371 (band 8: ruby baseline
    100 + 8, em 5, axis 106, adv 5: 479 - 106 - 2), base x 354 (baseline 124, axis 120).
  - `VerticalTextBlock.TheSideLineIsOnTheOverSide`: an UNDERLINE word 日本 at (50, 100): one `drawLine` at y 105 (the
    baseline 116, over edge 116 - 9 = 107, less 2) from x 50 to 70; a STRIKETHROUGH word: y 112.
  - `VerticalTextBlock.AWordBoxRunsDownTheColumn`: `wordBox` of Latin日 has width 60.
  - `VerticalTextBlock.DrawingAColumnAllocatesNothing`: a line with ruby, a sesame run and an underline word, every
    word at scale 1 (`render`'s ascender cache only grows for a word of another font or scale); the shim's recorders
    (`drawCalls`, the `drawLine` record, `fillRectCalls`) reserved first, as `VerticalDraw.DrawingAllocatesNothing`
    reserves `drawCalls`; no allocation, live or freed, in `render`.
  - `VerticalTextBlock.AHorizontalLineDrawsAsBefore`: the same words without `setVertical`: three `drawText` calls in
    the live frame at x 50, 60, 70.
- [ ] **Step 2: Run; they fail to compile.** `cmake --build build/test --target VerticalTextTest -j 8`
- [ ] **Step 3: Implement**, and the shim and CMake changes.
- [ ] **Step 4: Run** `VerticalTextTest`, `CjkLayoutTest`, `WordSizeLayoutTest`, `TextBlockRangeTest`,
  `EpubPipelineTest`: pass, goldens unchanged.
- [ ] **Step 5: Commit** `feat(epub): a vertical line draws down its column`.

### Task 5: Pages in the turned frame

**Files:**
- Modify: `lib/Epub/Epub/Page.h` (`Page::verticalBlockExtent`, `isVertical()`), `lib/Epub/Epub/Page.cpp` (`render`
  :298, `renderImagesFromGrayscaleCache` :310, `warmImageCaches` :319, `renderTextOnly` :396, `serialize` :417,
  `deserialize` :447; a frame scope in the anonymous namespace; `static_assert`s that `turnedframe`'s orientation
  numbers are `GfxRenderer::Orientation`'s)
- Modify: `test/epub_pipeline/GfxRenderer.h` (a recorder: `struct TextDraw { std::string text; Orientation frame; int
  x; int y; }; mutable std::vector<TextDraw>* textDraws = nullptr;`, appended to by `drawText` and `drawTextScaled`),
  `test/epub_pipeline/PipelineRunner.cpp` (`dumpPage`: ` turned=<extent>` on the PAGE line when non-zero)
- Test: `test/epub_pipeline/VerticalPageTest.cpp`, added to `EpubPipelineTest`

**Interfaces:**
- Consumes: `turnedframe::contentOf`, `contentOffsets`; Task 4's vertical lines.
- Produces: `uint16_t Page::verticalBlockExtent = 0;` ("a page of a vertical-rl section: the block extent its columns
  fill, the reader's viewport width; 0 on a horizontal page"), `bool Page::isVertical() const`. Serialized as a `u16`
  after the element count.
- The scope, for a vertical page, around `render`, `renderTextOnly`, `renderImagesFromGrayscaleCache` and
  `warmImageCaches`: `reader = getHeldOrientation()`; the offsets the caller passes (reader `xOffset`, `yOffset`) become
  `contentOffsets(getScreenWidth(reader), xOffset, yOffset, verticalBlockExtent)`; `setDrawOrientation(contentOf(reader))`
  for the scope's lifetime; the live orientation found on entry is restored on every exit, an aborted `renderTextOnly`
  included. A horizontal page takes none of it.

- [ ] **Step 1: Write the failing tests** (a page built by hand: one `PageLine` at (0, 0) holding a vertical line of
  one word, 日A, at x 0; `verticalBlockExtent` 460; recorder on; `render(renderer, 1, 10, 20)`):
  - `VerticalPage.ATurnedPageDrawsInItsContentFrame`: content offsets (20, 10); 日 in Portrait at (449, 16) (stub em 16,
    ascender 18: baseline 28, axis 28 - 6 = 22, x 479 - 22 - 8, y 20 + 14 - 18), A in LandscapeCounterClockwise at
    (36, 10); afterwards `getOrientation()` is Portrait.
  - `VerticalPage.RenderTextOnlyAndAnAbortRestoreTheFrame`: the same through `renderTextOnly`, and with a
    `CooperativeAbort` that fires (`abortable` true): the orientation is Portrait after both.
  - `VerticalPage.AHorizontalPageDrawsAsBefore`: `verticalBlockExtent` 0 and a horizontal line: one draw in Portrait at
    (10, 20).
- [ ] **Step 2: Run; they fail to compile.**
  `cmake --build build/test --target EpubPipelineTest -j 8 && ctest --test-dir build/test -R VerticalPage --output-on-failure`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all of `EpubPipelineTest`: pass, goldens unchanged (no page is vertical yet; the extent is
  written and read back on every page).
- [ ] **Step 5: Commit** `feat(epub): a vertical page draws in the turned frame`.

### Task 6: A section decides its mode and lays out in the turned frame

**Files:**
- Modify: `lib/Epub/Epub/Section.h` (includes `FlowContext.h`; `BuildParams` :189-215: `VerticalText verticalText =
  VerticalText::Off;`, "the reader's request for the book, Off for a language never set vertically
  (FlowContext::requestFor); hashed only when not Off"; `isVertical()`, `flowContext()` beside `isSimplified()` :331;
  members `VerticalText request_`, `bool vertical_`)
- Modify: `lib/Epub/Epub/Section.cpp`: `SECTION_FILE_VERSION` 104 at :36 with its line, above A3b's v103 line ("v104:
  a section may be set vertical-rl: the header keeps the vertical-text request and a status bit the outcome; a page
  keeps its block extent, a line its column bit"); `header` :155-174 (`kVerticalText`, a `u8` after `kImageRendering`);
  `kStatusVertical = 1 << 5` after `kStatusSimplified` (:193) with its comment; `calculatePropertyHash` :366-407 (the
  request appended when not Off; the `static_assert` 21 → 22); `writeSectionFileHeader` :627-661 (the byte; its
  `static_assert`); `loadSectionFile` :663-743 (read, match, the status bit); `runBuildSetup` :1104-1273 (`request_`,
  `vertical_ = false`; `st.visitor->setVerticalText(p.verticalText, epub->pageProgression())` after `setTypography`
  :1266); the latches :1644-1653 (`vertical_ = st.visitor->isVertical()`); the status write :1820
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (beside the flow stack :678-695 and `setTypography` :1029),
  `ChapterHtmlSlimParser.cpp`: `decideFrame` called in `startElement` after `resolveLogical` (:2225) for the first
  element that is neither `html` nor `head` (case-insensitively; normally `<body>`; `head`'s children never get there,
  the skip at :2037 takes them; `head` itself reaches :2225 and is skipped only at :2932); `emitPage` (:1382, the
  extent, before `completePageFn` at :1398); `addLineToPage` (:4658) and the cell lines (:5435): `setVertical`;
  `tryStartDropCapCapture` (:1686): false in a vertical section
- Modify: `test/epub_pipeline/PipelineRunner.{h,cpp}` (the header includes `Epub/FlowContext.h`;
  `Profile::verticalText = VerticalText::Off`; ` vertical=1` on the SPINE line when `section.isVertical()`;
  `forEachPage`, sharing one params builder with `runAndDump`, whose dump does not change)
- Modify: `test/epub_pipeline/EpubPipelineTest.cpp` (variants, below)
- Create: `test/epub_pipeline/VerticalLayoutTest.cpp` (added to `EpubPipelineTest`); goldens
  `test_writing_modes_vertical`, `test_cjk_ja_soseki_vertical_on`, `test_cjk_ko_hyeon_vertical_on`

**Interfaces:**
- Consumes: Task 3's `FlowContext`, Task 4's `TextBlock::setVertical`, Task 5's `Page::verticalBlockExtent`.
- Produces:
  - `void ChapterHtmlSlimParser::setVerticalText(VerticalText request, PageProgression ppd);` and `bool isVertical()
    const;`. Private `void decideFrame(CssWritingMode rootMode)`: once per parse; `vertical_ =
    FlowContext::verticalFor(request, rootMode, ppd)`; when vertical, `viewportWidth` and `viewportHeight` swap; one
    `LOG_INF("EHP", "Section set %s (request %u, root %u, ppd %u)", ...)`.
  - `bool Section::isVertical() const;` (the live build's parser while one runs, else the loaded or built outcome) and
    `FlowContext Section::flowContext() const;` (`FlowContext::of(request_, isVertical(), epub->pageProgression())`).
  - `bool pipeline_harness::forEachPage(const std::string& epubPath, const std::string& cacheDir, const Profile&
    profile, const std::function<void(int spine, const Section&, const Page&, GfxRenderer&)>& visit);`
  - `EpubPipelineTest`: `Variant::Vertical` (name `verticalText`, suffix `_vertical`, request Automatic, books
    `kVerticalBooks = {"test_writing_modes"}`) and `Variant::VerticalOn` (`verticalTextOn`, `_vertical_on`, On,
    `kVerticalOnBooks = {"test_cjk_ja_soseki", "test_cjk_ko_hyeon"}`). The default profile stays Off, so every
    existing golden is byte-identical.

**The tests.** `VerticalLayoutTest.cpp` builds one-chapter books as `WritingModeLayoutTest.cpp:79-120` does, with the
OPF's `dc:language` a parameter (`ja` by default) and `spineAttrs` for the ppd.
- [ ] **Step 1: Write the failing tests:**
  - `VerticalLayout.AVerticalSectionIsItsHorizontalTwinTurned`: a chapter with a heading, two indented paragraphs of
    kana, kanji, 「」、。, a Latin word and digits, a ruby group, an `<hr/>` and a 2 × 2 table, no upright character whose
    advance is not an em (the twin would differ by the seam of Task 8). Built On at 460 × 760 and Off at 760 × 460: the
    dumps are equal once ` vertical=1` and ` turned=460` are removed.
  - `VerticalLayout.TheBookDecidesUnderAutomatic`: `body.v { -webkit-writing-mode: vertical-rl }` → SPINE `vertical=1`;
    `<body style="writing-mode: horizontal-tb">` with spine `page-progression-direction="rtl"` → none; no writing mode
    with ppd rtl → `vertical=1`; neither → none; `html { writing-mode: vertical-rl }` with `embeddedStyle = false` and
    no ppd → none.
  - `VerticalLayout.OffKeepsAVerticalBookHorizontal`: the first book under Off dumps like the default profile's, with
    no `vertical=1`.
  - `VerticalLayout.EveryPageAndLineOfAVerticalSectionIsTurned` (`forEachPage`, the twin's book under On): every page
    has `verticalBlockExtent` 460, every `PageLine` and every table cell line `isVertical()`; under Off none does.
  - `VerticalLayout.TheHeaderKeepsItsRequest`: built Automatic, `loadSectionFile` with Automatic succeeds and
    `isVertical()`; with On it fails (a rebuild); `calculatePropertyHash` differs between Off and Automatic.
  - `VerticalLayout.ADropCapStaysInItsLine`: `<p><span style="float: left; font-size: 3em">吾</span>輩は猫である。…</p>`:
    Off has a line whose only word is 吾 (`NativeSizeLayoutTest.cpp:127` shows the shape); On has none.
- [ ] **Step 2: Run; they fail.**
  `cmake --build build/test --target EpubPipelineTest -j 8 && ctest --test-dir build/test -R VerticalLayout --output-on-failure`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Goldens.** `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`; `git status` shows exactly
  the three new goldens. Check by hand, and say so in the commit: `test_writing_modes_vertical`'s three spines have
  `vertical=1` (spine 3 by the ppd), its pages `turned=460`, its lines within x 0-760, and each line's words where the
  Off build at 760 × 460 puts them (A3b's layout: the quarter-em gaps beside `ABC`, the 4-px grid inset of a 760-px
  column); the two `_vertical_on` goldens likewise. Then the whole `EpubPipelineTest` without `UPDATE_GOLDENS`: pass.
- [ ] **Step 5: Commit** `feat(epub): a section decides its writing mode and lays out in the turned frame (section v104)`.

### Task 7: Images in the reader's frame

**Files:**
- Modify: `lib/Epub/Epub/blocks/ImageBlock.h`, `ImageBlock.cpp` (`render` :459, `renderGrayscaleFromCache` :411,
  `makeCrop` :415, `serialize`/`deserialize` :621-652)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`: the `<img>` branch's dimensions (`if (dimsOk) {` :2631)
  and its `ImageBlock` (:2857); `attachPendingFloatImage`'s `ImageBlock` (:1648-1650); `buildImageFromEpubPath`
  (:5130-5164); `placeImageBlockAsBlock` (:5213: a turned image is placed whole, never sliced); two private helpers,
  `void turnIfVertical(ImageDimensions& dims) const` (swaps width and height in a vertical section) and
  `void turnIfVertical(ImageBlock* image) const` (`setTurned()` in a vertical section)
- Modify: `test/epub_pipeline/PipelineRunner.cpp` (` turned=1` on IMG and CELLIMG lines)
- Test: `test/epub_pipeline/VerticalLayoutTest.cpp`

**Interfaces:**
- Produces: `void ImageBlock::setTurned()` ("the image of a vertical page: the box it takes on the page, width by
  height, is the image turned; it is decoded, cached and drawn upright at height by width in the reader's frame"; swaps
  the stored size, idempotent), `bool isTurned() const`, `int16_t drawnWidth() const`, `int16_t drawnHeight() const`.
  `getWidth()`, `getHeight()` and `getRenderedHeight()` stay the box on the page. Serialized as a flags byte after
  `srcHeight_` (bit 0 turned). A turned block's `render` and `renderGrayscaleFromCache` map their content-frame
  `(x, y)` with `turnedframe::toReader(getScreenWidth(held), {x, y, getWidth(), getHeight()})`, switch to the held
  orientation, run the existing code at the reader's rectangle, and restore the live orientation.
- In a vertical section the parser swaps the native `dims.width`/`dims.height` before sizing (the `<img>` branch and
  `buildImageFromEpubPath`, after its native out-parameters are written) and calls `setTurned()` on every
  `ImageBlock` it makes.

- [ ] **Step 1: Write the failing tests** (On, viewport 460 × 760, so the content frame is 760 × 460; PNGs from
  `syntheticPngHeader` in `SyntheticBook.h:67`):
  - `VerticalLayout.APortraitImageIsSizedTurnedIntoTheColumns`: `<p><img src="tall.png"/></p>`, 200 × 400: native
    swapped 400 × 200 fits, so `IMG ... x=180 w=400 h=200 ... turned=1` ((760 - 400) / 2); drawn 200 × 400.
  - `VerticalLayout.ALandscapeImageIsSizedTurned`: 400 × 200: `x=280 w=200 h=400 turned=1`; drawn 400 × 200.
  - `VerticalLayout.AnImageTallerThanThePageIsScaledNotSliced`: 230 × 1520: swapped 1520 × 230, scale min(760 / 1520,
    460 / 230) = 0.5: `x=0 w=760 h=115 turned=1`, one IMG; drawn 115 × 760.
  - `VerticalLayout.ACellImageIsTurned`: the 200 × 400 image in a one-cell table: `CELLIMG ... turned=1`, and its
    `drawnWidth()`/`drawnHeight()` are its `getHeight()`/`getWidth()`.
  - `VerticalLayout.ATurnedImageDrawsInsideTheReadersScreen` (`forEachPage`, the 230 × 1520 book, a `LogObserver`):
    `page.render(renderer, 1, 10, 20, /*forceLoad=*/true)` logs no `Render bounds rejected`, and the orientation is
    Portrait afterwards. (Reader rectangle: x 480 - (10 + y) - 115, y 20, 115 × 760, inside 480 × 800; the header-only
    PNG then fails to decode, which the test ignores.)
  - `VerticalLayout.AHorizontalImageIsNotTurned`: the first book under Off: `w=200 h=400`, no `turned`.
- [ ] **Step 2: Run; they fail.** `ctest --test-dir build/test -R VerticalLayout --output-on-failure`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all of `EpubPipelineTest`: pass; the three vertical goldens hold no image, so none moves.
- [ ] **Step 5: Commit** `feat(epub): images of a vertical page are sized and drawn in the reader's frame`.

### Task 8: The measurement seam

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h` (`setVertical`, the member beside `typography_` :137),
  `lib/Epub/Epub/ParsedText.cpp` (`measureWordWidth` :104-121 gains `const bool vertical = false` after `scale`: a
  vertical word is `verticalTextAdvance(renderer, fontId, text, style, scale)` of the same text a horizontal one
  measures, the one-space token unchanged). `vertical_` is passed wherever a token's own text is measured for its box,
  so a token's width stays its column advance through every pass A3b (#575) added: `calculateWordWidths` :890-900;
  `hyphenateWordAtIndex`'s prefix (:1669) and remainder (:1730), which replace a split token's width; `extractLine`'s
  `inkShift` `natural` (:1899), which push-in compares with the token's box. The single-character measures stay
  horizontal: `cjkEm`'s 一 (:129: the em is the advance of 一 either way), `rubyWidth` (:904) and `applyRubyWidths`'
  `room` (:931): kana and CJK marks, one em in a CJK font across a line or down a column.
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`: `setVertical(vertical_)` beside each `setTypography`
  (:2021, :2420, :5357) and, in `decideFrame`, on the current block (the one `setup()` opens at :4319, before
  `<body>`)
- Test: `test/epub_pipeline/VerticalLayoutTest.cpp`

**Interfaces:**
- Consumes: Task 2's `verticalTextAdvance`; Task 6's `decideFrame`.
- Produces: `void ParsedText::setVertical(bool vertical)` ("measure words as a column runs them").

- [ ] **Step 1: Write the failing test** `VerticalLayout.AnUprightCharacterAdvancesOneEm`: left-aligned profile
  (`paragraphAlignment = 1`), `<p>あ×い §を×え</p>`, built On at 460 × 760 and Off at 760 × 460 (the same line width).
  The two dumps hold the same W records; each one's x under On is its x under Off plus 5 for every × and 4 for every §
  in the words before it on the line: × and § are U, one em (16 in the stub) down a column, and 11 (6 + 215 % 7) and
  12 (6 + 167 % 7) across a line. Independent of how A2 cuts the text into tokens.
- [ ] **Step 2: Run; it fails** (the vertical widths are the horizontal ones).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all of `EpubPipelineTest`, `CjkLayoutTest`, `WordSizeLayoutTest`. No golden moves: none of
  `test_writing_modes`, `test_cjk_ja_soseki` and `test_cjk_ko_hyeon` holds an upright (U or Tu) character whose stub
  advance is not 16 (their words and ruby checked against UAX #50 on 2026-10-29). Should a `_vertical` golden move
  anyway, it may move only where an upright non-em character is: regenerate it and name the character in the commit;
  any other move is a finding to report.
- [ ] **Step 5: Commit** `feat(epub): a vertical word measures as its column runs`.

### Task 9: The vertical fixture, its goldens and the rule checker

**Files:**
- Create: `test/epubs/make_test_vertical_text.py` → `test_vertical_text.epub`; goldens `test_vertical_text` and
  `test_vertical_text_vertical`
- Modify: `test/epub_pipeline/EpubPipelineTest.cpp` (`kVerticalBooks` gains `test_vertical_text`)
- Create: `test/epub_pipeline/VerticalRulesTest.cpp` (added to `EpubPipelineTest`)

**The book** (language `ja`, spine `page-progression-direction="rtl"`; the docstring says what each part pins, as
`make_test_writing_modes.py` does):
- spine 1, calibre style (`body.v { -webkit-writing-mode: vertical-rl }`, `p { margin: 0; text-indent: 1em }`): a
  heading 第一章　夜の森; paragraphs with ruby (`<ruby>冥夜<rt>めいや</rt></ruby>`), small kana (ッ, ゃ, ァ), 、。,
  「」『』（）, ー, ……, ——, ！？, the Latin word Winterfell, 1998, 第13章, × and §, `か\u3099`, an underline span,
  a `text-emphasis: filled sesame` span; a 2 × 2 table with a cell image; a block image (a real 200 × 400 grey PNG
  written with zlib, so the device decodes it); a `float: left` 80 × 80 image; `<hr/>`; on the first page, a link to
  the colophon, `<a href="ch2.xhtml#colophon">奥付</a>` (the target of Task 14's link tap; the golden records how the
  build lays it out). Enough prose for the rule checker's floors below: at least 1,200 characters in paragraphs of 200
  or more, so that at least 20 of spine 1's columns are broken by the layout (the stub sets 47 characters to the
  vertical twin's 760-px column: 752 px on the whole-em grid), and at least 10 sideways runs (Winterfell, 1998, 13,
  「」『』（）, ー, ……, ——).
- spine 2, the colophon: `<body style="writing-mode: horizontal-tb">`, `<h2 id="colophon">奥付</h2>` and two short
  paragraphs.

- [ ] **Step 1:** Write the generator and run it: `python -I test/epubs/make_test_vertical_text.py`.
- [ ] **Step 2: Write the rule checker** (`VerticalRulesTest.cpp`, parameterised over `test_vertical_text` and
  `test_writing_modes` under Automatic and `test_cjk_ja_soseki` and `test_cjk_ko_hyeon` under On, through
  `forEachPage`):
  - `VerticalRules.UprightCharactersAreDrawnInTheReadersFrameAndRunsSideways`: every vertical page rendered with the
    recorder at offsets (10, 20); each draw in Portrait is one piece (`nextColumnPiece` takes all its bytes) whose
    first codepoint `drawsUpright`, at x in 0-479 and y in 0-799; each draw in LandscapeCounterClockwise holds only
    codepoints that do not (marks and selectors excepted); no draw in another frame; the orientation is Portrait after
    each page. Each book draws at least 100 upright pieces and at least one sideways one; `test_vertical_text` and
    `test_cjk_ko_hyeon` (its Western punctuation and digits: 42 runs) at least 10 sideways. Counted from the goldens
    with UAX #50 on 2026-10-29: upright 1,822, 879 and 128, sideways 5 (its ー; no brackets), 42 and 3 (`ABC` and the
    two ー of ニャーニャー) in soseki, hyeon and test_writing_modes.
  - `VerticalRules.KinsokuHoldsAtEveryColumnEnd`: over the vertical pages' `PageLine`s in order, for each line whose
    first word starts less than one em in from its `PageLine` (16 px in the stub: a column the layout broke; every
    paragraph of these books indents its first line by an em or more, while a broken column starts at the whole-em
    grid's inset, `ParsedText::gridInset_`, 4 px in a 760-px zh/ja column (760 % 16 / 2) and 0 in Korean, or a few
    pixels further out where a pushed-in opening mark starts it) next to unspaced CJK, its first codepoint is not
    `CjkLineBreak::isNoLineStart` and the previous line's last is not `isNoLineEnd`. At least 20 columns checked in
    `test_vertical_text` and in `test_cjk_ja_soseki` (about 38 at 760 px); none required in `test_writing_modes`
    (about one of its columns breaks at 760 px) or `test_cjk_ko_hyeon` (its columns break between Hangul words, which
    are not unspaced CJK). `CjkRules.KinsokuHoldsAtEveryCjkLineBreak` reads the same edge from the horizontal dump.
  - `VerticalRules.EveryLineAndImageOfAVerticalPageIsTurned`: on every page of a `vertical=1` section every `PageLine`
    and table cell line `isVertical()`, every page image and cell image `isTurned()`; on every other page none.
  - `VerticalLayout.AColophonStaysHorizontalAndTurnsRightToLeft` (in `VerticalLayoutTest.cpp`): `test_vertical_text`
    under Automatic: spine 1 `flowContext()` is {vertical, rtl}, spine 2 {horizontal, rtl}; under Off both {false,
    false}.
- [ ] **Step 3:** `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`; `git status` shows exactly the two new
  goldens. Check them by hand and say so in the commit: in `_vertical`, spine 1 `vertical=1` with `turned=460` pages
  and `turned=1` images, spine 2 neither; the default golden has neither.
- [ ] **Step 4: Run** all of `EpubPipelineTest` without `UPDATE_GOLDENS`: pass.
- [ ] **Step 5: Commit** `test(epub): a vertical fixture, its goldens, and the vertical rule checker`.

### Task 10: The settings, and the request the reader builds with

**Files:**
- Modify: `src/CrossPointSettings.h` (beside `LINE_COMPRESSION` :274: `enum VERTICAL_TEXT { VERTICAL_TEXT_AUTOMATIC =
  0, VERTICAL_TEXT_ON = 1, VERTICAL_TEXT_OFF = 2, VERTICAL_TEXT_COUNT }`, "stored by number, in VerticalText's order
  (Epub/FlowContext.h)", and `enum PAGE_TURN_BUTTONS { PAGE_TURN_FOLLOW_BOOK = 0, PAGE_TURN_FIXED = 1 }`; beside
  `lineSpacing` :473: `uint8_t verticalText = VERTICAL_TEXT_AUTOMATIC;`, `uint8_t pageTurnButtons =
  PAGE_TURN_FOLLOW_BOOK;`)
- Modify: `src/SettingsList.h`: after Paragraph Alignment (:353-357) `SettingInfo::Enum(StrId::STR_VERTICAL_TEXT,
  &CrossPointSettings::verticalText, {STR_AUTOMATIC, STR_STATE_ON, STR_STATE_OFF}, "verticalText", STR_CAT_READER)`
  with its subcategory; before the Left button rows (:441) `SettingInfo::Enum(StrId::STR_PAGE_TURN_BUTTONS,
  &CrossPointSettings::pageTurnButtons, {STR_FOLLOW_BOOK, STR_FIXED}, "pageTurnButtons", STR_CAT_CONTROLS)`
- Modify: `lib/I18n/translations/english.yaml`: `STR_VERTICAL_TEXT: "Vertical Text"`, `STR_AUTOMATIC: "Automatic"`,
  `STR_PAGE_TURN_BUTTONS: "Page Turn Buttons"`, `STR_FOLLOW_BOOK: "Follow Book"`, `STR_FIXED: "Fixed"`
- Modify, as `4d0c4c526` did for `lineSpacingOverride`: `src/RecentBooksStore.{h,cpp}` (`int8_t
  verticalTextOverride = -1;`), `src/JsonSettingsIO.cpp` (save and load, clamped to -1..2), `src/activities/ActivityResult.h`
  (`MenuResult::verticalTextOverride`), `src/activities/reader/QuickOverridesActivity.{h,cpp}` and
  `EpubReaderMenuActivity.{h,cpp}` (a Default / Automatic / ON / OFF row, shown only when the new `bool
  verticalAllowed` argument is true; "Default (Automatic)" as line spacing labels its default),
  `src/activities/reader/EpubReaderActivity.{h,cpp}` (`bookVerticalTextOverride`, read in `onEnter` beside :764;
  `applyBookReaderOverrides` :3129/:3140 and `layoutOverridesUnchanged`; `openQuickOverrides`, `openReaderMenu`)
- Modify: `src/activities/reader/EpubReaderActivity.cpp`: `bookVerticalAllowed_` from
  `TypographyProfile::forLanguage(epub->getLanguage()).verticalAllowed` in `onEnter`; `getEffectiveVerticalText()`;
  `makeSectionBuildParams` (:1402) sets `p.verticalText`; `drawCurrentPageToBuffer` (:7290-7410) sets it from
  `currentBook.verticalTextOverride` and the book's profile through the same `FlowContext::requestFor`;
  `static_assert`s that `VERTICAL_TEXT_*` equal `VerticalText`'s values

**Interfaces:**
- Consumes: `FlowContext::requestFor`, `Section::BuildParams::verticalText`.
- Produces: `VerticalText EpubReaderActivity::getEffectiveVerticalText() const` (`FlowContext::requestFor(setting,
  bookVerticalTextOverride, bookVerticalAllowed_)`); `SETTINGS.pageTurnButtons` for Task 11.

- [ ] **Step 1:** Make the changes.
- [ ] **Step 2: Build** `pio run -e default`: compiles; the strings generate.
- [ ] **Step 3: Run** the whole host suite (`SettingsGrouping`, `SettingInfoOptions` and the JSON tests read the
  settings list): pass.
- [ ] **Step 4: Commit** `feat(reader): Vertical Text, its per-book override, and Page Turn Buttons`.

### Task 11: Page progression in the reader

**Files:**
- Modify: `src/components/TapZones.h` (after `zoneFor`), `src/components/SliderGeometry.h`
- Modify: `src/activities/reader/ReaderUtils.h` (`detectTouchPageTurn` :135-176 gains `const bool rightToLeft = false`
  and maps through `TapZones::turnFor`/`turnForSwipe`; `LineReaderActivity` and `XtcReaderActivity` keep the default)
- Modify: `src/activities/reader/EpubReaderActivity.{h,cpp}`: `std::atomic<bool> pageProgressionRtl_{false}`, stored in
  `renderContents` (:5682) from `section->flowContext().pageProgressionRtl` before the page draw; in `loop()` (:1055-1091)
  the Left and Right buttons' built-in short and long actions swap when it is set and `SETTINGS.pageTurnButtons ==
  PAGE_TURN_FOLLOW_BOOK` (PageBack/PageForward and assigned actions unchanged); `detectTouchPageTurn(renderer,
  mappedInput, pageProgressionRtl_)` (:1116); `renderStatusBar` (:6699; its `GUI.drawStatusBar` call :6728) passes it
  as `fillFromRight`; the percent picker gets it as `reversed`; `appendDebugState` (:6584, under `SERIAL_TEST_API`) adds `flow=vertical-rl|horizontal` and
  `progression=rtl|ltr` from `section->flowContext()`
- Modify: `src/components/themes/BaseTheme.{h,cpp}` (`drawStatusBar` :186/:1110 gains `const bool fillFromRight =
  false`: each bar fills from `barMarginLeft + progressBarMaxWidth - barWidth`)
- Modify: `src/activities/SliderPickerActivity.{h,cpp}` (`Config::reversed`: the fill and knob from the right, Left
  steps +1 and Right -1, the hints `+` and `-` swapped, touch through `mirroredX`),
  `src/activities/reader/EpubReaderPercentSelectionActivity.h` (a `bool reversed` argument)
- Test: `test/tap_zones/TapZonesTest.cpp`, `test/slider_geometry/SliderGeometryTest.cpp`

**Interfaces:**
```cpp
// TapZones.h. What a tap or a horizontal swipe does to the page (spec 5.5): the Left column turns back and the Right
// one forward, swapped by the reader's inverted taps and again for pages that turn right to left; a swipe towards the
// left turns forward, towards the right back, mirrored the same way.
enum class Turn : uint8_t { None, Back, Forward };
Turn turnFor(Zone zone, bool inverted, bool rightToLeft);
Turn turnForSwipe(bool towardsLeft, bool rightToLeft);
// SliderGeometry.h. A bar that runs right to left: a finger at px means what px mirrored about the bar means, and the
// fill starts at the track's right end.
int mirroredX(int px, int barX, int barWidth);                                    // 2 * barX + barWidth - 1 - px
int reversedFillX(int value, int barX, int barWidth, int minValue, int maxValue);  // trackX + track - fill
```

- [ ] **Step 1: Write the failing tests:** `TapZones.APageTurnFollowsTheBooksDirection` (Left Back and Right Forward;
  `inverted` swaps; `rightToLeft` swaps; both restore; Centre, Top and Bottom are None), `TapZones.ASwipeFollowsIt`
  (towards left Forward, towards right Back, mirrored when right to left);
  `SliderGeometry.AReversedBarMirrorsTheTouchAndTheFill` (bar x 60, width 360, track 356 from 62: `valueForX(mirroredX(px))`
  is 0 at the track's right end (px 417) and 100 at its left end (px 62); value 25 fills 89 px from x 329).
- [ ] **Step 2: Run; they fail to compile.**
  `cmake --build build/test --target TapZonesTest SliderGeometryTest -j 8`
- [ ] **Step 3: Implement**, the pure functions first.
- [ ] **Step 4: Run** both suites: pass. `pio run -e default`: compiles.
- [ ] **Step 5: Commit** `feat(reader): pages, taps, swipes, the progress bar and the percent slider follow the book's
  direction`.

### Task 12: The reader around a turned page

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.{h,cpp}`: `getOrientedViewableTRBL(Orientation, int*, int*, int*, int*) const`
  beside :544 (the existing one calls it with `getOrientation()`); `tapToLogical(nx, ny, ...)` (:3043) maps in
  `getHeldOrientation()`, its comment (:410-420) saying so and why (a vertical page holds the turned frame for its
  whole draw); its inverse `logicalToTap(x, y, ...)` (:3054, comment :427-428) likewise, so the two stay inverses
- Modify: `src/MappedInputManager.cpp:381-383` (`wasSwipe()` in the held orientation)
- Modify: `src/SerialTestApi.cpp` `injectContact` (:772-803, `CMD:TOUCH`/`CMD:SWIPE`): the on-screen check at :782-783
  against `getScreenWidth/Height(getHeldOrientation())`. Its live reads and `logicalToTap` would map a contact sent
  while a vertical page draws through the turned frame, which is what Task 14 Step 11's tap right behind a turn sends
  (dev builds only, `SERIAL_TEST_API`)
- Modify: `src/activities/reader/ReaderUtils.h` (`detectTouchPageTurn`, `isTouchMenuTap`: width and height in the held
  orientation)
- Modify: `src/activities/reader/EpubReaderActivity.cpp`: `computeRenderLayout` (:4058) in the held orientation;
  `publishPageLinkTargets` (:6989): on a vertical page each word box is taken at `turnedframe::contentOffsets(...)` and
  recorded as `turnedframe::toReader(...)`; `openDictionary` (:2645): on a vertical section returns with a `LOG_INF`
  (BTN_DICTIONARY lands here too); `openReaderMenu` passes `wordLookupAvailable = !section->isVertical()`;
  `getImageOnlyPageYOffset` (:596): 0 for a vertical page
- Modify: `src/activities/reader/EpubReaderMenuActivity.{h,cpp}` (`bool wordLookupAvailable`, the constructor's last
  argument, after Task 10's: the Dictionary row at :146 only then)

- [ ] **Step 1:** Make the changes.
- [ ] **Step 2: Check** `grep -nE "getOrientation\(\)|getScreen(Width|Height)\(\)" src/MappedInputManager.cpp
  src/activities/reader/ReaderUtils.h` finds nothing (on master `39fba77f5` it finds the five live reads these changes
  replace: `MappedInputManager.cpp:382`, `ReaderUtils.h:159-160` and `:185-186`), `injectContact` in
  `src/SerialTestApi.cpp` reads none (on master :782-783, and `logicalToTap`'s live orientation), and
  `computeRenderLayout` reads no live orientation.
- [ ] **Step 3: Build** `pio run -e default` (C3: `CP_TOUCH_UI` 0 compiles the touch paths out; CI's S3 builds compile
  them) and run the whole host suite: pass.
- [ ] **Step 4: Commit** `fix(reader): geometry off the render task reads the held frame; links, images and the
  dictionary on a vertical page`.

### Task 13: Documents

**Files:**
- Modify: `docs/file-formats.md` (`section.bin`'s header table :231-253: `verticalText` after `imageRendering`, the
  vertical status bit; under the file layout, the page record's `u16` block extent, a line's column bit (flags bit 2),
  an image's flags byte; the property hash paragraph :216: the request when not Off)
- Modify: `docs/future_work/writing-modes.md` (as on master `39fba77f5`, after #573: the "Status: open items as of"
  date; the roadmap's step 3 row "shipped (#PR) ([plan](...))", and step 1's row, whose "FlowContext and page
  progression" step 3 now builds (ruling 1: step 1 reuses them); the "**Next step.**" paragraph under the roadmap,
  which names step 3 today, says it has shipped and names the next step as the maintainer rules (asked at the
  pre-flight, 2026-10-29; until answered: "not chosen yet: step 4 continues the vertical work, step 1 starts right to
  left"; not the "**Next step.**" that closes "Complex shaping: Arabic and the Indic scripts", which stays); "Left from
  the spike, for step 3" removed; "Parsed in step 0, read later": `writing-mode` read, `text-orientation` now step 4 only,
  and the OPF `page-progression-direction` row first read by step 3 (page turns; step 1 reuses it); what step 4
  inherits: the Tu placement in `drawsUpright` (with A3a in vertical: a pushed-in 、。 keeps its horizontal ink, see
  Task 14 Step 7), the dictionary through the frame, `text-orientation`/`text-combine-upright`, drop caps, vertical-lr,
  lone images centred along the block axis)
- Modify: `docs/future_work/cjk.md:386` (Line spacing's "Left for later": the column pitch is done, it is the line
  pitch in the turned frame)
- Modify: `USER_GUIDE.md` (3.7.2 Reader: Vertical Text; 3.7.3 Controls: Page Turn Buttons; 4 Reading Mode: a vertical
  book's pages turn right to left, and the dictionary is not available in vertical text yet; 5.3 and 5.4, the tap
  zones and page swipes: mirrored in a book whose pages turn right to left, the gesture bindings not)
- Modify: `docs/serial-test-api.md` (the STATE table: `flow`, `progression`; "Touch: `TOUCH` and `SWIPE`": coordinates
  are in the orientation the device is held in, which a vertical page's turned frame does not change)

- [ ] **Step 1:** Make the edits.
- [ ] **Step 2: Commit** `docs: vertical text in the guide, the cache formats and the roadmap`.

### Task 14: Firmware builds, flash and RAM, the X4, the X3 and a touch board

The acceptance book is `C:/_development/witchhunt-reader/.idea/annas-arch-91dbd462043f.epub` (1,663,588 B), and the
X3 has it in `/test`. The English control the spec names (A Game of Thrones) is not on hand; until the maintainer
supplies it, the control is an English book already on the X4's card whose page counts must not move:
`/test/The Anarchy - Dalrymple, William.epub` (40 spine items). Neither book goes into the repository. The X4 is on
COM5; `tools/devctl.py` and `tools/serial_transfer.py` need pyserial (the PlatformIO penv's python has it). Uploads to
the X3 and the touch board go through a no-reset helper in the scratchpad: a subclass of `serial_transfer.py`'s
`CrossPointSerial` whose `open()` opens `serial.Serial(port, 115200, timeout=3)` with the default lines (as devctl
does) and calls `_wait_ready(25)`, and whose `close()` sets RTS low, then DTR low, then closes (the tool's own open
deasserts both, which resets an ESP32-C3).

- [ ] **Step 1: Sizes.** Build `default` and `gh_release` on the branch, and `default` on master in a sibling worktree,
  one at a time: the master the PR merges into, `39fba77f5` or the newest when this task runs (`git fetch origin`,
  `git worktree add C:/tmp/wt-wm3-base origin/master`, then `git submodule update --init freeink-sdk` there). Record
  pio's `Flash:` and `RAM:` lines for both, and from `.pio/build/default/firmware.map` the sizes of
  `VerticalOrientation.cpp.o`, `VerticalDraw.cpp.o` and `FlowContext.cpp.o` (`.text`, `.rodata`). State the deltas,
  and the master commit they are against, in the PR. Keep the branch's and the base's
  `.pio/build/default/firmware.factory.bin`: the X3 is flashed with them (Step 10).
- [ ] **Step 2: The control under master.** With the base build of Step 1 on the X4 (`pio run -e default -t upload
  --upload-port COM5` in `C:/tmp/wt-wm3-base`), open the control's spine items 5, 20 and 40 at page 1
  (`cmd:ACT:READER:/test/The Anarchy - Dalrymple, William.epub@5:1 idle:120000 state`) and record their page counts.
- [ ] **Step 3: Back up and flash.** `cmd:GET:*` to a file in the scratchpad; `pio run -e default -t upload
  --upload-port COM5`. Upload `test_vertical_text.epub` and the acceptance book to `/test`. Reader font NotoSansJP 14
  pt (`cmd:SET:fontFamily=NotoSansJP`; if `GET` lists no such family, install it with the Font Manager first).
- [ ] **Step 4: The acceptance book under Automatic.** Spine items 4 (contents), 9 (prologue), 22 (the image) and 39
  (the longest) at page 1: `state check:flow=vertical-rl check:progression=rtl`, a screenshot each; from the log the
  page counts and build times (`createSectionFile spine=... parse done`) and the build arena's high-water mark
  (`createSectionFile spine=... arena: cap=... highWater=`); after five page turns each, the pre-render times and
  costs (`Pre-rendered page ... in ...ms (heap ... cost ...)`) and `heap` from STATE. The image on spine 22 is drawn
  upright, whole, and the log has no `Render bounds rejected`. Then `cmd:SET:verticalText=2` (Off) and the same four
  items: the horizontal page counts (spec 10.1's v/h columns). Restore Automatic.
- [ ] **Step 5: Turning.** In spine 9: `cmd:BTN:LEFT` turns forward and `cmd:BTN:RIGHT` back (Follow Book);
  `cmd:BTN:DOWN` forward; with `cmd:SET:pageTurnButtons=1` (Fixed) Right turns forward. With a bottom progress bar on, a
  screenshot shows it filling from the right. The per-book override: in the reader menu (`cmd:BTN:CONFIRM idle`,
  `select:Vertical Text.*`) set OFF; back in the book `state check:flow=horizontal check:progression=ltr`; then
  Default again, and `check:flow=vertical-rl`.
- [ ] **Step 6: The dictionary guard.** In a vertical page the reader menu has no Dictionary row (`select:` finds none)
  and `cmd:BTN:POWER` (bound to the dictionary on this X4; check `cmd:GET:btnShortPower`) leaves the reader on top.
- [ ] **Step 7: The fixture.** `test_vertical_text.epub` spine 1 and 2: screenshots (columns, ruby on the right, the
  side line on the right, sideways Latin, upright 、。 unplaced, the table as columns, the images upright); spine 2
  horizontal with `progression=rtl`. Note in the PR, without fixing it, where a 、 or 。 that A3b pushed in (a
  narrowed box at a column's end, or before a closing bracket) keeps its horizontal ink: drawn upright it can reach
  half an em past its box, into the next character or past the column's end (ruling 2; A3a in vertical is step 4's,
  spec 5.4).
- [ ] **Step 8: The control under the branch.** The same three spine items as Step 2: identical page counts. Read for
  ten minutes in a vertical book with Background-B building: the log has no `Parameters do not match`. If the sleep
  screen shows the current page (`cmd:GET:sleepScreen`), `cmd:SLEEP` in a vertical page and wake the X4 by hand: the
  sleep screen is the page in columns, and the log has no `SLP` `section cache not found ... rebuilding`. `CMD:SLEEP`
  switches USB off until a button is pressed, so this needs the maintainer at the X4 (asked at the pre-flight); without
  them the sleep check is skipped, Step 9 still runs, and the PR says so.
- [ ] **Step 9: Restore the X4:** every changed setting from the backup, verified with `GET`; flash master back unless
  the maintainer says otherwise.
- [ ] **Step 10: The X3** (spec 10.1 again, on the branch; not a gate). Flash Step 1's branch
  `firmware.factory.bin` to the X3 on COM6 with esptool: `PYTHONIOENCODING=utf-8 /c/pio/penv/Scripts/python.exe -m
  esptool --chip esp32c3 --port COM6 write-flash 0x0 .pio/build/default/firmware.factory.bin` (esptool 5.3 in the
  `C:\pio` penv). Then drive it with devctl (`--port COM6`). Nothing else resets the X3: no RTS pulse, no
  `CMD:REBOOT`, no `CMD:SLEEP`. If the X3 is silent on USB after the flash or at any point in the run, stop this step
  and report it (a silent X3 needs the maintainer's power cycle).
  - `cmd:GET:*` to a backup file; portrait (`cmd:SET:orientation=0`), NotoSansJP 14 pt, text anti-aliasing on as the
    spike ran (`cmd:SET:textAntiAliasing=1`), and `cmd:SET:sleepTimeoutMinutes=30` for the session (an idle X3 sleeps
    and drops off USB).
  - `cmd:FS:LS:/test` shows the acceptance book at 1,663,588 B. Upload `test_vertical_text.epub`: `cmd:ACT:USB_TRANSFER`,
    the no-reset helper's `upload()`, then `cmd:BTN:BACK`. Leaving the screen restarts the X3 to Home by design; if it
    stays silent on USB, stop here and report it (the maintainer power-cycles it).
  - The acceptance book's spine items 9, 39, 22 and 4 under Automatic, then under Off, exactly as Step 4 on the X4:
    pages, build ms, pre-render ms and cost, arena high-water, `heap` after five turns, a screenshot each, no `Render
    bounds rejected`. Set against spec 10.1's table: the spike's vertical column against the branch's.
  - `test_vertical_text.epub` spine 1: screenshots, as Step 7.
  - Restore every changed setting from the backup and verify with `GET`. Flash master back the same way (Step 1's
    base `firmware.factory.bin` from `C:/tmp/wt-wm3-base`) unless the maintainer says otherwise, as Step 9 does on
    the X4.
- [ ] **Step 11: The touch board** (one session; the maintainer attaches an X4 Pro or a T5S3). Build its env
  (`-e x4pro` or `-e lilygo_t5s3`, a cold S3 build of 15-30 min, in the background, the only pio run then) and flash it
  (`-t upload --upload-port <port>`), or hand the maintainer the `firmware.bin` if they would rather flash it. Back up
  with `cmd:GET:*`; upload `test_vertical_text.epub` and the acceptance book through the USB Transfer screen and the
  no-reset helper. Coordinates are the portrait screen's (`cmd:TOUCH`/`cmd:SWIPE`, `docs/serial-test-api.md`): W × H
  480 × 800 on the X4 Pro, 540 × 960 on the T5S3. In the acceptance book's spine 9 at page 2 under Automatic, each
  followed by `idle:5000:SCREEN state`:
  - taps (`cmd:SET:touchReaderCtl=1`): `cmd:TOUCH:<W/6>,<H/2>` turns forward, `cmd:TOUCH:<5W/6>,<H/2>` back;
  - inverted taps (`touchReaderCtl=3`): the left third turns back;
  - swipes (`touchReaderCtl=2`): `cmd:SWIPE:<W/4>,<H/2>,<3W/4>,<H/2>` (towards the right) turns forward, the reverse
    swipe back;
  - taps right behind a turn: `cmd:BTN:DOWN` then at once `cmd:TOUCH:<W/6>,<H/2>`, with no idle between: the page has
    moved forward by two (Review Focus 1);
  - a link target: `test_vertical_text.epub` spine 1 page 1, `shot:` to find 奥付 on the page, `cmd:TOUCH` on it:
    `check:spine=2/.*`;
  - the control: `cmd:SET:verticalText=2` (Off) in spine 9: the left third turns back, as in every horizontal book.

  Restore every changed setting from the backup and verify with `GET`; put the board's previous firmware back if the
  maintainer asks.
- [ ] **Step 12:** The PR (2-4 KB): what changed, the rulings, the review focus, the sizes, the X4 and X3 tables (pages
  v/h, build ms, pre-render ms, heap after a turn), the touch session's results and the screenshots. The S3 env not
  built in Step 11 is left to CI.
