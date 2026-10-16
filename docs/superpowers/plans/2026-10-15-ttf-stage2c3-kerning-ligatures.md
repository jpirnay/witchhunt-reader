# TTF Stage 2c-3: kerning and ligatures. Implementation plan

> **Status (2026-10-16): done.**
> - **SDK PR #7** merged into `witchhunt` as `e140f74`. **App branch:** `feat/ttf-kerning`, pin bumped.
> - **How it ran:** subagent-driven, with a review per task, a whole-branch review and one fix wave.
> - **Counts after the review fixes:**
>   - SDK `run_gpos` 45 checks (two hostile-table checks added);
>   - app `TtfFontTest` 93;
>   - host suite 2,281 before the final fix wave.
> - **Where the code differs from the task code below:**
>   - **`Gpos.cpp`:** out-of-range feature indices count against the walk budget, and the LangSys loop checks
>     the budget.
>   - **SDK docs:** `KernTable.h`, `Gpos.h` and the README say where the readers differ from `FT_Get_Kerning` and
>     from a shaper.
>   - **`TtfOpenType`:** at most 256 subtables per style (was 1,024); a ligature glyph must be `< numGlyphs`.
>   - **`TtfFont`:**
>     - a per-style bit set remembers left codepoints below U+0250 that kern with nothing;
>     - `kRenderVersion` 2, so stores built by 2c-2 are rebuilt;
>     - one line re-wrapped by clang-format.
>   - **More tests:** the subtable cap, truncated and broken layout tables, kerning next to a ligature, and the
>     uncovered-left memo.
> - **On the X3:** the results are in spec Appendix A, "Stage 2c-3". The deferred items are in spec section 11.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A TTF family kerns its text and draws its fi, fl, ff, ffi and ffl ligatures, as the font's own layout tables
say. Layout and drawing get the same values, from the glyph store's pair index after the first time. The layout
tables are read by one implementation, the SDK's, shared by FreeInkFont's FreeType font and our TTF engine.

**Architecture:**

- **In the SDK (freeink-sdk, Tasks 1-3), one PR:**
  - **`FreeInkOpenType`** (new library) builds FreeInkFont's three layout readers on their own, with their headers
    in `libs/font/FreeInkOpenType/include`: a consumer that draws fonts its own way kerns without FreeType.
    FreeInkFont still compiles the same sources, so its consumers see no change in their build.
  - **`Gpos`** reads the `kern` feature as a shaper applies it: activated by any script's DefaultLangSys (Cyrillic
    and Greek kern too), its lookups applied in LookupList order, the first matching subtable in each, the
    adjustments added. A caller that kerns many pairs resolves the subtables once (`ResolveKernSubtables`, byte
    offsets) and asks the resolved overload, which also says whether anything kerns the left glyph.
  - **`OpenType.h`** (internal) holds the bounds-checked view and coverage search `Gsub` and `Gpos` each had.
  - **`KernTable`** reads the legacy `kern` table (format 0) as `FT_Get_Kerning` does, for fonts without GPOS
    kerning.
- **In the app (Tasks 4-7):**
  - **`ttf::Kerning`, `ttf::Ligatures`** (new, `lib/EpdFont/ttf/TtfOpenType.{h,cpp}`) are the glue: a style resolves
    its GPOS subtables once into one heap block, or falls back to the legacy table; the ligatures for ff, fi, fl,
    ffi and ffl become the `EpdLigaturePair` table `EpdFont` already applies, keyed by U+FB00-FB04, and
    `glyphFor()` maps those codepoints to the glyphs GSUB names. ffi and ffl chain through ff, as `EpdFont`
    applies ligatures two at a time.
  - **`TtfFont`** resolves both per style when it opens the family:
    - every size's `EpdFontData` points at its style's ligature pairs;
    - a size's provider answers `kerning(left, right)` from the store's pair index (font units, so every size
      shares a pair), scaled to 4.4 pixels at its own size;
    - on a miss it asks `ttf::Kerning` and stores the pair, unless nothing kerns the left glyph (spec 8.1: the CJK
      repertoire would fill the index with zeros).
  - **The section cache:** kerning and ligatures change a TTF chapter's layout, so `SECTION_FILE_VERSION` goes
    to 83.

**Tech Stack:** C++17 SDK host tests (plain `sh` scripts, ASan/UBSan where they link); C++20 app host tests (gtest
via CMake, UCRT64 gcc); firmware gnu++2a (PlatformIO, ESP32-C3/S3); clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (branch `docs/ttf-fonts-design`): sections 5.4
(kerning and ligatures), 4.3 (the pair index), 6.3 (budgets) and the 2c-3 items of 8.1 (kerning pairs and the coverage
pre-check). This plan departs from 5.4 in the points recorded as decisions below and in the spec by Task 7.

**Pre-validated:** every task's code was built and tested in a prototype (`C:/tmp/wt-2c3-proto` and its
`freeink-sdk`), and the plan was replayed task by task on a fresh tree at `origin/master` (`d8b7800e0`, SDK
`f44c877`): each "see it fail" step fails as stated, each count matches, and every file the plan writes is identical
to the prototype. It has not run on the X3 yet (Task 7).

## Global Constraints

- **Two repositories, two PRs.**
  - SDK: branch `feat/opentype-readers` from `origin/witchhunt` (`f44c877`) of `jpirnay/freeink-sdk`, in its own
    worktree (Task 1). Its PR targets `witchhunt`; the user merges it.
  - App: branch `feat/ttf-kerning` from `origin/master` (`d8b7800e0`, Stage 2c-2 merged), in its own worktree
    (Task 4). Its PR targets `master` and is opened in Task 7, after the SDK PR has merged.
- **The SDK pin:** the app's `freeink-sdk` pin always equals the tip of `witchhunt`; never pin a side-branch commit.
  Tasks 4-6 build against the SDK branch checked out in the app worktree's submodule, and never stage
  `freeink-sdk` (every `git add` below names its paths). Task 7 moves the pin to the merged `witchhunt` tip.
- **SDK host tests on Windows:** MinGW has no ASan/UBSan, so the SDK scripts run as
  `SANITIZERS= CC=gcc CXX=g++ sh <script>` (Task 1 makes them take the override), with
  `export PATH=/c/msys64/ucrt64/bin:$PATH` set once. Elsewhere they run plain, with sanitizers.
- **SDK compatibility:** FreeInkFont's `library.json` dependencies and its font headers (`Font.h`, `FtFont.h`, ...)
  do not change, and its consumers need no new `lib_deps` line. `Gpos.h` and `Gsub.h` move to FreeInkOpenType's
  include directory, which FreeInkFont's own build adds; nothing outside FreeInkFont's sources and tests includes
  them today (checked across the SDK, the app and CrossPoint), and the PR says so. Only `FtFont`'s kerning values
  may change, for fonts whose `kern` feature has several lookups or lives under a script other than `latn`/`DFLT`
  (Review Focus 5).
- **Under RenderLock:** every `TtfFont` call runs under `RenderLock` (spec 3.4), kerning included; a kerning miss
  checks it (`checkLock()`), and logs a violation once.
- **Write once:** every flash byte is programmed at most once between erases; every `TtfFont` test ends with
  `flash.violations() == 0` and `flash.reprograms() == 0`.
- **Heap:** `makeUniqueNoThrow`, null-checked; a style whose kerning cannot be allocated draws without kerning.
- **No floating point** in `lib/EpdFont/ttf` (`scripts/check_no_float.py` runs after every firmware link): the 4.4
  scaling is integer, and the SDK readers are integer too.
- **Reading a font:** the SDK readers take a pointer to a table and its length and bound every read by it; the glue
  hands them `FontBytes::data()` plus the face's table offset and length. Their walks are capped (`kQueryBudget`,
  `kResolveBudget` in `Gpos.cpp`; 16 kern subtables), so a hostile table costs bounded time. `FontBytes::data()` is
  null for a font that is not mapped in place; such a font kerns nothing and has no ligatures.
- **App host test commands** (Git Bash, from the app worktree root): configure with
  `cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src`
  (the full suite needs `cmake --build build/test -j 8 -- -k` to get past `epub_build_inventory`, which does not build
  on Windows). The steps below write `cmake -S test -B build/test` for short.
- **Firmware builds:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
  `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`; one env at a time.
- **A header changed after a failed build:** CMake's Makefile generator can miss the change for the objects the
  failed build compiled, and the test binary then links two layouts of one struct (the 2c-3 replay died with
  `0xC0000374`, heap corruption, after `TtfFace.h` grew). The app tasks therefore change a header's layout before a
  step's failing compile, never after it. If a binary still crashes or misbehaves after a header change, rebuild the
  target with `cmake --build build/test --target <target> -j 8 --clean-first`. (The SDK scripts build from scratch
  every run.)
- **Formatting:** clang-format 21 (`/c/tmp/cf21/bin/clang-format-21.exe`) with the app's `.clang-format`; the SDK has
  no format check of its own, and its new and rewritten files follow the app's style. The code below is formatted.
- **Commits:** semantic prefixes; no `Co-Authored-By` trailer; PR titles `feat(font): ...` (SDK) and
  `feat(ttf): ...` (app).

## Review Focus

The five situations this plan's tests do not reach that are most likely to bite, most likely first. Task 7 checks
them on the X3 where it can:

1. **A font that kerns thousands of pairs** (a display face, a CJK font with proportional kerning). Expected: the
   pair index stops storing at about 5,700 pairs (70% of 8,192 slots) and those pairs come from GPOS every time
   (about 38 µs each), with no wipe. Pages stay correct, layout gets slower.
2. **Bold or italic words in regular text.** In flash mode one style is mapped at a time, so a kerning miss in
   another style remaps it. Expected: the right values, the cost only on misses (stored pairs read the index).
3. **Kerning next to a ligature** ("fi" then "x"). The provider is asked with U+FB01. Expected: the font's kerning
   for the ligature glyph (`glyphFor()` maps it), the same in layout and drawing.
4. **A malformed or hostile GPOS, GSUB or kern table.** Expected: reads past the table give 0, the walks stop at
   their budgets, a style keeps at most 1,024 subtables, and the font draws without kerning or ligatures. No crash,
   no long stall.
5. **FreeInkFont's other consumers.** `FtFont` kerns through the same `Gpos`, so a font whose `kern` feature has
   two lookups now gets both, and a font that kerns only under `cyrl` now kerns. Expected: kerning as a shaper
   gives it; for DejaVuSans and Bookerly no change (Tasks 2 and 4 compare every ASCII pair).

## File Structure

| Repository | File | Task | Responsibility |
| --- | --- | --- | --- |
| SDK | `libs/font/FreeInkOpenType/library.json` (new), `include/Gpos.h`, `include/Gsub.h` (moved) | 1 | the readers as a library of their own |
| SDK | `libs/font/FreeInkFont/library.json`, `test/host/run*.sh`, `docs/testing.md` | 1 | the new include path; compiler and sanitizer overrides |
| SDK | `libs/font/FreeInkFont/src/OpenType.h` (new), `src/Gpos.cpp`, `src/Gsub.cpp`, `FreeInkOpenType/include/Gpos.h` | 2 | the shared view; GPOS per shaper, resolved once |
| SDK | `libs/font/FreeInkFont/src/KernTable.cpp`, `FreeInkOpenType/include/KernTable.h` (new), `README.md` | 3 | the legacy kern table; the library's documentation |
| SDK | `libs/font/FreeInkFont/test/host/GposKerningTest.cpp` | 2, 3 | the readers' tests |
| App | `lib/EpdFont/ttf/TtfFace.{h,cpp}`, `FontBytes.h` | 4 | finds GPOS, GSUB and kern; the font's bytes in place |
| App | `lib/EpdFont/ttf/TtfOpenType.{h,cpp}` (new), `platformio.ini` | 4 | `ttf::Kerning`, `ttf::Ligatures` over the SDK readers |
| App | `test/ttf_font/TtfLayoutBuilder.h`, `TtfOpenTypeTest.cpp` (new), `CMakeLists.txt` | 4 | layout tables for built fonts; the glue's tests |
| App | `lib/EpdFont/ttf/TtfFont.{h,cpp}`, `lib/EpdFont/SdCardFontManager.cpp` | 5 | kerning through the pair index, ligatures in every size |
| App | `lib/Epub/Epub/Section.cpp` | 6 | `SECTION_FILE_VERSION` 83 |

## Decisions this plan makes (record them in the spec, Task 7)

- **One reader, the SDK's.** FreeInkFont's `Gpos`, `Gsub` and the new `KernTable` serve both `FtFont` and the TTF
  engine; the app keeps only glue. The spec said to copy `Gsub.cpp` and write our own GPOS reader.
- **The lookups of the kern feature add up**, the first subtable that applies in each counting. The spec and the
  step 0 prototype took the first match over all subtables. Bookerly gives the same either way: Task 4's test checks
  every ASCII pair of its four styles, resolved against per call.
- **Every script's `kern` feature counts**, through its DefaultLangSys; named language systems are not consulted
  (the readers have no language input). `Gsub`'s ligature policy (`latn`, else `DFLT`) does not change.
- **The legacy `kern` table** (format 0) kerns a font that has no GPOS kerning (the spec was silent).
- **Ligatures are keyed by U+FB00-FB04,** not U+F0000 + glyph ID: `EpdFont::getLigature` takes BMP pairs only, and
  ffi can only chain through a BMP ff.
- **As the spec says:** pairs are keyed by codepoint (BMP), in font units; a left glyph that kerns with nothing
  stores nothing; there is no RAM pair cache.

---

### Task 1: FreeInkOpenType, the layout readers as a library of their own (SDK)

A refactor with no change in behaviour: the readers' headers move to `libs/font/FreeInkOpenType/include`, a new
library builds `Gpos.cpp` and `Gsub.cpp` from FreeInkFont's `src` without the rest of FreeInkFont, and FreeInkFont
finds the headers through one more include path. The test scripts learn the include path, and take `SANITIZERS`,
`CC` and `CXX` from the environment so they run on Windows. The check that fails first: the readers compile against
the new include directory alone.

**Files** (SDK worktree):
- Create: `libs/font/FreeInkOpenType/library.json`
- Move: `libs/font/FreeInkFont/include/Gpos.h`, `libs/font/FreeInkFont/include/Gsub.h` to
  `libs/font/FreeInkOpenType/include/`
- Modify: `libs/font/FreeInkFont/library.json`, `libs/font/FreeInkFont/test/host/run.sh`,
  `libs/font/FreeInkFont/test/host/run_gpos.sh`, `libs/font/FreeInkFont/test/host/run_ligature.sh`,
  `docs/testing.md`

**Interfaces:**
- Produces:
  - The headers `Gpos.h` and `Gsub.h` in `libs/font/FreeInkOpenType/include` (`KernTable.h` joins them in Task 3).
  - The PlatformIO library `FreeInkOpenType` (`libs/font/FreeInkOpenType`), building `Gpos.cpp` and `Gsub.cpp`
    from `../FreeInkFont/src`; the app links it in Task 4.
  - The SDK font test scripts take `SANITIZERS`, `CC` and `CXX` overrides.

- [ ] **Step 1: The SDK worktree**

```bash
git -C /c/_development/witchhunt-reader/freeink-sdk fetch origin
git -C /c/_development/witchhunt-reader/freeink-sdk worktree add -b feat/opentype-readers /c/tmp/wt-sdk-opentype origin/witchhunt
cd /c/tmp/wt-sdk-opentype
git log --oneline -1   # f44c877
export PATH=/c/msys64/ucrt64/bin:$PATH
```

Tasks 1-3 run in `/c/tmp/wt-sdk-opentype`; their paths are relative to it.

- [ ] **Step 2: Run the standalone compile to see it fail**

Run: `for f in Gpos Gsub; do g++ -std=c++17 -fsyntax-only -I libs/font/FreeInkOpenType/include libs/font/FreeInkFont/src/$f.cpp || exit 1; done && echo standalone OK`
Expected: `fatal error: Gpos.h: No such file or directory`.

