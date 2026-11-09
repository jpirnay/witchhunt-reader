# Writing modes step 1, PR 1: the bidi library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring upstream crosspoint's `lib/MiniBidi` into the tree with exactly the named deviations, and add the shaping
layer with its Hebrew module. Nothing in the firmware calls either yet.

**Architecture:** The port lands in two commits: the files as upstream has them, then the build glue. Reviewers can
diff each file against upstream's commit. The deviations follow as their own commits:
- `applyBidiVisual` without Arabic shaping;
- the divine-name match out of its emission loop;
- a released workspace;
- a second class table for the scripts we draw.

`lib/TextShaping` turns a token's logical text into the code points it is drawn with, each carrying its source byte
offset (cluster). In step 1 its only module is the divine-name rule.

**Tech Stack:** C (minibidi), C++20, GoogleTest host tests (CMake), Python 3 generator over Unicode 17.0.0 UCD data,
PlatformIO (firmware build check only).

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md` (approved 2026-11-02). This plan is the
spec's "The pieces" item 1. It implements the spec's deviations 2, 3 and 4, the workspace half of deviation 1, and
the shaping layer.

## Global Constraints

- **Upstream baseline:** crosspoint-reader `develop` @ `e8299b5f126817282939059d6505f90429489276`, read from the
  local clone: `git -C C:/_development/crosspoint-reader show e8299b5f:<path>`.
- **Imported files** keep their headers and licence notices (mintty's minibidi, MIT; Ahmad Khalifa, Thomas Wolff).
  Any line we change carries a short `// witchhunt:` comment naming the deviation.
- **The import commit credits the upstream authors** of `lib/MiniBidi` and the test, as `Co-authored-by` trailers
  (the code is theirs):
  - `Uri Tauber <uritaube@gmail.com>`
  - `Zach Nelson <zach@zdnelson.com>`
  - `Husam Younis <youhusam@gmail.com>`
  - `Bob Vincent <pillarsdotnet@gmail.com>`
  - `Sung-jin Brian Hong <serialx@serialx.net>`

  No other commit gets a co-author trailer. No Claude trailer anywhere.
- **The firmware is unchanged by this PR.** Nothing includes the new libraries, so `default` and `gh_release` flash
  and RAM equal master's.
- **Nothrow:** no bare `new`, no exceptions, no `try`/`catch`. Allocation goes through `makeUniqueNoThrow`, as upstream's
  workspace does.
- **Formatting:** clang-format 21 (`/c/tmp/cf21/clang_format/data/bin/clang-format`). The `.c`/`.h`/`.cpp` files
  must pass `bin/clang-format-fix`'s rules (CI runs it on every tracked C/C++ file). `.t` tables are not formatted.
- **Host tests:**
  - build with `cmake --build build/test -j 6 -- -k 0`;
  - run with `ctest --test-dir build/test -j 8 -E HeapPeakRegression`;
  - the only expected failures are FirstChapter ×2, LongChapterPageTables ×2 and KeptReadingState ×1 (Windows-only);
  - every golden stays byte-identical.
- **Source files are written with the Write/Edit tools,** never a shell heredoc. Before each commit, check that no
  touched text file contains a NUL byte.

## Review Focus

1. **A token longer than `BIDI_MAX_LINE` (128 code points)** given to `applyBidiVisual`: upstream returns `false`
   and leaves the output untouched. The caller (PR 2) then draws the logical text. Pinned in Task 2.
2. **Niqqud order:** a pointed word comes out with each mark after its base, in logical order, so the renderer can
   overlay it on the glyph just drawn. Pinned in Task 2.
3. **The divine name inside a longer word, and near misses.** A prefix (לַיהוָה) keeps its letters and marks, and the
   name inside it is replaced. יהו and יהוי stay untouched. A mark after the final ה belongs to the name. Pinned in
   Task 4.
4. **Punctuation inside a left-to-right block keeps its class:**
   - the Greek question mark U+037E and the ideographic comma U+3001 stay ON;
   - U+3000 is WS;
   - fullwidth digits are EN.
   Mapping whole blocks to L would mis-order them next to Hebrew. Pinned in Task 3.
