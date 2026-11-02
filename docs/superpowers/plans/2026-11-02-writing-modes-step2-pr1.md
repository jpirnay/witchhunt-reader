# Writing modes step 2, measure first and PR 1: the shaping engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the design's gated measurements 1-3. Then bring upstream crosspoint's `lib/OtShaper` (#3787) into the
tree file for file, with deviation 1 (nothrow fixed-capacity storage), host-tested and called by nothing.

**Architecture:**
- Phase 0 measures upstream's engine as it is, outside the tree. It uses a durable workspace (`C:/tmp/wm2-measure`)
  and a throwaway worktree for the scratch builds. The figures go into a measurements file beside the design, and a
  missed pass line stops the work.
- PR 1 lands in step 1 PR 1's order:
  - the files as upstream has them, in one import commit credited to their author;
  - the host build glue, with a test of the engine against upstream's stored HarfBuzz output;
  - deviation 1 in two commits (the buffer, then the plan), every changed line marked `// witchhunt: deviation 1`.
- The oracle and the probe then run again on our engine. The firmware links nothing.

**Tech Stack:**
- C++20, `-fno-exceptions` on the device; GoogleTest host tests (CMake/Ninja, MSYS2 UCRT64 g++ 16.2.0).
- Python 3.14 with uharfbuzz 0.56.2 (HarfBuzz 14.5.0) and fontTools 4.63.0, for the oracle and the font merges.
- PlatformIO for the scratch builds and the size check; riscv32-esp-elf-g++ 14.2.0 for the C3's struct sizes.

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step2-design.md` (approved 2026-11-02): "Measure first",
items 1-3, and "The pieces", item 1.
- PR 1 owns deviation 1 only. Deviations 2-12 are PRs 2-4's.
- Step 1's seam (`lib/TextShaping`, "The shaping layer" of the step 1 design) is not touched.
- Nothing here depends on step 1 PR 2 (`feat/writing-modes-step1-pr2`, unmerged). The plan works against master.

## Global Constraints

From the design, verbatim:
- Every Latin, CJK and Hebrew golden stays byte-identical. Only step 2's fixtures change, and in PR 4 any Arabic line
  of step 1's fixtures (`test_rtl_markup` has none).
- The engine's output matches HarfBuzz 14.5 glyph for glyph on the oracle corpus (upstream's bar: 0 differences), for
  every font PR 3 and PR 4 ship, at two sizes, and with `CPac` from the install.
- Nothrow everywhere: a refused block or a failed run lays the token out unshaped and latches the status bit.
- No resident memory: the shaper's buffer, faces and plans live during a build only, and a page draw allocates nothing
  new for shaped words.
- Measured and reported on the X3 and the X4: flash on `default` and `gh_release`; static RAM; Background-B's lowest
  heap during an Indic build; the reading heap after a page turn on an Indic page; the loop task's stack high-water;
  build and install times.
- Attribution: crosspoint-reader #3787 (MIT) at `dd45ba61`, naming `0fbd1575` for the shaper's files; HarfBuzz's MIT
  notice in the ported files and the licences page; the Noto fonts' OFL.
- The device runs flash the web bundle as well (`web-<hash>.bin` to the card's root).
- (Deviation 1) The limits stay upstream's: `MAX_NESTING_LEVEL` 8, `MAX_CONTEXT_LENGTH` 64, a run's growth factor
  2 + 16, the op budget (`MAX_OPS_FACTOR`, `MAX_OPS_MIN`), `MAX_PLANNED_LOOKUPS` 4096.

PR 1 links nothing into the firmware, so its device items are flash and RAM only. The X3/X4 runs start in PR 2.

This plan's own:
- **Upstream baseline.** #3787's head is `dd45ba6138eb0d5a332d9c082bf7bcdfec6f16cc` (checked 2026-11-02 18:36 UTC,
  open).
  - The local clone holds the earlier head, `0fbd1575`, as branch `pr-3787`:
    `git -C C:/_development/crosspoint-reader show pr-3787:<path>`.
  - Every file this plan takes is blob-identical at both heads. That was checked through the GitHub API on
    2026-11-02, and Task 1 checks it again.
  - Read-only: nothing is fetched into that clone or written there.
- **The workspace.** Phase 0's files live in `C:/tmp/wm2-measure/`. It is durable because Task 9 runs the oracle and
  the probe again from it.
  - The folders: `upstream/` (an archive of upstream's files), `dl/fonts/` (downloads only), `pydeps/`, `tools/`
    (what this plan writes), `corpus/`, `merged/`, `bin/` and `logs/`.
  - Python that reads downloads runs as `python -I -X utf8 C:/tmp/wm2-measure/tools/pyrun.py C:/tmp/wm2-measure/pydeps
    <script> <args>`. `-I` ignores `PYTHONUTF8`, hence `-X utf8`.
- **Imported files keep upstream's bytes.** Any line we change carries `// witchhunt: deviation 1`, or a `#` comment
  in CMake or Python.
- **Credit.** The import commit carries `Co-authored-by: SSafayet <saimoom.akash@gmail.com>`.
  - SSafayet is the author of every #3787 commit, and none of them has a trailer.
  - No other commit gets a co-author trailer. No Claude trailer anywhere.
- **The firmware is unchanged by PR 1.** Nothing in `src` or `lib` includes `lib/OtShaper`.
  - `lib/Utf8/IndicReorder.cpp` is compiled into the firmware's libUtf8 but referenced by nothing.
  - So `default` and `gh_release` flash and RAM equal the baseline that Task 2 measures.
- **Nothrow:** no bare `new`, no exceptions, no `try`/`catch`. Engine allocations go through `makeUniqueNoThrow`
  (`lib/Memory/Memory.h`, which upstream has too).