- [ ] **Step 3: Move the headers**

<!-- plan-move: libs/font/FreeInkFont/include/Gpos.h libs/font/FreeInkOpenType/include/Gpos.h libs/font/FreeInkFont/include/Gsub.h libs/font/FreeInkOpenType/include/Gsub.h -->
```bash
mkdir -p libs/font/FreeInkOpenType/include
git mv libs/font/FreeInkFont/include/Gpos.h libs/font/FreeInkOpenType/include/Gpos.h
git mv libs/font/FreeInkFont/include/Gsub.h libs/font/FreeInkOpenType/include/Gsub.h
```

- [ ] **Step 4: The library, FreeInkFont's include path, the scripts and the testing guide**

The library takes its sources from FreeInkFont's `src` (`srcDir`), only the readers (`srcFilter`):

<!-- plan-file: libs/font/FreeInkOpenType/library.json -->
```json
{
  "name": "FreeInkOpenType",
  "version": "0.1.0",
  "description": "FreeInkFont's OpenType layout readers on their own: GPOS pair kerning and GSUB ligatures. For a consumer that draws fonts its own way and wants none of FreeInkFont's FreeType or stb_truetype engines.",
  "keywords": "font, opentype, gpos, gsub, kerning, ligatures",
  "authors": [
    {
      "name": "FreeInk SDK Contributors"
    }
  ],
  "dependencies": {},
  "platforms": "*",
  "frameworks": "*",
  "build": {
    "srcDir": "../FreeInkFont/src",
    "srcFilter": [
      "-<*>",
      "+<Gpos.cpp>",
      "+<Gsub.cpp>"
    ]
  },
  "license": "MIT"
}
```

FreeInkFont's own build finds the moved headers through one more include path; the scripts too, and they take the
compiler and the sanitizers from the environment; the testing guide lists `run_gpos.sh`, which it did not before:

<!-- plan-edit: libs/font/FreeInkFont/test/host/run.sh -->
```sh
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
```
with:

```sh
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/../FreeInkOpenType/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run.sh -->
```sh
SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'
```
with:

```sh
# Overridable where the sanitizers do not link (MinGW): SANITIZERS= CC=gcc CXX=g++ sh <this script>
SANITIZERS=${SANITIZERS-'-fsanitize=address,undefined -fno-omit-frame-pointer'}
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run.sh -->
```sh
if cc --version 2>&1
```
with:

```sh
if ${CC:-cc} --version 2>&1
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run.sh all -->
```sh
cc -std=c99 -O2 $INCLUDES
```
with:

```sh
${CC:-cc} -std=c99 -O2 $INCLUDES
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run.sh all -->
```sh
  cc -std=c11 $INCLUDES
```
with:

```sh
  ${CC:-cc} -std=c11 $INCLUDES
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run.sh all -->
```sh
c++ -std=c++17 $INCLUDES
```
with:

```sh
${CXX:-c++} -std=c++17 $INCLUDES
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_gpos.sh -->
```sh
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'
```
with:

```sh
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/../FreeInkOpenType/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
# Overridable where the sanitizers do not link (MinGW): SANITIZERS= CC=gcc CXX=g++ sh <this script>
SANITIZERS=${SANITIZERS-'-fsanitize=address,undefined -fno-omit-frame-pointer'}
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_gpos.sh -->
```sh
  cc -std=c11 $INCLUDES $SANITIZERS -c "$source" -o "$object"
```
with:

```sh
  ${CC:-cc} -std=c11 $INCLUDES $SANITIZERS -c "$source" -o "$object"
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_gpos.sh -->
```sh
c++ -std=c++17 $INCLUDES $SANITIZERS \
```
with:

```sh
${CXX:-c++} -std=c++17 $INCLUDES $SANITIZERS \
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_ligature.sh -->
```sh
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'
```
with:

```sh
INCLUDES="-I$FONT_ROOT/include -I$FONT_ROOT/../FreeInkOpenType/include -I$FONT_ROOT/third_party/freetype/include -I$FONT_ROOT/third_party/stb"
# Overridable where the sanitizers do not link (MinGW): SANITIZERS= CC=gcc CXX=g++ sh <this script>
SANITIZERS=${SANITIZERS-'-fsanitize=address,undefined -fno-omit-frame-pointer'}
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_ligature.sh -->
```sh
  cc -std=c11 $INCLUDES $SANITIZERS -c "$source" -o "$object"
```
with:

```sh
  ${CC:-cc} -std=c11 $INCLUDES $SANITIZERS -c "$source" -o "$object"
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_ligature.sh -->
```sh
c++ -std=c++17 $INCLUDES $SANITIZERS \
```
with:

```sh
${CXX:-c++} -std=c++17 $INCLUDES $SANITIZERS \
```
<!-- plan-edit: libs/font/FreeInkFont/library.json -->
```json
    "flags": ["-I third_party/stb", "-I third_party/freetype/include"]
```
with:

```json
    "flags": ["-I third_party/stb", "-I third_party/freetype/include", "-I ../FreeInkOpenType/include"]
```
<!-- plan-edit: docs/testing.md -->
```markdown
sh libs/font/FreeInkFont/test/host/run_ligature.sh
```
with:

```markdown
sh libs/font/FreeInkFont/test/host/run_ligature.sh
sh libs/font/FreeInkFont/test/host/run_gpos.sh
```
<!-- plan-edit: docs/testing.md -->
```markdown
Covers `FtFont::RenderOptions` (every `HintingMode` plus monochrome) against a
```
with:

```markdown
Where the sanitizers do not link (MinGW), run the three without them:
`SANITIZERS= CC=gcc CXX=g++ sh <script>`.

Covers `FtFont::RenderOptions` (every `HintingMode` plus monochrome) against a
```

- [ ] **Step 5: Run the checks to see them pass**

Run: `for f in Gpos Gsub; do g++ -std=c++17 -fsyntax-only -I libs/font/FreeInkOpenType/include libs/font/FreeInkFont/src/$f.cpp || exit 1; done && echo standalone OK`
Expected: `standalone OK`

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run.sh`
Expected: `7/7 checks passed` and `FtFont RenderOptions host test: OK`, as before the move.

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_ligature.sh`
Expected: `37/37 checks passed`, as before.

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_gpos.sh`
Expected: `15 checks, 0 failures`, as before.

- [ ] **Step 6: Commit**

```bash
git add libs/font/FreeInkOpenType libs/font/FreeInkFont/include libs/font/FreeInkFont/library.json libs/font/FreeInkFont/test/host docs/testing.md
git commit -m "refactor(font): FreeInkOpenType builds the layout readers on their own"
```

---

### Task 2: GPOS kerning as a shaper applies it, resolved once (SDK)

`Gpos.cpp` is rewritten over a shared view (`OpenType.h`, which `Gsub.cpp` now uses too):

- **The `kern` feature** is collected from every script's DefaultLangSys, required feature included, not from
  `latn` and `DFLT` only. Its lookups (at most 64, sorted, each once) apply in LookupList order.
- **Within a lookup** the first subtable that has the pair counts; **across lookups** the adjustments add up.
- **Resolved once:** `ResolveKernSubtables()` writes the PairPos subtables in the order they apply, as byte offsets
  from the table's start, with `kLookupStart` on each lookup's first; the resolved `PairKernAdjustment()` overload
  gives the per-call walk's value without the walk, and says whether any subtable covers the left glyph.
- **Budgets:** a per-call walk stops after 4,096 steps, a resolve after 65,536; coverage and class searches are
  binary, over at most 8,192 ranges.

The tests build GPOS tables with several lookups and scripts, check the resolved API against the per-call walk on
synthetic tables and on every ASCII pair of DejaVuSans, and sweep truncated and corrupted tables through the resolved
path under the sanitizers.

**Files** (SDK worktree):
- Create: `libs/font/FreeInkFont/src/OpenType.h`
- Modify: `libs/font/FreeInkFont/src/Gpos.cpp` (rewritten), `libs/font/FreeInkOpenType/include/Gpos.h`,
  `libs/font/FreeInkFont/src/Gsub.cpp`
- Test: `libs/font/FreeInkFont/test/host/GposKerningTest.cpp`

**Interfaces:**
- Consumes: Task 1 (the headers' new home).
- Produces (`Gpos.h`, namespace `freeink::font::gpos`):
  - `int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph)`
    (signature unchanged; `FtFont` keeps calling it);
  - `constexpr uint32_t kLookupStart = 0x80000000u;`
  - `unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity)`:
    the count the font has, writing at most `capacity` (`subtables` may be null to count);
  - `int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
    uint32_t leftGlyph, uint32_t rightGlyph, bool* covered = nullptr)`.
- Internal (`src/OpenType.h`, namespace `freeink::font::otl`): `tag()`, `kMaxRanges`, `struct View` (`has`, `u16`,
  `s16`, `u32`, `sub`), `findRange()`, `coverageIndex()`, `glyphClass()`; Task 3's `KernTable.cpp` uses `View`.

- [ ] **Step 1: Write the failing tests**

Helpers that build a lookup of several subtables, a GPOS whose scripts each list their own lookups, and the resolved
reader's answer for a pair; then the checks for lookups, scripts and the resolved API; then every ASCII pair of
DejaVuSans, resolved against per call:

<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
// Two layers: a synthetic hand-built GPOS blob exercises both PairPos
// formats (1 via an Extension wrapper, 2 via class matrices), activation,
// and malformed-input robustness deterministically; the DejaVuSans fixture
// then proves the FtFont plumbing end-to-end, including that stripping the
// legacy 'kern' table leaves kerning working through GPOS alone.
```
with:

```cpp
// Two layers: a synthetic hand-built GPOS blob exercises both PairPos
// formats (1 via an Extension wrapper, 2 via class matrices), activation by
// any script, lookups adding up, the resolved-once API, and malformed-input
// robustness deterministically; the DejaVuSans fixture then proves the FtFont
// plumbing end-to-end, including that stripping the legacy 'kern' table
// leaves kerning working through GPOS alone, and checks the resolved reader
// against the stateless one.
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
#include "FtFont.h"
#include "Gpos.h"
```
with:

```cpp
#include <ft2build.h>
#include FT_FREETYPE_H

#include "FtFont.h"
#include "Gpos.h"
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
// sfnt table lookup for plain (non-collection) fonts — enough for fixtures.
```
with:

```cpp
// A lookup holding several subtables, in order (no Extension).
std::vector<uint8_t> lookupOf(uint16_t type, const std::vector<std::vector<uint8_t>>& subtables) {
  std::vector<uint8_t> bytes;
  appendU16be(bytes, type);
  appendU16be(bytes, 0);
  appendU16be(bytes, uint16_t(subtables.size()));
  size_t offset = 6 + 2 * subtables.size();
  for (const auto& subtable : subtables) {
    appendU16be(bytes, uint16_t(offset));
    offset += subtable.size();
  }
  for (const auto& subtable : subtables) appendBlob(bytes, subtable);
  return bytes;
}

// A GPOS whose scripts each activate their own 'kern' feature through their
// DefaultLangSys, listing the given lookups.
std::vector<uint8_t> gposWith(const std::vector<std::pair<const char*, std::vector<uint16_t>>>& scripts,
                              const std::vector<std::vector<uint8_t>>& lookups) {
  std::vector<uint8_t> scriptList;
  appendU16be(scriptList, uint16_t(scripts.size()));
  size_t offset = 2 + 6 * scripts.size();
  for (size_t i = 0; i < scripts.size(); ++i) {
    appendTag(scriptList, scripts[i].first);
    appendU16be(scriptList, uint16_t(offset + 12 * i));
  }
  for (size_t i = 0; i < scripts.size(); ++i) {
    appendU16be(scriptList, 4);       // DefaultLangSys offset (from Script)
    appendU16be(scriptList, 0);       // LangSysCount
    appendU16be(scriptList, 0);       // LookupOrder
    appendU16be(scriptList, 0xFFFF);  // RequiredFeatureIndex
    appendU16be(scriptList, 1);       // FeatureIndexCount
    appendU16be(scriptList, uint16_t(i));
  }
  std::vector<uint8_t> featureList;
  appendU16be(featureList, uint16_t(scripts.size()));
  offset = 2 + 6 * scripts.size();
  for (size_t i = 0; i < scripts.size(); ++i) {
    appendTag(featureList, "kern");
    appendU16be(featureList, uint16_t(offset));
    offset += 4 + 2 * scripts[i].second.size();
  }
  for (const auto& script : scripts) {
    appendU16be(featureList, 0);
    appendU16be(featureList, uint16_t(script.second.size()));
    for (const uint16_t index : script.second) appendU16be(featureList, index);
  }
  std::vector<uint8_t> lookupList;
  appendU16be(lookupList, uint16_t(lookups.size()));
  offset = 2 + 2 * lookups.size();
  for (const auto& lookup : lookups) {
    appendU16be(lookupList, uint16_t(offset));
    offset += lookup.size();
  }
  for (const auto& lookup : lookups) appendBlob(lookupList, lookup);

  std::vector<uint8_t> bytes;
  appendU32be(bytes, 0x00010000);
  appendU16be(bytes, 10);
  appendU16be(bytes, uint16_t(10 + scriptList.size()));
  appendU16be(bytes, uint16_t(10 + scriptList.size() + featureList.size()));
  appendBlob(bytes, scriptList);
  appendBlob(bytes, featureList);
  appendBlob(bytes, lookupList);
  return bytes;
}

// The resolved reader's answer for a pair, resolving first.
int32_t resolvedKern(const std::vector<uint8_t>& gpos, uint32_t left, uint32_t right, bool* covered = nullptr) {
  uint32_t subtables[64];
  const unsigned count = freeink::font::gpos::ResolveKernSubtables(gpos.data(), gpos.size(), subtables, 64);
  return freeink::font::gpos::PairKernAdjustment(gpos.data(), gpos.size(), subtables, count < 64 ? count : 64, left,
                                                 right, covered);
}

// sfnt table lookup for plain (non-collection) fonts — enough for fixtures.
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
  expect(true, "truncation/corruption sweep completed");
```
with:

```cpp
  expect(true, "truncation/corruption sweep completed");

  // --- Lookups and scripts --------------------------------------------------
  using freeink::font::gpos::ResolveKernSubtables;
  {
    // Two lookups kerning the same pair: a shaper applies both.
    const std::vector<uint8_t> summed = gposWith({{"latn", {0, 1}}}, {lookup(2, pairPos1(10, 20, -100), false),
                                                                      lookup(2, pairPos1(10, 20, -20), false)});
    expect(PairKernAdjustment(summed.data(), summed.size(), 10, 20) == -120, "the kern feature's lookups add up");
    expect(resolvedKern(summed, 10, 20) == -120, "resolved: the lookups add up");

    // Within a lookup, the first subtable that has the pair wins.
    const std::vector<uint8_t> firstWins =
        gposWith({{"latn", {0}}}, {lookupOf(2, {pairPos1(10, 20, -100), pairPos1(10, 20, -50)})});
    expect(PairKernAdjustment(firstWins.data(), firstWins.size(), 10, 20) == -100,
           "the first matching subtable of a lookup wins");
    expect(resolvedKern(firstWins, 10, 20) == -100, "resolved: the first matching subtable wins");

    // Any script's DefaultLangSys activates its kern feature: Cyrillic kerned
    // under 'cyrl' only.
    const std::vector<uint8_t> scripts =
        gposWith({{"cyrl", {1}}, {"latn", {0}}},
                 {lookup(2, pairPos1(10, 20, -100), false), lookup(2, pairPos1(30, 40, -25), false)});
    expect(PairKernAdjustment(scripts.data(), scripts.size(), 30, 40) == -25, "a cyrl-only kern feature kerns");
    expect(PairKernAdjustment(scripts.data(), scripts.size(), 10, 20) == -100, "latn still kerns beside it");
    expect(resolvedKern(scripts, 30, 40) == -25, "resolved: a cyrl-only kern feature kerns");

    // A lookup listed by two scripts applies once.
    const std::vector<uint8_t> shared =
        gposWith({{"cyrl", {0}}, {"latn", {0}}}, {lookup(2, pairPos1(10, 20, -100), false)});
    expect(PairKernAdjustment(shared.data(), shared.size(), 10, 20) == -100, "a lookup two scripts list applies once");
  }

  // --- Resolved once ----------------------------------------------------------
  {
    uint32_t subtables[8];
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), subtables, 8) == 2, "two subtables resolved");
    expect((subtables[0] & freeink::font::gpos::kLookupStart) && (subtables[1] & freeink::font::gpos::kLookupStart),
           "each starts its own lookup");
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), nullptr, 0) == 2, "counting alone");
    uint32_t one[1] = {0};
    expect(ResolveKernSubtables(gpos.data(), gpos.size(), one, 1) == 2 && one[0] != 0,
           "a short buffer gets what fits, and the full count");
    const unsigned glyphs[] = {0, 10, 20, 30, 40, 99};
    bool agree = true;
    for (const unsigned left : glyphs) {
      for (const unsigned right : glyphs) {
        agree = agree && resolvedKern(gpos, left, right) == PairKernAdjustment(gpos.data(), gpos.size(), left, right);
      }
    }
    expect(agree, "resolved and stateless agree on every synthetic pair");
    bool covered = false;
    expect(resolvedKern(gpos, 10, 10, &covered) == 0 && covered,
           "a covered left glyph, kerning nothing with this right");
    expect(resolvedKern(gpos, 99, 20, &covered) == 0 && !covered, "an uncovered left glyph");
    expect(freeink::font::gpos::PairKernAdjustment(nullptr, 0, subtables, 2, 10, 20) == 0, "null table, resolved");
    for (size_t len = 0; len <= gpos.size(); ++len) {
      (void)ResolveKernSubtables(gpos.data(), len, subtables, 8);
      (void)freeink::font::gpos::PairKernAdjustment(gpos.data(), len, subtables, 2, 10, 20);
    }
    for (size_t i = 0; i < gpos.size(); ++i) {
      std::vector<uint8_t> corrupt = gpos;
      corrupt[i] ^= 0xFF;
      const unsigned count = ResolveKernSubtables(corrupt.data(), corrupt.size(), subtables, 8);
      (void)freeink::font::gpos::PairKernAdjustment(corrupt.data(), corrupt.size(), subtables, count < 8 ? count : 8,
                                                    10, 20);
    }
    expect(true, "resolved truncation/corruption sweep completed");
  }
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
    // Integer Font-API route shares the same fallback.
    expect(ft.kerning('A', 'V', 32, 0) <= 0, "integer kerning() API agrees in sign");
