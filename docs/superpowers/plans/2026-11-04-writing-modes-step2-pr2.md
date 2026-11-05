# Writing modes step 2, PR 2: TrueType faces that shape Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every installed TrueType style becomes a face the shaper reads in place, and the renderer can draw and measure
a shaped run. The pieces: GDEF in SD mode, upstream's `CPac` written at install, keys from 0x110000 as glyph IDs, one
`ot::Face` per style re-pointed when its mapping moves, and a glyph-run draw and measure over compact records. All of
it is host-tested over a Noto Sans Devanagari subset. Nothing lays out shaped text yet (PR 3).

**Architecture:**
- **The engine** (`lib/OtShaper`) gains three things:
  - deviation 12's `Face::rebase`;
  - deviation 4's `CPac` writer, our C++ port of the converter's `lookup_filters()`;
  - carried item 2: the planner's scratch freed before the filters are computed.
- **The TrueType engine** (`lib/EpdFont/ttf`):
  - A style block holds the whole file (flash mode) or its tables with GDEF (SD mode), then the `CPac`. Its room is
    counted at the probe, and the install writes it from the mapped tables.
  - `kRenderVersion` 4 and glyph-store format 5 make every family install again.
  - A key from 0x110000 is a glyph ID.
  - `ShapingFace` (`lib/EpdFont`) is the narrow seam PR 3's shaping layer reads, through `GlyphProvider`,
    `EpdFontFamily` and `GfxRenderer`.
- **The renderer** (`lib/GfxRenderer`):
  - `GlyphRun.h` holds the record format and the pen walk.
  - `drawGlyphRun` and `glyphRunWidth` draw and measure through the per-glyph blitters `drawText` and
    `drawTextAtScale` use, so every orientation and gray pass a line draw uses.
  - The page scan records glyph IDs for the glyph-ID prefetch.
- **A dev-only serial command** measures measure-first item 4 on the X3. The one reader-visible change is the
  reinstall.

**Tech Stack:**
- C++20 (`-std=gnu++2a`; `-fno-exceptions` on the device).
- GoogleTest host tests (CMake/Ninja, MSYS2 UCRT64 g++ 16.2.0).
- Python 3.14 with fontTools 4.63.0 and uharfbuzz 0.56.2 (HarfBuzz 14.5.0), from `C:/tmp/wm2-measure/pydeps`, for
  the fixture.
- PlatformIO for the firmware.

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step2-design.md` (approved 2026-11-02), with
`docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`.
- **What PR 2 owns:** "The pieces", item 2; deviations 3, 4 (the install's half), 5 (the key and the record), 10 and
  12; the renderer bullets of "Drawing"; the install bullets of "Fonts"; and measure-first item 4.
- **PR 3's, not here:**
  - the shaping layer's face and glued-group interface, and its modules (Indic, IndicReorder's use);
  - the TextBlock's bit 2, `ParsedText`, the section version, `kStatusShapingDegraded` and the in-place gate;
  - the Font Manager's families, and the on-device draw of a shaped page.
- **The seams PR 3 uses,** each tested here on its own:
  - the face: `GfxRenderer::shapingFace`, `ShapingFace::current` and `TtfFontSet::releaseShapingFaces`;
  - the records: `GlyphRecord` and `glyph_run::encode`;
  - the draw: `GfxRenderer::drawGlyphRun`, `glyphRunWidth` and `prefetchGlyphRun`.

**Carried from PR 1's final review:**
1. **A moved `ot::Plan` keeps flag pointers into the plan it left** (`FixedVector`'s move copies `fits_`). This goes
   to **PR 3**, which is the first to keep plans for a build. PR 2 holds none in the firmware: its dev command builds
   one on the heap and frees it in place.
   - The flags only matter while a list grows, and `build()`/`read()` bind them afresh.
   - So PR 3 holds each plan by `std::unique_ptr` (never moved), or adds a test that a moved plan shapes, and rebuilds
     to the same lists.
2. **The planner's scratch released before `finish()`:** here, in **Task 2**.
   - PR 2 makes the `CPac` route the normal one. The computed route becomes the fallback for a block whose `CPac`
     fails validation, and this lowers that fallback's peak.
   - PR 3 then measures the X3's floor with it in.
3. **`IndicReorder` grows a `std::string`:** **PR 3**, its first caller, and **nothrow** (deviation 9 extended).
   - Under `-fno-exceptions` a failed growth aborts, against the design's "Nothrow everywhere".
   - The output goes into a buffer of twice the token's bytes, from the build's arena. A two-part vowel at most doubles
     its bytes (ো becomes ে ... া).
4. **`ottest::decodeUtf8` reads past a truncated sequence:** here, in **Task 1**, because PR 2's tests decode words
   through it.

## Global Constraints

From the design, verbatim:
- Every Latin, CJK and Hebrew golden stays byte-identical. Only step 2's fixtures change, and in PR 4 any Arabic line
  of step 1's fixtures (`test_rtl_markup` has none).
- Nothrow everywhere: a refused block or a failed run lays the token out unshaped and latches the status bit.
- No resident memory: the shaper's buffer, faces and plans live during a build only, and a page draw allocates nothing
  new for shaped words.
- Measured and reported on the X3 and the X4: flash on `default` and `gh_release`; static RAM; Background-B's lowest
  heap during an Indic build; the reading heap after a page turn on an Indic page; the loop task's stack high-water;
  build and install times.
- Attribution: crosspoint-reader #3787 (MIT) at `dd45ba61`, naming `0fbd1575` for the shaper's files; HarfBuzz's MIT
  notice in the ported files and the licences page; the Noto fonts' OFL.
- The device runs flash the web bundle as well (`web-<hash>.bin` to the card's root).
- (Deviation 5) Glyph IDs reach the TrueType engine as keys 0x110000 + gid, past Unicode in the store's 21-bit code
  point field.
- (Deviation 12) The re-point moves "every `Table` view and the `CPac` filter records by the difference, without the
  cmap search or `initFilters`' validation". A plan points at its face's filter records, not into the font, so it
  stays valid across the move.
- (The record) "`u16 glyph`, an advance in 12.4 pixels with one bit saying an offset follows, then `i8 dx`, `i8 dy`
  when it does. 4 bytes a glyph, 6 with an offset."
- (Drawing) No kerning, ligatures or tracking inside a run. Every rotation and gray pass the line draw uses.

This plan's own:
- **What PR 2 measures:** flash, static RAM and install times, and item 4's plan and word timings. The build heap,
  the stack and the build times need PR 3's builds.
- **Branches.** This plan is on `docs/writing-modes-step2-pr2`, from master `087e10230`.
  - The code goes on `feat/writing-modes-step2-pr2`, cut from it, so the PR carries the plan as #603 carried PR 1's.
  - The PR opens against master.
- **Upstream files keep their bytes.** A changed line in `lib/OtShaper` carries `// witchhunt: deviation N`.
  - Our files say so in their first comment.
  - Comments say what and why, never history.
- **Credit.** Nothing here is mainly #3787's code, so no commit has a co-author trailer, and no commit has a Claude
  trailer.
  - The `CPac` writer and the fixture script are our ports of #3787's Python. Each names its source in a header, in
    the repo's form (`// Ported from crosspoint-reader PR #3787 ("feat: add Indic language support without
    HarfBuzz", SSafayet / @SSafayet). ...`), and in its commit body.
- **Nothrow:**
  - no bare `new`, no exceptions, no `try`; allocations through `makeUniqueNoThrow` (`lib/Memory/Memory.h`);
  - every new allocation's comment gives its size and why it is not on the stack;
  - stack locals stay under 256 B;
  - never declare a local `std::string word(...)` with parentheses (the Arduino `word()` macro breaks the firmware
    build only).
- **What stays resident:** 8 B per loaded TrueType style (its file size and an empty face pointer), and nothing else.
  `ScaledGlyphEntry` stays 16 B on the C3.
- **Host tests, from Git Bash only.** After `git submodule update --init freeink-sdk`, configure once:
  `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DGIT_EXECUTABLE="C:/Program Files/Git/cmd/git.exe"
  -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src
  -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-wm1/build/test/_deps/arduinojson-src`.
  - Step 3's ArduinoJson copy (`C:/tmp/wt-serial-api`) is gone; `wt-wm1`'s is the same release.
  - Build: `cmake --build build/test --target <T> -j 8`. Run: `ctest --test-dir build/test -R <pattern>
    --output-on-failure`, with no `-j` beside other worktrees.
  - The whole suite: `cmake --build build/test -j 8 -- -k 0`, then `ctest --test-dir build/test`.
  - The only expected failures are step 3's five Windows-only `EpubPipelineTest` cases: FirstChapter ×2,
    LongChapterPageTables ×2, KeptReadingState.
  - `git ls-files --eol test/epub_pipeline/goldens | grep w/crlf` must print nothing.