- **Host tests, from Bash only** (PowerShell's PATH lacks UCRT64):
  - configure once per worktree: `cmake -S test -B build/test -G Ninja -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1
    -DGIT_EXECUTABLE="C:/Program Files/Git/cmd/git.exe"`;
  - build with `cmake --build build/test -j 6 -- -k 0`;
  - run with `ctest --test-dir build/test -j 8 -E HeapPeakRegression`;
  - the only expected failures are Windows-only: FirstChapter ×2, LongChapterPageTables ×2, KeptReadingState,
    DictionaryTest's long-folder test, and KOReaderProgressReplyTest not built.
- **Firmware builds:**
  - Check that no other pio or scons process runs (`tasklist | grep -i -E "pio|scons"`), and never kill one.
  - `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1`, then `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`,
    in the background (a cold build passes 600 s).
  - Record the `Flash:` and `RAM:` lines' bytes used. Nothing in this plan is flashed to a device.
- **Formatting:** clang-format 21 (`/c/tmp/cf21/clang_format/data/bin/clang-format`) with `--dry-run --Werror` on every
  touched `.c`/`.h`/`.cpp`. Upstream's `.clang-format` equals ours (compared 2026-11-02).
- **Files** are written with the Write and Edit tools, never a shell heredoc. Before each commit, check that no touched
  text file contains a NUL byte. Binary fixtures are copied with `git show` (byte-exact in Git Bash).

## Review Focus

1. **A word longer than the shaping buffer holds:** a Malayalam or Tamil compound past PR 3's per-call cap of 32 or
   48 code points. `ot::shape` returns false with `buffer.successful` still true, and `maxRunLength` names 32 or 48, so
   PR 3 shapes the word in pieces rather than drawing it unshaped. Pinned in Task 7 (`MaxRunLengthIsTwoNPlusSixteen`,
   `ARunPastTheStorageFails`).
2. **A heap that refuses one of a plan's blocks** (the first shaped word of an X3 build at its floor):
   - the build fails cleanly, or runs unfiltered with the same glyphs;
   - nothing leaks;
   - the same `Plan` builds again once memory is back (the rebuild the latch earns).
   Pinned in Task 8.
3. **A font whose lookups grow a run past 2n + 16** (a reader's own TTF): the run fails, and the same buffer shapes the
   next word as a fresh one would. Pinned in Task 7 (`AFailedRunLeavesTheBufferUsable`).
4. **A run whose passes rebuild the glyph string** (a dotted circle, a decomposition) swaps `info` and `scratch`. The
   next run binds them afresh, with no two arrays sharing memory. Pinned in Task 7 (`TheNextRunUndoesASwap`).
5. **The firmware stays unchanged although lib/Utf8 grows.**
   - PlatformIO compiles every source of a library in use, so `IndicReorder.cpp` is built into libUtf8.
   - Nothing references it, so the sizes must stay at the baseline.
   Pinned in Task 10, Step 5.

---

## Phase 0: measure first (items 1-3)

These tasks run in order, before any of PR 1's code.
- Each task writes its section of `docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`. Task 1
  creates the file, and Task 4 commits it on `docs/writing-modes-step2`, the branch this plan is on.
- The design's "Memory, flash and time" table is not edited: the measurements file replaces its estimates.

The pass lines, from the design's "Measure first":

| Item | Pass line | Missed |
|---|---|---|
| 1 | 0 differences from HarfBuzz on upstream's corpus under our compiler and flags | stop and look: no PR until the difference is understood |
| 2 | flash for PRs 1-3 at most 90 KB; at least 250 KB of the app slot free after them | the design reopens |
| 3 | each merged family fits the font area, and builds in place or not, as this design says (flash or SD mode; the in-place gate) | the design reopens |

A miss in item 1 stops Phase 0 at once. A miss in item 2 or 3 lets Phase 0 finish, and Task 4 reports it. Either way
PR 1 does not start. 1 KB is 1,024 B throughout.

**Network access needed:**
- the GitHub API (`gh`) for #3787's head and blobs;
- `raw.githubusercontent.com` for the Noto fonts (`notofonts/notofonts.github.io` at
  `5c82cef0a8f08806f932062e18c02be62be278f0`, its `main` on 2026-11-02) and, in Task 10, HarfBuzz's `COPYING` at tag
  `14.5.0`;
- PyPI for `uharfbuzz==0.56.2` and `fonttools==4.63.0`. Per uharfbuzz's release notes, 0.56.2 is "Update to HarfBuzz
  14.5.0", and it ships a `cp310-abi3-win_amd64` wheel;
- GitHub for googletest, when Task 3's scratch worktree configures;
- in Task 6, HarfBuzz's 14.5.0 release tarball, which `scripts/gen_ot_shaper_tables.py` downloads with its sha256
  pinned.

### Task 1: Item 1, upstream's engine on the host: the probe and the HarfBuzz oracle

**Files:**
- Create (workspace, not committed): `C:/tmp/wm2-measure/tools/pyrun.py`, `versions.py`, `OtShaperProbe.cpp`,
  `sizeof_c3.cpp`, `oracle.sh`
- Create: `docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md` (sections "Sources" and "Item 1")

**Interfaces:**
- Produces: `tools/OtShaperProbe.cpp`. It builds against upstream's engine and, unchanged, against ours; Task 9
  commits it. Usage: `ot_shaper_probe <corpus dir> <script> <font> [<script> <font>]...`, where `<script>` is a corpus
  file stem (`devanagari` … `sinhala`). Its position in `generate_corpus.py`'s `SCRIPTS` list is its `ot::Script`.
- Produces: `tools/oracle.sh <ot_shape_cli> <log>`, which runs the matrix of Step 9 and exits 0 only when every run
  reports `N/N lines identical`.
- Produces: `dl/fonts/<Family>-<Style>.ttf` (48 files) and `corpus/<script>.txt` (10 files), used by Tasks 3 and 9.

- [ ] **Step 1: Check #3787's head and the taken files.**
  - `gh api repos/crosspoint-reader/crosspoint-reader/pulls/3787 --jq '.head.sha, .state'`. Expected:
    `dd45ba6138eb0d5a332d9c082bf7bcdfec6f16cc`, `open`.
  - Check these folders: `lib/OtShaper`, `lib/Utf8`, `scripts`, `test/ot_shaper`, `test/complex_shaper`,
    `test/complex_shaper/data`, `test/indic_text`. For each one, compare
    `gh api "repos/crosspoint-reader/crosspoint-reader/contents/<dir>?ref=<head>" --jq '.[] | select(.type=="file") | "\(.sha) \(.path)"'`
    with the blob lines of `git -C C:/_development/crosspoint-reader ls-tree pr-3787 <dir>/`.
  - Expected: the 45 files of Task 5 have the same blob SHA.
  - If the head moved but the taken files did not, record the new head and go on.
  - If a taken file changed, stop and report to the maintainer. The design's baseline is `dd45ba61`, and a resync is
    the maintainer's decision.
- [ ] **Step 2: The workspace.**
  - Create the folders listed under Global Constraints.
  - Copy upstream's files: `git -C C:/_development/crosspoint-reader archive pr-3787 lib/OtShaper lib/Utf8
    scripts/gen_ot_shaper_machines.py scripts/gen_ot_shaper_tables.py scripts/gen_ot_shaper_tables.cc test/ot_shaper
    test/complex_shaper test/indic_text | tar -x -C C:/tmp/wm2-measure/upstream`.
  - Install the Python packages: `python -m pip install --target C:/tmp/wm2-measure/pydeps uharfbuzz==0.56.2
    fonttools==4.63.0`. Never pass `--force-reinstall`.
  - Write `pyrun.py`. It puts `argv[1]` first on `sys.path`, sets `sys.argv = argv[2:]`, and runs `argv[2]` with
    `runpy.run_path(..., run_name="__main__")`.
  - Write `versions.py`. It prints the Python version, `unicodedata.unidata_version`, `uharfbuzz.__version__`,
    `uharfbuzz.version_string()` and `fontTools.version`. Expected: HarfBuzz `14.5.0` and fontTools `4.63.0`.
  - Record the Python and Unicode versions (3.14.7 and 16.0.0 on this machine), since the corpus depends on them.
- [ ] **Step 3: The fonts.** Fetch each of the 48 files with `curl --fail -sSL -o C:/tmp/wm2-measure/dl/fonts/<F>-<S>.ttf
  https://raw.githubusercontent.com/notofonts/notofonts.github.io/5c82cef0a8f08806f932062e18c02be62be278f0/fonts/<F>/unhinted/ttf/<F>-<S>.ttf`.
  - `<F>` is `NotoSans<X>` and `NotoSerif<X>` for each `<X>` in Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil,
    Telugu, Kannada, Malayalam and Sinhala. It is also `NotoNaskhArabic`, `NotoSansArabic`, and the Latin sources
    `NotoSans` and `NotoSerif`.
  - `<S>` is `Regular` and `Bold`.
  - Every path was listed at that commit on 2026-11-02 (`NotoSansDevanagari-Regular.ttf`, for example, is 184,228 B).
  - Unhinted, because our engine does not hint, and the layout tables are the hinted files' own. Upstream's
    `sd-fonts.yaml` uses `hinted`.
  - Record a "Sources" table in the measurements file: each file's name, bytes, sha256, and name ID 5 read with
    fontTools.
- [ ] **Step 4: The corpus.** Run `python -I -X utf8 tools/pyrun.py pydeps upstream/test/ot_shaper/generate_corpus.py
  corpus`, which writes ten files. Record the per-script counts it prints.
- [ ] **Step 5: Write the probe, `tools/OtShaperProbe.cpp`.** It is one file with no test framework.
  - It replaces every global `operator new` and `new[]` form (throwing and `std::nothrow`) and every `operator delete`
    form with a counting pair over `malloc`. Each block carries a 16-byte header holding its size.
  - It counts allocations, throwing allocations, live bytes and the peak of live bytes.
  - Under `#if __has_include("OtFixedVector.h")` (our engine, from Task 7 on), it binds each `ot::Buffer` to a block
    of `ot::Buffer::bytesFor(4096)` bytes before first use. Upstream's buffer reserves for itself.
  - It prints four Markdown tables to stdout, one row per entry. Times are `std::chrono::steady_clock` microseconds.
  - **Sizes** (printed once): `sizeof` of `ot::GlyphInfo`, `GlyphPosition`, `Buffer`, `Face`, `Plan`, `PlannedLookup`
    and `Stage`.
  - **Faces** (one row per font):
    - `Face::init` with the font's `CPac` and without it, best of 100 each;
    - a copy of a `Face`, best of 1,000. This stands in for deviation 12's re-point, which moves the same views.
  - **Plans** (one row per font and route).
    - The routes:
      - `computed`: `CPac` and `CPpl` dropped from the `FaceTables` (the TrueType route);
      - `stored`: `CPpl` dropped, `CPac` kept, for a font that has a `CPac`.
    - The columns:
      - `Plan::build`'s result;
      - `lookups[GSUB].size()` and `lookups[GPOS].size()`;
      - the two `stages` sizes;
      - `memoryBytes()`;
      - the heap peak above the live bytes before the call;
      - the allocation count;
      - the build time, best of 20.
  - **Shaping** (one row per font, on route `computed`, with no language and `Scale::set(2133, 33, upem)`). Each line
    of the font's corpus is shaped once, all with one buffer. The columns:
    - lines, the most code points in one line, the most glyphs out, and the largest glyphs minus code points;
    - lines that failed with `successful` false, and lines that failed with it true;
    - allocations and the heap peak during the loop;
    - the mean and the maximum time per line.
- [ ] **Step 6: Build with our compiler and flags** (Bash; `U=C:/tmp/wm2-measure/upstream`, `B=C:/tmp/wm2-measure/bin`):
  ```
  g++ -std=c++20 -O2 -fno-exceptions -static -I$U/lib/OtShaper -I$U/lib/Utf8 $U/test/ot_shaper/OtShapeCli.cpp $U/lib/OtShaper/*.cpp -o $B/ot_shape_cli_upstream.exe
  g++ -std=c++20 -O2 -fno-exceptions -static -I$U/lib/OtShaper C:/tmp/wm2-measure/tools/OtShaperProbe.cpp $U/lib/OtShaper/*.cpp -o $B/ot_shaper_probe_upstream.exe
  ```
  Expected: both link. A compile error in upstream's code under these flags is a finding: record it, and stop as for
  a miss.
- [ ] **Step 7: Run the probe.**
  - Pass the four fixtures (`$U/test/complex_shaper/data/NotoSans<Script>-Regular.layout`), then the 40 Indic TTFs,
    each after its script, and write the output to `logs/probe-upstream.md`.
  - Expected: every `computed` build is ok, `stored` is ok on the four fixtures, and both columns of failed lines are
    0 (the 2n + 16 bound holds on the corpus).
  - A failure here is not a pass line, but the design's memory table rests on these figures, so Task 4 reports it.
- [ ] **Step 8: The sizes on the C3.**
  - `tools/sizeof_c3.cpp` includes `OtShaper.h` and has one `static_assert(sizeof(T) == 0, "T")` for each type in the
    Sizes table.
  - Compile it: `/c/pio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-g++.exe -std=gnu++2a -Os -fno-exceptions
    -fsyntax-only -I$U/lib/OtShaper tools/sizeof_c3.cpp`.
  - Expected: one error per assert, each reading `the comparison reduces to '(N == 0)'`, where N is the size on the C3.
  - Record N beside the host's sizes and the design's figures: `GlyphInfo` 20, `GlyphPosition` 20, a 62 B slot,
    `matchStack` 1,152 B, a `Face` of about 120 B.
- [ ] **Step 9: The oracle.** `tools/oracle.sh` runs one `compare_harfbuzz.py` run per font, size and language, and
  appends each run's summary line to the log.
  - The fonts: the four fixtures and the 40 Indic TTFs.
  - The sizes: `--scale 2133 --ppem 33` (16 pt at 150 dpi, upstream's default) and `--scale 1600 --ppem 25` (12 pt).
  - The languages: none, and the script's own:
    - Devanagari: `hi`, `mr`, `ne`, `sa`;
    - Bengali: `bn`, `as`;
    - the others: `pa`, `gu`, `or`, `ta`, `te`, `kn`, `ml`, `si`.
  - Each run: `python -I -X utf8 tools/pyrun.py pydeps upstream/test/ot_shaper/compare_harfbuzz.py --cli <cli> --font
    <font> --scale S --ppem P [--lang L] --show 20 corpus/<script>.txt`.
  - That is about 210 runs. The script prints a total and exits 1 when any run differs.
  - Run `tools/oracle.sh bin/ot_shape_cli_upstream.exe logs/oracle-upstream.log`. Expected: every run reports
    `<font>: N/N lines identical`.
  - If every line of a run mismatches by one trailing glyph, a CR reached the CLI. That is a fault of the harness, not
    the engine.
- [ ] **Step 10: Record item 1.**
  - Write the tools' versions, the four probe tables, the C3 sizes, an oracle table (font, size, language, lines,
    identical) and the verdict.
  - Pass: every oracle run reports N/N.
  - Miss: keep the first 20 mismatches of each failing run, do not start Tasks 2-3, and report to the maintainer.

### Task 2: Item 2, the engine's flash in `default`

**Files:**
- Scratch only, never committed: a worktree `C:/tmp/wt-wm2-scratch`, and `src/OtShaperFlashProbe.cpp` in it. Task 9
  reuses the worktree and then removes it.
- Modify: the measurements file (section "Item 2")

**Interfaces:**
- Produces: the baseline sizes (`default` and `gh_release`, Flash and RAM) that Task 10 compares PR 1 against, and the
  engine's cost E.

- [ ] **Step 1: The scratch worktree.**
  - `git -C C:/tmp/wt-wm2 worktree add --detach C:/tmp/wt-wm2-scratch master`.
  - `git -C C:/tmp/wt-wm2-scratch submodule update --init --reference C:/_development/witchhunt-reader/freeink-sdk
    freeink-sdk`.
- [ ] **Step 2: The baseline, on master as it is,** the design's "same tree without the call".
  - Build `default`, then `gh_release`, and record Flash and RAM for each.
  - These are master's own sizes, which Task 10 checks PR 1 against.
- [ ] **Step 3: Copy upstream's engine in:** `git -C C:/_development/crosspoint-reader archive pr-3787 lib/OtShaper
  lib/Utf8/IndicScripts.h lib/Utf8/IndicReorder.h lib/Utf8/IndicReorder.cpp | tar -x -C C:/tmp/wt-wm2-scratch`.
- [ ] **Step 4: The call.** Write `src/OtShaperFlashProbe.cpp`:
  - Every input is a `volatile` global, so the compiler folds nothing: a run flag, a font pointer and its length, the
    script and the language string.
  - A function marked `__attribute__((constructor))` returns unless the flag is set. When it is set, it calls:
    - `ot::tablesFromSfnt` and `Face::init`;
    - `Plan::build`, the runtime planner of our route, and not `Plan::load`, which our route never calls;
    - `Scale::set`;
    - `ot::shape` on two code points, into a local `ot::Buffer`. Under `__has_include("OtFixedVector.h")` it first
      binds the buffer to a local 64-slot array;
    - `indicReorderForDisplay`, which PR 3 links for bitmap faces.
  - Nothing sets the flag, so the code is linked and never run.
- [ ] **Step 5: Build again.** Build `default`, then `gh_release`, with the call, and record Flash and RAM.
- [ ] **Step 6: Evaluate and record item 2.**
  - E is the `default` delta. The allowance A is 15,360 B (15 KB), for the glue, the `CPac` writer and the glyph-run
    draw that PRs 2-3 add; the design gives no figure for it.
  - P2 is step 1 PR 2's `default` delta, as its PR body records it. Its plan records `Flash:` in its final task. If no
    figure exists yet, use 20,480 B and mark the free-slot figure provisional; PR 3 measures it for real.
  - Pass:
    - E + A ≤ 92,160 B;
    - 6,553,600 − (`default` with the call + A + P2) ≥ 256,000 B. The app slot is 0x640000 B, from `partitions.csv`.
  - Also record the `gh_release` delta and the RAM delta (upstream's was +224 B), and compare E with the design's
    estimate of 55-75 KB.

### Task 3: Item 3, the merged fonts of decision 6

**Files:**
- Create (workspace): `tools/merge_families.py`, `tools/arabic_sample.py`
- Scratch only: `C:/tmp/wt-wm2-scratch/test/ttf_font/Step2MergedFamiliesProbe.cpp`, added to that tree's
  `test/ttf_font/CMakeLists.txt`
- Modify: the measurements file (section "Item 3")

**Interfaces:**
- Consumes: Task 1's `dl/fonts/` and `corpus/`.
- Produces: `merged/<Family>/<Family>-<Style>.ttf`, and beside each one a `.sizes` file holding
  `cpac=<bytes> gdef=<bytes>`.

- [ ] **Step 1: The Arabic sample.** `tools/arabic_sample.py corpus` writes `corpus/arabic.txt`, one string a line:
  - every `ARABIC LETTER` in U+0620-064A and U+0671-06D3, by itself, after U+0628, between two U+0628, and before
    U+0628;
  - U+0644 followed by each of U+0622, U+0623, U+0625 and U+0627;
  - U+0628 with each of U+064B-0652, and with U+0651 U+064E;
  - U+0628 U+200C U+0628.
- [ ] **Step 2: The merges and their checks.** `python -I -X utf8 tools/pyrun.py pydeps tools/merge_families.py
  dl/fonts merged corpus logs/fonts.md`.
  - **Latin in the script fonts:** for each of the 44 script files, count how many of the 52 letters U+0041-005A and
    U+0061-007A it maps, and how many of the 95 code points U+0020-007E. This tests #3787's claim that Noto's Indic
    TTFs carry no Latin.
  - **The pairs:** the 22 families, regular with regular and bold with bold:
    - each `NotoSans<X>` with Noto Sans;
    - each `NotoSerif<X>` with Noto Serif;
    - `NotoNaskhArabic` with Noto Serif, and `NotoSansArabic` with Noto Sans.
  - The recipe is decision 6 as the maintainer refined it on 2026-11-02, after the first run of this task: Latin
    letters from the Latin font, the rest from the script font, and `vhea`/`vmtx` dropped. The first recipe's figures
    stay in the measurements file, marked superseded.
  - **The script font:** A-Z and a-z leave its cmap, in every subtable. Its glyphs stay, so its glyph IDs and its own
    shaping are unchanged. Only Noto Sans Sinhala maps any; the other files go into the merge as they are.
  - **Noto's Latin** is a `fontTools.subset` of the Latin font:
    - The ranges kept: U+0020-007E, U+00A0-024F, U+2000-206F, U+20A0-20CF, U+2122 and U+FFFD, less every code point
      the script font maps other than A-Z and a-z. So the Latin letters always come from the Latin font. Space,
      digits, punctuation and everything else the script font maps come from the script font.
    - The options: `layout_features='*'`, `glyph_names=True`, `notdef_outline=True`, `name_IDs='*'`, `hinting=False`.
  - **upem:** if the subset's differs from the script font's, scale the subset with
    `fontTools.ttLib.scaleUpem.scale_upem`, and record it.
  - **The merge:** `fontTools.merge.Merger(options=Options(drop_tables=["vhea", "vmtx"])).merge([script, latin])`.
    - The script font goes first, so its glyph order, cmap and names win.
    - `vhea` and `vmtx` are dropped: Noto Serif Bengali and Noto Serif Tamil have them, Noto Serif does not, and
      fontTools' `vhea` merge raises on the missing table.
    - A merge that raises is recorded, and that family misses.
  - **Sizes:** for each merged style, record its bytes, GDEF's bytes and `CPac`'s.
    - `CPac` = 16 + the sum, over GSUB and GPOS, of (28 × lookups + 24 × subtables). That is upstream's format
      (`docs/file-formats.md` of #3787), counted on the merged font.
    - Write both into the `.sizes` file.
  - **The oracle against the script's own font,** with HarfBuzz only, at 16 pt (2133/33):
    - Every line of the script's corpus is shaped by the script's own file and by the merged one. The Indic scripts
      use `corpus/<script>.txt` with the languages of Task 1. Arabic uses `corpus/arabic.txt` with direction rtl,
      script `Arab`, and no language, `ar`, `fa` and `ur`.
    - Glyph IDs, advances and offsets must be equal on every line.
  - **Latin kept whole:** each line of the Latin corpus, restricted to Latin letters, shapes identically in the
    merged family and in Noto Sans or Serif itself.
    - A line is cut into its longest runs of A-Z and a-z. Each run is shaped by the merged font and by the Latin font
      file (HarfBuzz, 2133/33, script and direction guessed).
    - They must match in glyph names, advances and offsets. Each merged glyph's name is mapped back through the
      merger's own renaming (`fontTools.merge.cmap.computeMegaGlyphOrder`, which adds `.n` to a duplicate name).
      A glyph of the script font's part never matches.
    - The first recipe's check compared whole lines with the subset. It could not hold: the subset leaves out the
      script font's code points, the space among them.
    - The Latin corpus:
      - `The quick brown fox jumps over the lazy dog.`
      - `AVATAR Wave fi fl ffi`
      - `“Quotes” — 1,234.56 (x) [y] {z} €`
- [ ] **Step 3: The fit, in the scratch worktree.**
  - Configure that tree's host tests as under Global Constraints, then build `TtfFontTest` only, with
    `Step2MergedFamiliesProbe.cpp` added to it.
  - The test reads the folder named by `STEP2_MERGED`. For each family it:
    - probes each style the way `ShippedFamiliesTest` does (`MemoryFile`, `ttf::probeFile`);
    - computes the flash bytes: the file + `CPac` padded to 4;
    - computes the SD bytes: `sdBytes` + 16 + GDEF padded to 4 + 16 + `CPac` padded to 4. GDEF and `CPac` become two
      more tables of the block (deviations 10 and 4);
    - calls `glyphstore::planLayout` with both styles in `kModeFlash`, else both in `kModeSd`;
    - uses the X3's font area, 3,473,408 B (3,538,944 − 65,536), and 32 free map pages, as `ShippedFamiliesTest`
      does;
    - cross-checks with `ttf::fitsStore`, and prints the mode, flash, SD or none.
  - Run `STEP2_MERGED=C:/tmp/wm2-measure/merged build/test/ttf_font/TtfFontTest.exe
    --gtest_filter=Step2MergedFamilies.*`.
- [ ] **Step 4: The in-place clause.** Record that item 3 has nothing to measure for it.
  - The gate keys on the book's language with a TrueType font (`InPlaceBuildGate`,
    `TypographyProfile::cjkRepertoire`), not on a family's mode.
  - PR 3 extends that rule to the shaped scripts (the design's "The X4's in-place build").
- [ ] **Step 5: Record item 3.**
  - One table row per family and style: bytes, GDEF, `CPac`, flash bytes, SD bytes, mode, both oracle results, and
    the Latin counts.
  - Pass: all 22 families merged; both checks hold, every corpus line equal against the script's own font and every
    Latin letter run equal against the Latin font; and every family fits in flash or SD mode.

### Task 4: The gate

**Files:**
- Modify: the measurements file (section "The gate")

- [ ] **Step 1: Write the gate.**
  - Give each item's figure against its pass line, with pass or miss.
  - Add an "estimate → measured" table for the design's rows this phase measured: the glyph buffer per call, a face,
    a plan, the runtime filters, `CPac`, and flash.
- [ ] **Step 2: Commit** on `docs/writing-modes-step2`: `git add
  docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`, then `git commit -m "docs(writing-modes):
  step 2 measure first, items 1-3"`.
- [ ] **Step 3: Report to the maintainer,** one line per item.
  - On any miss, stop: the design reopens. For item 1 that means no PR until the difference is understood.
  - When all three pass, PR 1 starts with Task 5.

---

## PR 1: the engine

The branch is `feat/writing-modes-step2-pr1`, cut from `docs/writing-modes-step2` after Task 4. So the PR carries the
design, this plan and the measurements, as step 1's #599 carried its own design and plan. It opens against master.

### Task 5: Import upstream's engine, the Indic tables and their tests

**Files:**
- Create, verbatim from `pr-3787` (45 files):
  - `lib/OtShaper/`: `OtBuffer.cpp`, `OtBuffer.h`, `OtCategories.h`, `OtContext.cpp`, `OtFace.cpp`, `OtFace.h`,
    `OtGpos.cpp`, `OtGsub.cpp`, `OtIndic.cpp`, `OtLayout.cpp`, `OtLayout.h`, `OtLayoutInternal.h`, `OtNormalize.cpp`,
    `OtPlan.cpp`, `OtPlan.h`, `OtPlanBuild.cpp`, `OtShaper.cpp`, `OtShaper.h`, `OtShaperInternal.h`, `OtSort.h`,
    `OtSyllableMachines.h`, `OtTable.cpp`, `OtTable.h`, `OtUnicodeData.h`, `OtUse.cpp`;
  - `lib/Utf8/IndicScripts.h`, `IndicReorder.h`, `IndicReorder.cpp`;
  - `scripts/gen_ot_shaper_machines.py`, `gen_ot_shaper_tables.py`, `gen_ot_shaper_tables.cc`;
  - `test/ot_shaper/CMakeLists.txt`, `OtShaperTest.cpp`, `OtShapeCli.cpp`, `compare_harfbuzz.py`, `generate_corpus.py`;
  - `test/complex_shaper/ExpectedShaping.h`, `generate_fixtures.py`, and in `data/`: `NotoSansBengali-Regular.layout`,
    `NotoSansDevanagari-Regular.layout`, `NotoSansSinhala-Regular.layout`, `NotoSansTamil-Regular.layout`, `OFL.txt`;
  - `test/indic_text/CMakeLists.txt`, `IndicTextTest.cpp`.
- Not taken:
  - `test/complex_shaper/CMakeLists.txt`, `ComplexShaperTest.cpp` and `stubs/`, which build `ComplexShaper` (the
    design's "Not taken");
  - upstream's `lib/Utf8/Utf8.{h,cpp}` and `Utf8ComposeTable.h`. Ours stay; step 1 PR 1 added the argument-order
    overload that `IndicReorder.cpp` and the test use.
  - `generate_fixtures.py` comes in, but cannot run here, because it imports upstream's `shaping_blob.py`, which is
    not taken. So the fixtures are frozen at upstream's bytes.

**Interfaces:**
- Produces: upstream's API, unchanged: `ot::shape`, `ot::maxRunLength`, `ot::languageTagsFor` (`OtShaper.h`);
  `ot::Face`, `ot::FaceTables`, `ot::tablesFromSfnt`, `ot::Scale` (`OtFace.h`); `ot::Plan` (`OtPlan.h`); `ot::Buffer`,
  `ot::RunContext`, `ot::setHeapCheck` (`OtBuffer.h`); `indic::scriptOf`, `indic::syllableBreakAllowed`
  (`IndicScripts.h`); `indicReorderForDisplay` (`IndicReorder.h`).

- [ ] **Step 1: The branch.** In `C:/tmp/wt-wm2`, run `git switch -c feat/writing-modes-step2-pr1 docs/writing-modes-step2`.
  Initialise the submodule as in Task 2 Step 1, and configure the host tests.
- [ ] **Step 2: Copy the 45 files** with `git -C C:/_development/crosspoint-reader show pr-3787:<path> > <path>`, then
  check them. For each file, `git hash-object <path>` must equal its blob in `git -C C:/_development/crosspoint-reader
  ls-tree pr-3787 <path>`. Expected: 45 of 45.
- [ ] **Step 3: Commit the import by itself,** before anything builds it.
  - The message: `chore(shaping): import upstream OtShaper, the Indic tables and their tests (crosspoint-reader #3787)`.
  - The body says:
    - the source: #3787, "feat: add Indic language support without HarfBuzz", by SSafayet (MIT), at `dd45ba61`, with
      the files as at `0fbd1575` (blob-identical);
    - what `lib/OtShaper` is: a port of HarfBuzz 14.5.0's Indic and USE shapers and its GSUB/GPOS engine, whose MIT
      notice comes with the credits commit;
    - that the fixtures are layout subsets of Noto Sans (OFL), and that nothing builds any of this yet.
  - End it with the trailer `Co-authored-by: SSafayet <saimoom.akash@gmail.com>`.

### Task 6: Host build glue, and the engine against upstream's stored HarfBuzz output

**Files:**
- Modify: `test/CMakeLists.txt`
- Create: `test/ot_shaper_port/CMakeLists.txt`, `test/ot_shaper_port/OtShaperPortTest.cpp`

**Interfaces:**
- Consumes: Task 5's files.
- Produces:
  - in `test/CMakeLists.txt`, right after `set(REPO_ROOT ...)`, upstream's two lines: `file(GLOB OT_SHAPER_SOURCES
    ${REPO_ROOT}/lib/OtShaper/*.cpp)` and `set(OT_SHAPER_INCLUDE_DIR ${REPO_ROOT}/lib/OtShaper ${REPO_ROOT}/lib/Memory)`.
    Ours adds `lib/Memory`, which Task 8's `makeUniqueNoThrow` needs; a `#` comment says so;
  - the executables `OtShaperTest`, `ot_shape_cli` and `IndicTextTest` (upstream's), and `OtShaperPortTest` (ours);
  - in `OtShaperPortTest.cpp`, the helper `std::vector<StoredGlyph> shapeAsStored(const ot::Face& face, const ot::Plan&
    plan, const char* utf8)`, with `struct StoredGlyph { uint32_t gid; int advance12_4; int dx; int dy; bool
    operator==(const StoredGlyph&) const = default; }`. It returns an empty vector when `ot::shape` fails.

- [ ] **Step 1: The glue.** Add the two lines above. Next to `add_subdirectory(text_shaping)`, add
  `add_subdirectory(indic_text)` and `add_subdirectory(ot_shaper)`. `complex_shaper` gets no entry, since it holds data
  only.
- [ ] **Step 2: Build and run upstream's tests:** `cmake --build build/test -j 6 --target OtShaperTest IndicTextTest
  ot_shape_cli`, then `ctest --test-dir build/test -R "OtShaper|OtPlan|Indic|Bengali"`.
  - Expected: all 34 tests pass unchanged, 11 in `OtShaperTest` and 23 in `IndicTextTest`.
  - Fix only what fails to configure or compile in our glue, and say what in the commit.
- [ ] **Step 3: The generators reproduce their headers.**
  - Run `python -I -X utf8 scripts/gen_ot_shaper_machines.py`, then `git diff --exit-code
    lib/OtShaper/OtSyllableMachines.h`.
  - Run `python -I -X utf8 scripts/gen_ot_shaper_tables.py`, then `git diff --exit-code lib/OtShaper/OtUnicodeData.h`.
    This script downloads HarfBuzz 14.5.0's tarball (its sha256 is pinned in the script) and builds it with UCRT64's
    `c++`, which takes minutes.
  - Expected: no difference. Git's autocrlf hides CRLF.
  - If a difference goes away once clang-format 21 runs on the regenerated file, record that in the commit body.
  - Restore both files with `git checkout --`. Any other difference: stop and report.
- [ ] **Step 4: Format check** on the imported `.h`/`.cpp`. Expected: clean. If not, make the fix whitespace-only, in
  this commit, and list the files in its body.
- [ ] **Step 5: Commit** `build(shaping): build OtShaper, IndicReorder and their tests on the host`.
- [ ] **Step 6: Write the reference tests** in `OtShaperPortTest.cpp`. `ComplexShaperTest`'s HarfBuzz check moves to
  the engine.
  - `OtShaperReference.BuiltPlansMatchHarfBuzz` covers all 56 words of `kShapingFixtures` (`ExpectedShaping.h`):
    Bengali 26, Devanagari 16, Tamil 8 and Sinhala 6. For each word:
    - the fixture's font, with `CPac` and `CPpl` dropped from its `FaceTables` (the TrueType route);
    - `Plan::build(face, script, ot::languageTagsFor(word.language))`;
    - `Scale::set(kFixturePpem26_6, (kFixturePpem26_6 + 32) >> 6, upem)`, that is 2133 and 33;
    - `EXPECT_EQ(shapeAsStored(...), the word's ExpectedGlyph list)`, with the script and the word in the message.
  - `OtShaperReference.StoredFiltersMatchHarfBuzz` is the same test with `CPac` kept and `CPpl` dropped.
  - `shapeAsStored` converts the way `ExpectedShaping.h` was made (ComplexShaper's `appendGlyphs`):
    - `advance12_4 = (xAdvance + 2) >> 2`;
    - `dx = roundPixels(xOffset)`;
    - `dy = -roundPixels(yOffset)`;
    - where `roundPixels(v) = v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64)`.
  - At file scope: `static_assert(ot::MAX_NESTING_LEVEL == 8)`, `static_assert(ot::MAX_CONTEXT_LENGTH == 64)` and
    `static_assert(ot::MAX_PLANNED_LOOKUPS == 4096)`.
  - `test/ot_shaper_port/CMakeLists.txt` follows `test/ot_shaper/CMakeLists.txt`. It also includes
    `${REPO_ROOT}/test/complex_shaper`, sets `SHAPING_FIXTURE_DIR`, and is added to `test/CMakeLists.txt`.
- [ ] **Step 7: Run them.** `ctest --test-dir build/test -R OtShaperReference`. Expected: PASS. They describe the
  import, so if one fails, the oracle and the fixtures disagree: stop and report.
- [ ] **Step 8: Commit** `test(shaping): the engine against upstream's stored HarfBuzz output`.

### Task 7: Deviation 1, the buffer: a fixed-capacity view type, bound to the caller's memory

**Files:**
- Create: `lib/OtShaper/OtFixedVector.h`, `test/ot_shaper_port/FixedVectorTest.cpp`
- Modify:
  - `lib/OtShaper/OtBuffer.h`, `lib/OtShaper/OtBuffer.cpp`;
  - `lib/OtShaper/OtNormalize.cpp`: the three `std::vector<GlyphInfo>&` locals in `reorderMarks` and `recompose`;
  - `lib/OtShaper/OtUse.cpp`: `std::vector<uint16_t>& index`;
  - `lib/OtShaper/OtShaper.h`: `shape()`'s comment only;
- Modify: `test/ot_shaper/OtShaperTest.cpp`, `test/ot_shaper/OtShapeCli.cpp` (see Step 4)
- Modify: `test/ot_shaper_port/OtShaperPortTest.cpp`, `test/ot_shaper_port/CMakeLists.txt`

**Interfaces:**
- Produces, in `lib/OtShaper/OtFixedVector.h` (our file; its header says `witchhunt: deviation 1` and why):
  ```cpp
  namespace ot {
  // The calls lib/OtShaper makes of std::vector, over memory its owner binds; it never allocates. A growth past
  // the capacity changes nothing, returns false (insert: nullptr) and clears the flag given to bind().
  template <typename T>  // static_assert(std::is_trivially_copyable_v<T>)
  class FixedVector {
   public:
    FixedVector() = default;                        // unbound: capacity 0
    FixedVector(const FixedVector&) = delete;
    FixedVector& operator=(const FixedVector&) = delete;
    FixedVector(FixedVector&& other) noexcept;     // other is left unbound
    FixedVector& operator=(FixedVector&& other) noexcept;
    void bind(T* data, size_t capacity, bool* fits = nullptr);  // size 0
    size_t size() const;  size_t capacity() const;  bool empty() const;
    T* data();  const T* data() const;
    T* begin();  T* end();  const T* begin() const;  const T* end() const;
    T& operator[](size_t i);  const T& operator[](size_t i) const;
    T& back();  const T& back() const;
    void clear();
    bool push_back(const T& value);
    bool resize(size_t n);                  // new elements value-initialised
    bool assign(size_t n, const T& value);
    T* insert(T* at, const T& value);
    T* erase(T* at);
    void swap(FixedVector& other);          // exchanges data, size, capacity and flag
  };
  }  // namespace ot
  ```
- Produces, in `struct ot::Buffer`:
  - `info`, `pos`, `scratch` and `indices` become `FixedVector<GlyphInfo>`, `FixedVector<GlyphPosition>`,
    `FixedVector<GlyphInfo>` and `FixedVector<uint16_t>`.
  - New members:
    ```cpp
    static constexpr size_t SLOT_BYTES = 2 * sizeof(GlyphInfo) + sizeof(GlyphPosition) + sizeof(uint16_t);  // 62
    static constexpr size_t bytesFor(unsigned slots) { return slots * SLOT_BYTES + alignof(GlyphInfo) - 1; }
    void setStorage(void* block, size_t bytes);
    Buffer() = default;
    Buffer(const Buffer&) = delete;             // its arrays' flag is its own `successful`
    Buffer& operator=(const Buffer&) = delete;
    ```
  - `setStorage` aligns the block's start up to `alignof(GlyphInfo)`, and its slot count is the bytes that remain
    divided by `SLOT_BYTES`. It binds the four arrays empty, in the order `info`, `scratch`, `pos`, `indices`, each to
    that many slots and each with the flag `&successful`. A null block or 0 bytes means no storage.
  - `prepare(maxGlyphs)` keeps upstream's resets and its `maxGlyphs > UINT16_MAX + 1u` check. It then binds the four
    arrays afresh, which undoes a swap of `info` and `scratch`, and returns `maxGlyphs <= slots`.
    - There is no heap check and no `reserve()`.
    - A false return leaves `successful` true, as upstream's refused reservation does, and `maxRunLength` reads the
      bound capacity.
  - These comments switch from reservation to storage wording, each marked: the `Buffer` comment and `prepare()`'s,
    `HeapCheck`'s "(plan filters, buffer growth)" (which becomes "(plan lists and filters)"), and `ot::shape`'s "the
    heap check refused the buffer's reservation".

- [ ] **Step 1: Write the failing tests.**
  - In `FixedVectorTest.cpp`, the suite `FixedVector`, each test passing a `bool ok = true` to `bind`:
    - `BindsEmpty`: bound over `int[4]`, the vector has size 0, capacity 4, is empty, and `data()` is the array.
    - `PushBackFillsThenRefuses`: four `push_back` calls return true. The fifth returns false, the size stays 4, the
      contents stay {1, 2, 3, 4}, and `ok` is false.
    - `InsertShiftsTheTail`:
      - {1, 2, 3} with `insert(begin() + 1, 9)` gives {1, 9, 2, 3} and returns `begin() + 1`;
      - an insert into a full vector returns nullptr, changes nothing and clears `ok`.
    - `EraseShiftsTheTailDown`: {1, 2, 3, 4} with `erase(begin() + 1)` gives {1, 3, 4} and returns `begin() + 1`.
    - `ResizeValueInitialises`:
      - `resize(3)` on {7} gives {7, 0, 0}, and `resize(1)` gives {7};
      - `resize(5)` at capacity 4 returns false, leaves the size as it was and clears `ok`.
    - `AssignFills`: `assign(3, 5)` gives {5, 5, 5}. `assign(5, 1)` returns false and changes nothing.
    - `SwapExchangesBindings`: with `a` over A holding 2 elements and `b` over B holding 1, after `a.swap(b)`:
      `a.data() == B`, `a.size() == 1`, `b.data() == A` and `b.size() == 2`.
    - `MoveLeavesTheSourceUnbound`: after `c = std::move(a)`, `a.capacity() == 0` and `c` holds a's elements.
    - `AnUnboundVectorRefusesEverything`: capacity 0, and `push_back` returns false.
  - In `OtShaperPortTest.cpp`, the suite `OtShaperBuffer`:
    - `MaxRunLengthIsTwoNPlusSixteen`: after `setStorage` over `bytesFor(S)`, `ot::maxRunLength` is 0 for S = 16, 0
      for 17, 1 for 18, 32 for 80 and 48 for 112.
    - `NoStorageShapesNothing`: on a buffer without storage, `ot::shape` of हिन्दी returns false, `successful` stays
      true, and `maxRunLength` is 0.
    - `ARunPastTheStorageFails`, at S = 80:
      - a 33-code-point Devanagari run (हिन्दी five times, then क ि क) returns false with `successful` true;
      - its first 32 code points return true, with the glyphs a 256-slot buffer gives.
    - `AFailedRunLeavesTheBufferUsable`:
      - The face is the Tamil fixture with its GSUB replaced by one MultipleSubst lookup (glyph 5 to ten of glyph 6),
        built as upstream's `initWithGsub` does. The table, as big-endian u16:
        `1 0 0 0 10 | 1 4 | 2 0 1 8 | 1 30 1 8 | 10 6 6 6 6 6 6 6 6 6 6 | 1 1 5`. That is the header with the
        LookupList at 10; the Lookup at 14, of type 2; MultipleSubstFormat1 at 22, its coverage at +30 and its one
        Sequence at +8; the Sequence at 30; the Coverage at 52.
      - On a buffer bound to `bytesFor(64)` bytes: `prepare(22)`, then push three `GlyphInfo` with `codepoint` 5
        and `mask` 1, then
        `ot::applyLookup(face, scale, buffer, ot::GSUB, 0, settings)` with `settings.mask = 1`. Then `successful` is
        false, since the third expansion passes 22.
      - Then, on the same buffer, `ot::shape` of தமிழ் with the unmodified fixture's face and plan gives what a fresh
        buffer gives.
    - `TheNextRunUndoesASwap`: shape েদের, then হৃদয় on the same buffer. The glyphs equal a fresh buffer's for
      হৃদয়. Normalisation swaps `info` and `scratch` on every run, and েদের, a broken syllable, also takes a dotted
      circle, which swaps them again.
- [ ] **Step 2: Run them.** `cmake --build build/test -j 6 --target OtShaperPortTest`. Expected: it fails to compile,
  because `OtFixedVector.h`, `setStorage` and `bytesFor` do not exist yet.
- [ ] **Step 3: Implement** `OtFixedVector.h` and the `Buffer` changes. In `OtNormalize.cpp` and `OtUse.cpp`, the
  locals become `FixedVector<...>&`, each marked. `OtShaper.cpp` needs no change, since its calls are the view's.
- [ ] **Step 4: Bind the buffers in the tests,** each changed line marked:
  - `OtShaperTest.cpp`: `shapeWord`'s buffer is bound to a static block of `bytesFor(256)` bytes, and in
    `DropsExtensionLookupsThatMixSubtableTypes` the buffer is bound to `bytesFor(8)` bytes before `prepare(8)`;
  - `OtShapeCli.cpp`: the buffer is bound to `bytesFor(4096)` bytes. A longer line prints `FAIL`;
  - our reference tests bind theirs to `bytesFor(256)` bytes.
- [ ] **Step 5: Run** `ctest --test-dir build/test -R "OtShaper|OtPlan|FixedVector|Indic|Bengali"`. Expected: every
  test passes, upstream's 34 included.
- [ ] **Step 6: Format check, then commit** `feat(shaping): the shaping buffer in the caller's memory, nothrow
  (deviation 1)`.

### Task 8: Deviation 1, the plan: lists, filters and the planner's scratch in nothrow blocks

**Files:**
- Modify: `lib/OtShaper/OtPlan.h`, `OtPlan.cpp`, `OtPlanBuild.cpp`, `OtLayout.h`, `OtLayout.cpp`
- Modify: `test/ot_shaper/OtShaperTest.cpp` (`ConverterFiltersMatchWhatTheShaperComputes`)
- Create: `test/ot_shaper_port/OtShaperAllocTest.cpp`; modify `test/ot_shaper_port/CMakeLists.txt`

**Interfaces:**
- Consumes: `ot::FixedVector` (Task 7), and `makeUniqueNoThrow<uint8_t[]>` from `lib/Memory/Memory.h`.
- Produces:
  - `Plan`:
    - `lookups[2]` and `stages[2]` become `FixedVector<PlannedLookup>` and `FixedVector<Stage>`;
    - the private `lookupDigests_`, `subtableStarts_` and `subtableDigests_` become `FixedVector<uint8_t>`,
      `FixedVector<uint32_t>` and `FixedVector<uint8_t>`;
    - new private members, all marked:
      ```cpp
      bool allocateLists(int table, size_t lookupCount, size_t stageCount);       // binds lookups[table], stages[table]
      bool allocateFilters(int table, size_t lookupCount, size_t subtableCount);  // binds the three filter arrays
      std::unique_ptr<uint8_t[]> listBlock_[2];
      std::unique_ptr<uint8_t[]> filterBlock_[2];
      bool listsFit_ = true;  // cleared by a refused growth of lookups[] or stages[]
      ```
    - `Plan` is move-only. `*this = Plan()` frees the blocks, as upstream's assignment freed its vectors.
  - `void appendSubtableDigests(const Face& face, int table, uint32_t lookupIndex, FixedVector<uint8_t>& out)` stops at
    the first record that does not fit, and writes nothing past `out`'s size.
- The rules:
  - Each block comes from `makeUniqueNoThrow<uint8_t[]>`, right after upstream's `heapAvailable()` check, where
    upstream used to `reserve()`.
  - `Plan::build` allocates, in this order: the builder's scratch, GSUB's lists, GPOS's lists, GSUB's computed filters
    and GPOS's. A table with `CPac`, or with no lookups, gets no filter block. `Plan::read` allocates GSUB's lists and
    GPOS's.
  - A refused list block makes `build` or `load` return false, with the plan reset to empty, as upstream's refused
    heap check does. A refused filter block leaves that table unfiltered (upstream's `continue`).
  - The builder's scratch holds `FeatureBuilder::infos`, `pauses[2]` and `PlanBuilder::features_`. It is one block of
    `MAX_FEATURES` (64) `Info` and `FeatureMap` entries and 2 × `MAX_STAGES` (32) `Pause` entries, about 3.1 KB, freed
    when `build` returns.
    - The two constants live in `OtPlanBuild.cpp`'s anonymous namespace.
    - The block is fixed-size and independent of the font, so it asks no heap check: upstream checks only memory that
      depends on the font or the text.
    - The most any planner asks for today is 47 features (USE) and 17 GSUB stages (Indic).
  - Every array the planner or the loader fills is bound with a flag, and `build()` and `read()` check it before
    `finish()`: a refused growth fails the plan.
  - `buildFilters` drops a table's computed filters when any of its three arrays refused a growth.
  - `memoryBytes()` keeps upstream's formula, which reads capacities.
  - `<vector>` leaves `OtBuffer.h`, `OtPlan.h` and `OtLayout.h`. After this task, `grep -n "std::vector\|reserve("
    lib/OtShaper` finds no code.

- [ ] **Step 1: Write the failing tests** in `OtShaperAllocTest.cpp`, an executable of its own.
  - The harness:
    - It replaces every global `operator new` and `new[]` form (throwing and `std::nothrow`) and every
      `operator delete` form with a counting pair over `malloc`.
    - While armed, it counts throwing calls, nothrow calls and live nothrow bytes.
    - The nothrow forms return nullptr for the `refuseAt`-th call since arming, counted from 0.
    - Words come from `ExpectedShaping.h`, and buffers are bound to static storage.
  - `OtShaperAlloc.NothingThrowingIsAllocated`: armed across `Face::init` (with `CPac` kept and dropped),
    `Plan::build` on both faces, `Plan::load`, and `ot::shape` of each fixture word.
    - Expected: 0 throwing calls, and 0 nothrow calls inside `ot::shape`.
  - `OtShaperAlloc.EachRefusedBlockFailsTheBuildCleanly`, for each fixture with `CPac` and `CPpl` dropped:
    - An unrefused build makes 5 nothrow allocations. If a fixture has no lookups in a table, assert against the
      counts per table instead.
    - Refuse the k-th allocation, for k from 0 to 4:
      - for k < 3, `build` returns false and the plan is empty (`lookups[GSUB].size() == 0`,
        `lookups[GPOS].size() == 0`, `shaper == ShaperKind::Default`), with 0 live bytes;
      - for k ≥ 3, `build` returns true and every word shapes exactly as it does with the unrefused plan.
    - After the plans are destroyed, 0 live bytes remain.
  - `OtShaperAlloc.EachRefusedBlockFailsTheLoadCleanly`: for each fixture as stored, `Plan::load` makes 2
    allocations. Refusing either one makes `load` return false and leaves the plan empty, with 0 live bytes.
  - `OtShaperAlloc.AFailedBuildCanBeRetried`: refuse GSUB's lists (k = 1), disarm, and build the same `Plan` again.
    It returns true, and the words shape as with a fresh plan.
- [ ] **Step 2: Run them.** Expected: they build, and they fail.
  - `NothingThrowingIsAllocated` counts the plan's `std::vector` growth as throwing calls.
  - The refusal tests see no nothrow allocations to refuse.
- [ ] **Step 3: Implement** the rules above in `OtPlan.{h,cpp}`, `OtPlanBuild.cpp` and `OtLayout.{h,cpp}`, each changed
  line marked.
  - In `OtShaperTest.cpp`'s `ConverterFiltersMatchWhatTheShaperComputes`, `subtables` becomes a
    `FixedVector<uint8_t>` bound over a `std::vector<uint8_t>` of
    `ot::Face::subtableCount(face.lookup(t, i)) * ot::DIGEST_BYTES` bytes. Mark the change.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "OtShaper|OtPlan|FixedVector|Indic|Bengali"`. Expected: all
  pass. Then check the grep above.
- [ ] **Step 5: Format check, then commit** `feat(shaping): plans in nothrow blocks, no std::vector left in the
  engine (deviation 1)`.

### Task 9: The oracle and the probe on our engine; the scratch flash build

**Files:**
- Create: `test/ot_shaper_port/OtShaperProbe.cpp`, a copy of `C:/tmp/wm2-measure/tools/OtShaperProbe.cpp` byte for
  byte
- Modify: `test/ot_shaper_port/CMakeLists.txt` (target `ot_shaper_probe`, not a test)
- Modify: the measurements file (new section "PR 1: the engine re-measured")

- [ ] **Step 1: Build ours with the design's flags** (Bash, from `C:/tmp/wt-wm2`):
  ```
  g++ -std=c++20 -O2 -fno-exceptions -static -Ilib/OtShaper -Ilib/Utf8 -Ilib/Memory test/ot_shaper/OtShapeCli.cpp lib/OtShaper/*.cpp -o C:/tmp/wm2-measure/bin/ot_shape_cli_ours.exe
  g++ -std=c++20 -O2 -fno-exceptions -static -Ilib/OtShaper -Ilib/Memory C:/tmp/wm2-measure/tools/OtShaperProbe.cpp lib/OtShaper/*.cpp -o C:/tmp/wm2-measure/bin/ot_shaper_probe_ours.exe
  ```
- [ ] **Step 2: The oracle.** Run `tools/oracle.sh bin/ot_shape_cli_ours.exe logs/oracle-ours.log`, the same matrix
  as Task 1. Expected: every run N/N, with the same line counts as Task 1. A miss stops the PR.
- [ ] **Step 3: The probe.** Run it with Task 1's arguments, writing `logs/probe-ours.md`, and compare it with
  `logs/probe-upstream.md`. Expected:
  - the build results, lookup counts, stage counts and glyph maxima are equal;
  - 0 allocations happen in the shaping loop;
  - the heap peak during a build is no higher than upstream's.
  Run Task 1 Step 8 again against this tree's `lib/OtShaper`, with `-Ilib/Memory` added.
- [ ] **Step 4: The scratch flash build.**
  - Overwrite the scratch tree's copy with this branch's files: `git -C C:/tmp/wt-wm2 archive HEAD lib/OtShaper
    lib/Utf8/IndicScripts.h lib/Utf8/IndicReorder.h lib/Utf8/IndicReorder.cpp | tar -x -C C:/tmp/wt-wm2-scratch`.
  - Keep Task 2's `src/OtShaperFlashProbe.cpp`. Build `default`, then `gh_release`, and record each engine cost
    against Task 2's master baseline, next to upstream's cost from Task 2.
  - Then run `git -C C:/tmp/wt-wm2 worktree remove --force C:/tmp/wt-wm2-scratch`.
- [ ] **Step 5: Commit the probe, then record.**
  - Add `add_executable(ot_shaper_probe OtShaperProbe.cpp ${OT_SHAPER_SOURCES})`, with `${OT_SHAPER_INCLUDE_DIR}`,
    linked to `crosspoint_test_common`, and build it.
  - Write the new section of the measurements file: the oracle table, the two probes side by side, the C3 sizes, and
    the engine's scratch cost.
  - Commit `test(shaping): the measure-first probe as a host tool, and PR 1's figures`.

### Task 10: Credits, roadmap and the PR

**Files:**
- Create: `lib/OtShaper/COPYING`
- Modify: `README.md` ("Vendored third-party components (`lib/`)"), `docs/future_work/writing-modes.md`

- [ ] **Step 1: HarfBuzz's notice.**
  - Fetch `https://raw.githubusercontent.com/harfbuzz/harfbuzz/14.5.0/COPYING` into
    `C:/tmp/wm2-measure/dl/harfbuzz/COPYING`.
  - Check that it begins `HarfBuzz is licensed under the so-called "Old MIT" license.`
  - Copy it to `lib/OtShaper/COPYING` unchanged.
- [ ] **Step 2: The README entry,** in the style of the minibidi entry:
  - **OtShaper**, by SSafayet (crosspoint-reader #3787, MIT): a port of HarfBuzz 14.5.0's Indic and Universal Shaping
    Engine shapers and of its GSUB/GPOS engine. It does the OpenType shaping of the ten Indic scripts.
  - Link https://github.com/crosspoint-reader/crosspoint-reader/pull/3787. HarfBuzz's licence is "Old MIT",
    [`lib/OtShaper/COPYING`](lib/OtShaper/COPYING).
  - Vendored under `lib/OtShaper` at `dd45ba61` (the shaper's files as at `0fbd1575`), with `lib/Utf8/IndicScripts.h`
    and `IndicReorder`.
  - Modified: nothrow fixed-capacity storage (`OtFixedVector.h`) in place of `std::vector`, the design's deviation 1.
  - The test fonts are Noto Sans layout subsets (OFL, `test/complex_shaper/data/OFL.txt`).
  - The web licences page is PR 3's, when the firmware first links the engine.
- [ ] **Step 3: The roadmap.**
  - Row 2's status becomes `in progress: PR 1 #<n> (the engine: crosspoint-reader #3787's OtShaper, nothrow)`, with
    links to the design, the measurements file and this plan.
  - The "Next step." paragraph under "Upstream's shaper (crosspoint-reader #3787)" says the shaper is being ported in
    step 2 and was measured first, and that the allocation fix is offered upstream (decision 7).
  - Row 1 is left alone, since step 1 PR 2 edits it.
- [ ] **Step 4: The whole host suite** on the final tree. Expected: only the known Windows-only failures, and every
  golden byte-identical.
- [ ] **Step 5: The firmware size check.** Build `default`, then `gh_release`, from this branch.
  - Expected: Flash and RAM equal Task 2's baseline. If master has moved since Task 2, compare with a build of the
    merge base instead.
  - A difference must be explained before going on. `IndicReorder.cpp` is compiled into libUtf8, but nothing may link
    it.
- [ ] **Step 6: cppcheck,** which CI runs, with `lib/` in scope:
  - `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high`, in the background.
  - A finding in an imported file gets an inline `// cppcheck-suppress <id>` with a reason, marked
    `// witchhunt: build glue`, and the commit body lists it. That is not a deviation, since no code changes.
- [ ] **Step 7: Format check and the NUL check** on every touched file, then commit `docs(writing-modes): step 2 PR 1,
  the shaping engine`.
- [ ] **Step 8: The PR.**
  - Run `gh pr view --json state` on the branch. Expected: no PR yet.
  - Push, and open a PR against master titled `feat(shaping): the OpenType shaping engine, nothrow (writing modes
    step 2, PR 1)`.
  - The body follows #599's: the design, the plan and the measurements; the import; deviation 1; that nothing calls
    the engine; the oracle and probe figures; the unchanged firmware sizes and the scratch build's engine cost.
  - Then fill row 2's `#<n>` in a commit `docs(writing-modes): step 2 PR 1 is #<n>`, and push. Merging is the
    maintainer's.

### Task 11: The comment on #3787 (decision 7)

**Files:** none.

- [ ] **Step 1: After PR 1 merges,** collect the permalinks:
  - `https://github.com/jpirnay/witchhunt-reader/blob/<merge SHA>/lib/OtShaper/OtFixedVector.h`;
  - `https://github.com/jpirnay/witchhunt-reader/commit/<sha>` for the commits of Tasks 7 and 8.
- [ ] **Step 2: Fill in the draft below** with the counts from the measurements file, and give it to the maintainer.
  Do not post it: decision 7 has the maintainer post it from the maintainer's account.

  > We have ported `lib/OtShaper` from this PR (at dd45ba61) into witchhunt-reader, an X3/X4 fork, file for file. It
  > matches HarfBuzz 14.5 on `generate_corpus.py`'s strings: <runs> runs over <fonts> fonts at two sizes, 0
  > differences. Thank you for it.
  >
  > One change of ours may be useful here. `Buffer::prepare()`, `Plan::read()`, `planLookups()` and `buildFilters()`
  > call `heapAvailable()` and then `reserve()` a `std::vector`, and `FeatureBuilder`'s lists grow by `push_back`
  > unchecked. The check and the allocation are two steps: the check can pass and the allocation still fail, and with
  > no check installed any size passes. Under `-fno-exceptions` a failed vector allocation calls `abort()`.
  >
  > In our port those vectors are one fixed-capacity view type with the same calls (`push_back`, `insert`, `erase`,
  > `resize`, `assign`, `swap`, `size`, `capacity`, `data`). It is bound to memory allocated nothrow:
  > - the buffer's four arrays to one block the caller hands in (`Buffer::setStorage`);
  > - a plan's lists and filters to blocks it allocates once their sizes are known;
  > - the planner's lists to one small block.
  >
  > A refused block fails the plan, or leaves it unfiltered, as a refused heap check does today. Growth past the
  > capacity fails the run, as `canGrow()` does. The limits are unchanged, and so is the output.
  >
  > The view type: <link>. The two commits: <link>, <link>. We are glad to open a PR against your branch if that helps.