```
with:

```cpp
    // Integer Font-API route shares the same fallback.
    expect(ft.kerning('A', 'V', 32, 0) <= 0, "integer kerning() API agrees in sign");

    // Every printable ASCII pair: the resolved reader gives what the stateless
    // one gives.
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    expect(FT_Init_FreeType(&library) == 0 &&
               FT_New_Memory_Face(library, font.data(), FT_Long(font.size()), 0, &face) == 0,
           "FreeType opens the fixture");
    std::vector<uint32_t> subtables(256);
    const unsigned count =
        hasGpos ? ResolveKernSubtables(&font[gposOffset], gposLength, subtables.data(), unsigned(subtables.size())) : 0;
    expect(count <= subtables.size(), "the fixture's subtables fit");
    bool resolvedAgrees = true;
    int kerned = 0;
    for (unsigned l = 0x21; l < 0x7F; ++l) {
      for (unsigned r = 0x21; r < 0x7F; ++r) {
        const unsigned gl = FT_Get_Char_Index(face, l);
        const unsigned gr = FT_Get_Char_Index(face, r);
        if (hasGpos) {
          const int32_t stateless = PairKernAdjustment(&font[gposOffset], gposLength, gl, gr);
          const int32_t resolved = freeink::font::gpos::PairKernAdjustment(&font[gposOffset], gposLength,
                                                                          subtables.data(), count, gl, gr);
          resolvedAgrees = resolvedAgrees && stateless == resolved;
          kerned += stateless != 0;
        }
      }
    }
    expect(resolvedAgrees, "resolved GPOS agrees with the stateless reader on every ASCII pair");
    expect(!hasGpos || kerned > 100, "the fixture's GPOS kerns ASCII pairs (not a vacuous match)");
    printf("info: %u kern subtables resolved, %d ASCII pairs kerned\n", count, kerned);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_gpos.sh`
Expected: a compile error, `'ResolveKernSubtables' is not a member of 'freeink::font::gpos'`.

- [ ] **Step 3: The shared view, and GSUB on it**

<!-- plan-file: libs/font/FreeInkFont/src/OpenType.h -->
```cpp
#pragma once

// FreeInk SDK — what the OpenType layout readers (Gpos.cpp, Gsub.cpp) share: a bounds-checked view of a table, and
// the coverage and class lookups both table types are built from. Internal to the readers.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace otl {

constexpr uint32_t tag(const char (&t)[5]) {
  return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]);
}

// Coverage and class range lists are capped: real fonts compact them into ranges (tens to low hundreds even for
// CJK), and a hostile count must not buy an unbounded search.
constexpr unsigned kMaxRanges = 8192;

struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  unsigned u16(size_t offset) const { return has(offset, 2) ? unsigned(data[offset]) * 256 + data[offset + 1] : 0; }
  int32_t s16(size_t offset) const { return int32_t(int16_t(u16(offset))); }
  uint32_t u32(size_t offset) const { return has(offset, 4) ? (uint32_t(u16(offset)) << 16) | u16(offset + 2) : 0; }
  // Offset 0 is OpenType's own NULL convention for an absent optional
  // field — treating it as a real offset would alias `this` view's own
  // header as the child table (harmless memory-safety-wise since it's still
  // in bounds, but can manufacture a bogus "match" out of a malformed font).
  View sub(size_t offset) const {
    return (offset != 0 && has(offset, 1)) ? View{data + offset, size - offset} : View{};
  }
};

// The index of `first` among sorted records of `stride` bytes whose range [start, end] is the first two uint16 of each
// record, or -1. OpenType requires range lists sorted by start, so a binary search; an unsorted (malformed) list can
// only miss, never read out of bounds.
inline int findRange(View view, size_t at, unsigned count, size_t stride, unsigned glyph) {
  unsigned low = 0;
  unsigned high = count;
  while (low < high) {
    const unsigned middle = (low + high) / 2;
    const size_t record = at + middle * stride;
    if (glyph < view.u16(record)) {
      high = middle;
    } else if (glyph > view.u16(record + 2)) {
      low = middle + 1;
    } else {
      return int(middle);
    }
  }
  return -1;
}

// CoverageTable lookup (GSUB and GPOS alike): the coverage index of `glyph`, or -1.
inline int coverageIndex(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  const unsigned rawCount = view.u16(2);
  const unsigned count = rawCount < kMaxRanges ? rawCount : kMaxRanges;
  if (format == 1 && view.has(4, size_t(count) * 2)) {
    unsigned low = 0;
    unsigned high = count;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (view.u16(4 + middle * 2) < glyph)
        low = middle + 1;
      else
        high = middle;
    }
    return low < count && view.u16(4 + low * 2) == glyph ? int(low) : -1;
  }
  if (format == 2 && view.has(4, size_t(count) * 6)) {
    const int range = findRange(view, 4, count, 6, glyph);
    if (range < 0) return -1;
    const size_t record = 4 + size_t(range) * 6;
    return int(view.u16(record + 4) + glyph - view.u16(record));
  }
  return -1;
}

// ClassDefTable lookup: the glyph's class, with 0 (the spec's catch-all
// class) for any glyph the table doesn't list.
inline unsigned glyphClass(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  if (format == 1) {
    const unsigned start = view.u16(2);
    const unsigned rawCount = view.u16(4);
    const unsigned count = rawCount < kMaxRanges ? rawCount : kMaxRanges;
    if (!view.has(6, size_t(count) * 2)) return 0;
    return (glyph >= start && glyph - start < count) ? view.u16(6 + (glyph - start) * 2) : 0;
  }
  if (format == 2) {
    const unsigned rawCount = view.u16(2);
    const unsigned count = rawCount < kMaxRanges ? rawCount : kMaxRanges;
    if (!view.has(4, size_t(count) * 6)) return 0;
    const int range = findRange(view, 4, count, 6, glyph);
    return range < 0 ? 0 : view.u16(4 + size_t(range) * 6 + 4);
  }
  return 0;
}

}  // namespace otl
}  // namespace font
}  // namespace freeink
```

`Gsub.cpp` drops its own copies (its 8,192-range cap is `kMaxRanges` now, and format 2 coverage is a binary search
instead of a linear one):

<!-- plan-edit: libs/font/FreeInkFont/src/Gsub.cpp -->
```cpp
constexpr uint32_t kTagDflt = uint32_t('D') << 24 | uint32_t('F') << 16 | uint32_t('L') << 8 | 'T';

struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  unsigned u16(size_t offset) const { return has(offset, 2) ? unsigned(data[offset]) * 256 + data[offset + 1] : 0; }
  uint32_t u32(size_t offset) const { return has(offset, 4) ? (uint32_t(u16(offset)) << 16) | u16(offset + 2) : 0; }
  // Offset 0 is OpenType's own NULL convention for an absent optional
  // field — treating it as a real offset would alias `this` view's own
  // header as the child table (harmless memory-safety-wise since it's still
  // in bounds, but can manufacture a bogus "match" out of a malformed font).
  View sub(size_t offset) const {
    return (offset != 0 && has(offset, 1)) ? View{data + offset, size - offset} : View{};
  }
};

// GSUB CoverageTable lookup: returns the coverage index of `glyph`, or -1.
// Format 1's search is a binary search (cheap even at the 16-bit field's
// natural max), but format 2's is linear, and coverageIndex() isn't reached
// through the outer loop's `work` budget below — so unlike every other count
// in this file, this one keeps its own cap. 8192 is far beyond any real
// font's coverage range-list size (coverage compacts into ranges, so even
// huge CJK fonts stay in the tens-to-low-hundreds), but still bounds
// worst-case scan time against a hostile "count" field.
int coverageIndex(View view, unsigned glyph) {
  const unsigned format = view.u16(0);
  const unsigned rawCount = view.u16(2);
  const unsigned count = rawCount < 8192 ? rawCount : 8192;
  if (format == 1 && view.has(4, size_t(count) * 2)) {
    unsigned low = 0;
    unsigned high = count;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (view.u16(4 + middle * 2) < glyph)
        low = middle + 1;
      else
        high = middle;
    }
    return low < count && view.u16(4 + low * 2) == glyph ? int(low) : -1;
  }
  if (format == 2 && view.has(4, size_t(count) * 6)) {
    for (unsigned i = 0; i < count; ++i) {
      const size_t position = 4 + i * 6;
      const unsigned start = view.u16(position);
      const unsigned end = view.u16(position + 2);
      if (glyph >= start && glyph <= end) return int(view.u16(position + 4) + glyph - start);
    }
  }
  return -1;
}
```
with:

```cpp
constexpr uint32_t kTagDflt = uint32_t('D') << 24 | uint32_t('F') << 16 | uint32_t('L') << 8 | 'T';
```
<!-- plan-edit: libs/font/FreeInkFont/src/Gsub.cpp -->
```cpp
#include <string.h>

namespace freeink {
```
with:

```cpp
#include <string.h>

#include "OpenType.h"

namespace freeink {
```
<!-- plan-edit: libs/font/FreeInkFont/src/Gsub.cpp -->
```cpp
namespace {

constexpr uint32_t kTagLatn
```
with:

```cpp
namespace {

using otl::coverageIndex;
using otl::View;

constexpr uint32_t kTagLatn
```

- [ ] **Step 4: GPOS**

<!-- plan-file: libs/font/FreeInkOpenType/include/Gpos.h -->
```cpp
#pragma once

// FreeInk SDK — minimal OpenType GPOS pair-kerning reader.
//
// FreeType's FT_Get_Kerning() reads only the legacy 'kern' table, but fonts
// built with modern toolchains (fontmake, Glyphs, current Google Fonts
// releases, all CFF/.otf fonts) ship their kerning exclusively in the GPOS
// table's 'kern' feature — interpreting that is normally HarfBuzz's job,
// which is too heavy to vendor here for one lookup type. This reads exactly
// the 'kern' feature's Pair Adjustment lookups (type 2, and 9 wrapping 2) in
// both PairPos formats (1: per-glyph pair sets, 2: class matrices),
// bounds-checked and capped against malformed/hostile font data, in the same
// style as the GSUB ligature reader (see Gsub.h).
//
// The 'kern' feature is the one any script's DefaultLangSys activates (its
// required feature included): Latin, Cyrillic and Greek kern alike, and fonts
// usually point every script at the same lookups. Named language systems are
// not consulted, since this API has no language input. The feature's lookups
// apply as a shaper applies them: in LookupList order, the first subtable
// that matches the pair in each, their adjustments added.
//
// Only the FIRST value record's X-advance adjustment is returned (how much
// the left glyph's advance changes before drawing the right glyph) — that is
// the entirety of horizontal pair kerning. Placement adjustments, second-
// glyph records, and mark/cursive attachment are out of scope.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace gpos {

// Returns the X-advance adjustment for drawing `rightGlyph` after
// `leftGlyph`, in FONT UNITS (FUnits, typically negative), or 0 when no
// active lookup covers the pair (or the table is absent/malformed). Scale by
// the face's x_scale (FT_MulFix) for 26.6 pixels at the current size.
// `gposBytes`/`gposSize` is the raw content of the font's 'GPOS' table (e.g.
// from FT_Load_Sfnt_Table), not the whole font file.
//
// It walks the script, feature and lookup lists on every call; a caller that
// kerns many pairs of one font resolves them once (below).
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph);

// --- Resolved once, for many pairs ---

// The kern feature's PairPos subtables in the order they apply, as byte
// offsets from the start of the GPOS table (so they stay valid when the
// caller's copy or mapping of the table moves), kLookupStart set on the first
// subtable of each lookup.
constexpr uint32_t kLookupStart = 0x80000000u;

// Writes up to `capacity` subtables to `subtables` (null: only count) and
// returns how many the font has: more than `capacity` when they did not all
// fit. 0 when the font does not kern, or the table is too malformed to walk.
unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity);

// PairKernAdjustment over resolved subtables: the same value, without the
// walk. `covered`, when not null, is set to whether any subtable covers
// `leftGlyph` at all; a caller remembering pairs need not remember the ones
// whose left glyph kerns with nothing.
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, bool* covered = nullptr);

}  // namespace gpos
}  // namespace font
}  // namespace freeink
```

<!-- plan-file: libs/font/FreeInkFont/src/Gpos.cpp -->
```cpp
#include "Gpos.h"

#include <string.h>

#include "OpenType.h"