- **Firmware builds:**
  - First check that no other pio or scons process runs (`tasklist | grep -i -E "pio|scons"`); never kill one.
  - `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
    `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`, one at a time, in the background (a cold build passes
    600 s).
  - Record the `Flash:` and `RAM:` lines' bytes. `x4pro` and `lilygo_t5s3` are left to CI.
- **Formatting:** clang-format 21, `/c/tmp/cf21/bin/clang-format-21.exe -style=file`. Run `-i` on every touched
  `.c`/`.h`/`.cpp`, then `--dry-run --Werror`, which must print nothing.
- **Files** are written with the Write and Edit tools, never a shell heredoc. Before each commit, check that no touched
  text file contains a NUL byte. Binary fixtures come only from the fixture script.
- **Devices** (Task 12):
  - The X4 is on COM5. The X3 is on COM6 (step 3's port; confirm it before use).
  - Rules: `C:/Users/jens/.claude/projects/c---development-witchhunt-reader/memory/serial-test-commands.md`.
    - Flash with esptool `write-flash 0x0 .pio/build/default/firmware.factory.bin`.
    - Drive the devices with `tools/devctl.py`, with pyserial and Pillow on its path.
    - On the X3: no RTS pulse, no `CMD:REBOOT`, no `CMD:SLEEP`. Files reach it through its USB Transfer screen over
      a no-reset serial open.
  - Before flashing, check that no other session uses the device or the build (shared devices).
  - After every flash, copy `.pio/build/default/web-<hash>.bin` to the card's root. A cleanup never removes
    `/web-*.bin`.

## Review Focus

1. **A reader's own TrueType file with a hostile or enormous lookup list:** a LookupList declaring 65,535 lookups,
   offsets past the table, or an Extension lookup mixing subtable types.
   - Expected: discovery reads a bounded number of bytes.
   - Expected: the install writes no more than it reserved.
   - Expected: the face then shapes with computed filters or none, never wrong ones.
   - Pinned in Task 3 (`AnExtensionLookupMixingTypesHasNoSubtables`), Task 5 (`AHostileLookupListIsCountedBounded`)
     and Task 8 (`ABlockWhoseCpacFailsValidationShapesWithComputedFilters`).
2. **A power cut while a style's `CPac` is being written,** after its tables were copied. Expected: the next boot
   installs again, and does not keep that block with a torn `CPac`. Pinned in Task 6
   (`AKeptBlockWithATornCpacIsWrittenAgain`).
3. **A family that fit before the update but no longer fits whole with its `CPac`** (a large file just under a page
   multiple). Expected: it installs in SD mode, or is refused before anything is erased, never half-installed. Pinned
   in Task 6 (`AFamilyThatFitsWholeOnlyWithoutItsCpacGoesToTheCard`).
4. **A stored glyph run drawn in a font that draws no glyph keys** (a `.cpfont` or built-in face under the ID).
   Expected: nothing is drawn, no SD read, no U+FFFD, and the provider is not asked. Pinned in Task 7
   (`AFontThatDrawsNoGlyphKeysIsNotAsked`).
5. **A glyph run at a scale** (a word at an inline size, a superscript, a synthesised body size). Expected: the scaled
   mask cache holds the run's glyphs, and never serves a code point's mask for a glyph key with the same low 16 bits.
   Pinned in Task 10 (`AGlyphKeyHasACacheKeyOfItsOwn`) and in Task 10's native-size canvas test.

---

## The engine

### Task 1: Deviation 12, `ot::Face::rebase`; the test decoder stops at a truncated sequence

**Files:**
- Modify: `lib/OtShaper/OtFace.h:98-148` (class `Face`: one public member), `lib/OtShaper/OtFace.cpp` (after
  `Face::init`, `:127-162`)
- Modify: `test/ot_shaper_port/OtShaperTestHelpers.h:22-35` (`decodeUtf8`)
- Test: `test/ot_shaper_port/OtShaperPortTest.cpp` (suites `OtFaceRebase`, `OtShaperHelpers`)

**Interfaces:**
- Produces, in `class ot::Face`:
  ```cpp
  // witchhunt: deviation 12 - the tables moved: the same bytes, mapped at `to` where init() read them at `from`
  // (a flash-mode style block mapped again). Every view and the CPac filter records move by `to - from`; no cmap
  // search, no CPac validation. Every view must lie in the block mapped at `from`.
  void rebase(const uint8_t* from, const uint8_t* to);
  ```
- `filters(t)` still returns `&filters_[t]`, so a plan built before the move reads the moved records.

- [ ] **Step 1: The branch.** In `C:/tmp/wt-wm2`, after this plan's commit:
  `git switch -c feat/writing-modes-step2-pr2 docs/writing-modes-step2-pr2`. Init the submodule and configure the
  host tests (Global Constraints).
- [ ] **Step 2: Write the failing tests.**
  - `OtFaceRebase.AMovedFaceShapesAsAFreshOne`, for each of the four `kShapingFixtures`:
    - copy the font into two vectors, A and B;
    - `init` a face over A's tables (CPpl dropped), and build each word's plan on it;
    - `face.rebase(A.data(), B.data())`, then `std::fill(A, 0xA5)`;
    - every word's `shapeAsStored` equals `ExpectedShaping.h`'s;
    - then the same with `CPac` dropped (the plan's computed filters).
  - `OtFaceRebase.TheFilterRecordsMoveButTheirAddressStays`:
    - after the rebase, `face.filters(ot::GSUB)` is the pointer it was before;
    - its `lookupDigests` is B's address of the bytes it pointed at in A.
  - `OtShaperHelpers.DecodeUtf8StopsAtATruncatedSequence`: `const char s[] = "\xE0\xA4\0A";`.
    `ottest::decodeUtf8(s, out, 4)` returns 1, and the code point count of `"\xF0\x9F"` is 1.
- [ ] **Step 3: Run them.** `cmake --build build/test --target OtShaperPortTest -j 8`. Expected: a compile error,
  since `rebase` is not a member. Once it builds, the decoder test fails: it counts 2.
- [ ] **Step 4: Implement.**
  - `rebase` maps each non-empty view `t` to `Table(to + (t.data() - from), t.length())`: `cmapSubtable_`, `hmtx_`,
    the three GDEF views, `gsub_`, `gpos_`, both `filters_` entries (their two pointers and `subtableStarts`) and
    `compiledPlans_`.
  - `decodeUtf8` advances by the bytes it read (`k`), not `n`. Mark it `// witchhunt: carried item 4`.
- [ ] **Step 5: Run** `ctest --test-dir build/test -R "OtFaceRebase|OtShaperHelpers|OtShaper|OtPlan|FixedVector"`.
  Expected: all pass.
- [ ] **Step 6: Format check, then commit** `feat(shaping): a face re-pointed when its mapping moves (deviation 12)`.

### Task 2: The planner's scratch goes before the lookup filters (carried item 2)

**Files:**
- Modify: `lib/OtShaper/OtPlanBuild.cpp:298-339` (`PlanBuilder::build`), using `scratch_`, `features_` and
  `bindScratch` (`:502-526`)
- Test: `test/ot_shaper_port/OtShaperAllocTest.cpp`, its harness (`:24-80`) and one new test

**Interfaces:**
- No API change. On the computed route, `Plan::build` allocates as PR 1 left it: scratch, GSUB lists, GPOS lists,
  GSUB filters, GPOS filters. The scratch is now freed before the first filter block.

- [ ] **Step 1: The harness and the failing test.**
  - The harness gains `peakNothrowBytes` (the largest `liveNothrowBytes` while armed) and `sizes[8]`, the size of each
    armed nothrow call by index.
  - `OtShaperAlloc.TheComputedRouteFreesTheScratchBeforeTheFilters`, for each fixture with `CPac` and `CPpl` dropped,
    armed across `Plan::build`:
    - 5 nothrow calls;
    - `peakNothrowBytes == sizes[1] + sizes[2] + std::max(sizes[0], sizes[3] + sizes[4])`.
  - The same test with `CPac` kept (3 calls): `peakNothrowBytes == sizes[0] + sizes[1] + sizes[2]`.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R OtShaperAlloc`. Expected: the computed case fails, with the peak
  the sum of all five.
- [ ] **Step 3: Implement.** After the `wouldStage` loop (`gsubStage()` reads `features_`), and before
  `plan_.finish(face_)`:
  - unbind `features_`, `b.infos` and both `b.pauses`;
  - `scratch_.reset()`;
  - mark the lines `// witchhunt: deviation 1 - freed before finish() computes the filters`.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "OtShaperAlloc|OtShaper|OtPlan"`. Expected: all pass, PR 1's
  refusal tests included.
- [ ] **Step 5: Format check, then commit** `perf(shaping): the planner's scratch goes before the lookup filters
  (deviation 1)`.

### Task 3: Deviation 4, the `CPac` table written from a face, in pieces

**Files:**
- Create: `lib/OtShaper/OtFilterWriter.h`, `lib/OtShaper/OtFilterWriter.cpp` (ours; `witchhunt: deviation 4` in the
  header comment)
- Modify: `lib/OtShaper/OtLayout.h:36-42`, `lib/OtShaper/OtLayout.cpp:247-266` (one function beside
  `appendSubtableDigests`)
- Test: `test/ot_shaper_port/OtShaperPortTest.cpp` (suite `OtFilterWriter`)

**Interfaces:**
- Produces, in `OtLayout.h`, marked `// witchhunt: deviation 4`:
  ```cpp
  // The glyphs subtable `subtable` of a lookup can start at: one record of appendSubtableDigests().
  Digest subtableDigest(const Face& face, int table, uint32_t lookupIndex, uint16_t subtable);
  ```
- Produces, in `OtFilterWriter.h`:
  ```cpp
  namespace ot {
  // The CPac table (crosspoint-reader #3787's docs/file-formats.md) for a face's GSUB and GPOS: what Face::init()
  // validates and Plan::settings() reads. Written in order, a piece at a time, so an install streams it into flash.
  class FilterTableWriter {
   public:
    // Lookups per table past this get no table at all (the probe stops counting there too).
    static constexpr uint32_t kMaxLookups = MAX_PLANNED_LOOKUPS;
    explicit FilterTableWriter(const Face& face);
    // The table's bytes: 16 + 28 a lookup + 24 a subtable; 0 with no lookups, or past kMaxLookups in a table.
    uint32_t bytes() const;
    // Writes the next min(room, bytes left) bytes to `out` and returns how many; 0 once all are written.
    uint32_t write(uint8_t* out, uint32_t room);
  };
  }  // namespace ot
  ```