5. **Overflow in the shaping layer:** a token that needs more units than the caller's buffer holds returns `false`
   (drawn as is), never a partial result. Pinned in Task 4.

---

### Task 1: Import upstream's MiniBidi and its test, and build them on the host

**Files:**
- Create (verbatim from `e8299b5f`):
  - `lib/MiniBidi/minibidi.c`, `minibidi.h`, `bidiclasses.t`, `bidi_pairs.t`, `BidiUtils.h`, `BidiUtils.cpp`;
  - `test/minibidi_arabic/CMakeLists.txt`, `MiniBidiArabicTest.cpp`, `stubs/Logging.h`.
- Modify:
  - `lib/Utf8/Utf8.h`: one overload taking upstream's argument order;
  - `test/CMakeLists.txt`: register the test directory.

**Interfaces:**
- Produces: `BidiUtils::startsWithRtl`, `detectParagraphLevel`, `isTransparentMark`, `applyBidiVisual` and
  `computeVisualWordOrder`, with upstream's signatures (`lib/MiniBidi/BidiUtils.h`).
- Produces: minibidi's C API (`bidi_class`, `is_rtl_class`, `mirror`, `do_bidi`, `do_shape`, `bidi_char`,
  `BIDI_MAX_LINE`, the class enum), in `minibidi.h`.
- Produces: `inline void utf8AppendCodepoint(uint32_t cp, std::string& out)` in `Utf8.h`, forwarding to our
  `utf8AppendCodepoint(std::string&, uint32_t)`. It exists so ported upstream code compiles unchanged.

- [ ] **Step 1: Copy the nine files verbatim** with `git -C C:/_development/crosspoint-reader show e8299b5f:<path> >
  <path>` (byte-for-byte; this is a copy, not a heredoc). Check each with `git -C C:/_development/crosspoint-reader
  diff --no-index` or `cmp` against the `show` output.
- [ ] **Step 2: Commit the import alone**, before anything builds it:
  - message `chore(bidi): import upstream MiniBidi (crosspoint-reader e8299b5f)`;
  - the body names the PRs (#1700, #2541, #2554, #3834, #4116) and says nothing builds it yet;
  - the five `Co-authored-by` trailers.
- [ ] **Step 3: Add the `Utf8.h` overload**, with a one-line comment saying it is upstream's argument order for
  ported code.
- [ ] **Step 4: Register the test:** `add_subdirectory(minibidi_arabic)` in `test/CMakeLists.txt`, next to
  `default_ignorable`. Upstream's `CMakeLists.txt` names `crosspoint_test_common`, `${REPO_ROOT}/lib/Utf8/Utf8.cpp`
  and `${REPO_ROOT}/lib/Memory`, which all exist here. Fix only what fails to configure, and say what in the commit.
- [ ] **Step 5: Build and run:** `cmake --build build/test -j 6 --target MiniBidiArabicTest` and
  `ctest --test-dir build/test -R "ArabicShaping|HebrewShaping|FarsiShaping|UrduShaping|SindhiShaping|PashtoShaping|KurdishShaping|JoinerShaping|MixedDirection|Regression|TransparentMark"`.
  Expected: every upstream test passes unchanged.
- [ ] **Step 6: Format check:** run clang-format 21 `--dry-run --Werror` on the imported `.c`, `.h` and `.cpp`
  files.
  - Expected: clean (upstream formats with the style we inherited).
  - If not, apply the formatting in this commit and list the files in its body. It must be whitespace only.
- [ ] **Step 7: Commit** `build(bidi): build MiniBidi and its test on the host`.

### Task 2: The BidiUtils deviations: no Arabic shaping, no divine-name match, a releasable workspace

**Files:**
- Modify: `lib/MiniBidi/BidiUtils.h`, `lib/MiniBidi/BidiUtils.cpp`
- Modify: `test/minibidi_arabic/MiniBidiArabicTest.cpp` (one line: its helper calls the shaping variant)
- Create: `test/minibidi/CMakeLists.txt`, `test/minibidi/BidiUtilsTest.cpp`
- Modify: `test/CMakeLists.txt` (`add_subdirectory(minibidi)`)

**Interfaces:**
- Consumes: Task 1's BidiUtils.
- Produces:
  - `bool applyBidiVisual(const char* utf8, std::string& out, int paragraphLevel = -1)`: upstream's signature,
    **without** `do_shape`. Arabic letters come out reordered but as their base code points; lam-alef is not
    ligated.
  - `bool applyBidiVisualShaped(const char* utf8, std::string& out, int paragraphLevel = -1)`: upstream's behaviour,
    with `do_shape`. Nothing in the firmware calls it in step 1, so the linker drops it and `do_shape`'s tables.
  - `void releaseWorkspace()`: frees the shared workspace under `bidiMutex`; the next call allocates it again.
- Both visual functions share one body, a `template <bool Shape>` helper. `if constexpr (Shape)` calls `do_shape`
  into `shaped`. Otherwise the emission loop reads `line` directly. So the non-shaping instantiation contains no
  call to `do_shape`. The joiner bookkeeping, the filter (ligature placeholder, ZWJ and ZWNJ) and the
  marks-after-base emission are unchanged for both.
- The divine-name block that #3834 added to the emission loop is removed from both (the spec's deviation 3; Task 4
  re-implements it in logical order).