namespace freeink {
namespace font {
namespace gpos {

namespace {

using otl::View;

constexpr uint32_t kTagKern = otl::tag("kern");
// A kern feature lists a handful of lookups; more is malformed or hostile.
constexpr unsigned kMaxKernLookups = 64;
// Reads a walk may make: a hostile table costs bounded time. Resolving is done
// once per font, so it may read more than a single stateless query.
constexpr unsigned kQueryBudget = 4096;
constexpr unsigned kResolveBudget = 65536;

// Every set bit in a ValueFormat contributes one uint16 to its ValueRecord
// (placements, advances, and device-table offsets alike).
unsigned valueRecordSize(unsigned valueFormat) {
  unsigned bits = valueFormat & 0xFFFFu;
  unsigned count = 0;
  while (bits) {
    count += bits & 1u;
    bits >>= 1;
  }
  return count * 2;
}

constexpr unsigned kXAdvanceBit = 0x0004;

// Byte offset of the XAdvance field inside a ValueRecord: one uint16 per set
// bit below it (XPlacement 0x0001, YPlacement 0x0002).
unsigned xAdvanceOffset(unsigned valueFormat) {
  unsigned offset = 0;
  if (valueFormat & 0x0001) offset += 2;
  if (valueFormat & 0x0002) offset += 2;
  return offset;
}

// A PairPos subtable's adjustment for (left, right). `found` distinguishes a
// genuine record (even a zero-valued one — format 2 matrices are dense, and a
// matched pair must stop the lookup's subtables exactly as a shaper's would) from
// "this subtable does not cover the pair". `covered` is set when the subtable
// covers `left`, whatever the right glyph.
int32_t pairAdjustment(View subtable, unsigned left, unsigned right, bool& found, bool& covered) {
  found = false;
  const unsigned format = subtable.u16(0);
  const unsigned valueFormat1 = subtable.u16(4);
  const unsigned valueFormat2 = subtable.u16(6);
  if ((valueFormat1 & kXAdvanceBit) == 0) return 0;  // no X-advance data → nothing to kern with
  const unsigned len1 = valueRecordSize(valueFormat1);
  const unsigned len2 = valueRecordSize(valueFormat2);
  const unsigned advanceAt = xAdvanceOffset(valueFormat1);

  if (format == 1) {
    const int coverage = otl::coverageIndex(subtable.sub(subtable.u16(2)), left);
    if (coverage < 0) return 0;
    covered = true;
    const unsigned setCount = subtable.u16(8);
    if (unsigned(coverage) >= setCount || !subtable.has(10, size_t(setCount) * 2)) return 0;
    View set = subtable.sub(subtable.u16(10 + unsigned(coverage) * 2));
    const unsigned pairCount = set.u16(0);
    const size_t recordSize = 2 + size_t(len1) + len2;
    if (!set.has(2, size_t(pairCount) * recordSize)) return 0;
    // PairValueRecords are sorted by SecondGlyph.
    unsigned low = 0;
    unsigned high = pairCount;
    while (low < high) {
      const unsigned middle = (low + high) / 2;
      if (set.u16(2 + middle * recordSize) < right)
        low = middle + 1;
      else
        high = middle;
    }
    if (low >= pairCount || set.u16(2 + low * recordSize) != right) return 0;
    found = true;
    return set.s16(2 + low * recordSize + 2 + advanceAt);
  }

  if (format == 2) {
    // Coverage lists the first glyphs; a covered left glyph always resolves
    // to a (possibly zero) record through the class matrix.
    if (otl::coverageIndex(subtable.sub(subtable.u16(2)), left) < 0) return 0;
    covered = true;
    const unsigned class1Count = subtable.u16(12);
    const unsigned class2Count = subtable.u16(14);
    const unsigned class1 = otl::glyphClass(subtable.sub(subtable.u16(8)), left);
    const unsigned class2 = otl::glyphClass(subtable.sub(subtable.u16(10)), right);
    if (class1 >= class1Count || class2 >= class2Count) return 0;
    const size_t recordSize = size_t(len1) + len2;
    const size_t record = 16 + (size_t(class1) * class2Count + class2) * recordSize;
    if (!subtable.has(16, size_t(class1Count) * class2Count * recordSize)) return 0;
    found = true;
    return subtable.s16(record + advanceAt);
  }

  return 0;
}

// The lookups the 'kern' feature of any script's DefaultLangSys lists, sorted
// (LookupList order, the order a shaper applies them) and without repeats.
unsigned kernLookups(View gpos, uint16_t (&out)[kMaxKernLookups], unsigned& work, const unsigned budget) {
  View scripts = gpos.sub(gpos.u16(4));
  View features = gpos.sub(gpos.u16(6));
  View lookups = gpos.sub(gpos.u16(8));
  const unsigned scriptCount = scripts.u16(0);
  const unsigned featureCount = features.u16(0);
  const unsigned lookupCount = lookups.u16(0);
  // Counts are 16-bit fields with no artificial low cap: `.has()` prevents
  // out-of-bounds reads, while the `work` budget bounds the walk.
  if (!scripts.has(2, size_t(scriptCount) * 6) || !features.has(2, size_t(featureCount) * 6) ||
      !lookups.has(2, size_t(lookupCount) * 2)) {
    return 0;
  }
  unsigned count = 0;
  const auto addFeature = [&](const unsigned featureIndex) {
    if (featureIndex >= featureCount || ++work > budget) return;
    const size_t record = 2 + size_t(featureIndex) * 6;
    if (features.u32(record) != kTagKern) return;
    View feature = features.sub(features.u16(record + 4));
    const unsigned lookupIndexCount = feature.u16(2);
    if (!feature.has(4, size_t(lookupIndexCount) * 2)) return;
    for (unsigned i = 0; i < lookupIndexCount && ++work <= budget; ++i) {
      const unsigned index = feature.u16(4 + i * 2);
      if (index >= lookupCount) continue;
      unsigned at = 0;
      while (at < count && out[at] < index) ++at;
      if ((at < count && out[at] == index) || count == kMaxKernLookups) continue;
      memmove(out + at + 1, out + at, (count - at) * sizeof out[0]);
      out[at] = uint16_t(index);
      ++count;
    }
  };
  for (unsigned s = 0; s < scriptCount && ++work <= budget; ++s) {
    const View script = scripts.sub(scripts.u16(2 + size_t(s) * 6 + 4));
    const View langSys = script.has(0, 4) ? script.sub(script.u16(0)) : View{};
    if (!langSys.has(0, 6)) continue;
    const unsigned requiredFeatureIndex = langSys.u16(2);
    const unsigned featureIndexCount = langSys.u16(4);
    if (!langSys.has(6, size_t(featureIndexCount) * 2)) continue;
    if (requiredFeatureIndex != 0xFFFFu) addFeature(requiredFeatureIndex);
    for (unsigned i = 0; i < featureIndexCount; ++i) addFeature(langSys.u16(6 + i * 2));
  }
  return count;
}

// Calls `visit(subtable, firstOfItsLookup)` for each PairPos subtable of the
// kern lookups, in the order they apply.
template <typename Visit>
void forEachKernSubtable(View gpos, const unsigned budget, Visit visit) {
  unsigned work = 0;
  uint16_t indices[kMaxKernLookups];
  const unsigned count = kernLookups(gpos, indices, work, budget);
  View lookups = gpos.sub(gpos.u16(8));
  for (unsigned k = 0; k < count; ++k) {
    View lookup = lookups.sub(lookups.u16(2 + size_t(indices[k]) * 2));
    const unsigned type = lookup.u16(0);
    const unsigned subtableCount = lookup.u16(4);
    if ((type != 2 && type != 9) || !lookup.has(6, size_t(subtableCount) * 2)) continue;
    bool first = true;
    for (unsigned j = 0; j < subtableCount; ++j) {
      if (++work > budget) return;
      View subtable = lookup.sub(lookup.u16(6 + j * 2));
      if (type == 9) {
        // Extension Positioning: format 1 wraps another lookup type at a
        // 32-bit offset. Unwrap it if it wraps a pair adjustment (2).
        if (!subtable.has(0, 8) || subtable.u16(0) != 1 || subtable.u16(2) != 2) continue;
        subtable = subtable.sub(subtable.u32(4));
      }
      if (subtable.data == nullptr) continue;
      visit(subtable, first);
      first = false;
    }
  }
}

// Adds a subtable's adjustment unless an earlier subtable of the same lookup
// already matched: what a shaper does with the kern feature's lookups.
struct Accumulator {
  int32_t total = 0;
  bool matched = false;  // in the current lookup
  bool covered = false;

  void apply(View subtable, const bool firstOfItsLookup, const unsigned left, const unsigned right) {
    if (firstOfItsLookup) matched = false;
    if (matched) return;
    bool found = false;
    const int32_t value = pairAdjustment(subtable, left, right, found, covered);
    if (found) {
      total += value;
      matched = true;
    }
  }
};

}  // namespace

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph) {
  if (gposBytes == nullptr || leftGlyph == 0 || rightGlyph == 0) return 0;
  const View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  Accumulator sum;
  forEachKernSubtable(gpos, kQueryBudget,
                      [&](View subtable, const bool first) { sum.apply(subtable, first, leftGlyph, rightGlyph); });
  return sum.total;
}

unsigned ResolveKernSubtables(const uint8_t* gposBytes, size_t gposSize, uint32_t* subtables, unsigned capacity) {
  if (gposBytes == nullptr) return 0;
  const View gpos{gposBytes, gposSize};
  if (!gpos.has(0, 10)) return 0;
  unsigned count = 0;
  forEachKernSubtable(gpos, kResolveBudget, [&](View subtable, const bool first) {
    if (subtables != nullptr && count < capacity) {
      subtables[count] = uint32_t(subtable.data - gposBytes) | (first ? kLookupStart : 0u);
    }
    ++count;
  });
  return count;
}

int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, const uint32_t* subtables, unsigned count,
                           uint32_t leftGlyph, uint32_t rightGlyph, bool* covered) {
  Accumulator sum;
  if (gposBytes != nullptr && subtables != nullptr && leftGlyph != 0 && rightGlyph != 0) {
    const View gpos{gposBytes, gposSize};
    for (unsigned i = 0; i < count; ++i) {
      const bool first = (subtables[i] & kLookupStart) != 0;
      const View subtable = gpos.sub(subtables[i] & ~kLookupStart);
      if (subtable.data != nullptr) {
        sum.apply(subtable, first, leftGlyph, rightGlyph);
      } else if (first) {
        sum.matched = false;  // a lookup whose first subtable cannot be read still starts afresh
      }
    }
  }
  if (covered != nullptr) *covered = sum.covered;
  return sum.total;
}

}  // namespace gpos
}  // namespace font
}  // namespace freeink
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_gpos.sh`
Expected: `36 checks, 0 failures` and `FtFont GPOS kerning host test: OK`.

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_ligature.sh`
Expected: `37/37 checks passed` (GSUB on the shared view gives the same ligatures).

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run.sh`
Expected: `FtFont RenderOptions host test: OK`

Run: `for f in Gpos Gsub; do g++ -std=c++17 -fsyntax-only -I libs/font/FreeInkOpenType/include libs/font/FreeInkFont/src/$f.cpp || exit 1; done && echo standalone OK`
Expected: `standalone OK`

- [ ] **Step 6: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --style=file:C:/_development/witchhunt-reader/.clang-format --dry-run --Werror libs/font/FreeInkFont/src/OpenType.h libs/font/FreeInkFont/src/Gpos.cpp libs/font/FreeInkFont/src/Gsub.cpp libs/font/FreeInkOpenType/include/Gpos.h
git add libs/font/FreeInkFont/src libs/font/FreeInkOpenType/include/Gpos.h libs/font/FreeInkFont/test/host/GposKerningTest.cpp
git commit -m "feat(font): GPOS kerning as a shaper applies it, resolved once"
```

---

### Task 3: The legacy kern table, the documentation, and the SDK PR (SDK)

`KernTable` reads what `FT_Get_Kerning` reads: the Microsoft `kern` table (version 0), its horizontal format 0
subtables that hold kerning values (not minimums, not cross-stream), added together unless a subtable overrides; at
most 16 subtables, two binary searches each. The test checks it against FreeType on every ASCII pair of DejaVuSans.
The README documents the library; then the SDK PR.

**Files** (SDK worktree):
- Create: `libs/font/FreeInkOpenType/include/KernTable.h`, `libs/font/FreeInkFont/src/KernTable.cpp`
- Modify: `libs/font/FreeInkOpenType/library.json`, `libs/font/FreeInkFont/test/host/run_gpos.sh`,
  `libs/font/FreeInkFont/README.md`, `docs/testing.md`
- Test: `libs/font/FreeInkFont/test/host/GposKerningTest.cpp`

**Interfaces:**
- Consumes: Task 2 (`otl::View`).
- Produces (`KernTable.h`): `int32_t freeink::font::kerntable::PairKernAdjustment(const uint8_t* kernBytes,
  size_t kernSize, uint32_t leftGlyph, uint32_t rightGlyph, bool* covered = nullptr)`, in font units.

- [ ] **Step 1: Write the failing tests**

A kern table builder, the rules on a built table, and DejaVuSans against `FT_Get_Kerning`; the script compiles the
new source:

<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
// Host test for the GPOS pair-kerning reader (Gpos.h) and FtFont's kerning
// fallback through it. Run via test/host/run_gpos.sh under ASan/UBSan.
//
// Two layers: a synthetic hand-built GPOS blob exercises both PairPos
```
with:

```cpp
// Host test for the GPOS pair-kerning reader (Gpos.h) and FtFont's kerning
// fallback through it, and for the legacy kern table reader (KernTable.h).
// Run via test/host/run_gpos.sh under ASan/UBSan.
//
// Two layers: synthetic hand-built GPOS and kern blobs exercise both PairPos
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
// leaves kerning working through GPOS alone, and checks the resolved reader
// against the stateless one.
```
with:

```cpp
// leaves kerning working through GPOS alone, and checks the resolved reader
// against the stateless one and the kern table reader against FreeType.
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
#include "Gpos.h"
```
with:

```cpp
#include "Gpos.h"
#include "KernTable.h"
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
// sfnt table lookup for plain (non-collection) fonts — enough for fixtures.
```
with:

```cpp
// A legacy kern table, version 0: one subtable per entry of `subtables`, each (coverage, sorted pairs).
std::vector<uint8_t> kernTableWith(
    const std::vector<std::pair<uint16_t, std::vector<std::pair<uint32_t, int16_t>>>>& subtables) {
  std::vector<uint8_t> bytes;
  appendU16be(bytes, 0);
  appendU16be(bytes, uint16_t(subtables.size()));
  for (const auto& [coverage, pairs] : subtables) {
    appendU16be(bytes, 0);
    appendU16be(bytes, uint16_t(14 + 6 * pairs.size()));
    appendU16be(bytes, coverage);
    appendU16be(bytes, uint16_t(pairs.size()));
    appendU16be(bytes, 0);
    appendU16be(bytes, 0);
    appendU16be(bytes, 0);
    for (const auto& [pair, value] : pairs) {
      appendU32be(bytes, pair);
      appendU16be(bytes, uint16_t(value));
    }
  }
  return bytes;
}

// sfnt table lookup for plain (non-collection) fonts — enough for fixtures.
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
    expect(true, "resolved truncation/corruption sweep completed");
  }
```
with:

```cpp
    expect(true, "resolved truncation/corruption sweep completed");
  }

  // --- The legacy kern table -------------------------------------------------
  {
    using freeink::font::kerntable::PairKernAdjustment;
    const std::vector<uint8_t> table = kernTableWith({{0x0001, {{(10u << 16) | 20, -40}, {(10u << 16) | 40, 15}}},
                                                     {0x0001, {{(10u << 16) | 20, -5}}},
                                                     {0x0003, {{(10u << 16) | 20, -999}}},  // minimum values
                                                     {0x0005, {{(30u << 16) | 40, -999}}}});  // cross-stream
    bool covered = false;
    expect(PairKernAdjustment(table.data(), table.size(), 10, 20, &covered) == -45 && covered,
           "horizontal subtables add up; minimum and cross-stream ones do not count");
    expect(PairKernAdjustment(table.data(), table.size(), 10, 30, &covered) == 0 && covered,
           "a covered left glyph, kerning nothing with this right");
    expect(PairKernAdjustment(table.data(), table.size(), 30, 40, &covered) == 0 && !covered,
           "only a cross-stream subtable lists it: not covered");
    const std::vector<uint8_t> override =
        kernTableWith({{0x0001, {{(10u << 16) | 20, -40}}}, {0x0009, {{(10u << 16) | 20, -7}}}});
    expect(PairKernAdjustment(override.data(), override.size(), 10, 20) == -7, "an override subtable replaces");
    expect(PairKernAdjustment(nullptr, 0, 10, 20) == 0, "null kern table");
    for (size_t len = 0; len <= table.size(); ++len) (void)PairKernAdjustment(table.data(), len, 10, 20);
    for (size_t i = 0; i < table.size(); ++i) {
      std::vector<uint8_t> corrupt = table;
      corrupt[i] ^= 0xFF;
      (void)PairKernAdjustment(corrupt.data(), corrupt.size(), 10, 20);
    }
    expect(true, "kern table truncation/corruption sweep completed");
  }
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
    // Every printable ASCII pair: the resolved reader gives what the stateless
    // one gives.