- The rules:
  - Lookup and subtable counts are the engine's own: `Face::lookupCount(t)`, and `Face::subtableCount(face.lookup(t,
    i))`. So an Extension lookup that `Face::lookup()` drops has no subtables, and `initFilters` always accepts what
    the writer writes for a face.
  - A lookup's digest is `lookupDigest()`. The order is upstream's: the header, then per table the lookup digests,
    the `u32` first-subtable indices (big-endian) and the subtable digests.
  - Two passes over the coverage: the unions, then the subtables. The state is a few counters and one 24-byte record;
    nothing is allocated.

- [ ] **Step 1: Write the failing tests.**
  - `OtFilterWriter.WritesTheConvertersTableByteForByte`, for each `.layout` fixture:
    - a face over its tables with `CPac` dropped;
    - `bytes()` equals the fixture's own `CPac` length;
    - one `write()` into a buffer of that size gives the fixture's `CPac` bytes (`memcmp` 0). Upstream's
      `shaping_blob.py` wrote them.
  - `OtFilterWriter.PiecesOfAnySizeGiveTheSameBytes`: pieces of 1, 7, 24 and 4096 bytes give the one-call bytes, and
    `write()` returns 0 after the last.
  - `OtFilterWriter.TheTableItWritesIsTheOneTheFaceReads`, for each fixture:
    - a face whose `CPAC` view is the writer's output has non-null `filters(GSUB)` and `filters(GPOS)`;
    - every word shapes as `ExpectedShaping.h` says.
  - `OtFilterWriter.AFaceWithoutLookupsHasNoTable`: the Tamil fixture with GSUB and GPOS dropped gives `bytes() == 0`,
    and `write()` 0.
  - `OtFilterWriter.AnExtensionLookupMixingTypesHasNoSubtables`:
    - the Tamil fixture's GSUB gets a second lookup of type 7 whose two subtables extend types 1 and 4, built as
      `initWithGsub` builds one;
    - in the writer's output, that lookup's first-subtable index equals the next lookup's;
    - a face over it has non-null `filters(GSUB)`.
- [ ] **Step 2: Run** `cmake --build build/test --target OtShaperPortTest -j 8`. Expected: a compile error (no
  `OtFilterWriter.h`).
- [ ] **Step 3: Implement** `subtableDigest` (the loop body of `appendSubtableDigests` for one subtable) and the
  writer.
  - The header comment of `OtFilterWriter.h`: "Ported from crosspoint-reader PR #3787 (SSafayet / @SSafayet): the
    `CPac` format and `lookup_filters()` in `lib/EpdFont/scripts/shaping_blob.py`. Here in C++ over an `ot::Face`,
    written in pieces at install, its counts as `Face::lookup()` reads the lookups (witchhunt: deviation 4)."
  - `OT_SHAPER_SOURCES` globs `lib/OtShaper/*.cpp`, so the host build takes the new file.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "OtFilterWriter|OtShaper|OtPlan"`. Expected: all pass.
- [ ] **Step 5: Format check, then commit** `feat(shaping): the CPac table written from a face, in pieces (deviation
  4)`. The body names the port and its source.

## The TrueType engine

### Task 4: A Noto Sans Devanagari subset, and HarfBuzz's runs for it

**Files:**
- Create: `lib/EpdFont/scripts/make_shaping_fixtures.py`
- Create, generated: `test/fixtures/fonts/NotoSansDevanagari-Shaping-Regular.ttf`, `...-Shaping-Bold.ttf`,
  `test/ttf_font/ExpectedGlyphRuns.h`
- Modify: `test/fixtures/fonts/README.md` (a table row, the source rows, regenerating, licences)
- Modify: `test/ot_shaper_port/OtShaperTestHelpers.h`: `StoredGlyph`, `roundPixels`, `BoundBuffer` and
  `shapeAsStored` move there from `OtShaperPortTest.cpp:29-66`; `shapeAsStored` gains
  `uint32_t ppem26_6 = kFixturePpem26_6`
- Modify: `test/ot_shaper_port/CMakeLists.txt` (`TTF_FIXTURE_DIR`, `${REPO_ROOT}/test/ttf_font` on
  `OtShaperPortTest`'s include path)
- Test: `test/ot_shaper_port/OtShaperPortTest.cpp` (`OtShaperReference.TheTrueTypeSubsetShapesAsHarfBuzz`)

**Interfaces:**
- Produces, in `ExpectedGlyphRuns.h` (generated, "Do not edit"):
  ```cpp
  struct RunGlyph { uint16_t gid; int16_t advance12_4; int8_t dx; int8_t dy; };
  struct ExpectedRun { const char* utf8; const char* language; const RunGlyph* glyphs; uint8_t count; };
  struct RunSet { const char* file; uint32_t ppem26_6; const ExpectedRun* runs; size_t count; };
  constexpr RunSet kDevanagariRunSets[4];  // Regular and Bold, each at 1867 (14 pt) and 2560 (a 40 px native size)
  ```
- The script:
  - **Sources:** `--source-dir` with Noto Sans Devanagari Regular and Bold, unhinted, from
    `notofonts/notofonts.github.io` at `5c82cef0a8f08806f932062e18c02be62be278f0`.
    - It checks the sha256 of each (Regular `9c7d935139ea6a1e6ad9dbac4f6d27ece1e04bca8123c8888d00a0f9df4724cd`, Bold
      `ff2f76a23aad41e0608c2d7dbc4bacd247ff3bec78f0ec2a8fb106b561636e58`) and downloads nothing.
  - **Words:** upstream's 16 Devanagari words and languages from `test/complex_shaper/generate_fixtures.py:51-65`,
    taken verbatim.
  - **The subset:** `fontTools.subset` of their code points, plus U+0020, U+200C, U+200D and U+25CC.
    - Options: `layout_features=['*']`, `hinting=False`, `notdef_outline=True`, `name_IDs=['*']`.
    - It keeps GDEF, GSUB and GPOS; it fails if either file reaches 65,536 B.
  - **The runs:** HarfBuzz over each subset file. `font.scale = (ppem, ppem)` and `font.ppem = ((ppem + 32) >> 6,)
    * 2`, script `Deva`, ltr, the word's language.
    - Each glyph is stored as `generate_fixtures.py:89-90` and `:150-153` convert it: `(x_advance + 2) >> 2`,
      `round_px(x_offset)`, `-round_px(y_offset)`.
  - **Its header comment:** "The words, the conversion and `round_px` are crosspoint-reader PR #3787's
    `generate_fixtures.py` (SSafayet / @SSafayet)."
  - The output is byte-identical on a second run.

- [ ] **Step 1: Write the script, then run it:**
  `python -I -X utf8 C:/tmp/wm2-measure/tools/pyrun.py C:/tmp/wm2-measure/pydeps lib/EpdFont/scripts/make_shaping_fixtures.py
  --source-dir C:/tmp/wm2-measure/dl/fonts --fonts-out test/fixtures/fonts --header-out test/ttf_font/ExpectedGlyphRuns.h`.
  - Expected: two fonts under 64 KB each, and 64 runs.
  - Run it again: `git status --short test/fixtures test/ttf_font` shows only the three new files.
  - Run clang-format 21 on the header and run it once more. If the header then differs, make the script emit the
    formatted form.
- [ ] **Step 2: The README.**
  - A row for the two subsets: what they are (the words above), and what they are used for (step 2 PR 2's shaping
    tests).
  - Their source rows, with URL and sha256.
  - Regenerating, as above.
  - Licences: derived from Noto Sans Devanagari, under the SIL OFL 1.1, with the copyright of the source's name ID
    0 (the script prints it).
- [ ] **Step 3: The failing test.** First move the helpers; the move changes no test.
  `OtShaperReference.TheTrueTypeSubsetShapesAsHarfBuzz`, for each `RunSet`:
  - a face over the subset file's own tables (`tablesFromSfnt` on the file, `CPac` and `CPpl` cleared);
  - each run's plan from `languageTagsFor(run.language)`;
  - `shapeAsStored(face, plan, run.utf8, set.ppem26_6)` equals the run's glyphs.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R OtShaperReference`. Expected: PASS, since the engine and
  HarfBuzz agree. A miss is a fixture or conversion bug: stop and report.
- [ ] **Step 5: Format and NUL check, then commit** `test(ttf): a Noto Sans Devanagari subset and HarfBuzz's runs for
  it`. The body names the script's port and the fonts' source.

### Task 5: Room for GDEF and `CPac` in a style block, counted at the probe

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFamily.h`: `FileProbe` `:39-56`, `SdBlock` `:68-118` (with `kMaxTables` `:95`), and
  `fitsStore`'s comment `:120-126`
- Modify: `lib/EpdFont/ttf/TtfFamily.cpp`:
  - `SdTables`, `kSdTrueType` and `kSdCff` (`:32-51`), and `sdBlockBytes` (`:69-72`);
  - `probeFile` (`:192-267`);
  - `SdBlock::plan` (`:271-316`), `addTable` (`:319-331`), `directoryWord` (`:335-350`) and `read` (`:352-386`)
- Modify: `lib/EpdFont/SdCardFontRegistry.cpp:119-130` (the fit asked with `probe.flashBytes`),
  `test/ttf_font/ShippedFamiliesTest.cpp:57-61`
- Modify: `test/ttf_font/CMakeLists.txt`: `${OT_SHAPER_SOURCES}` and `${OT_SHAPER_INCLUDE_DIR}` on `TtfFontTest`, for
  the tests' writer
- Test: `test/ttf_font/TtfFamilyTest.cpp`

**Interfaces:**
- Consumes: `ot::FilterTableWriter` (Task 3), in tests only. `TtfFamily.cpp` stays free of `lib/OtShaper`.
- Produces, in `ttf::`:
  ```cpp
  struct LayoutCounts { uint32_t lookups = 0; uint32_t subtables = 0; };
  // GSUB's or GPOS's lookups and their declared subtable counts, read from the file in small pieces. A table with
  // more than ot::FilterTableWriter::kMaxLookups lookups counts as none (its font gets no CPac). False: a read
  // failed.
  bool countLookups(const FileSource& file, uint32_t offset, uint32_t length, LayoutCounts* out);
  // The room a CPac takes: 16 + 28 a lookup + 24 a subtable; 0 without lookups. At least what the writer writes.
  constexpr uint32_t cpacBytesFor(const LayoutCounts& gsub, const LayoutCounts& gpos);
  // A flash-mode block: the file, then its CPac at align4(fileBytes).
  constexpr uint32_t flashBlockBytes(uint32_t fileBytes, uint32_t cpacBytes);  // cpacBytes 0: fileBytes
  // The CPac room of one file: its directory's GSUB and GPOS, counted. False: a read failed. The probe, SdBlock::plan
  // and the install's flash-mode sizes (Task 6) all count through it.
  bool filterTableBytes(const FileSource& file, uint32_t* bytes);
  ```
- `FileProbe` gains `uint32_t cpacBytes` and `uint32_t flashBytes` (`flashBlockBytes(file.size, cpacBytes)`).
  `sdBytes` now holds GDEF and the `CPac` room. A read that fails while counting gives `{}` (style -1), as every
  other read failure of the probe does.
- `SdBlock`:
  - `kSdTrueType` and `kSdCff` list `GDEF` (optional, after `firstOptional`); `kMaxTables` goes to 14.
  - `plan()` counts the lookups, and lays the `CPac` out as a made table of the directory: tag `CPac`, checksum 0,
    after every copied table and before the outline table (glyf past the block; CFF's rebuilt table stays last).
  - New: `uint32_t cpacAt() const`, `uint32_t cpacBytes() const`. `bytes()` includes the room.
  - `source()` reads the `CPac` range as erased (0xFF): the install writes it (Task 6).
- `fitsStore(fileBytes, ...)`: `fileBytes` is now each style's flash-mode block (`FileProbe::flashBytes`).

- [ ] **Step 1: Write the failing tests** (TtfFamilyTest, with the subset and Bookerly from Task 4):
  - `TtfFamily.AnSdModeBlockHoldsGdefAndRoomForCpac`, on the subset's Regular, as `source()` reads the block:
    - its directory lists GDEF, with the file's bytes;
    - it lists `CPac`, of `probe.cpacBytes`, after every other copied table and before glyf's `fontOffset`;
    - `bytes() == probe.sdBytes`.
  - `TtfFamily.TheCpacRoomIsWhatTheWriterWrites`: for the subset and `Bookerly-Regular.ttf`,
    `cpacBytesFor(counts) == ot::FilterTableWriter(face).bytes()`, with the face over the file's own tables.
  - `TtfFamily.AFontWithoutLookupsReservesNoCpac`: a `TtfBuilder` font has `cpacBytes == 0` and `flashBytes == size`.
  - `TtfFamily.TheCffBlockKeepsItsCffTableLast`: on `NotoSansJP-Cff.otf` (GPOS `kern`):
    - the `CPac` record ends at or before the CFF record's offset;
    - the CFF record is still the block's last, `blockOffset + cff bytes == bytes()`, and its length runs past it.
    - Drawing from the card is the existing SD-mode CFF tests' job (`TtfFontTest.cpp:2156-...`, Step 4).
  - `TtfFamily.AHostileLookupListIsCountedBounded` (Review Focus 1):
    - a built font whose GSUB LookupList declares 65,535 lookups, all offsets past the table: `cpacBytes == 0`, and
      `MemoryFile::reads` stays under 100;
    - a built font with 4,096 lookups has `cpacBytes == 16 + 28 × 4096` plus its subtables.
  - `TtfFamily.ACardReadFailingWhileCountingIsTransient`: `MemoryFile::readsTo` cuts reads at the LookupList.
    `SdBlock::plan` returns `Transient`, and `probeFile` gives style -1.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R TtfFamily`. Expected: the new tests fail (or do not compile).
- [ ] **Step 3: Implement.**
  - **Reads:** the LookupList's offsets in pieces of 64 (128 B on the stack), then each lookup's 6-byte header.
  - **The table's own guards, as `Face` reads it:** major version 1; a null or out-of-table offset counts as a lookup
    of no subtables.
  - **Callers:** `SdCardFontRegistry` passes `probes[...].flashBytes`, and `ShippedFamiliesTest` asks `fitsStore` with
    it.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "TtfFamily|TtfFont|ShippedOtf|TtfRegistry"`.
  - Expected: all pass.
  - A test whose chip size counts pages (TtfFontTest's SD-mode tests, `:1624-1835`) may now see bigger SD blocks.
    For each one that fails, recompute its constant, or its comment's page figures, from `planLayout`. List them in
    the commit body with the block sizes before and after.
- [ ] **Step 5: Format check, then commit** `feat(ttf): room for GDEF and CPac in a style block, counted at the
  probe`.

### Task 6: The install writes each style's `CPac`; every TrueType family installs again

**Files:**
- Modify: `lib/EpdFont/ttf/GlyphStoreFormat.h`: `kFormatVersion` (`:30-38`), and `StyleInfo` (`:162-170`) and its
  comment
- Modify: `lib/EpdFont/ttf/FlashGlyphStore.h:100-107` (`beginInstall`), and
  `lib/EpdFont/ttf/FlashGlyphStore.cpp`: `validStyles` (`:107-126`) and `beginInstall` (`:138-186`)
- Modify: `lib/EpdFont/ttf/TtfFamily.h:128-134` (`kRenderVersion`)
- Modify: `lib/EpdFont/ttf/TtfFont.h`: `StoreEvents` (`:336-344`), and `Style` (`:160-180`, `fileBytes`)
- Modify: `lib/EpdFont/ttf/TtfFont.cpp`:
  - `InstallWork` (`:41-47`), `holdsFile` (`:80-91`) and `TtfFontSet::install` (`:197-331`);
  - `TtfFont::openStyles` (`:671-682`) and `fontBytes` (`:910-921`)
- Modify: `docs/file-formats.md:97` and `:551-628` (the glyph store: the style blocks' `CPac`, SD mode's GDEF,
  version 5, render version 4), plus a new subsection "`CPac` in a style block"
- Test: `test/glyph_store/GlyphStoreFormatTest.cpp:89-90`, `test/glyph_store/GlyphStoreInstallTest.cpp`,
  `test/ttf_font/TtfFamilyTest.cpp:293-298`
- Create: `test/ttf_font/TtfShapingTest.cpp`. Add it to `test/ttf_font/CMakeLists.txt`.
- Modify: `test/ttf_font/TtfFontTestUtil.h`. `Reference`, `referenceAt`, `emboldenedAt`, `px`, `bitmapOf` and
  `expectDrawnAs` move there, unchanged, from `TtfFontTest.cpp`'s anonymous namespace (`:96-212`).

**Interfaces:**
- Consumes: `ot::FilterTableWriter`, `ot::tablesFromSfnt`, `ot::Face` (Tasks 3, PR 1);
  `SdBlock::cpacAt`/`cpacBytes`, `flashBlockBytes` and `filterTableBytes` (Task 5).
- Produces:
  - `glyphstore::kFormatVersion = 5`. The history comment: "5: a style block ends with its `CPac` (writing modes step
    2), an SD-mode block holds GDEF, and `StyleInfo::fileSize` is set in both modes".
  - `StyleInfo::fileSize`: the TTF file's size in both modes. In flash mode the block's `CPac` starts at
    `align4(fileSize)` and runs to `size`.
  - `validStyles` refuses a flash-mode style with `fileSize == 0` or `align4(fileSize) > size`.
  - `beginInstall(const Layout&, const uint8_t identity[], const StyleInfo* info, uint8_t keepStyles, bool*
    noMemory)`: `info` has one record per block, every mode. Flash mode takes `fileSize` from it; SD mode takes all
    of it, as before.
  - `ttf::kRenderVersion = 4`. The history comment: "4: shaping tables at install (writing modes step 2): every block
    ends with its `CPac`, an SD-mode block holds GDEF, and keys from 0x110000 are glyph IDs".
  - `TtfFontSet::StoreEvents::lastInstallMs`: the last install's time, 0 without the `nowMs` hook.
  - `TtfFont::Style::fileBytes`: `info.fileSize`. Flash mode's `FontBytes` is bounded by it, so the engine sees the
    file and not its `CPac`.
- The install, per block not kept:
  1. Copy the source's bytes before the `CPac` (flash mode: the whole file; SD mode: up to `cpacAt()`).
  2. Map `[0, cpacAt)` with `flash_.map()`; the store is closed and nothing else is mapped.
  3. Build `work->face` over `tablesFromSfnt` of the mapped bytes, with `CPAC` and `CPPL` cleared. Use `fileBytes`
     in flash mode and `cpacAt()` in SD mode, so the made `CPac` record past them is skipped.
  4. Stream `FilterTableWriter` through `work->buffer` into `writeStyleData`, yielding between pieces. Unmap.
  5. Copy what lies past the room: SD mode's rebuilt CFF table.
  6. Log `"style block %u: CPac %u of %u B"` per block, and the install's milliseconds.
- **Kept blocks.** A block is kept only when the source's bytes outside the room match, and its `CPac` does too. The
  `CPac` is checked by regenerating it from the flash copy of the tables, mapped as above, and comparing it, a half
  buffer at a time.
- **`InstallWork`** gains `ot::Face face`: 120 B on the C3, 224 on the host, beside its 4 KB buffer. It lives on the
  heap for one install.
- **Sizes:** flash mode is `flashBlockBytes(file size, filterTableBytes)`; SD mode is `SdBlock::bytes()`.

- [ ] **Step 1: Write the failing tests.**
  - `GlyphStoreFormat`: `kFormatVersion` is 5.
  - `GlyphStoreInstall.AFlashModeStyleKeepsItsFileSize`: `beginInstall` with an info array, then `finishInstall`, then
    `open`. `styleInfo(0).fileSize` is the given size.
  - `GlyphStoreInstall.AFlashModeFileLargerThanItsBlockIsRefused`: `beginInstall` returns false, and nothing is
    erased.
  - `TtfFamily.TheIdentityChangesWithTheRenderVersion` (`:293-298`): version 4, `identityAt(styles, 4)`.
  - In `TtfShapingTest.cpp` (suite `TtfShapingInstall`), with the subset's two styles as one family:
    - `TheInstallWritesEachBlocksCpacAfterItsFile`:
      - after `load`, the block's bytes at `align4(fileSize)` (read through the chip) equal `FilterTableWriter` over
        the file's own tables;
      - the rest of the room reads 0xFF;
      - glyphs still draw as `reference()`.
    - `AnSdModeBlocksCpacIsATableOfItsDirectory`: Bookerly as the body and the subset as an accent, on the chip of
      `ABodyTooLargeForFlashModeTakesTheAccentFamiliesToTheCardWithIt` (26 pages, or as Task 5 left it).
      - the accent `readsCard()`;
      - its block's `CPac` record's bytes equal the writer's over the block's own tables;
      - its GDEF record's bytes equal the file's.
    - `AKeptBlockKeepsItsCpac`: as `AnotherSetWithTheSameLeadingFamilyKeepsItsStyleBlocks` (`:1592`), with the subset
      as the leading family. Its blocks see no erase, `reprograms()` stays 0, and their `CPac` is the writer's.
    - `AKeptBlockWithATornCpacIsWrittenAgain` (Review Focus 2):
      - load once;
      - through `chip.raw()`, set 64 bytes of the first block's `CPac` to 0xFF, and erase the header sector, as a cut
        before `finishInstall` leaves them;
      - load again: the block's sectors are erased once more, and its `CPac` equals the writer's.
    - `AFamilyThatFitsWholeOnlyWithoutItsCpacGoesToTheCard` (Review Focus 3):
      - a `TtfBuilder` font padded (`extraTables`, `zzzz`) to 65,528 B, with a GPOS of 10 lookups;
      - the chip is the smallest whole-page font area `glyphstore::planLayout` fits at the file's own size;
      - the family loads, `readsCard()`, its glyphs draw, and its `CPac` is written.
    - `TheInstallTimeIsReported`: with a `nowMs` hook that steps 5 ms a call, `lastInstallMs > 0` after an install.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R "GlyphStore|TtfFamily|TtfShapingInstall"`. Expected: the new
  tests fail.
- [ ] **Step 3: Implement** as the Interfaces block says.
  - `holdsFile` becomes `holdsBlock(flash, offset, source, cpacAt, cpacBytes, work)`. It compares
    `[0, min(cpacAt, size))` and `[cpacAt + cpacBytes, size)` from the source, then the `CPac` by regeneration.
  - `openStyles` sets `style.fileBytes`, and `fontBytes()` bounds flash mode by it.
- [ ] **Step 4: Run the TTF suites,** `ctest --test-dir build/test -R "GlyphStore|Ttf|ShippedOtf"`.
  - Expected: all pass.
  - A failing test that counts pages: recompute its constant or its comment from `planLayout`, as in Task 5, and list
    it in the commit body. A glyph that draws differently is a bug, not a constant.
- [ ] **Step 5: The docs.** `docs/file-formats.md`:
  - the table row (`:97`) says version 5;
  - the style-block bullet (`:570-576`) adds `GDEF` to SD mode's tables, and the `CPac` after the file (flash mode)
    or as a made table before the outline table (SD mode);
  - the identity bullet says render version 4;
  - the version history (`:625-628`) gets its line for 5;
  - the new subsection "`CPac` in a style block" gives upstream's field table (version, reserved, the two lookup
    counts, the two subtable counts, then per table the lookup digests, first-subtable indices and subtable
    digests). It credits #3787's `docs/file-formats.md` for the format, and says how the install writes and checks
    it.
- [ ] **Step 6: Format check, then commit** `feat(ttf): the install writes each style's CPac; every TrueType family
  installs again`.

### Task 7: Keys from 0x110000 draw glyph IDs, with a glyph-ID prefetch

**Files:**
- Create: `lib/EpdFont/GlyphKey.h`
- Modify: `lib/EpdFont/GlyphProvider.h:94-130` (two virtuals), and `lib/EpdFont/EpdFontFamily.h:24-43`,
  `lib/EpdFont/EpdFontFamily.cpp:33-56`
- Modify: `lib/EpdFont/ttf/TtfOpenType.h:47-53`, `lib/EpdFont/ttf/TtfOpenType.cpp:45-50` (`Ligatures::glyphFor`)
- Modify: `lib/EpdFont/ttf/TtfFont.h` (`StyleProvider` `:123-136`; a private `prefetchGlyphs`), and
  `lib/EpdFont/ttf/TtfFont.cpp`:
  - `prefetch` (`:1058-1134`): its second half becomes a helper the glyph-ID prefetch shares;
  - `render` (`:943-989`) needs no change, since it calls `glyphFor`
- Modify: `lib/EpdFont/ttf/GlyphStoreFormat.h:114-119` (the key's comment; a `static_assert` that `glyph_key::kLast`
  fits 21 bits)
- Test: `test/ttf_font/TtfShapingTest.cpp` (suite `TtfGlyphKeys`), `test/glyph_provider/GlyphProviderTest.cpp` (the
  defaults)

**Interfaces:**
- Produces, in `GlyphKey.h` (ours, pure `constexpr`):
  ```cpp
  // Writing modes step 2, deviation 5: a key from kBase names a TrueType font's glyph by its ID, past Unicode, so no
  // text ever reads as a glyph.
  namespace glyph_key {
  constexpr uint32_t kBase = 0x110000;
  constexpr uint32_t kLast = kBase + 0xFFFF;
  constexpr bool is(uint32_t cp) { return cp >= kBase && cp <= kLast; }
  constexpr uint32_t of(uint16_t glyph) { return kBase + glyph; }
  constexpr uint16_t glyphOf(uint32_t key) { return static_cast<uint16_t>(key - kBase); }
  }  // namespace glyph_key
  ```
- `GlyphProvider` gains:
  - `virtual bool drawsGlyphKeys() const { return false; }`;
  - `virtual void prefetchGlyphs(const uint16_t* glyphs, size_t count)`, a no-op by default.
  - `TtfFont::StyleProvider` overrides both: true, and `owner->prefetchGlyphs(*size, style, glyphs, count)`.
- `EpdFontFamily` gains:
  ```cpp
  /// Whether the font drawing `style` takes glyph keys (its provider says drawsGlyphKeys()).
  bool drawsGlyphKeys(Style style = REGULAR) const;
  /// The glyph `key` (glyph_key) names, from the font drawing `style`, without fallbacks; invalid, asking nothing,
  /// when that font draws no glyph keys.
  EpdGlyphRef findGlyphKey(uint32_t key, Style style = REGULAR) const;
  /// The glyph-ID prefetch: the font drawing `style` renders the glyphs it lacks in one batch, if it draws keys.
  void prefetchGlyphs(const uint16_t* glyphs, size_t count, Style style) const;
  ```
- `Ligatures::glyphFor` returns `glyph_key::glyphOf(cp)` for a key below `face.numGlyphs`, and 0 for any other key.
  So `TtfFont::render`, `prefetch`, the store, synthetic styles and the ring treat a key like a code point.
- `TtfFont::prefetchGlyphs(Size&, uint8_t style, const uint16_t* glyphs, size_t count)` follows `prefetch`'s rules:
  at most `kPrefetchGlyphs`, nothing in ring mode, glyph order, one batch, marks for missing glyphs.

- [ ] **Step 1: Write the failing tests** (with the subset, 14 pt):
  - `TtfGlyphKeys.AGlyphKeyDrawsThatGlyph`:
    - with `gid = ttf::glyphIndex(U+0915)`, `font->glyph(kRegular, glyph_key::of(gid))` is drawn as `reference(file,
      0x0915, 14)`;
    - a second lookup renders nothing new (`renders()` unchanged).
  - `TtfGlyphKeys.AKeyPastTheFontsGlyphsIsMissingOnce`: `glyph_key::of(numGlyphs)` gives nullptr twice, with one
    lookup of the font.
  - `TtfGlyphKeys.ASyntheticBoldKeyDrawsEmboldened`: on the Regular alone, the bold key is drawn as `emboldenedAt`
    U+0915.
  - `TtfGlyphKeys.KeysDrawInRingMode`: after `markFull()`, every glyph of हिन्दी's run (from `ExpectedGlyphRuns.h`)
    draws by key, from the ring, as `referenceAt` its gid would (rendered by index).
  - `TtfGlyphKeys.PrefetchGlyphsStoresTheRunInABatch`:
    - `EpdFontFamily::prefetchGlyphs` with हिन्दी's gids renders each once;
    - the lookups after it are hits;
    - a second prefetch renders nothing;
    - in ring mode it writes nothing.
  - `TtfGlyphKeys.AFontThatDrawsNoGlyphKeysIsNotAsked` (Review Focus 4):
    - a family over a counting fake provider (`drawsGlyphKeys()` false) returns an invalid ref from `findGlyphKey`,
      with 0 `missGlyph` calls;
    - so does a built-in face with no provider.
  - In `GlyphProviderTest.cpp`: the default `drawsGlyphKeys()` is false, and the default `prefetchGlyphs` does
    nothing.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R "TtfGlyphKeys|GlyphProvider"`. Expected: a compile error, then
  failures.
- [ ] **Step 3: Implement.** `findGlyphKey` checks the provider, then calls `EpdFont::findGlyph`.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "Ttf|GlyphProvider|FallbackFace|FontCacheManager"`. Expected:
  all pass.
- [ ] **Step 5: Format check, then commit** `feat(ttf): keys from 0x110000 draw glyph IDs, with a glyph-ID prefetch`.

### Task 8: One shaping face per style over its mapped block (deviations 3 and 12)

**Files:**
- Create: `lib/EpdFont/ShapingFace.h`, `lib/EpdFont/ShapingFace.cpp`
- Create: `lib/EpdFont/ttf/TtfShaping.h`, `lib/EpdFont/ttf/TtfShaping.cpp`
- Create: `test/ttf_font/MovingNorFlash.h`
- Modify:
  - `lib/EpdFont/GlyphProvider.h` (one virtual, `class ShapingFace;` declared);
  - `lib/EpdFont/EpdFontFamily.{h,cpp}`;
  - `lib/EpdFont/ttf/TtfFont.h` (`Style` `:160-180`, `StyleProvider`, `TtfFont` public, `TtfFontSet` public near
    `releaseMissMemory` `:357-362`);
  - `lib/EpdFont/ttf/TtfFont.cpp` (`openStyles` `:660-670` resets the face; the new functions);
  - `lib/GfxRenderer/GfxRenderer.h:649-705`, `lib/GfxRenderer/GfxRenderer.cpp` (one query);
  - `test/ttf_font/CMakeLists.txt` (`ShapingFace.cpp`, `TtfShaping.cpp`, `${REPO_ROOT}/test/ot_shaper_port` and
    `${REPO_ROOT}/test/complex_shaper` on the include path, `SHAPING_FIXTURE_DIR`)
- Test: `test/ttf_font/TtfShapingTest.cpp` (suites `ShapingFace`, `TtfShapingFace`)

**Interfaces:**
- Consumes: `ot::Face::rebase` (Task 1), `ot::FilterTableWriter` (Task 3), `Style::fileBytes` (Task 6).
- Produces, in `ShapingFace.h` (ours):
  ```cpp
  /// One TrueType file's shaping tables for a section build (writing modes step 2, deviations 3 and 12): an ot::Face
  /// over the tables where the glyph store maps them, nothing copied. Made by a font's provider on first ask
  /// (GlyphProvider::shapingFace) and kept until the font set's releaseShapingFaces(); plans built on it go first.
  class ShapingFace {
   public:
    virtual ~ShapingFace() = default;
    /// The face as mapped now. Fetches the mapping first and renders no glyph. The first call builds the face
    /// (Face::init, CPac validated); a later one whose mapping moved re-points it (Face::rebase). nullptr when the
    /// block cannot be mapped now (asked again next time) or init() refused the tables (never again). Call it
    /// before each Plan::build and each ot::shape: a glyph lookup in another flash-mode style moves the mapping.
    const ot::Face* current();
   protected:
    virtual const uint8_t* map() = 0;                                     // the block's base now; nullptr: unmapped
    virtual bool tables(const uint8_t* base, ot::FaceTables* out) = 0;    // the views over the block at `base`
   private:
    ot::Face face_;
    const uint8_t* base_ = nullptr;
    enum class State : uint8_t { Unbuilt, Built, Refused } state_ = State::Unbuilt;
  };
  ```
- Produces, in `TtfShaping.h`:
  ```cpp
  namespace ttf {
  /// The views a style block gives the shaper. Flash mode (fileBytes > 0): the file's own tables, and the CPac at
  /// align4(fileBytes) up to blockBytes. SD mode (fileBytes == 0): the block's directory, its CPac record included.
  /// CPpl is always empty (plans are built, never read), and a file's own CPac is never taken.
  bool shapingTables(const uint8_t* block, uint32_t blockBytes, uint32_t fileBytes, ot::FaceTables* out);
  }
  ```
- `GlyphProvider` gains `virtual ShapingFace* shapingFace(uint32_t* ppem26_6) { return nullptr; }`. The TrueType
  override sets the provider's size's ppem and returns `owner->shapingFace(style)`.
- `EpdFontFamily::shapingFace(Style style, uint32_t* ppem26_6) const`: the provider's answer for the font drawing
  `style`, or nullptr.
- `GfxRenderer`:
  ```cpp
  /// The face a shaper reads for `fontId`'s `style` (the TrueType file that draws it, a synthetic style's source),
  /// and the pixels per em (26.6) its glyphs are drawn at before the font's base scale; nullptr for every other font.
  ShapingFace* shapingFace(int fontId, EpdFontFamily::Style style, uint32_t* ppem26_6) const;
  ```
- `TtfFont`:
  - `ShapingFace* shapingFace(uint8_t style)`: the face of the file that draws `style`. It is made on first ask with
    `makeUniqueNoThrow` (about 130 B on the C3), and is nullptr for a style the family does not draw, or without the
    memory.
  - It is a private `FileShapingFace : ShapingFace`. Its `map()` is `set_.store_.mapStyle(block)`, and its
    `tables()` is `shapingTables(base, bytes, onCard ? 0 : fileBytes, out)`.
- `TtfFont::Style` gains `std::unique_ptr<ShapingFace> shaping`, reset by `openStyles`.
- `TtfFontSet::releaseShapingFaces()` frees every family's. PR 3 calls it at a build's end, after its plans.

- [ ] **Step 1: The test double.** `MovingNorFlash` is a `NorFlash` over a `SimNorFlash`:
  - every `map()` returns a fresh heap copy of the range;
  - every `program()` and `erase()` reaches the chip and each live copy that overlaps it;
  - `unmap()` fills the copy with 0xA5 and frees it.
  - So the store keeps working, and a flash-mode block lands at a new address every time it is mapped.
- [ ] **Step 2: Write the failing tests.**
  - `ShapingFace` (a fake subclass over two copies of the Devanagari `.layout` fixture, its `map()` answering from a
    script of bases):
    - `TheFirstCallBuildsAndAMoveRepoints`:
      - `tables()` is asked once over three calls (A, A, B);
      - after the move, with A poisoned, a plan built at the first call shapes every Devanagari fixture word as
        `ExpectedShaping.h` says.
    - `AnUnmappedBlockGivesNoFaceForNow`: `map()` nullptr gives nullptr, then a mapped call builds.
    - `RefusedTablesAreNotAskedAgain`: no `head` gives nullptr, and `tables()` is asked once over two calls.
  - `TtfShapingFace`, over the installed subset (`ExpectedGlyphRuns.h`, via `shapeAsStored` at the set's ppem):
    - `ShapesAsHarfBuzzInFlashMode`:
      - for Regular and Bold, `EpdFontFamily::shapingFace(style, &ppem)` gives `ppem == 1867` at 14 pt;
      - with `nativeFonts(40)`, its provider gives 2560;
      - each `RunSet`'s runs shape, through `current()`, plan by plan, as the oracle says.
    - `ShapesAsHarfBuzzInSdMode`: the same with the subset as the SD-mode accent of Task 6's
      `AnSdModeBlocksCpacIsATableOfItsDirectory`.
    - `TheFaceWithAndWithoutCpacShapesAlike` (the design's named test), in both modes:
      - the face from `current()` has non-null `filters(GSUB)` and `filters(GPOS)`;
      - a second `ot::Face` over the same `shapingTables()` with `CPAC` cleared has null filters;
      - plans built on each shape every run identically, and as the oracle.
    - `AFaceRepointedAfterAnotherStyleMappedShapesAlike` (deviation 12), on `MovingNorFlash`:
      - take the Regular's face, `current()`, and build every run's plan;
      - look up a Bold glyph, which maps the Bold block and frees and poisons the Regular's;
      - `current()` again: the same pointer, its views now in the new mapping;
      - every run shapes with the plans built before the move, as the oracle.
    - `ABlockWhoseCpacFailsValidationShapesWithComputedFilters` (Review Focus 1): through `chip.raw()`, set the first
      block's `CPac` version to 2, with no reinstall. `filters()` is null, and every run shapes as the oracle.
    - `ASyntheticStyleSharesItsSourceFilesFace`: on the Regular alone, `shapingFace(BOLD) == shapingFace(REGULAR)`.
    - `ALoadedSetHoldsNoFaceUntilAsked`: after `load` and `setBodySize`, the first `shapingFace()` raises
      `heapBytesHeld` (`TtfFontTest.cpp`'s counter) by the face's bytes, a second one by nothing, and
      `releaseShapingFaces()` lowers it by the same bytes.
    - `ABitmapFontHasNoShapingFace`: a family over a fake provider without the override gives nullptr.
- [ ] **Step 3: Run** `ctest --test-dir build/test -R "ShapingFace|TtfShapingFace"`. Expected: a compile error, then
  failures.
- [ ] **Step 4: Implement** as the Interfaces block says.
  - `ShapingFace::current()`: map, and on nullptr return nullptr.
    - `Unbuilt`: `tables()` then `init()`; `Built` or `Refused` follows.
    - `Built` with another base: `face_.rebase(base_, base)`.
    - It keeps `base_`.
  - `TtfFont::shapingFace` calls `checkLock()`.
  - `GfxRenderer::shapingFace` is `findFontFamily(fontId)`, then `family->shapingFace(...)`.
- [ ] **Step 5: Run** `ctest --test-dir build/test -R "Ttf|ShapingFace|ShippedOtf|GlyphStore"`. Expected: all pass.
- [ ] **Step 6: Format check, then commit** `feat(ttf): one shaping face per style over its mapped block (deviations 3
  and 12)`.

## The renderer

### Task 9: Compact glyph records for shaped runs (decision 4)

**Files:**
- Create: `lib/GfxRenderer/GlyphRun.h` (header-only)
- Create: `test/glyph_run/CMakeLists.txt` (`lib/GfxRenderer` and `lib/EpdFont` on the include path),
  `test/glyph_run/GlyphRunTest.cpp`. Add the subdirectory to `test/CMakeLists.txt`.

**Interfaces:**
- Produces:
  ```cpp
  /// One glyph of a shaped run as a section stores it (writing modes step 2, decision 4): u16 glyph, then u16 whose
  /// bit 15 says an offset follows and whose bits 0-14 are the advance in 12.4 pixels (two's complement: -1024 to
  /// 1023.9375 px, 8.5 em at 120 px), then i8 dx, i8 dy (whole pixels, y down) when it says so. Little-endian, read
  /// unaligned. Advances and offsets are in the pixels the run is drawn at.
  struct GlyphRecord {
    uint16_t glyph = 0;
    int16_t advance = 0;  // 12.4 pixels
    int8_t dx = 0;
    int8_t dy = 0;
  };
  namespace glyph_run {
  constexpr int32_t kMinAdvance = -16384;
  constexpr int32_t kMaxAdvance = 16383;
  constexpr size_t recordBytes(const GlyphRecord& r) { return (r.dx != 0 || r.dy != 0) ? 6 : 4; }
  /// Writes `r` at `out` (`room` bytes); false, writing nothing, when it does not fit or its advance is outside the
  /// field. A run with such a glyph is laid out unshaped (PR 3).
  bool encode(const GlyphRecord& r, uint8_t* out, size_t room, size_t* written);
  /// Reads the record at `in` (`room` bytes left); false when it is cut off.
  bool decode(const uint8_t* in, size_t room, GlyphRecord* r, size_t* read);
  }  // namespace glyph_run
  /// A run's records: `count` of them in `bytes` bytes.
  struct GlyphRun {
    const uint8_t* records = nullptr;
    uint16_t count = 0;
    uint16_t bytes = 0;
  };
  /// Calls visit(record, penX) for each whole record in order. penX is the pen in whole pixels from `x`:
  /// per record x + sum of toPixel(advance) when !accumulate (drawText's differential rounding), else
  /// (16x + sum of advances + 8) >> 4 (drawTextAtScale's cursor). Stops at a record cut off by `bytes`.
  template <typename Visit> void forEachGlyph(const GlyphRun& run, int x, bool accumulate, Visit&& visit);
  /// How far the pen moves over `run`, snapped as forEachGlyph snaps it.
  int glyphRunAdvance(const GlyphRun& run, bool accumulate);
  ```
- Includes only `<cstdint>`, `<cstddef>`, `<cstring>` and `EpdFontData.h` (for `fp4`).

- [ ] **Step 1: Write the failing tests** (`GlyphRun`):
  - `ARecordWithoutAnOffsetIsFourBytes`: `{0x0105, 48}` encodes as `05 01 30 00`.
  - `AnOffsetAddsTwoBytes`: `{7, 16, -3, 2}` encodes as `07 00 10 80 FD 02`, and decodes back.
  - `ANegativeAdvanceRoundTrips`: `-16` (-1 px) encodes its field as `0x7FF0`, and decodes to -16.
  - `AnAdvancePastTheFieldIsRefused`: 16384 and -16385 return false, and nothing is written.
  - `TheWidestGlyphAt120PxFits`: 3 em at 120 px (5760) round-trips.
  - `ARecordCutShortEndsTheWalk`: `bytes = 5` over a 4-byte record and a 6-byte one visits one record.
  - `ThePenSnapsPerRecordUnscaled`: advances 88 and 88 (5.5 px). With `accumulate` false, the pens are 0 and 6, and
    the width is 12.
  - `ThePenAccumulatesAtAScale`: the same with `accumulate` true gives pens 0 and 6, and width 11.
  - `AMarkSitsAtThePenPlusItsOffset`: `{base, 128}` then `{mark, 0, -5, -3}`. The mark's pen is 8; drawn at x 3 and
    dy -3.
- [ ] **Step 2: Run** `cmake --build build/test --target GlyphRunTest -j 8`. Expected: a compile error (no header).
- [ ] **Step 3: Implement** the header.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R GlyphRun`. Expected: PASS.
- [ ] **Step 5: Format check, then commit** `feat(render): compact glyph records for shaped runs (decision 4)`.

### Task 10: Draw and measure a shaped run by glyph ID; the page scan's glyph-ID prefetch

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.h`:
  - the text API (`:649-705`);
  - `ScaledGlyphEntry` (`:155-165`), which gains one byte, `kind`, in its padding;
  - `findScaledGlyphMask` and `allocScaledGlyphMask` (`:1040-1041`)
- Modify: `lib/GfxRenderer/GfxRenderer.cpp`:
  - `findScaledGlyphMask`/`allocScaledGlyphMask` (`:199-246`);
  - `renderCharImpl` (`:927-1120`) and `renderCharAtScale` (`:1184-1262`), each split into a lookup and a draw of a
    resolved glyph;
  - the new functions after `drawTextAtScale` (`:1579-1630`)
- Modify: `lib/GfxRenderer/ScaledGlyphResample.h` (the cache key), and `test/scaled_glyph/CMakeLists.txt`
  (`lib/EpdFont` on the include path)
- Modify: `lib/GfxRenderer/FontCacheManager.h` (`:35-43`, `ScanEntry` `:104-107`), and
  `lib/GfxRenderer/FontCacheManager.cpp` (`recordText` `:148-178`, `endScanAndPrewarm` `:188-224`, and a
  `prewarmGlyphs` beside `prewarmCache` `:79-130`)
- Test: `test/scaled_glyph/ScaledGlyphResampleTest.cpp`, `test/font_cache_manager/FontCacheManagerTest.cpp`,
  `test/ttf_font/TtfShapingTest.cpp` (suite `TtfGlyphRun`)

**Interfaces:**
- Consumes: `GlyphRun`, `forEachGlyph` and `glyphRunAdvance` (Task 9); `EpdFontFamily::drawsGlyphKeys`,
  `findGlyphKey` and `prefetchGlyphs`, and `glyph_key` (Task 7).
- Produces, in `GfxRenderer`:
  ```cpp
  /// Draws a shaped run at (x, y), y the line's top as drawText takes it. Each record's glyph comes by ID from
  /// `fontId`'s `style` (findGlyphKey: no fallbacks) and is drawn at the pen plus its offset; the pen moves by the
  /// record's advance. No kerning, ligatures or tracking. `scale` is drawTextScaled's: the glyphs are drawn at it
  /// times the font's base scale, through renderCharAtScale's path (sup/sub keep its dark threshold); the records'
  /// advances and offsets are in drawn pixels already, so SUP/SUB halve nothing here. A record whose glyph the font
  /// lacks draws nothing but still moves the pen. A font that draws no glyph keys draws nothing (logged once).
  /// While the page is scanned, records the run's glyph IDs instead (FontCacheManager::recordGlyphs).
  void drawGlyphRun(int fontId, int x, int y, const GlyphRun& run, bool black = true,
                    EpdFontFamily::Style style = EpdFontFamily::REGULAR, float scale = 1.0f) const;
  /// How far drawGlyphRun's pen moves: glyphRunAdvance, accumulated when scale x the base scale is not 1.
  int glyphRunWidth(int fontId, const GlyphRun& run, float scale = 1.0f) const;
  /// The glyph-ID prefetch for a section build's warm step: the run's glyphs, 64 at a time (128 B on the stack).
  void prefetchGlyphRun(int fontId, const GlyphRun& run, EpdFontFamily::Style style) const;
  ```
- Produces, in `ScaledGlyphResample.h`:
  ```cpp
  /// The scaled-mask cache's key for `cp`: a BMP code point (kind 0) or a glyph key's ID (kind 1); not cacheable for
  /// anything else (above the BMP, as before).
  struct ScaledGlyphKey { uint16_t id; uint8_t kind; bool cacheable; };
  constexpr ScaledGlyphKey scaledGlyphKeyOf(uint32_t cp);
  ```
  `find`/`allocScaledGlyphMask` match `(id, kind)`. `static_assert(sizeof(ScaledGlyphEntry) <= 16)` still holds on
  the C3.
- Produces, in `FontCacheManager`:
  - `void recordGlyphs(const GlyphRun& run, int fontId, EpdFontFamily::Style style)`: the IDs go per (font, base
    style) into `ScanEntry::glyphsByStyle[4]`, a `std::string` of little-endian `u16`s.
    - It is capped at 512 IDs (1 KB) a style, and grown in 128-ID steps behind `heapHasBlockFor`, exactly as
      `recordText` grows.
  - `endScanAndPrewarm` prewarms each list, after the texts, with `prewarmGlyphs(fontId, ids, count, style)`, which
    calls `family->prefetchGlyphs`.
- The split of the two blitters, each into a lookup and a draw of a resolved glyph:
  - `renderCharImpl<rotation>(…, cp, …)` calls `drawResolvedGlyph<rotation>(renderer, mode, fontData, glyph, x, y,
    pixelState)`;
  - `renderCharAtScale(…, cp, …)` calls `drawResolvedGlyphAtScale(renderer, mode, fontData, glyph, cacheCp, x, y,
    pixelState, scale, minRaw2Bit)`;
  - `drawText`'s and `drawTextAtScale`'s pixels do not change.

- [ ] **Step 1: Write the failing tests.**
  - `ScaledGlyphResample.AGlyphKeyHasACacheKeyOfItsOwn` (Review Focus 5):
    - `scaledGlyphKeyOf(glyph_key::of(0x41))` is `{0x41, 1, true}`, and `scaledGlyphKeyOf('A')` is `{0x41, 0, true}`;
    - U+1F600 and `glyph_key::kLast + 1` are not cacheable.
  - `FontCacheManager.AScannedGlyphRunIsPrewarmedByGlyphId`: during a `PrewarmScope`, `recordGlyphs` of a 3-glyph
    run in a fake TTF-like family (a provider counting `prefetchGlyphs`). At `endScanAndPrewarm` the provider gets
    those 3 IDs once, in the style recorded.
  - `FontCacheManager.GlyphIdsPastTheCapAreLeftToTheDraw`: 600 IDs give one prefetch of 512.
  - `TtfGlyphRun.AGlyphRunDrawsTheOraclesGlyphsAtItsPositions`, for each `RunSet` and run (both sizes, both styles):
    - The records are encoded from the oracle's glyphs: `advance12_4`, `dx`, `dy`.
    - The draw side composites into a 2-bit canvas (400 × 120): `forEachGlyph(run, 20, false, ...)` with each glyph
      from `findGlyphKey` and its bitmap from `bitmapOf()`, at `(penX + dx + left, 80 + dy - top)`. The 2560 sets
      look glyphs up in a family over `nativeFonts(40)`'s fonts.
    - The reference composites the same canvas by hand. Each glyph is rendered by index (`ttf::Rasterizer::render`
      on the file at the set's ppem), at `20 + sum of fp4::toPixel(previous advances) + dx`.
    - The two canvases are byte-identical, and `glyphRunAdvance` equals the reference's pen at the end.
    - `GfxRenderer.cpp` does not build on the host (HalDisplay). This drives the walk `drawGlyphRun` drives and the
      lookup it uses; the per-glyph blit is `renderCharImpl`'s, which every line draw already uses.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R "ScaledGlyphResample|FontCacheManager|TtfGlyphRun"`. Expected:
  failures and compile errors.
- [ ] **Step 3: Implement** as the Interfaces block says.
  - `drawGlyphRun`:
    - scan mode records and returns;
    - a font for which `family->drawsGlyphKeys(style)` is false draws nothing, logged once per session;
    - `total = (scale <= 0 ? 1 : scale) × fontBaseScale(fontId)`;
    - the baseline is drawText's at total 1, and `y + int(rawFontAscenderSize × total + 0.5)` otherwise;
    - `forEachGlyph(run, x, total != 1, ...)` culls glyphs wholly off the screen, as `drawText` does (`:1540-1550`).
  - In the firmware nothing calls `drawGlyphRun` until PR 3, so the linker drops it. The `default` build still
    compiles it.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "ScaledGlyphResample|FontCacheManager|Ttf|GlyphRun"`. Expected:
  all pass. Then build `default` (Global Constraints). Expected: it links.
- [ ] **Step 5: Format check, then commit** `feat(render): draw and measure a shaped run by glyph ID`.

## On the device

### Task 11: `CMD:TTF:SHAPE`, one word shaped on the device (dev builds)

**Files:**
- Modify: `src/util/SerialCommandGrammar.h` (beside `parseFsMove` `:68`) and its `.cpp`;
  `test/serial_command_grammar/SerialCommandGrammarTest.cpp`
- Modify: `src/SerialTestApi.cpp`: `cmdTtf` (`:909-939`) takes `SHAPE:` under `#if SERIAL_TEST_API`, and
  `appendStats` (`:890-906`) gains `last_install_ms`
- Modify: `docs/serial-test-api.md`, "TrueType fonts: `TTF`" (`:637-...`)

**Interfaces:**
- Consumes: `TtfFont::shapingFace` (Task 8); `ot::Plan`, `ot::Buffer`, `ot::shape`, `ot::languageTagsFor` and
  `indic::scriptOf` (PR 1); `glyph_run::encode` and `glyphRunAdvance` (Task 9); `StoreEvents::lastInstallMs`
  (Task 6).
- Produces:
  - `bool SerialCommandGrammar::parseTtfShape(std::string_view arg, std::string_view& language, std::string_view&
    text)` for `SHAPE:<bcp47>:<utf8>`. The language is empty or 1-15 of `[A-Za-z0-9-]`; the text is 1-192 bytes.
  - `CMD:TTF:SHAPE:<lang>:<text>`, under RenderLock, on the loaded set's body family, regular style, body size.
    - It shapes the text's first Indic run: the code points from the first that `indic::scriptOf` maps, up to 48.
    - It replies `TTF_SHAPE:script=<name> cpac=<B> init_us=<t> repoint_us=<t|-1> plan_us=<t> plan_bytes=<n>
      shape_us=<t> glyphs=<n> width=<px> stored=<n>`.
    - `init_us` is the first `current()`. `repoint_us` is the `current()` after a glyph of another flash-mode style
      was looked up, or -1 when there is none.
    - `stored` is how many of the run's glyph keys the store then holds, after `prefetchGlyphs`.
    - Errors: `TTF_ERROR:no TrueType family loaded`, `...:no Indic letter`, `...:no shaping face`, `...:plan failed`,
      `...:shape failed`, `...:no memory`.
    - Its blocks are freed before the reply: the plan (`makeUniqueNoThrow<ot::Plan>`, about 1-2 KB), the buffer
      (`Buffer` 1,272 B and `bytesFor(112)` 6,947 B) and the records (6 × 112 B); then `releaseShapingFaces()`.
  - `CMD:TTF` gains `last_install_ms`.

- [ ] **Step 1: The failing grammar test.** `SerialCommandGrammar.TtfShapeTakesALanguageAndAWord`:
  - `SHAPE:hi:हिन्दी` gives `hi` and the word; `SHAPE::कि` gives an empty language;
  - `SHAPE:hi:` and `SHAPE:h!:x` are refused, and so is a 193-byte text.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R SerialCommandGrammar`. Expected: FAIL. Implement, then PASS.
- [ ] **Step 3: The command and its doc section.** Show the reply's fields and one example. Say that it is dev-only,
  measures one plan and one word (the design's measure-first item 4), and links the shaper into `default` but not
  into a release build.
- [ ] **Step 4: Build `default` and `gh_release`.** Expected: both link. `gh_release` has no `SERIAL_TEST_API`, so no
  `ot::shape`: check `grep -c "ot5shape" .pio/build/gh_release/firmware.map` gives 0.
- [ ] **Step 5: Format check, then commit** `feat(serial): CMD:TTF:SHAPE, one word shaped on the device (dev
  builds)`.

### Task 12: The baseline, the device runs and measure-first item 4

**Files:**
- Modify: `docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`, a new section "PR 2: TrueType faces
  that shape (item 4)"

- [ ] **Step 1: The baseline.**
  - Make a scratch worktree of the branch's merge base with master: `git worktree add --detach C:/tmp/wt-wm2-base
    $(git merge-base HEAD master)`.
  - Build `default` and `gh_release` there, and record `Flash:` and `RAM:`.
  - Keep its `default` `firmware.factory.bin` for Step 2.
- [ ] **Step 2: Before, on the X4** (COM5), with the baseline flashed and its web bundle copied:
  - `CMD:GET:fontFamily` and `fontSize`; then `CMD:SET:fontFamily` to a TrueType family on its card (for example
    NotoSansRegularOnly);
  - open `/TEST/ePubTest.epub@3:2`; then, with `fontFamily` set to NotoSansTC/Otf, a page of a `/books/test_cjk_*`
    book. Take `CMD:IDLE:60000:SCREEN`, then `CMD:SCREENSHOT` of each, and `CMD:TTF`.
- [ ] **Step 3: After, on the X4.** Flash the branch's `default` and copy its web bundle. Open the same two pages.
  - Expected: one `EVT:FONT_INSTALLED` per family; then `CMD:TTF` shows the set, and `last_install_ms > 0`.
  - Expected: the two screenshots are pixel-identical to Step 2's. The rasterizer is unchanged, so the pages lay out
    again to the same pixels.
  - A difference stops the task: report it.
- [ ] **Step 4: Item 4 on the X3** (COM6). Flash the branch's `default` with esptool and copy the web bundle.
  - Upload `C:/tmp/wm2-measure/merged/NotoSansDevanagari/*.ttf` and `.../NotoSansBengali/*.ttf` to
    `/.crosspoint/fonts/NotoSansDevanagari/` and `/NotoSansBengali/`, over the USB Transfer screen and a no-reset open.
  - For each family:
    - `CMD:SET:fontFamily=<it>`; open `/TEST/ePubTest.epub`; wait for `EVT:FONT_INSTALLED`;
    - `CMD:TTF` gives `last_install_ms`; the serial log gives each block's `CPac` bytes;
    - send `CMD:TTF:SHAPE:hi:हिन्दी` and `CMD:TTF:SHAPE:hi:प्रार्थना` (Devanagari), or `CMD:TTF:SHAPE:bn:শ্রীঈশ্বরচন্দ্র`
      and `CMD:TTF:SHAPE:bn:কর্ত্তৃক` (Bengali), three times each.
  - Also, from `CMD:BOOTLOG`: discovery's time with both families on the card, against the baseline's on the same
    card (the `[ms]` of `Discovery complete` against the line before it).
- [ ] **Step 5: The X4's figures.** Repeat Step 4's `SHAPE` commands on the X4 with one of the two families.
- [ ] **Step 6: Restore.**
  - Both devices: the settings read in Step 2 (on the X4: fontFamily 0, fontSize 14 pt by label, DE, sleep 60,
    publisherFonts 1).
  - The X3 keeps the two families for PR 3; the X4 keeps its fonts. Nothing removes `/web-*.bin`.
- [ ] **Step 7: Flash and RAM.** Build `default` and `gh_release` from the branch.
  - Record each env's delta against Step 1.
  - Split `default`'s delta from the map file: the engine the dev command links (PR 1's measured 47,576 B) against
    the rest.
  - The gate (item 2's allowance A = 15,360 B, for PRs 2-3's glue, the writer and the run draw): `gh_release`'s delta
    must stay under it. Missed, stop and report: PR 3's room is gone.
  - Record the running total for PRs 1-3 against the 90 KB pass line.
- [ ] **Step 8: Write the section**, then remove the scratch worktree.
  - Tables: install times and `CPac` bytes per style (against item 3's counted figures); `init_us`, `repoint_us`,
    `plan_us`, `plan_bytes` and `shape_us` per word and board (min/median/max of three); discovery time; flash and
    RAM.
  - The estimate-to-measured rows for the design's "`CPac`" and "The reinstall" lines.
  - Commit `docs(writing-modes): step 2 PR 2's figures on the X3 and the X4`.

### Task 13: Credits, roadmap, the whole suite and the PR

**Files:**
- Modify: `README.md:176` (the OtShaper entry), `docs/future_work/writing-modes.md:24` (row 2)

- [ ] **Step 1: The README entry.**
  - Its last sentence becomes: the firmware links the engine's face, table and coverage code from PR 2 on, for the
    install's `CPac` (`OtFilterWriter`, ours, a port of #3787's `lookup_filters()`).
  - Add: the test subsets of Noto Sans Devanagari (OFL, `test/fixtures/fonts/README.md`).
  - The tree has no web licences page, so the README entry stays the credit, as minibidi's is. Say so in the PR body
    for the maintainer.
- [ ] **Step 2: The roadmap.** Row 2's status: `in progress: PR 1 #603 (the engine); PR 2 #<n> (TrueType faces that
  shape)`, with this plan linked.
- [ ] **Step 3: The whole host suite** on the final tree, with master merged in if it moved. Expected: only the five
  known Windows-only failures, and every golden byte-identical.
- [ ] **Step 4: cppcheck.** `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect
  high`, in the background.
  - A finding in our code is fixed.
  - A finding in an upstream file gets an inline `// cppcheck-suppress <id>` with its reason, marked
    `// witchhunt: build glue`, and listed in the commit body.
- [ ] **Step 5: Format check and the NUL check** on every touched file, then commit `docs(writing-modes): step 2 PR
  2, TrueType faces that shape`.
- [ ] **Step 6: The PR.**
  - `gh pr view --json state` on the branch: expect no PR.
  - Push, and open a PR against master titled `feat(shaping): TrueType faces that shape (writing modes step 2,
    PR 2)`.
  - The body:
    - the design, this plan and the measurements;
    - deviations 3, 4, 5, 10 and 12, and where each lives;
    - the reinstall, which readers see once;
    - the seams PR 3 uses;
    - the four carried items and where each went;
    - Task 12's figures;
    - that the glyph-run draw has no caller until PR 3, so its on-device check is PR 3's.
  - Fill row 2's `#<n>` in a commit `docs(writing-modes): step 2 PR 2 is #<n>`, and push. Merging is the
    maintainer's.