- Every changed line carries a `// witchhunt:` comment naming the deviation.

- [ ] **Step 1: Write the failing tests** in `test/minibidi/BidiUtilsTest.cpp`, reusing the encode/decode
  helpers' pattern from upstream's test:
  - `ApplyBidiVisualDoesNotShapeArabic`: logical {0x0633, 0x0644, 0x0627, 0x0645} at level -1 → visual
    {0x0645, 0x0627, 0x0644, 0x0633}.
  - `ApplyBidiVisualLeavesLamAlefApart`: {0x0644, 0x0627} → {0x0627, 0x0644}.
  - `ApplyBidiVisualKeepsTheDivineName`: {0x05D9, 0x05D4, 0x05D5, 0x05D4} → {0x05D4, 0x05D5, 0x05D4, 0x05D9}.
  - `ShapedVariantStillShapes`: `applyBidiVisualShaped` on {0x0644, 0x0627} → {0xFEFB} (the isolated lam-alef, as
    upstream's `LamAlefIsolatedVariants` expects).
  - `NiqqudFollowsItsBaseInLogicalOrder`: שָׁלוֹם = {0x05E9, 0x05B8, 0x05C1, 0x05DC, 0x05D5, 0x05B9, 0x05DD} →
    {0x05DD, 0x05D5, 0x05B9, 0x05DC, 0x05E9, 0x05B8, 0x05C1}.
  - `BracketsMirrorInARightToLeftRun`: {0x0028, 0x05E9, 0x05DC, 0x05D5, 0x05DD, 0x0029} at level 1 →
    {0x0028, 0x05DD, 0x05D5, 0x05DC, 0x05E9, 0x0029}.
  - `ATokenPastTheLineLimitIsLeftUnprocessed`: 129 × U+05D0 → returns false and `out` is untouched (preset to
    "x", still "x").
  - `TheWorkspaceComesBackAfterRelease`: `applyBidiVisual` works, `releaseWorkspace()`, it works again with the same
    result. Then `releaseWorkspace()` twice in a row is harmless.
  - `ComputeVisualWordOrderReversesARightToLeftLine`: words {"שלום", "עולם"}, paragraph RTL → order {1, 0}.
  - `ComputeVisualWordOrderKeepsAnEmbeddedLatinRun`: {"abc", "שלום", "עולם", "def"}, paragraph LTR → {0, 2, 1, 3}.
- [ ] **Step 2: Run them:** `cmake --build build/test -j 6 --target BidiUtilsTest` → fails to compile
  (`applyBidiVisualShaped` and `releaseWorkspace` are undeclared). After a stub declaration they fail on the
  shaping, divine-name and release expectations.
- [ ] **Step 3: Implement** the template body and the two public functions, remove the divine-name block, add
  `releaseWorkspace()`. In upstream's test, change `shapeVisual` to call `BidiUtils::applyBidiVisualShaped`.
  That is the one line, with a `// witchhunt:` comment.
- [ ] **Step 4: Run** both test executables. Expected: every test passes, upstream's included.
- [ ] **Step 5: Commit** `feat(bidi): applyBidiVisual without Arabic shaping or the divine-name match; a releasable workspace`.

### Task 3: The class table for the scripts we draw

**Files:**
- Create: `scripts/gen_bidi_classes_ext.py`, and `lib/MiniBidi/bidiclasses_ext.t` (generated, committed)
- Modify: `lib/MiniBidi/minibidi.c` (`bidi_class()` searches the second table when upstream's misses)
- Test: `test/minibidi/BidiClassTest.cpp` (added to `test/minibidi/CMakeLists.txt`)

**Interfaces:**
- Consumes: Task 1's `bidi_class(ucschar)`.
- Produces: the same `bidi_class` with more coverage. `bidiclasses.t` stays verbatim. `bidi_class()` binary-searches
  upstream's `lookup[]` first, then a second static array built from `#include "bidiclasses_ext.t"`, then returns
  ON. The second search is the deviation, marked `// witchhunt:`.

**The generator.**
- Usage: `python -I scripts/gen_bidi_classes_ext.py <DerivedBidiClass.txt> [<out.t>]`, default output
  `lib/MiniBidi/bidiclasses_ext.t`. It follows `scripts/gen_vertical_orientation.py`'s shape.
- The input is Unicode 17.0.0's `extracted/DerivedBidiClass.txt`. It is downloaded to the session scratchpad, not
  committed: https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedBidiClass.txt. Check the first line,
  `# DerivedBidiClass-17.0.0.txt`. Honour the file's `@missing` default lines before its explicit ranges.
- It reads `lib/MiniBidi/bidiclasses.t` to learn upstream's ranges, and emits only code points upstream does not
  list.
- **Left-to-right blocks, with the file's classes but NSM read as L:**
  - Greek: 0370-03FF, 1F00-1FFF.
  - Armenian 0530-058F.
  - The Indic scripts: Devanagari 0900-097F, Bengali 0980-09FF, Gurmukhi 0A00-0A7F, Gujarati 0A80-0AFF,
    Oriya 0B00-0B7F, Tamil 0B80-0BFF, Telugu 0C00-0C7F, Kannada 0C80-0CFF, Malayalam 0D00-0D7F, Sinhala 0D80-0DFF,
    Devanagari Extended A8E0-A8FF.
  - Georgian: 10A0-10FF, 1C90-1CBF, 2D00-2D2F.
  - Ethiopic: 1200-139F, 2D80-2DDF, AB00-AB2F.
  - Hangul: 1100-11FF, 3130-318F, A960-A97F, AC00-D7AF, D7B0-D7FF.
  - CJK and kana: 3000-303F, 3040-309F, 30A0-30FF, 3100-312F, 3190-31FF, 3200-32FF, 3300-33FF, 3400-4DBF,
    4E00-9FFF, F900-FAFF, FE10-FE1F, FE30-FE4F, FF00-FFEF, and 20000-2FFFF, 30000-3FFFF.
- **Blocks with the file's classes as they are:**
  - Hebrew presentation forms FB1D-FB4F;
  - General Punctuation 2000-206F, the code points upstream lacks;
  - Currency Symbols 20A0-20CF;
  - U+2212 and U+2213.
- ON runs are not emitted, since the fallback already gives ON.
- Adjacent runs of one class are merged. The output is `{0xFIRST, 0xLAST, CLASS},` lines in ascending order. The
  header says `Generated by scripts/gen_bidi_classes_ext.py from DerivedBidiClass-17.0.0.txt; do not edit.`
- It exits non-zero if an emitted run overlaps upstream's table or the runs are not strictly ascending.

- [ ] **Step 1: Write the failing test** `test/minibidi/BidiClassTest.cpp`, one `EXPECT_EQ(bidi_class(cp), CLASS)`
  per value.
  - Left to right: 0x03B1 L; 0x0915 L; 0x094D L; 0x3042 L; 0x3099 L; 0x4E00 L; 0xAC00 L; 0x10D0 L; 0x0561 L;
    0x1200 L; 0xFF21 L; 0x20000 L.
  - Punctuation, spaces and numbers: 0x037E ON; 0x3001 ON; 0x3000 WS; 0xFF10 EN; 0x20AC ET; 0x2212 ES.
  - Hebrew presentation forms: 0xFB2A R; 0xFB1E NSM; 0xFB29 ES.
  - Upstream's entries unchanged: 0x05D0 R; 0x0627 AL; 0x0041 L; 0x0300 NSM; 0x200F R.
  - `isTransparentMark`: 0x094D false; 0x3099 false; 0xFB1E true; 0x05B0 true.
- [ ] **Step 2: Run it:** it fails on every value the second table adds (they read ON today).
- [ ] **Step 3: Write the generator, download the UCD file, generate `bidiclasses_ext.t`, and add the second search
  to `bidi_class()`.**
- [ ] **Step 4: Run** `BidiClassTest`, `BidiUtilsTest` and upstream's test. Expected: all pass. Run the generator
  twice: the output is identical.
- [ ] **Step 5: Commit** `feat(bidi): bidi classes for the scripts we draw`. The body says where the UCD file comes
  from and why NSM reads as L in the left-to-right blocks (see the spec's deviation 4).

### Task 4: The shaping layer and its Hebrew module

**Files:**
- Create: `lib/TextShaping/TextShaping.h`, `TextShaping.cpp`, `HebrewShaper.h`, `HebrewShaper.cpp`
- Create: `test/text_shaping/CMakeLists.txt`, `test/text_shaping/TextShapingTest.cpp`
- Modify: `test/CMakeLists.txt` (`add_subdirectory(text_shaping)`)

**Interfaces:**
- Consumes: `BidiUtils::isTransparentMark(uint32_t)` (Task 1). `lib/TextShaping` depends on `lib/MiniBidi`, never
  the reverse.
- Produces, in `TextShaping.h` (namespace `TextShaping`):
  ```cpp
  // One drawn code point and the byte offset, in the token's logical UTF-8 text, of the source code point
  // (or the first one of the matched span) it came from. HarfBuzz calls this offset a cluster.
  struct Unit {
    uint32_t cp;
    uint16_t cluster;
  };
  // Shapes one token's logical UTF-8 text (len bytes, need not be NUL-terminated). Returns true and sets
  // units[0..count) when what is drawn differs from the text's own code points. Returns false, leaving count and
  // units untouched, when the text is drawn as it is, or when the result would need more than `capacity` units.
  bool shape(const char* text, size_t len, Unit* units, size_t capacity, size_t& count);
  ```
- `shape` returns false after a byte scan when the text has no byte 0xD7 (no Hebrew letter in U+05C0-05FF), so a
  Latin or CJK token costs one pass. Otherwise it calls `HebrewShaper::shape` (same signature, in `HebrewShaper.h`).
  Step 2's Indic and Arabic modules are later branches of the same dispatch.
- **The Hebrew module (the divine name, upstream #3834, in logical order):**
  - The match is U+05D9 U+05D4 U+05D5 U+05D4, in logical order. Any `isTransparentMark` code points may sit between
    the letters, and those right after the final U+05D4 belong to the match.
  - A match may start anywhere in the token: prefixes stay.
  - Each match becomes two units, U+05D4 and U+0027, both with the cluster of the match's U+05D9. Every other code
    point is copied with its own byte offset.
  - It returns true only if at least one match was replaced.
- The file header credits the rule: `// The divine-name rule is crosspoint-reader PR #3834 (Uri Tauber); upstream
  matches it in visual order inside BidiUtils::applyBidiVisual, here it is matched in logical order as a shaping
  module (writing-modes step 1 design, decision 3).`

- [ ] **Step 1: Write the failing tests** in `TextShapingTest.cpp` (encode logical code points to UTF-8; compare
  `{cp, cluster}` pairs):
  - `TheNameBecomesHeAndGeresh`: יהוה → {{0x05D4, 0}, {0x0027, 0}}.
  - `ThePointedNameDropsItsMarks`: {0x05D9, 0x05B0, 0x05D4, 0x05D5, 0x05B8, 0x05D4} → {{0x05D4, 0}, {0x0027, 0}}.
  - `APrefixStays`: {0x05DC, 0x05B7, 0x05D9, 0x05D4, 0x05D5, 0x05B8, 0x05D4} → {{0x05DC, 0}, {0x05B7, 2},
    {0x05D4, 4}, {0x0027, 4}}.
  - `AMarkAfterTheLastHeBelongsToTheName`: {0x05D9, 0x05D4, 0x05D5, 0x05D4, 0x0591} → {{0x05D4, 0}, {0x0027, 0}}.
  - `PunctuationAfterTheNameKeepsItsOffset`: {0x05D9, 0x05D4, 0x05D5, 0x05D4, 0x002E} → {{0x05D4, 0}, {0x0027, 0},
    {0x002E, 8}}.
  - `NearMissesAreDrawnAsTheyAre`: יהו, יהוי, שלום → false each.
  - `LatinAndCjkAreDrawnAsTheyAre`: "hello", "日本" → false.
  - `TwoNamesInOneToken`: יהוה + U+05BE (maqaf) + יהוה → five units, clusters 0, 0, 8, 10, 10.
  - `ABufferTooSmallDrawsAsIs`: יהוה with capacity 1 → false, count untouched.
- [ ] **Step 2: Run them:** they fail to compile (`TextShaping.h` missing).
- [ ] **Step 3: Implement `TextShaping` and `HebrewShaper`.** Decode with `utf8NextCodepoint` over a bounded copy, or
  a length-checked decoder; the text need not be NUL-terminated.
- [ ] **Step 4: Run** `TextShapingTest`. Expected: all pass.
- [ ] **Step 5: Commit** `feat(text-shaping): the shaping layer, with the divine-name rule as its Hebrew module`.

### Task 5: Credits, roadmap and the PR checks

**Files:**
- Modify: `README.md` ("Vendored third-party components (`lib/`)": a minibidi entry)
- Modify: `docs/design/writing-modes.md` (roadmap row 1 and "Next step.")

**Interfaces:** none.

- [ ] **Step 1: README entry.**
  - **minibidi**, from mintty (Ahmad Khalifa, Thomas Wolff; MIT), by way of crosspoint-reader's `lib/MiniBidi`
    (MIT), @ e8299b5f.
  - Vendored under `lib/MiniBidi`, modified as the spec's deviations 2-4 say: no Arabic shaping in
    `applyBidiVisual`, the divine-name match moved to `lib/TextShaping`, a releasable workspace, and a second class
    table generated by `scripts/gen_bidi_classes_ext.py`.
  - Follow the TJpgDec entry's style.
- [ ] **Step 2: Roadmap.**
  - Row 1 becomes "in progress: PR 1 (the library) #<n>", with links to the design and this plan.
  - "Next step." names step 1 PR 2 (layout and drawing), then the joint shaper (step 2, Indic first).
- [ ] **Step 3: The whole host suite** on the final tree: only the five known Windows-only failures.
- [ ] **Step 4: Firmware build.**
  - Check no other pio build runs, and never kill one. `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1`.
  - Build `default`, then `gh_release`, in the background.
  - Record flash and RAM. Expected: equal to master's (`default` 6,150,595 / 57,776; `gh_release` 6,038,001 /
    57,272). If they differ, find out why before going on.
- [ ] **Step 5: clang-format 21** `--dry-run --Werror` on every touched C/C++ file; no NUL bytes in touched files.
- [ ] **Step 6: Commit** `docs(writing-modes): step 1 PR 1, the bidi library`.