```
with:

```cpp
    // Every printable ASCII pair: the resolved reader gives what the stateless
    // one gives, and the kern table reader what FreeType gives.
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
           "FreeType opens the fixture");
```
with:

```cpp
           "FreeType opens the fixture");
    size_t kernOffset = 0;
    size_t kernLength = 0;
    const bool hasKern = findTable(font, "kern", kernOffset, kernLength);
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
    bool resolvedAgrees = true;
    int kerned = 0;
```
with:

```cpp
    bool resolvedAgrees = true;
    bool kernAgrees = true;
    int kerned = 0;
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
          kerned += stateless != 0;
        }
```
with:

```cpp
          kerned += stateless != 0;
        }
        if (hasKern) {
          FT_Vector delta{0, 0};
          FT_Get_Kerning(face, gl, gr, FT_KERNING_UNSCALED, &delta);
          kernAgrees = kernAgrees && freeink::font::kerntable::PairKernAdjustment(&font[kernOffset], kernLength, gl,
                                                                                  gr) == int32_t(delta.x);
        }
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/GposKerningTest.cpp -->
```cpp
    printf("info: %u kern subtables resolved, %d ASCII pairs kerned\n", count, kerned);
```
with:

```cpp
    expect(kernAgrees, "the kern table reader agrees with FT_Get_Kerning on every ASCII pair");
    printf("info: %u kern subtables resolved, %d ASCII pairs kerned; legacy kern table %s\n", count, kerned,
           hasKern ? "checked against FreeType" : "absent");
```
<!-- plan-edit: libs/font/FreeInkFont/test/host/run_gpos.sh -->
```sh
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" "$FONT_ROOT/src/Gsub.cpp" "$FONT_ROOT/src/Gpos.cpp" \
```
with:

```sh
  "$FONT_ROOT/src/FtFont.cpp" "$FONT_ROOT/src/FontAlloc.c" "$FONT_ROOT/src/Gsub.cpp" "$FONT_ROOT/src/Gpos.cpp" \
  "$FONT_ROOT/src/KernTable.cpp" \
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_gpos.sh`
Expected: `fatal error: KernTable.h: No such file or directory`.

- [ ] **Step 3: The reader**

<!-- plan-file: libs/font/FreeInkOpenType/include/KernTable.h -->
```cpp
#pragma once

// FreeInk SDK — the legacy TrueType 'kern' table, for fonts that have no GPOS
// kerning (see Gpos.h). Reads what FreeType's FT_Get_Kerning() reads: the
// Microsoft table (version 0), its horizontal format 0 subtables that hold
// kerning values (not minimums, not cross-stream), added together unless a
// subtable says it overrides. Bounds-checked like the layout readers.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace kerntable {

// The kerning of `rightGlyph` after `leftGlyph` in FONT UNITS, or 0. `kernBytes`/`kernSize` is the raw content of the
// font's 'kern' table. `covered`, when not null, is set to whether any subtable kerns `leftGlyph` with anything.
int32_t PairKernAdjustment(const uint8_t* kernBytes, size_t kernSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           bool* covered = nullptr);

}  // namespace kerntable
}  // namespace font
}  // namespace freeink
```

<!-- plan-file: libs/font/FreeInkFont/src/KernTable.cpp -->
```cpp
#include "KernTable.h"

#include "OpenType.h"

namespace freeink {
namespace font {
namespace kerntable {

namespace {

using otl::View;

// A table lists a handful of subtables; more is malformed or hostile.
constexpr unsigned kMaxSubtables = 16;
constexpr unsigned kHorizontal = 0x0001;
constexpr unsigned kMinimum = 0x0002;
constexpr unsigned kCrossStream = 0x0004;
constexpr unsigned kOverride = 0x0008;

}  // namespace

int32_t PairKernAdjustment(const uint8_t* kernBytes, size_t kernSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           bool* covered) {
  bool anyCovered = false;
  int32_t total = 0;
  const View kern{kernBytes, kernBytes != nullptr ? kernSize : 0};
  // Version 0 only: Apple's version 1 table (a 32-bit 0x00010000) is not read, as FreeType does not either.
  if (kern.has(0, 4) && kern.u16(0) == 0 && leftGlyph <= 0xFFFF && rightGlyph <= 0xFFFF) {
    const unsigned tables = kern.u16(2);
    const uint32_t key = leftGlyph << 16 | rightGlyph;
    size_t at = 4;
    for (unsigned t = 0; t < tables && t < kMaxSubtables && kern.has(at, 6); ++t) {
      const unsigned length = kern.u16(at + 2);
      const unsigned coverage = kern.u16(at + 4);
      const View subtable = kern.sub(at);
      at += length;
      if ((coverage >> 8) != 0 || (coverage & (kHorizontal | kMinimum | kCrossStream)) != kHorizontal) {
        if (length < 6) break;
        continue;
      }
      // Format 0: nPairs, three search hints, then (left, right, value) records sorted by left << 16 | right.
      const unsigned pairs = subtable.u16(6);
      if (!subtable.has(14, size_t(pairs) * 6)) break;
      // The first pair at or after (left, 0) says whether the left glyph kerns at all; the second search finds the
      // pair.
      const auto firstAtOrAfter = [&](const uint32_t wanted) {
        unsigned low = 0;
        unsigned high = pairs;
        while (low < high) {
          const unsigned middle = (low + high) / 2;
          if (subtable.u32(14 + size_t(middle) * 6) < wanted)
            low = middle + 1;
          else
            high = middle;
        }
        return low;
      };
      const unsigned first = firstAtOrAfter(leftGlyph << 16);
      if (first < pairs && subtable.u32(14 + size_t(first) * 6) >> 16 == leftGlyph) anyCovered = true;
      const unsigned found = firstAtOrAfter(key);
      if (found < pairs && subtable.u32(14 + size_t(found) * 6) == key) {
        const int32_t value = subtable.s16(14 + size_t(found) * 6 + 4);
        total = (coverage & kOverride) != 0 ? value : total + value;
      }
      if (length < 6) break;
    }
  }
  if (covered != nullptr) *covered = anyCovered;
  return total;
}

}  // namespace kerntable
}  // namespace font
}  // namespace freeink
```

FreeInkOpenType builds it too:

<!-- plan-edit: libs/font/FreeInkOpenType/library.json -->
```json
readers on their own: GPOS pair kerning and GSUB ligatures. For
```
with:

```json
readers on their own: GPOS pair kerning (resolved once or per call), GSUB ligatures and the legacy kern table. For
```
<!-- plan-edit: libs/font/FreeInkOpenType/library.json -->
```json
      "+<Gsub.cpp>"
```
with:

```json
      "+<Gsub.cpp>",
      "+<KernTable.cpp>"
```

- [ ] **Step 4: Document the library**

<!-- plan-edit: libs/font/FreeInkFont/README.md -->
```markdown
### FtFont integration APIs
```
with:

```markdown
### The layout readers on their own: FreeInkOpenType

The three table readers, `Gpos.h` (GPOS pair kerning), `Gsub.h` (GSUB
ligatures) and `KernTable.h` (the legacy `kern` table), use nothing from
FreeType or stb_truetype: they take a table's raw bytes and glyph IDs. Their
headers live in `../FreeInkOpenType/include`, and the `FreeInkOpenType`
library builds just their three sources, so a consumer that draws fonts its
own way can kern and find ligatures without FreeInkFont's engines. A consumer
that already depends on FreeInkFont does not need it: FreeInkFont compiles the
same readers.

GPOS kerning follows what a shaper does with the `kern` feature: the feature
is the one any script's DefaultLangSys activates (Latin, Cyrillic and Greek
kern alike), its lookups apply in LookupList order, the first subtable that
matches the pair counts in each lookup, and the lookups' adjustments add up.
`PairKernAdjustment(gpos, size, left, right)` walks the script, feature and
lookup lists on every call. A caller kerning many pairs resolves the
subtables once with `ResolveKernSubtables()` (byte offsets into the table, so
they survive the table moving) and passes them to the resolved overload,
which gives the same value without the walk and can also report whether the
left glyph kerns with anything at all. `kerntable::PairKernAdjustment()` reads
what `FT_Get_Kerning()` reads, for fonts with no GPOS kerning. All three are
bounds-checked and keep their work bounded on hostile counts. `run_gpos.sh`
checks the resolved reader against the per-call one, and the kern table
reader against FreeType, on DejaVuSans.

### FtFont integration APIs
```
<!-- plan-edit: docs/testing.md -->
```markdown
invalidation, and a null-pointer safety check on `Gsub::LigatureGlyphId`.
```
with:

```markdown
invalidation, and a null-pointer safety check on `Gsub::LigatureGlyphId`.

Covers the GPOS and legacy kern table readers (FreeInkOpenType): both PairPos
formats, the `kern` feature under any script, lookups adding up, the
resolved-once API against the per-call walk, malformed-table robustness, and
on DejaVuSans the resolved reader against the per-call one and the kern table
reader against `FT_Get_Kerning`.
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `SANITIZERS= CC=gcc CXX=g++ sh libs/font/FreeInkFont/test/host/run_gpos.sh`
Expected: `43 checks, 0 failures` and `legacy kern table checked against FreeType`.

Run: `for f in Gpos Gsub KernTable; do g++ -std=c++17 -fsyntax-only -I libs/font/FreeInkOpenType/include libs/font/FreeInkFont/src/$f.cpp || exit 1; done && echo standalone OK`
Expected: `standalone OK`

- [ ] **Step 6: Commit, push, and open the SDK PR**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --style=file:C:/_development/witchhunt-reader/.clang-format --dry-run --Werror libs/font/FreeInkFont/src/KernTable.cpp libs/font/FreeInkOpenType/include/KernTable.h
git add libs/font/FreeInkOpenType libs/font/FreeInkFont/src/KernTable.cpp libs/font/FreeInkFont/test/host libs/font/FreeInkFont/README.md docs/testing.md
git commit -m "feat(font): read the legacy kern table; document FreeInkOpenType"
git push -u origin feat/opentype-readers
gh pr create --repo jpirnay/freeink-sdk --base witchhunt --head feat/opentype-readers \
  --title "feat(font): FreeInkOpenType: GPOS kerning as a shaper applies it, resolved once, and the legacy kern table" \
  --body "The OpenType layout readers as a library of their own (FreeInkOpenType), so a consumer with its own font engine kerns and finds ligatures without FreeType. GPOS: the kern feature from every script's DefaultLangSys, its lookups added up, the first matching subtable in each; a resolved-once API for callers that kern many pairs. KernTable: the legacy kern table, as FT_Get_Kerning reads it. FreeInkFont builds the same sources; Gpos.h and Gsub.h move to FreeInkOpenType/include, which FreeInkFont's build adds (a consumer that included them directly adds FreeInkOpenType; none in this SDK does). FtFont's kerning changes only for fonts whose kern feature has several lookups or sits under a script other than latn/DFLT. Tests: run_gpos.sh (43 checks; DejaVuSans resolved == per call, kern table == FreeType), run_ligature.sh, run.sh. Used by witchhunt-reader's TTF engine (Stage 2c-3)."
```

Ask the user to review and merge the PR; do not merge it yourself. Go on with Task 4 meanwhile: it builds against
this branch, and Task 7 moves the app's pin once the PR has merged.

---

### Task 4: Kerning and ligatures for the TTF engine, over the SDK readers (app)

The glue, on its own: `ttf::Face` finds the three optional tables, `FontBytes` hands out the mapped font, and
`TtfOpenType` resolves a style's kerning and ligatures through the SDK readers. The host tests run it on fonts the
test builds (which table kerns, which wins, a broken table) and on Bookerly (the resolved reader against the SDK's
per-call walk on every ASCII pair, and its fi and fl ligatures). The OpenType rules themselves are the SDK's tests.

**Files** (app worktree):
- Create: `lib/EpdFont/ttf/TtfOpenType.h`, `lib/EpdFont/ttf/TtfOpenType.cpp`, `test/ttf_font/TtfLayoutBuilder.h`,
  `test/ttf_font/TtfOpenTypeTest.cpp`
- Modify: `lib/EpdFont/ttf/TtfFace.h`, `lib/EpdFont/ttf/TtfFace.cpp`, `lib/EpdFont/ttf/FontBytes.h`,
  `test/ttf_font/CMakeLists.txt`, `platformio.ini`

**Interfaces:**
- Consumes: Tasks 2 and 3 (`gpos::ResolveKernSubtables`, the resolved `gpos::PairKernAdjustment`,
  `kerntable::PairKernAdjustment`) and FreeInkFont's existing `gsub::LigatureGlyphId(const uint8_t* gsubBytes,
  size_t gsubSize, const uint32_t* sequence, unsigned length)`.
- Produces:
  - `ttf::Face::gpos`, `gposLength`, `gsub`, `gsubLength`, `kern`, `kernLength` (0 when absent).
  - `const uint8_t* FontBytes::data() const`: the font in place, or null.
  - `class ttf::Kerning { static constexpr uint16_t kMaxSubtables = 1024; bool resolve(FontBytes&, const Face&);
    bool any() const; uint16_t subtableCount() const; bool legacy() const; int32_t pair(FontBytes&, const Face&,
    uint16_t left, uint16_t right, bool* covered = nullptr) const; }`: glyph IDs in, font units out.
  - `struct ttf::Ligatures { static constexpr uint32_t kFirst = 0xFB00; EpdLigaturePair pairs[5]; uint8_t count;
    uint16_t glyphs[5]; uint16_t glyphFor(FontBytes&, const Face&, uint32_t codepoint) const; }` and
    `bool ttf::resolveLigatures(FontBytes&, const Face&, Ligatures*)`.
  - Test helpers (`test/ttf_font/TtfLayoutBuilder.h`): `layoutTable(scripts, features, lookups, gsub)`,
    `legacyKernTable(pairs)`, and the `PairSubtable`, `LayoutLookup`, `LayoutFeature`, `LayoutScript` structs.

- [ ] **Step 1: The app worktree, on the SDK branch**

```bash
git -C /c/_development/witchhunt-reader fetch origin
git -C /c/_development/witchhunt-reader worktree add -b feat/ttf-kerning /c/tmp/wt-ttf-kerning origin/master
cd /c/tmp/wt-ttf-kerning
git submodule update --init freeink-sdk
git -C freeink-sdk fetch origin feat/opentype-readers
git -C freeink-sdk checkout --detach FETCH_HEAD   # Task 3's head; not staged until Task 7
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src
```

Tasks 4-7 run in `/c/tmp/wt-ttf-kerning`; their paths are relative to it.

- [ ] **Step 2: Write the failing tests**

The builder makes the layout tables a test needs: a GPOS or GSUB table from scripts, features and lookups (pair
adjustment subtables in format 1 or 2, optionally wrapped in Extension subtables; ligature subtables), and the legacy
kern table.

<!-- plan-file: test/ttf_font/TtfLayoutBuilder.h -->
```cpp
#pragma once
// Builds the OpenType layout tables the kerning and ligature tests need (GPOS pair adjustment, GSUB ligatures, the
// legacy kern table), to go into a built font through Font::extraTables.
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "TtfBuilder.h"

namespace ttf_test {

/// A PairPos subtable. Format 1 lists pairs per left glyph; format 2 kerns classes (`class1` and `class2` give a
/// glyph's class, `values` is class1Count x class2Count). Left glyphs must be listed in glyph order.
struct PairSubtable {
  uint16_t format = 1;
  std::vector<std::pair<uint16_t, std::vector<std::pair<uint16_t, int16_t>>>> pairs;  // format 1
  std::vector<uint16_t> coverage;                                                     // format 2: left glyphs
  std::vector<std::pair<uint16_t, uint16_t>> class1;                                  // format 2: glyph, class
  std::vector<std::pair<uint16_t, uint16_t>> class2;
  uint16_t class1Count = 0;
  uint16_t class2Count = 0;
  std::vector<int16_t> values;
};

struct LayoutLookup {
  uint16_t type = 2;       // 2 pair adjustment (GPOS), 4 ligature (GSUB), or any other to be ignored
  bool extension = false;  // wrapped in an Extension subtable (GPOS type 9, GSUB type 7)
  std::vector<PairSubtable> pairSubtables;
  std::vector<std::pair<std::vector<uint16_t>, uint16_t>> ligatures;  // GSUB: components (first included), glyph
};

struct LayoutFeature {
  std::string tag;
  std::vector<uint16_t> lookups;
};

struct LayoutScript {
  std::string tag;
  std::vector<uint16_t> features;  // the default language system's
};

namespace detail {

inline void putTag(Bytes& out, const std::string& tag) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
}

inline Bytes coverage(std::vector<uint16_t> glyphs) {
  Bytes out;
  put16(out, 1);
  put16(out, static_cast<uint32_t>(glyphs.size()));
  for (const uint16_t g : glyphs) put16(out, g);
  return out;
}

// A ClassDef in format 2, one range per glyph.
inline Bytes classDef(std::vector<std::pair<uint16_t, uint16_t>> classes) {
  std::sort(classes.begin(), classes.end());
  Bytes out;
  put16(out, 2);
  put16(out, static_cast<uint32_t>(classes.size()));
  for (const auto& [glyph, cls] : classes) {
    put16(out, glyph);
    put16(out, glyph);
    put16(out, cls);
  }
  return out;
}

inline Bytes pairSubtable(const PairSubtable& t) {
  Bytes out;
  if (t.format == 1) {
    std::vector<uint16_t> lefts;
    for (const auto& p : t.pairs) lefts.push_back(p.first);
    const uint32_t header = 10 + 2 * static_cast<uint32_t>(t.pairs.size());
    put16(out, 1);
    put16(out, 0);  // coverage, set below
    put16(out, 0x0004);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(t.pairs.size()));
    for (size_t i = 0; i < t.pairs.size(); ++i) put16(out, 0);
    out.resize(header);
    for (size_t i = 0; i < t.pairs.size(); ++i) {
      set16(out, 10 + 2 * i, static_cast<uint32_t>(out.size()));
      put16(out, static_cast<uint32_t>(t.pairs[i].second.size()));
      for (const auto& [second, value] : t.pairs[i].second) {
        put16(out, second);
        put16(out, static_cast<uint16_t>(value));
      }
    }
    set16(out, 2, static_cast<uint32_t>(out.size()));
    const Bytes cov = coverage(lefts);
    out.insert(out.end(), cov.begin(), cov.end());
    return out;
  }
  put16(out, 2);
  put16(out, 0);  // coverage
  put16(out, 0x0004);
  put16(out, 0);
  put16(out, 0);  // classDef1
  put16(out, 0);  // classDef2
  put16(out, t.class1Count);
  put16(out, t.class2Count);
  for (const int16_t v : t.values) put16(out, static_cast<uint16_t>(v));
  set16(out, 2, static_cast<uint32_t>(out.size()));
  const Bytes cov = coverage(t.coverage);
  out.insert(out.end(), cov.begin(), cov.end());
  set16(out, 8, static_cast<uint32_t>(out.size()));
  const Bytes c1 = classDef(t.class1);
  out.insert(out.end(), c1.begin(), c1.end());
  set16(out, 10, static_cast<uint32_t>(out.size()));
  const Bytes c2 = classDef(t.class2);
  out.insert(out.end(), c2.begin(), c2.end());
  return out;
}

// LigatureSubstFormat1 for ligatures that all start with the same glyph.
inline Bytes ligatureSubtable(const std::vector<std::pair<std::vector<uint16_t>, uint16_t>>& ligatures) {
  Bytes out;
  put16(out, 1);
  put16(out, 0);  // coverage
  put16(out, 1);  // one ligature set
  put16(out, 8);  // at 8
  put16(out, static_cast<uint32_t>(ligatures.size()));
  const size_t offsets = out.size();
  for (size_t i = 0; i < ligatures.size(); ++i) put16(out, 0);
  for (size_t i = 0; i < ligatures.size(); ++i) {
    set16(out, offsets + 2 * i, static_cast<uint32_t>(out.size() - 8));
    put16(out, ligatures[i].second);
    put16(out, static_cast<uint32_t>(ligatures[i].first.size()));
    for (size_t c = 1; c < ligatures[i].first.size(); ++c) put16(out, ligatures[i].first[c]);
  }
  set16(out, 2, static_cast<uint32_t>(out.size()));
  const Bytes cov = coverage({ligatures.front().first.front()});
  out.insert(out.end(), cov.begin(), cov.end());
  return out;
}

}  // namespace detail

/// A GPOS or GSUB table: its scripts (each with a default language system), features and lookups.
inline Bytes layoutTable(const std::vector<LayoutScript>& scripts, const std::vector<LayoutFeature>& features,
                         const std::vector<LayoutLookup>& lookups, const bool gsub = false) {
  Bytes scriptList;
  put16(scriptList, static_cast<uint32_t>(scripts.size()));
  for (size_t i = 0; i < scripts.size(); ++i) {
    detail::putTag(scriptList, scripts[i].tag);
    put16(scriptList, 0);
  }
  for (size_t i = 0; i < scripts.size(); ++i) {
    set16(scriptList, 2 + 6 * i + 4, static_cast<uint32_t>(scriptList.size()));
    put16(scriptList, 4);  // default language system right after this header
    put16(scriptList, 0);
    put16(scriptList, 0);
    put16(scriptList, 0xFFFF);
    put16(scriptList, static_cast<uint32_t>(scripts[i].features.size()));
    for (const uint16_t f : scripts[i].features) put16(scriptList, f);
  }
  Bytes featureList;
  put16(featureList, static_cast<uint32_t>(features.size()));
  for (size_t i = 0; i < features.size(); ++i) {
    detail::putTag(featureList, features[i].tag);
    put16(featureList, 0);
  }
  for (size_t i = 0; i < features.size(); ++i) {
    set16(featureList, 2 + 6 * i + 4, static_cast<uint32_t>(featureList.size()));
    put16(featureList, 0);
    put16(featureList, static_cast<uint32_t>(features[i].lookups.size()));
    for (const uint16_t l : features[i].lookups) put16(featureList, l);
  }
  Bytes lookupList;
  put16(lookupList, static_cast<uint32_t>(lookups.size()));
  for (size_t i = 0; i < lookups.size(); ++i) put16(lookupList, 0);
  for (size_t i = 0; i < lookups.size(); ++i) {
    const LayoutLookup& lookup = lookups[i];
    set16(lookupList, 2 + 2 * i, static_cast<uint32_t>(lookupList.size()));
    const size_t at = lookupList.size();
    std::vector<Bytes> subtables;
    if (lookup.type == 4 && !lookup.ligatures.empty()) subtables.push_back(detail::ligatureSubtable(lookup.ligatures));
    for (const auto& p : lookup.pairSubtables) subtables.push_back(detail::pairSubtable(p));
    put16(lookupList, lookup.extension ? (gsub ? 7 : 9) : lookup.type);
    put16(lookupList, 0);
    put16(lookupList, static_cast<uint32_t>(subtables.size()));
    for (size_t s = 0; s < subtables.size(); ++s) put16(lookupList, 0);
    for (size_t s = 0; s < subtables.size(); ++s) {
      set16(lookupList, at + 6 + 2 * s, static_cast<uint32_t>(lookupList.size() - at));
      if (lookup.extension) {
        put16(lookupList, 1);
        put16(lookupList, lookup.type);
        put32(lookupList, 8);
      }
      lookupList.insert(lookupList.end(), subtables[s].begin(), subtables[s].end());
    }
  }
  Bytes out;
  put32(out, 0x00010000);
  put16(out, 10);
  put16(out, static_cast<uint32_t>(10 + scriptList.size()));
  put16(out, static_cast<uint32_t>(10 + scriptList.size() + featureList.size()));
  out.insert(out.end(), scriptList.begin(), scriptList.end());
  out.insert(out.end(), featureList.begin(), featureList.end());
  out.insert(out.end(), lookupList.begin(), lookupList.end());
  return out;
}

/// The legacy kern table, version 0, one horizontal format 0 subtable.
inline Bytes legacyKernTable(std::vector<std::pair<std::pair<uint16_t, uint16_t>, int16_t>> pairs) {
  std::sort(pairs.begin(), pairs.end());
  Bytes out;
  put16(out, 0);
  put16(out, 1);
  put16(out, 0);
  put16(out, static_cast<uint32_t>(14 + 6 * pairs.size()));
  put16(out, 0x0001);
  put16(out, static_cast<uint32_t>(pairs.size()));
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  for (const auto& [glyphs, value] : pairs) {
    put16(out, glyphs.first);
    put16(out, glyphs.second);
    put16(out, static_cast<uint16_t>(value));
  }
  return out;
}

}  // namespace ttf_test
```

The tests: which table a style kerns from, on built fonts, then Bookerly against the SDK's per-call walk; the
ligatures likewise.

<!-- plan-file: test/ttf_font/TtfOpenTypeTest.cpp -->
```cpp
// Kerning and ligatures for the TTF engine (TtfOpenType): the glue over the SDK's FreeInkFont layout readers, which
// test the OpenType rules themselves (libs/font/FreeInkFont/test/host). Here: that a style finds and resolves its
// tables, which table it kerns from, and the ligatures EpdFont applies, on built fonts and on Bookerly.
#include <gtest/gtest.h>

#include <Gpos.h>
#include <Gsub.h>

#include <cstdint>
#include <vector>

#include "TtfBuilder.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
#include "TtfOpenType.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

// A font with glyphs 1..n for 'A'..: every glyph a box, so only the layout tables differ between tests.
Font boxes(const uint16_t n) {
  Font font;
  font.glyphs.resize(n + 1);
  for (uint16_t g = 0; g <= n; ++g) font.glyphs[g].contours = {rect(0, 0, 400, 600)};
  for (uint16_t g = 1; g <= n; ++g) font.cmap[static_cast<uint32_t>('A' + g - 1)] = g;
  return font;
}

struct Parsed {
  std::vector<uint8_t> file;
  FontBytes bytes;
  ttf::Face face;
  explicit Parsed(const Font& font) : file(buildFont(font)), bytes(file.data(), static_cast<uint32_t>(file.size())) {
    EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  }
};

// A GPOS whose 'latn' kern feature kerns 1-2 by -50 and 1-3 by 30.
Bytes kernedGpos() {
  LayoutLookup lookup;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -50}, {3, 30}}}};
  lookup.pairSubtables = {pairs};
  return layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {lookup});
}

}  // namespace

TEST(TtfLayoutTables, TheFaceFindsTheLayoutTables) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}, {"kern", legacyKernTable({{{1, 2}, -40}})}};
  Parsed p(font);
  EXPECT_NE(0u, p.face.gpos);
  EXPECT_NE(0u, p.face.kern);
  EXPECT_EQ(0u, p.face.gsub);
}

TEST(TtfKerning, GposKernsThroughTheResolvedSubtables) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}};
  Parsed p(font);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_EQ(1u, kerning.subtableCount());
  EXPECT_FALSE(kerning.legacy());
  bool covered = false;
  EXPECT_EQ(-50, kerning.pair(p.bytes, p.face, 1, 2, &covered));
  EXPECT_TRUE(covered);
  EXPECT_EQ(30, kerning.pair(p.bytes, p.face, 1, 3));
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 1, 4, &covered));
  EXPECT_TRUE(covered);  // 1 kerns, just not with 4: worth remembering as 0
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 3, 1, &covered));
  EXPECT_FALSE(covered);  // 3 kerns with nothing
}

TEST(TtfKerning, WithoutGposTheLegacyKernTableKerns) {
  Font font = boxes(4);
  font.extraTables = {{"kern", legacyKernTable({{{1, 2}, -40}, {{1, 4}, 15}, {{3, 1}, -5}})}};
  Parsed p(font);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_TRUE(kerning.legacy());
  bool covered = false;
  EXPECT_EQ(-40, kerning.pair(p.bytes, p.face, 1, 2, &covered));
  EXPECT_TRUE(covered);
  EXPECT_EQ(15, kerning.pair(p.bytes, p.face, 1, 4));
  EXPECT_EQ(-5, kerning.pair(p.bytes, p.face, 3, 1));
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 2, 1, &covered));
  EXPECT_FALSE(covered);
}

TEST(TtfKerning, GposWinsOverTheLegacyTable) {
  Font font = boxes(4);
  font.extraTables = {{"GPOS", kernedGpos()}, {"kern", legacyKernTable({{{1, 2}, -40}})}};
  Parsed p(font);
  ttf::Kerning kerning;
  ASSERT_TRUE(kerning.resolve(p.bytes, p.face));
  EXPECT_FALSE(kerning.legacy());
  EXPECT_EQ(-50, kerning.pair(p.bytes, p.face, 1, 2));
}

TEST(TtfKerning, AFontWithoutKerningResolvesNothing) {
  Parsed p(boxes(4));
  ttf::Kerning kerning;
  EXPECT_FALSE(kerning.resolve(p.bytes, p.face));
  EXPECT_FALSE(kerning.any());
  bool covered = true;
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 1, 2, &covered));
  EXPECT_FALSE(covered);
}

TEST(TtfKerning, ABrokenGposKernsNothing) {
  Font font = boxes(4);
  Bytes gpos = kernedGpos();
  set16(gpos, 8, 0xFFF0);  // the lookup list points past the table
  font.extraTables = {{"GPOS", gpos}};
  Parsed p(font);
  ttf::Kerning kerning;
  EXPECT_FALSE(kerning.resolve(p.bytes, p.face));
  EXPECT_EQ(0, kerning.pair(p.bytes, p.face, 1, 2));
}

TEST(TtfKerning, BookerlyResolvedAgreesWithTheStatelessReader) {
  // The resolved subtables must give what the SDK's per-call walk gives: every pair of printable ASCII in each of
  // the four styles.
  for (const char* name : {"Bookerly/Bookerly-Regular.ttf", "Bookerly/Bookerly-Bold.ttf",
                           "Bookerly/Bookerly-Italic.ttf", "Bookerly/Bookerly-BoldItalic.ttf"}) {
    const std::vector<uint8_t> file = readFont(name);
    FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
    ttf::Kerning kerning;
    ASSERT_TRUE(kerning.resolve(bytes, face)) << name;
    int kerned = 0;
    for (uint32_t l = 0x21; l < 0x7F; ++l) {
      for (uint32_t r = 0x21; r < 0x7F; ++r) {
        const uint16_t gl = ttf::glyphIndex(bytes, face, l);
        const uint16_t gr = ttf::glyphIndex(bytes, face, r);
        const int32_t expected =
            freeink::font::gpos::PairKernAdjustment(file.data() + face.gpos, face.gposLength, gl, gr);
        ASSERT_EQ(expected, kerning.pair(bytes, face, gl, gr)) << name << " " << char(l) << char(r);
        kerned += expected != 0;
      }
    }
    EXPECT_GT(kerned, 500) << name;  // a real kerning table, not a vacuous match
  }
}

// --- Ligatures ---

TEST(TtfLigatures, TheFontsFfiLigaturesBecomeEpdFontPairs) {
  Font font = boxes(10);
  font.cmap['f'] = 1;
  font.cmap['i'] = 2;
  font.cmap['l'] = 3;
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 1, 2}, 7}, {{1, 1, 3}, 8}, {{1, 1}, 4}, {{1, 2}, 5}, {{1, 3}, 6}};  // ffi ffl ff fi fl
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  Parsed p(font);
  ttf::Ligatures ligatures;
  ASSERT_TRUE(ttf::resolveLigatures(p.bytes, p.face, &ligatures));
  EXPECT_EQ(5, ligatures.count);
  const uint16_t expectedGlyphs[5] = {4, 5, 6, 7, 8};  // ff fi fl ffi ffl
  for (int k = 0; k < 5; ++k) EXPECT_EQ(expectedGlyphs[k], ligatures.glyphs[k]) << k;
  // Sorted as EpdFont searches them; ffi and ffl chain through ff.
  EXPECT_EQ(('f' << 16 | 'f'), ligatures.pairs[0].pair);
  EXPECT_EQ(0xFB00u, ligatures.pairs[0].ligatureCp);
  EXPECT_EQ((0xFB00u << 16 | 'i'), ligatures.pairs[3].pair);
  EXPECT_EQ(0xFB03u, ligatures.pairs[3].ligatureCp);
  for (int k = 1; k < 5; ++k) EXPECT_LT(ligatures.pairs[k - 1].pair, ligatures.pairs[k].pair);
  EXPECT_EQ(5, ligatures.glyphFor(p.bytes, p.face, 0xFB01));  // the GSUB glyph, whatever the cmap says
  EXPECT_EQ(2, ligatures.glyphFor(p.bytes, p.face, 'i'));
}

TEST(TtfLigatures, WithoutAnFfLigatureFfiDoesNotChain) {
  Font font = boxes(10);
  font.cmap['f'] = 1;
  font.cmap['i'] = 2;
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 1, 2}, 7}, {{1, 2}, 5}};  // ffi and fi, no ff
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  Parsed p(font);
  ttf::Ligatures ligatures;
  ASSERT_TRUE(ttf::resolveLigatures(p.bytes, p.face, &ligatures));
  EXPECT_EQ(1, ligatures.count);  // fi: "ffi" then draws f + fi
  EXPECT_EQ(0xFB01u, ligatures.pairs[0].ligatureCp);
}

TEST(TtfLigatures, AFontWithoutGsubHasNone) {
  Parsed p(boxes(4));
  ttf::Ligatures ligatures;
  EXPECT_FALSE(ttf::resolveLigatures(p.bytes, p.face, &ligatures));
  EXPECT_EQ(0, ligatures.count);
}

TEST(TtfLigatures, BookerlyHasTheFiAndFlLigatures) {
  for (const char* name : {"Bookerly/Bookerly-Regular.ttf", "Bookerly/Bookerly-Bold.ttf",
                           "Bookerly/Bookerly-Italic.ttf", "Bookerly/Bookerly-BoldItalic.ttf"}) {
    const std::vector<uint8_t> file = readFont(name);
    FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
    ttf::Ligatures ligatures;
    ASSERT_TRUE(ttf::resolveLigatures(bytes, face, &ligatures)) << name;
    EXPECT_NE(0, ligatures.glyphs[1]) << name << " fi";
    EXPECT_NE(0, ligatures.glyphs[2]) << name << " fl";
  }
}
```

The test target builds them with the SDK readers, as the firmware links them through FreeInkOpenType:

<!-- plan-edit: test/ttf_font/CMakeLists.txt -->
```cmake
  TtfRegistryTest.cpp
```
with:

```cmake
  TtfRegistryTest.cpp
  TtfOpenTypeTest.cpp
```
<!-- plan-edit: test/ttf_font/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
```
with:

```cmake
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
  # the SDK's OpenType layout readers TtfOpenType uses (the firmware links them through FreeInkOpenType)
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/Gpos.cpp
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/Gsub.cpp
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/KernTable.cpp
```
<!-- plan-edit: test/ttf_font/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
```
with:

```cmake
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkOpenType/include
  ${REPO_ROOT}/lib/Logging
```

The face's new fields and the font's bytes in place come now too, before the first compile: every object the failing
build compiles must already see the new headers (see "A header changed after a failed build" in Global Constraints).

<!-- plan-edit: lib/EpdFont/ttf/TtfFace.h -->
```cpp
  uint32_t cmapEnd = 0;       ///< end of the cmap table: the subtable's reads stay below it
```
with:

```cpp
  uint32_t cmapEnd = 0;       ///< end of the cmap table: the subtable's reads stay below it
  uint32_t gpos = 0;          ///< the layout tables kerning and ligatures read (TtfOpenType); 0 when absent
  uint32_t gposLength = 0;
  uint32_t gsub = 0;
  uint32_t gsubLength = 0;
  uint32_t kern = 0;  ///< the legacy kerning table
  uint32_t kernLength = 0;
```

<!-- plan-edit: lib/EpdFont/ttf/FontBytes.h -->
```cpp
  uint32_t size() const { return size_; }
```
with:

```cpp
  uint32_t size() const { return size_; }
  /// The file in place, for readers that take a pointer and bound themselves (the SDK's OpenType layout readers).
  const uint8_t* data() const { return data_; }
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: a compile error, `TtfOpenType.h: No such file or directory`.

- [ ] **Step 4: Find the tables**

<!-- plan-edit: lib/EpdFont/ttf/TtfFace.cpp -->
```cpp
  face->cmapEnd = cmap.offset + cmap.length;
```
with:

```cpp
  // Optional: a font without them draws without kerning or ligatures.
  Table layout;
  if (findTable(bytes, numTables, tag("GPOS"), 10, &layout)) {
    face->gpos = layout.offset;
    face->gposLength = layout.length;
  }
  if (findTable(bytes, numTables, tag("GSUB"), 10, &layout)) {
    face->gsub = layout.offset;
    face->gsubLength = layout.length;
  }
  if (findTable(bytes, numTables, tag("kern"), 4, &layout)) {
    face->kern = layout.offset;
    face->kernLength = layout.length;
  }

  face->cmapEnd = cmap.offset + cmap.length;
```

- [ ] **Step 5: The glue**

<!-- plan-file: lib/EpdFont/ttf/TtfOpenType.h -->
```cpp
#pragma once

#include <EpdFontData.h>

#include <cstdint>
#include <memory>

#include "FontBytes.h"
#include "TtfFace.h"

/// Kerning and ligatures for the TTF engine, from the font's layout tables (spec 5.4). The tables are read by the SDK's
/// FreeInkFont readers (Gpos.h, Gsub.h, KernTable.h), shared with its FreeType font; this is the glue: resolving them
/// once per style, and the ligatures as EpdFont applies them. Offsets only, so a resolved style stays valid when the
/// font's mapping moves. Integer only.
namespace ttf {

/// A style's kerning: the GPOS 'kern' feature's subtables, resolved once, or, for a font without them, the legacy
/// 'kern' table. A pair's kerning is what OpenType layout gives (see Gpos.h).
class Kerning {
 public:
  /// More would be a malformed or hostile table: the first ones are kept.
  static constexpr uint16_t kMaxSubtables = 1024;

  /// Resolves the subtables (one heap block, 4 bytes each). False, with nothing held, for a font without kerning or
  /// when memory is short; kerning is then 0.
  bool resolve(FontBytes& bytes, const Face& face);
  bool any() const { return count_ != 0 || legacy_; }
  uint16_t subtableCount() const { return count_; }
  /// Kerned from the legacy kern table (the font has no GPOS kerning).
  bool legacy() const { return legacy_; }

  /// The horizontal kerning between two glyphs, in font units. `covered` (optional) is false when nothing the font
  /// kerns starts with `left`: such a pair is always 0 and need not be remembered.
  int32_t pair(FontBytes& bytes, const Face& face, uint16_t left, uint16_t right, bool* covered = nullptr) const;

 private:
  std::unique_ptr<uint32_t[]> subtables_;  // offsets into the GPOS table (Gpos.h)
  uint16_t count_ = 0;
  bool legacy_ = false;
};

/// The ligatures EpdFont applies (fi, fl, ff, ffi, ffl) where the font's GSUB has them. They are keyed by their
/// Unicode presentation forms, U+FB00-FB04, whatever the font's cmap says of those: `glyphs` gives the glyph each one
/// draws. ffi and ffl chain through ff, as EpdFont applies them two at a time.
struct Ligatures {
  static constexpr uint32_t kFirst = 0xFB00;  // ff, fi, fl, ffi, ffl
  EpdLigaturePair pairs[5] = {};
  uint8_t count = 0;
  uint16_t glyphs[5] = {};  // for U+FB00 + i; 0 when the font has no such ligature

  /// The glyph that draws `codepoint`: a ligature's, else the cmap's.
  uint16_t glyphFor(FontBytes& bytes, const Face& face, uint32_t codepoint) const;
};

/// Resolves the ligatures; false (none) for a font without them.
bool resolveLigatures(FontBytes& bytes, const Face& face, Ligatures* out);

}  // namespace ttf
```

<!-- plan-file: lib/EpdFont/ttf/TtfOpenType.cpp -->
```cpp
#include "TtfOpenType.h"

#include <Gpos.h>
#include <Gsub.h>
#include <KernTable.h>
#include <Memory.h>

namespace ttf {

bool Kerning::resolve(FontBytes& bytes, const Face& face) {
  subtables_.reset();
  count_ = 0;
  legacy_ = false;
  const uint8_t* font = bytes.data();
  if (font == nullptr) return false;
  if (face.gpos != 0) {
    const uint8_t* gpos = font + face.gpos;
    const unsigned found = freeink::font::gpos::ResolveKernSubtables(gpos, face.gposLength, nullptr, 0);
    if (found != 0) {
      const uint16_t keep = found < kMaxSubtables ? static_cast<uint16_t>(found) : kMaxSubtables;
      subtables_ = makeUniqueNoThrow<uint32_t[]>(keep);
      if (!subtables_) return false;
      freeink::font::gpos::ResolveKernSubtables(gpos, face.gposLength, subtables_.get(), keep);
      count_ = keep;
      return true;
    }
  }
  // No GPOS kerning: the legacy table, read in place at each pair.
  legacy_ = face.kern != 0;
  return legacy_;
}

int32_t Kerning::pair(FontBytes& bytes, const Face& face, const uint16_t left, const uint16_t right,
                      bool* covered) const {
  const uint8_t* font = bytes.data();
  if (font != nullptr && count_ != 0) {
    return freeink::font::gpos::PairKernAdjustment(font + face.gpos, face.gposLength, subtables_.get(), count_, left,
                                                   right, covered);
  }
  if (font != nullptr && legacy_) {
    return freeink::font::kerntable::PairKernAdjustment(font + face.kern, face.kernLength, left, right, covered);
  }
  if (covered) *covered = false;
  return 0;
}

uint16_t Ligatures::glyphFor(FontBytes& bytes, const Face& face, const uint32_t codepoint) const {
  if (codepoint >= kFirst && codepoint < kFirst + 5 && glyphs[codepoint - kFirst] != 0) {
    return glyphs[codepoint - kFirst];
  }
  return glyphIndex(bytes, face, codepoint);
}

bool resolveLigatures(FontBytes& bytes, const Face& face, Ligatures* out) {
  *out = Ligatures{};
  if (face.gsub == 0 || bytes.data() == nullptr) return false;
  const uint32_t f = glyphIndex(bytes, face, 'f');
  const uint32_t i = glyphIndex(bytes, face, 'i');
  const uint32_t l = glyphIndex(bytes, face, 'l');
  if (f == 0) return false;
  const uint32_t sequences[5][3] = {{f, f, 0}, {f, i, 0}, {f, l, 0}, {f, f, i}, {f, f, l}};
  const unsigned lengths[5] = {2, 2, 2, 3, 3};
  for (uint8_t k = 0; k < 5; ++k) {
    if (sequences[k][lengths[k] - 1] == 0) continue;
    const uint32_t glyph = freeink::font::gsub::LigatureGlyphId(bytes.data() + face.gsub, face.gsubLength,
                                                                sequences[k], lengths[k]);
    out->glyphs[k] = glyph <= 0xFFFF ? static_cast<uint16_t>(glyph) : 0;
  }
  // In the order EpdFont searches them (left << 16 | right); ffi and ffl chain through ff.
  const auto add = [out](const uint32_t left, const uint32_t right, const uint8_t k) {
    if (out->glyphs[k] != 0) out->pairs[out->count++] = EpdLigaturePair{left << 16 | right, Ligatures::kFirst + k};
  };
  add('f', 'f', 0);
  add('f', 'i', 1);
  add('f', 'l', 2);
  if (out->glyphs[0] != 0) {
    add(Ligatures::kFirst, 'i', 3);
    add(Ligatures::kFirst, 'l', 4);
  }
  return out->count != 0;
}

}  // namespace ttf
```

<!-- plan-edit: test/ttf_font/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
```
with:

```cmake
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfOpenType.cpp
```

The firmware links the SDK readers through FreeInkOpenType (Task 6 builds it):

<!-- plan-edit: platformio.ini -->
```ini
  Icons=symlink://freeink-sdk/libs/assets/Icons
```
with:

```ini
  Icons=symlink://freeink-sdk/libs/assets/Icons
; FreeInkFont's OpenType layout readers on their own (GPOS kerning, GSUB ligatures, the legacy kern table): the
; TTF engine's kerning and ligatures (lib/EpdFont/ttf/TtfOpenType), without FreeInkFont's FreeType and stb engines.
  FreeInkOpenType=symlink://freeink-sdk/libs/font/FreeInkOpenType
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 81 tests.` (70 before; `BookerlyResolvedAgreesWithTheStatelessReader` compares 8,836 pairs per
style.)

- [ ] **Step 7: Commit**

```bash
git add lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp lib/EpdFont/ttf/FontBytes.h lib/EpdFont/ttf/TtfOpenType.h lib/EpdFont/ttf/TtfOpenType.cpp test/ttf_font platformio.ini
git commit -m "feat(ttf): kerning and ligatures from a font's layout tables, through the SDK readers"
```

---

### Task 5: `TtfFont` kerns and draws ligatures (app)

`TtfFont` resolves a style's kerning and ligatures when it opens the family, and:

- **`configure()`** points every size's `EpdFontData` at the style's ligature pairs, so `EpdFont::applyLigatures`
  sees them in layout and drawing;
- **`render()` and `prefetch()`** draw a ligature codepoint with the glyph GSUB names (`glyphFor()`);
- **each size's provider** answers `kerning()`: the stored pair if the index has it, else the font's (then stored,
  when anything kerns the left glyph), scaled to the size;
- **the logs** name what the regular style kerns and ligates (at load) and how many pairs were worked out from the
  font (at unload); Task 7 reads them.

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`, `lib/EpdFont/SdCardFontManager.cpp`
- Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: Task 4 (`ttf::Kerning`, `ttf::Ligatures`, `ttf::resolveLigatures`).
- Produces: `int8_t TtfFont::kerning(uint8_t style, uint32_t left, uint32_t right)` (the body size, 4.4 pixels),
  `uint32_t TtfFont::kernLookups() const`, and `StyleProvider::kerning()`.

- [ ] **Step 1: Write the failing tests**

<!-- plan-edit: test/ttf_font/TtfFontTest.cpp -->
```cpp
#include "TtfFontTestUtil.h"
#include "TtfTestUtil.h"
```
with:

```cpp
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
#include "TtfTestUtil.h"
```
<!-- plan-append: test/ttf_font/TtfFontTest.cpp -->
```cpp
// --- Kerning and ligatures ---

namespace {

// Glyphs 1-4 for A-D, 1000 units per em. A kerns B by -100 units and D by 0; C kerns with nothing.
Font kernedBoxes() {
  Font font;
  font.glyphs.resize(5);
  for (auto& glyph : font.glyphs) glyph.contours = {rect(0, 0, 400, 600)};
  for (uint16_t g = 1; g <= 4; ++g) font.cmap[static_cast<uint32_t>('A' + g - 1)] = g;
  LayoutLookup lookup;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -100}, {4, 0}}}};
  lookup.pairSubtables = {pairs};
  font.extraTables = {{"GPOS", layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {lookup})}};
  return font;
}

}  // namespace

TEST_F(TtfFontTest, KerningIsTheFontsAtEachSizesScale) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  // -100 units at 29.17 px per em: -2.9 px, in 4.4. Asked again, as EpdFont asks, it comes from the store.
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(-47, font->font(kRegular)->getKerning('A', 'B'));
  EXPECT_EQ(1u, font->kernLookups());
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(48, fonts));
  EXPECT_EQ(-77, fonts[kRegular]->getKerning('A', 'B'));  // the same pair at 48 px: -4.8 px
  EXPECT_EQ(1u, font->kernLookups());
}

TEST_F(TtfFontTest, StoredKerningServesTheNextBoot) {
  useBuiltFont(kernedBoxes());
  {
    const auto font = open();
    ASSERT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  }
  const auto font = open();
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(0u, font->kernLookups());
}

TEST_F(TtfFontTest, OnlyPairsWhoseLeftGlyphKernsAreStored) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(0, font->kerning(kRegular, 'C', 'A'));  // C kerns with nothing: no slot spent
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'D'));  // A kerns, not with D: remembered as 0
  EXPECT_LT(writes, flash.programCalls());
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'D'));
  EXPECT_EQ(2u, font->kernLookups());
}

TEST_F(TtfFontTest, KerningGoesOnWithoutWritesInRingMode) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  flash.failNextProgram();
  ASSERT_NE(nullptr, font->glyph(kRegular, 'A'));
  ASSERT_TRUE(font->ringMode());
  const uint32_t writes = flash.programCalls();
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(writes, flash.programCalls());
}

TEST_F(TtfFontTest, AFontWithoutKerningAsksNothing) {
  Font plain = kernedBoxes();
  plain.extraTables.clear();
  useBuiltFont(plain);
  const auto font = open();
  EXPECT_EQ(0, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(0u, font->kernLookups());
}

TEST_F(TtfFontTest, TheFiLigatureDrawsTheGlyphGsubNames) {
  Font font;
  font.glyphs.resize(4);
  font.glyphs[0].contours = {rect(0, 0, 400, 600)};
  font.glyphs[1].contours = {rect(0, 0, 300, 700)};  // f
  font.glyphs[2].contours = {rect(0, 0, 200, 500)};  // i
  font.glyphs[3].contours = {rect(0, 0, 900, 700)};  // the fi ligature: wider than f and i together
  font.cmap = {{'f', 1}, {'i', 2}};
  LayoutLookup liga;
  liga.type = 4;
  liga.ligatures = {{{1, 2}, 3}};
  font.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"liga", {0}}}, {liga}, /*gsub=*/true)}};
  useBuiltFont(font);
  const auto ttf = open();
  ASSERT_NE(nullptr, ttf);
  const EpdFont* regular = ttf->font(kRegular);
  EXPECT_EQ(1u, regular->data->ligaturePairCount);
  const char* rest = "i";
  EXPECT_EQ(0xFB01u, regular->applyLigatures('f', rest));
  const EpdGlyph* fi = ttf->glyph(kRegular, 0xFB01);
  ASSERT_NE(nullptr, fi);
  EXPECT_GE(fi->width, 26);  // 900 units at 29.17 px per em
  // Every size shares the style's table.
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(ttf->nativeFonts(40, fonts));
  EXPECT_EQ(regular->data->ligaturePairs, fonts[kRegular]->data->ligaturePairs);
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: compile errors: `'class TtfFont' has no member named 'kerning'` and
`'class TtfFont' has no member named 'kernLookups'`.

- [ ] **Step 3: Implement**

<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
#include "TtfFamily.h"
#include "TtfRasterizer.h"
```
with:

```cpp
#include "TtfFamily.h"
#include "TtfOpenType.h"
#include "TtfRasterizer.h"
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
/// its own entry in the store's size table. Kerning and ligatures come in later stages.
```
with:

```cpp
/// its own entry in the store's size table. Kerning comes from the font's GPOS (or legacy kern) table through the
/// store's pair index, and the fi/fl/ff/ffi/ffl ligatures from its GSUB table (spec 5.4).
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  uint32_t storeHits() const { return hits_; }
```
with:

```cpp
  uint32_t storeHits() const { return hits_; }
  /// Pairs whose kerning was worked out from the font, not read from the store.
  uint32_t kernLookups() const { return kernLookups_; }

  /// The kerning between two codepoints of `style` at the body size, in 4.4 pixels (EpdFont's unit).
  int8_t kerning(uint8_t style, uint32_t left, uint32_t right) { return kerning(body_, style, left, right); }
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
    void prefetch(const char* utf8Text) override { owner->prefetch(*size, style, utf8Text); }
  };
```
with:

```cpp
    void prefetch(const char* utf8Text) override { owner->prefetch(*size, style, utf8Text); }
    int8_t kerning(const uint32_t leftCp, const uint32_t rightCp) override {
      return owner->kerning(*size, style, leftCp, rightCp);
    }
  };
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  struct Style {
    bool present = false;
    uint8_t block = 0;  // the store's style block
    uint32_t bytes = 0;
    ttf::Face face;
  };
```
with:

```cpp
  struct Style {
    bool present = false;
    uint8_t block = 0;  // the store's style block
    uint32_t bytes = 0;
    ttf::Face face;
    ttf::Kerning kerning;
    ttf::Ligatures ligatures;  // every size's EpdFontData points at its pairs
  };
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  const EpdGlyph* glyph(Size& size, uint8_t style, uint32_t codepoint);
```
with:

```cpp
  const EpdGlyph* glyph(Size& size, uint8_t style, uint32_t codepoint);
  int8_t kerning(const Size& size, uint8_t style, uint32_t left, uint32_t right);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.h -->
```cpp
  uint32_t renders_ = 0;
  uint32_t hits_ = 0;
```
with:

```cpp
  uint32_t renders_ = 0;
  uint32_t hits_ = 0;
  uint32_t kernLookups_ = 0;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    style.present = false;
    style.face = {};
    if (files[s].size == 0) continue;
```
with:

```cpp
    style.present = false;
    style.face = {};
    style.kerning = ttf::Kerning{};
    style.ligatures = ttf::Ligatures{};
    if (files[s].size == 0) continue;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    style.present = true;
```
with:

```cpp
    style.present = true;
    // Kerning and ligatures, resolved once per style (spec 5.4): a style without them draws without.
    style.kerning.resolve(bytes, style.face);
    ttf::resolveLigatures(bytes, style.face, &style.ligatures);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    style.data.provider = &style.provider;
```
with:

```cpp
    style.data.provider = &style.provider;
    const ttf::Ligatures& ligatures = styles_[s].ligatures;
    style.data.ligaturePairs = ligatures.count != 0 ? ligatures.pairs : nullptr;
    style.data.ligaturePairCount = ligatures.count;
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  LOG_INF("TTF", "family loaded: %u B scratch, %u B per native size, log %lu of %lu B used",
          static_cast<unsigned>(rasterizer_.scratchBytes()), static_cast<unsigned>(sizeof(Size)),
          static_cast<unsigned long>(store_.logUsed()), static_cast<unsigned long>(store_.geometry().logSize));
```
with:

```cpp
  LOG_INF("TTF", "family loaded: %u B scratch, %u B per native size, log %lu of %lu B used",
          static_cast<unsigned>(rasterizer_.scratchBytes()), static_cast<unsigned>(sizeof(Size)),
          static_cast<unsigned long>(store_.logUsed()), static_cast<unsigned long>(store_.geometry().logSize));
  LOG_INF("TTF", "regular style: %u kern subtables%s, %u ligatures", styles_[0].kerning.subtableCount(),
          styles_[0].kerning.legacy() ? " (legacy kern table)" : "", styles_[0].ligatures.count);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
  const uint16_t index = ttf::glyphIndex(bytes, face, codepoint);
```
with:

```cpp
  const uint16_t index = styles_[style].ligatures.glyphFor(bytes, face, codepoint);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
    const uint16_t index = ttf::glyphIndex(bytes, face, cp);
```
with:

```cpp
    const uint16_t index = styles_[style].ligatures.glyphFor(bytes, face, cp);
```
<!-- plan-edit: lib/EpdFont/ttf/TtfFont.cpp -->
```cpp
bool TtfFont::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
```
with:

```cpp
int8_t TtfFont::kerning(const Size& size, const uint8_t style, const uint32_t left, const uint32_t right) {
  if (style >= 4 || !styles_[style].present || !styles_[style].kerning.any() || size.ppem == 0) return 0;
  const Style& s = styles_[style];
  // The store keys a pair by its codepoints (BMP only) and keeps font units, so every size shares it.
  const bool storable = left <= 0xFFFF && right <= 0xFFFF;
  int16_t units = 0;
  if (!storable || !store_.isOpen() ||
      !store_.findPair(static_cast<uint16_t>(left), static_cast<uint16_t>(right), style, &units)) {
    checkLock();
    FontBytes bytes;
    if (!fontBytes(style, &bytes)) return 0;
    const uint16_t l = s.ligatures.glyphFor(bytes, s.face, left);
    const uint16_t r = s.ligatures.glyphFor(bytes, s.face, right);
    bool covered = false;
    const int32_t value = l != 0 && r != 0 ? s.kerning.pair(bytes, s.face, l, r, &covered) : 0;
    ++kernLookups_;
    if (bytes.ioError()) return 0;  // SD mode: not an answer, ask again
    units = static_cast<int16_t>(value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : value);
    // A left glyph that kerns with nothing costs no slot (spec 8.1): the CJK repertoire would fill the index.
    if (!covered) return 0;
    if (storable && !ringMode_) {
      store_.storePair(static_cast<uint16_t>(left), static_cast<uint16_t>(right), style, units);
    }
  }
  // Font units to 4.4 pixels at this size, rounded, clamped like the converter's kern matrix.
  const int64_t num = static_cast<int64_t>(units) * size.ppem;  // 26.6 ppem: units * ppem / upem * 16
  const int64_t den = 4 * static_cast<int64_t>(s.face.unitsPerEm);
  const int64_t value = (num >= 0 ? num + den / 2 : num - den / 2) / den;
  return static_cast<int8_t>(value < INT8_MIN ? INT8_MIN : value > INT8_MAX ? INT8_MAX : value);
}

bool TtfFont::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
```

<!-- plan-edit: lib/EpdFont/SdCardFontManager.cpp -->
```cpp
    LOG_INF("SDMGR", "TTF family %s unloaded: %lu glyphs rendered, %lu read from the store", loadedFamilyName_.c_str(),
            static_cast<unsigned long>(ttf_->renders()), static_cast<unsigned long>(ttf_->storeHits()));
```
with:

```cpp
    LOG_INF("SDMGR",
            "TTF family %s unloaded: %lu glyphs rendered, %lu read from the store, %lu kerning pairs worked out",
            loadedFamilyName_.c_str(), static_cast<unsigned long>(ttf_->renders()),
            static_cast<unsigned long>(ttf_->storeHits()), static_cast<unsigned long>(ttf_->kernLookups()));
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 87 tests.`

- [ ] **Step 5: Commit**

```bash
git add lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp lib/EpdFont/SdCardFontManager.cpp test/ttf_font/TtfFontTest.cpp
git commit -m "feat(ttf): TtfFont kerns through the store's pair index and draws GSUB ligatures"
```

---

### Task 6: The section cache, and the builds (app)

**Files:**
- Modify: `lib/Epub/Epub/Section.cpp`

- [ ] **Step 1: Bump the section version**

<!-- plan-edit: lib/Epub/Epub/Section.cpp -->
```cpp
constexpr uint8_t SECTION_FILE_VERSION = 82;  // v82: a TTF family draws every size with a face of its own
```
with:

```cpp
constexpr uint8_t SECTION_FILE_VERSION = 83;  // v83: a TTF family kerns and draws fi/fl ligatures
                                              // v82: a TTF family draws every size with a face of its own
```

- [ ] **Step 2: Build the firmware, both boards**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`, then the same with `-e x4pro`.
Expected: `[SUCCESS]` for each, the linker finding the readers in FreeInkOpenType (no undefined
`freeink::font::gpos::...`); `check_no_float` passes (it runs after the link). The default image is about
6,316,800 bytes (96.4% of the partition).

- [ ] **Step 3: Run the whole host suite**

Run: `cmake --build build/test -j 8 -- -k; (cd build/test && ctest -j 8 2>&1 | tail -3)`
Expected: `100% tests passed out of 2278` (2,261 before).

- [ ] **Step 4: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror $(git diff --name-only origin/master -- '*.h' '*.cpp')
git add lib/Epub/Epub/Section.cpp
git commit -m "feat(ttf): lay TTF chapters out again for kerning and ligatures (section version 83)"
```

---

### Task 7: On the X3, the SDK pin, and the record (app)

The device run covers what the host tests cannot: kerning in real layout and drawing, the pair index filling over a
chapter, and the time that costs. It needs the user for the on-device steps: ask for each one, and capture the serial
log meanwhile. BookerlyTTF is on the X3's card (`/.crosspoint/fonts/BookerlyTTF`). The device runs the SDK branch;
the pin moves after the SDK PR has merged (Step 6).

**Files:**
- Modify: `freeink-sdk` (the pin)
- Modify (docs branch `docs/ttf-fonts-design`, worktree `C:/tmp/wt-ttf-docs`):
  `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, this plan

- [ ] **Step 1: Flash and start the log**

Flash: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`.
Capture in the background, without a reset:
`/c/pio/penv/Scripts/python.exe /c/tmp/capture_reset.py COM6 /c/tmp/x3-2c3.log 3600 --no-reset`

- [ ] **Step 2: A kerned page**

Ask the user to open a book in BookerlyTTF at 14 pt, at a page with capital pairs such as "AV", "To", "Ty", "We"
(a title page or a heading helps), and words with "fi", "fl" or "ff".
Expected:
- the log at load: `regular style: N kern subtables, M ligatures` (N > 0);
- the capital pairs visibly tighter than before;
- fi and fl drawn as one glyph where M > 0;
- no `glyph rendered without RenderLock`.

- [ ] **Step 3: Time and the pair index**

Ask the user to turn 10 pages forward and back, then leave the book.
Expected:
- the unload line: `... kerning pairs worked out` (a few hundred on a first chapter);
- page turns as before; note the first page's render time against a page turned back to;
- a second visit to the same pages works out almost no pairs.

- [ ] **Step 4: Styles**

Ask the user to open a page with bold or italic words in running text (most novels have some).
Expected: the words kern as the regular text does, and nothing is drawn in the wrong style.

- [ ] **Step 5: Record**

In the spec (docs branch):
- 5.4 gets an "As built in 2c-3" block with the decisions above (the SDK's readers, shared; lookups add; every
  script; the legacy table; U+FB00-FB04);
- section 8's table: the 2c-3 status;
- 8.1: kerning pairs and the coverage pre-check, done;
- Appendix A: the X3 numbers (subtables and ligatures, pairs worked out, the first page's time).

Add a status block to the top of this plan. Commit on `docs/ttf-fonts-design`:

```bash
git -C /c/tmp/wt-ttf-docs add docs/superpowers
git -C /c/tmp/wt-ttf-docs commit -m "docs(ttf): Stage 2c-3 as built: kerning and ligatures, X3 results"
```

- [ ] **Step 6: The SDK pin**

Once the user has merged the SDK PR (Task 3) into `witchhunt`:

```bash
git -C freeink-sdk fetch origin
git -C freeink-sdk diff --stat origin/feat/opentype-readers origin/witchhunt -- libs/font docs/testing.md   # empty if the PR merged unchanged
git -C freeink-sdk checkout --detach origin/witchhunt
git -C freeink-sdk log --oneline -4   # Tasks 1-3's commits (or their squash) are in it
```

If the PR changed in review, run Task 4's and Task 5's test command again. Then:

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 87 tests.`

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`
Expected: `[SUCCESS]`.

```bash
git add freeink-sdk
git commit -m "chore: bump freeink-sdk to witchhunt $(git -C freeink-sdk rev-parse --short HEAD) (FreeInkOpenType)"
```

The branch is then ready for superpowers:finishing-a-development-branch (its PR targets `master`).
