# Writing modes step 2, PR 3: shaped Indic text on the page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Hindi, Bengali, Tamil or Malayalam book in a TrueType family that maps its script reads as printed. Each
Indic word is shaped once at section build, kept in the section as compact glyph records, and drawn by glyph ID. A font
that cannot shape gets upstream's vowel reordering instead.

**Architecture:**
- **The parser** keeps a lone ZWJ or ZWNJ after a shaped-script letter (deviation 8). It also takes upstream's three
  syllable rules: fallback breaks between syllables, no bionic split, no drop cap.
- **The shaping layer** (`lib/TextShaping`) gains two parts:
  - the text rules: which glued tokens merge (deviation 7, and the Hebrew name), upstream's run scan, and
    `IndicReorder` into a bounded buffer (carried item 3);
  - the Indic module, `IndicShaper::shapeToken`. It shapes a token's runs through a `ShapingFace` into pieces: glyph
    records, reordered text or plain text. Each call works in one block of the build's lent arena. Its run scan,
    piecewise loop and glyph rounding port #3787's `ComplexShaper`.
- **The face** keeps its own plans (`ShapingFace::plan`). So no plan outlives its face or is moved (carried item 1),
  and a build's end frees both (`GfxRenderer::releaseShapingFaces`).
- **Layout** (`ParsedText`):
  - merges glued groups, and shapes each Indic token into a paragraph store (`ShapedStore`) that keeps its records
    across passes;
  - breaks lines on the shaped widths, and cuts a line's tokens into glued pieces at run edges;
  - a paragraph that needs bidi shapes nothing until PR 4, which owns the line pass's run-cut pieces.
- **Storage and draw:** bit 2 of the TextBlock's bidi region holds glyph forms. `render`, `wordBox` and the
  dictionary's word selection draw and measure them with PR 2's `drawGlyphRun` and `glyphRunWidth`.
- **The build:** `SECTION_FILE_VERSION` 109. `kStatusShapingDegraded` (status bit 6) earns a spine one rebuild per
  session. The X4's in-place gate refuses a shaped-script book in a TrueType set.
- **Proof:** four fixture books with goldens and a rule checker; on the devices, measure-first item 6 and before/after
  pages.

**Tech Stack:**
- C++20 (`-std=gnu++2a`; `-fno-exceptions` on the device).
- GoogleTest host tests (CMake/Ninja, MSYS2 UCRT64 g++ 16.2.0).
- Python 3.14 with fontTools 4.63.0 and uharfbuzz 0.56.2 (HarfBuzz 14.5.0), from `C:/tmp/wm2-measure/pydeps`, for the
  fixture fonts; Pillow for the device images.
- PlatformIO for the firmware.

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step2-design.md` (approved 2026-11-02), with
`docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md` (its "PR 2" section included).
- **What PR 3 owns:**
  - "The pieces", item 3, except the Font Manager's families (below);
  - deviations 6 (in paragraphs without bidi), 7, 8 and 9;
  - "How shaping runs" steps 1-10, for paragraphs without bidi;
  - "What is stored" (bit 2) and the TextBlock bullet of "Drawing";
  - measure-first items 5 and 6.
- **The seams PR 2 (#624, merged as `618312bb4`) left, which this plan uses:**
  - the face: `GfxRenderer::shapingFace`, `ShapingFace::current()` (under RenderLock only; `repointed()`,
    `cpacBytes()`) and `TtfFontSet::releaseShapingFaces()`. A set reload or that call frees every face;
  - the records: `GlyphRecord`, `glyph_run::encode`/`decode`, `GlyphRun`, `glyphRunAdvance`
    (`lib/GfxRenderer/GlyphRun.h`);
  - the draw: `GfxRenderer::drawGlyphRun`, `glyphRunWidth`, `prefetchGlyphRun`, and the page scan's
    `FontCacheManager::recordGlyphs`.
- **Carried from PR 2's ledger:**
  1. A moved `ot::Plan` keeps flag pointers into the plan it left: **Task 4**. Plans live in their face, each held by
     `std::unique_ptr` and never moved.
  2. `IndicReorder` grows a `std::string`: **Task 5**. A bounded overload writes into a buffer of twice the token's
     bytes, from the build's arena.
  3. The X4's `CMD:TTF:SHAPE` figures, owed: **Task 13**.

**Scope check: what moves to a following PR (the maintainer decides).** The design's PR 3 also holds the Font
Manager's 20 Indic families. This plan moves them to a **PR 3b, "the Font Manager's Indic families"**:
- **What moves:**
  - the merge recipe in `lib/EpdFont/scripts/sd-fonts.yaml` and `build-sd-fonts.py`;
  - the HarfBuzz oracle on each merged font, both ways (the script's own corpus, and the Latin letter runs against Noto
    Sans or Serif);
  - the 40 files under `assets/sd-fonts/`, and `fonts-v3.json` regenerated with `--manifest`;
  - the user guide's family list;
  - with them, the constraint "matches HarfBuzz ... for every font PR 3 and PR 4 ship, at two sizes, and with `CPac`
    from the install".
- **Why it can wait:**
  - No firmware code depends on the families. The Font Manager already reads `format: ttf`
    (`src/util/FontDownloadRules.h`).
  - The families add about 8 MB of font files (item 3's merged sizes), which would bury a layout diff of several
    thousand lines.
  - They need another reviewer's skill: fontTools and the oracle, not C++ layout.
  - PR 3's device runs need only the merged Noto Sans Devanagari and Noto Sans Bengali families, which have been on the
    X3's card since PR 2.
- Even without the families, PR 3 has 14 tasks.

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
- (Step 4) The output, "as upstream's `appendGlyphs` rounds it: the glyph, its advance in 12.4 pixels, its offset in
  whole pixels (y down), in visual order". So the advance is `(xAdvance + 2) >> 2`, `dx = roundPixels(xOffset)` and
  `dy = -roundPixels(yOffset)`, with `roundPixels(v) = v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64)`.
- (Step 5) "The records live in a per-paragraph store whose capacity survives the paragraphs (rule 1), reserved from
  an estimate and grown nothrow in chunks (invariant 9). A token measured again in a later pass reuses its records."
- (Step 7) "A token with a shaped run and other text becomes glued pieces, cut at the run's edges, so a display form is
  all glyphs or all code points ... a token that is one run (the usual Indic word) stays one word."
- (Step 8) "A glued gap next to a glyph piece takes no kerning."
- (Step 9) "Its logical text stays its text, for the dictionary, footnote links, the QR text and KOSync."
- (The record) "Records hold glyphs only." "A word has at most one form, UTF-8 or glyphs; load rejects a word with
  both." "Every line without a shaped word keeps its bytes; every Latin, CJK and Hebrew line is unchanged."
- (Deviation 7) Same style: merged. Another style, inside a syllable (`syllableBreakAllowed` false at the glue):
  merged, shaped as one run and drawn in the first token's style. Another style between syllables: each token shaped
  on its own, with its neighbours' runs as `RunContext` context. "A merged token keeps the first token's link and
  visible offset; the parts' logical texts are joined."
- (Deviation 8) "The parser keeps a lone ZWJ or ZWNJ when the code point before it belongs to a shaped script, a letter
  or a mark alike (U+0900-0DFF ...), whatever follows." "A run of two or more joiners is still dropped whole, and so is
  a joiner after any other code point." "The look-behind crosses chunk boundaries ... A kept joiner is held until the
  next code point arrives, in this call or a later one; a second joiner then drops both and the rest of the run." "A
  kept joiner counts nowhere in the visible offsets, as step 1's kept controls do."
- (Time) Pass line: "a shaped chapter builds in at most twice the time of a Latin chapter of the same length. Missed,
  #3787's direct-mapped run cache comes back as a class C block of the build".

From PR 2's rulings, binding here:
- **Glyph records are in drawn pixels.** A word is shaped at its drawn size: the face's ppem (`shapingFace`'s 26.6
  value) times `fontBaseScale(fontId)` times the word's scale (`ParsedText::wordScale`). `drawGlyphRun` gets the
  word's scale, and SUP/SUB halve nothing.
- `drawGlyphRun`, `glyphRunWidth` and `drawTextScaled` share one unscaled window (0.99-1.01). Widths are now stored
  from it, so changing it needs a `SECTION_FILE_VERSION` bump.
- A `CPac` exists only for a font whose GSUB or GPOS names a shaped script. Every other face shapes with computed
  filters or not at all, never wrongly.

This plan's own:
- **What PR 3 measures:**
  - items 5 and 6;
  - flash and static RAM;
  - the X4's `CMD:TTF:SHAPE` figures that PR 2 owes.
  The install time of a merged Devanagari family is PR 2's figure (4,497 ms on the X3), cited.
- **Flash gates** (item 2's pass lines, as PR 2 left them):
  - `gh_release` may grow by at most 81,772 B against the branch's merge base with master. That is 92,160 B for PRs
    1-3 less PR 2's +10,388 B.
  - `default` must stay at or under 6,297,600 B, which keeps 256,000 B of the 6,553,600 B slot free.
- **Branches.** The code goes on `feat/writing-modes-step2-pr3` in `C:/tmp/wt-wm3`, cut from PR 2's branch at
  `fb2099b6e`.
  - #624 merged into master as `618312bb4`. Merge `origin/master` in before Task 1, and again before the PR.
  - The PR opens against master, never stacked on #624's branch.
- **Upstream files keep their bytes.** A changed line in `lib/OtShaper`, `lib/Utf8/IndicReorder.*` or
  `lib/Utf8/IndicScripts.h` carries `// witchhunt: deviation N`.
  - Our files say so in their first comment.
  - Comments say what and why, never history.
- **Credit.** No commit is mainly #3787's code, so none has a co-author trailer, and no commit has a Claude trailer.
  - `IndicShaper.cpp` ports `ComplexShaper`'s `scanRun`, the piecewise loop of `appendShapedRun` and the rounding of
    `appendGlyphs` (`lib/EpdFont/ComplexShaper.cpp` at `0fbd1575`).
  - The Hyphenator's `appendFallbackIndexes`, the focus-split rule and the drop-cap rule (`f0757278`) are ported
    lines.
  - Each names its source in the repo's form (`// Ported from crosspoint-reader PR #3787 ("feat: add Indic language
    support without HarfBuzz", SSafayet / @SSafayet). ...`), and in its commit body.
- **Nothrow:**
  - no bare `new`, no exceptions, no `try`; allocations through `makeUniqueNoThrow` (`lib/Memory/Memory.h`);
  - every new allocation's comment gives its size and why it is not on the stack;
  - stack locals stay under 256 B;
  - never declare a local `std::string word(...)` with parentheses (the Arduino `word()` macro breaks the firmware
    build only).
- **What stays resident:** nothing new. The faces, their plans, the store and the call block live for one build, or for
  one paragraph's passes.
- **Host tests, from Git Bash only.** After `git submodule update --init freeink-sdk`, configure once:
  `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DGIT_EXECUTABLE="C:/Program Files/Git/cmd/git.exe"
  -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src
  -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-wm1/build/test/_deps/arduinojson-src`.
  - Build: `cmake --build build/test --target <T> -j 8`. Run: `ctest --test-dir build/test -R <pattern>
    --output-on-failure`, with no `-j` beside other worktrees.
  - The whole suite: `cmake --build build/test -j 8 -- -k 0`, then `ctest --test-dir build/test`.
  - The only expected failures are the five Windows-only `EpubPipelineTest` cases: FirstChapter ×2,
    LongChapterPageTables ×2, KeptReadingState.
  - Goldens are regenerated only on purpose, with `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`.
    `git ls-files --eol test/epub_pipeline/goldens | grep w/crlf` must print nothing.
- **Firmware builds:**
  - First check that no other pio or scons process runs (`tasklist | grep -i -E "pio|scons"`); never kill one.
  - `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
    `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`, one at a time, in the background (a cold build passes
    600 s).
  - Record the `Flash:` and `RAM:` lines' bytes. `x4pro` and `lilygo_t5s3` are left to CI.
- **Formatting:** clang-format 21, `/c/tmp/cf21/bin/clang-format-21.exe -style=file`. Run `-i` on every touched
  `.c`/`.h`/`.cpp`, then `--dry-run --Werror`, which must print nothing.
- **Files** are written with the Write and Edit tools, never a shell heredoc.
  - Before each commit, check that no touched text file contains a NUL byte.
  - Binary fixtures come only from their scripts.
  - No literal bidi control character (U+200E/F, U+202A-202E, U+2066-2069, U+061C) in any source: write `\u` escapes,
    or byte escapes in C++ strings. Write joiners in tests as `"\xE2\x80\x8D"` (ZWJ) and `"\xE2\x80\x8C"` (ZWNJ).
- **Devices** (Task 13):
  - The X4 is on COM5. The X3 is on COM6 (confirm both before use).
  - Rules: `C:/Users/jens/.claude/projects/c---development-witchhunt-reader/memory/serial-test-commands.md`.
    - Flash with esptool `write-flash 0x0 .pio/build/default/firmware.factory.bin`.
    - Drive the devices with `tools/devctl.py`, with pyserial and Pillow on its path; files go with its `put:` step.
    - On the X3: no RTS pulse, no `CMD:REBOOT`, no `CMD:SLEEP`.
  - Before flashing, check that no other session uses the device or the build (shared devices).
  - After every flash, copy `.pio/build/default/web-<hash>.bin` to the card's root. A cleanup never removes
    `/web-*.bin`.

## Review Focus

1. **A build paused between two slices while the font set reloads, or the reader font or its size changes**
   (Background-B preempted; a Font Manager install; a size change from the reader menu).
   - Expected: no plan outlives its face.
   - Expected: the next slice shapes with fresh faces and plans, and reuses no records shaped at another size.
   - Pinned in Task 4 (`ShapingFacePlans.AFaceFreedBetweenTwoTokensLeavesNoPlanBehind`), Task 8
    (`ShapedLayout.RecordsOfAnotherSizeAreNotReused`) and Task 9
    (`ShapedLine.AFaceReleasedBetweenTwoPassesShapesAfresh`).
2. **A Hindi word chosen in the dictionary's word selection.**
   - Expected: the highlight draws it shaped, as the page does.
   - Expected: the lookup, the footnote link match and the QR text read its logical text.
   - Pinned in Task 3 (`TextBlockGlyphForms.AGlyphWordKeepsItsLogicalText`) and Task 13 (a screenshot of the
     selection).
3. **A Sanskrit or Malayalam compound wider than the line** (a narrow table cell, a large heading).
   - Expected: it splits only between syllables, each part shaped on its own, with a hyphen drawn and no dotted circle.
   - Expected: the parts' visible offsets are those KOSync counts.
   - Pinned in Task 8 (`ShapedLayout.AForcedSplitFallsBetweenSyllables`), Task 9
     (`ShapedLine.AWordWiderThanTheLineSplitsBetweenSyllables`) and Task 12 (the rule checker's line-start rule, on the
     Malayalam book's table cell).
4. **A cold Hindi page drawn with the glyph store full** (ring mode). Expected: every shaped glyph draws, and the page's
   heap stays where a Latin page's is. Pinned in Task 13, Step 6: `CMD:TTF:FULL`, then page turns against screenshots
   taken before it, and `STATE heap` within 2 KB of the Latin page's.
5. **A Hindi book in a font that cannot shape it:** a `.cpfont` or built-in font, or a TrueType family without the
   script (Bookerly).
   - Expected: no plan is built, and the words take `IndicReorder`'s form.
   - Expected: nothing is latched degraded, so the spine is not rebuilt at every open.
   - Pinned in Task 8 (`ShapedLayout.AFontThatCannotShapeBuildsNoPlan`), Task 9
     (`ShapedLine.AFontThatCannotShapeReordersAndLatchesNothing`) and Task 12 (the `_unshaped` golden).

---

## The parser and upstream's rules

### Task 1: Joiners kept after a shaped-script letter (deviation 8)

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (the word-buffer members, beside `lastTextCodepoint_`
  `:165`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`:
  - `consumeText` (`:3803`): the default-ignorable skip (`defaultIgnorableLen`, `:4008`), the whitespace branch
    (`:3946-3984`) and the multi-byte text path;
  - `flushPartWordBuffer` (`:1211`) and `startNewTextBlock` (`:2164`)
- Modify: `lib/Epub/Epub/ParsedText.cpp`: `visibleBytesOutsideControls` (`:173`)
- Create: `test/epub_pipeline/JoinerParseTest.cpp`, added to `EpubPipelineTest`'s sources in
  `test/epub_pipeline/CMakeLists.txt`
- Test: `test/epub_pipeline/ParsedTextOffsetTest.cpp`, `test/epub_pipeline/VisibleTextOffsetLutTest.cpp`

**Interfaces:**
- Produces, in `ChapterHtmlSlimParser`:
  ```cpp
  // Deviation 8. The last text code point consumeText took, kept across calls and inline element edges (0 after
  // whitespace and at a block's start); and a ZWJ or ZWNJ held after a shaped-script letter until the next code point
  // says whether it is alone.
  uint32_t joinerLookBehind_ = 0;
  uint32_t heldJoiner_ = 0;         // 0, U+200C or U+200D
  bool droppingJoinerRun_ = false;  // a second joiner came: it, the held one and the rest of the run go
  ```
- A file-local `bool keepsJoinerAfter(uint32_t cp)` in `ChapterHtmlSlimParser.cpp`: U+0900-0DFF. PR 4 adds the Arabic
  blocks.
- The rules:
  - A joiner is held when `keepsJoinerAfter(joinerLookBehind_)`, none is held, and no run is being dropped.
  - It goes into the word buffer when the next text code point comes, in this call or a later one, before that code
    point. That may be the next token, after an element edge.
  - At whitespace or a block's end it goes in last, if the word has text (the chillu's joiner at a word's end).
  - A joiner while one is held drops both, and drops every joiner after them until a non-joiner.
  - A kept joiner counts nowhere:
    - no token's visible offset starts at it (the next code point sets the offset, as `appendPartWordText` does);
    - `visibleBytesOutsideControls` leaves its 3 bytes out, as it does an uncounted control;
    - it does not update `joinerLookBehind_`.
  - Every face draws a kept joiner in unshaped text as nothing: `TtfFont`'s `kIgnorable`, and the built-in and
    `.cpfont` glyphs `DefaultIgnorableTest` checks.

- [ ] **Step 1: The branch.** In `C:/tmp/wt-wm3` on `feat/writing-modes-step2-pr3`: `git fetch origin && git merge
  origin/master`, which is expected to be clean. Init the submodule and configure the host tests (Global Constraints).
- [ ] **Step 2: Write the failing tests** (`Joiners`, over `syntheticBook` and `pipeline_harness::forEachPage`; each
  reads the page's word texts):
  - `AHalfFormJoinerAfterAViramaIsKept`: `<p>क्` ZWJ `ष</p>`. The word holds `E2 80 8D` between the virama and ष.
  - `AZwnjBetweenConsonantsIsKept`: `<p>क` ZWNJ `ष</p>`.
  - `AChilluJoinerAtAWordsEndIsKept`: `<p>അവന്` ZWJ ` വന്നു</p>`. The first word ends with ZWJ.
  - `AJoinerAfterAnythingElseIsDropped`: `a` ZWJ `b`, `1` ZWJ, and a ZWJ after a space all come out without it.
  - `ARunOfJoinersIsDroppedWhole`:
    - `क` ZWJ ZWJ `ष` gives `कष`;
    - `क`, 300 ZWNJs, then `ष` gives `कष`.
  - `AJoinerSplitFromItsLetterByAChunkBoundaryIsKept`:
    - sweep a filler of 80-92 `क` (as `TextNodeRules.ACodepointSplitAtTheChunkBoundaryStillLandsExactly` sweeps), so
      the SAX parser's 256-byte flush falls before, inside and after `्` ZWJ;
    - the joiner is kept every time;
    - with a second ZWJ after it, both drop every time.
  - `AJoinerAtAnInlineElementsEdgeIsKept`: `<p>क्<b>` ZWJ `ष</b></p>` and `<p>क्` ZWJ `<b>ष</b></p>`.
    - The two tokens' texts joined hold the joiner.
    - The second token's visible offset is the offset of ष's first byte, never of the joiner.
  - `ParsedTextOffset.AKeptJoinerCountsNothingWhereAWordIsSplit`: a 14-consonant Devanagari word with a ZWNJ after the
    fourth consonant, forced to split after the sixth. The remainder's offset is the first offset plus 18 (six
    consonants), with the joiner's 3 bytes not counted.
  - `SyntheticLutFixture.KeptJoinersMoveNoPageStart`:
    - 120 paragraphs `<p>क्` ZWJ `ष कमल।</p>`, each a one-line paragraph;
    - every page starts at a paragraph's start, so its offset is that paragraph's index times the paragraph's
      visible bytes (joiner included, as VisibleText counts the source);
    - the page breaks equal those of the same book with the joiner removed.
- [ ] **Step 3: Run** `cmake --build build/test --target EpubPipelineTest -j 8 && ctest --test-dir build/test -R
  "Joiners|ParsedTextOffset|SyntheticLut"`. Expected: the new tests fail, since every joiner is dropped today.
- [ ] **Step 4: Implement** as the Interfaces block says.
  - The multi-byte text path updates `joinerLookBehind_` per code point.
  - The ASCII fast path sets it to the byte.
  - Whitespace and `startNewTextBlock` set it to 0.
- [ ] **Step 5: Run** `ctest --test-dir build/test -R "EpubPipeline|Joiners|ParsedTextOffset|SyntheticLut|TextNode"`.
  Expected: all pass but the five known failures, and every golden is unchanged.
- [ ] **Step 6: Format and NUL check, then commit** `feat(epub): keep a lone joiner after an Indic letter (deviation
  8)`.

### Task 2: Upstream's three syllable rules: fallback breaks, bionic reading, drop caps

**Files:**
- Modify: `lib/Epub/Epub/hyphenation/Hyphenator.cpp`: the fallback loops in `appendSegmentPatternBreaks` (`:92-98`)
  and in `breakOffsets` (`:243-249`)
- Modify: `lib/Epub/Epub/ParsedText.cpp`: `applyBionicReadingTransform` (`:1657`), beside its combine exception
  (`:1686-1694`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`: `consumeText`'s drop-cap branch ("Route drop-cap span
  text into the capture buffer", `:3893-3925`)
- Create: `test/hyphenation_eval/IndicFallbackTest.cpp` (added to `HyphenationEvaluationTest`),
  `test/epub_pipeline/IndicRulesTest.cpp` (added to `EpubPipelineTest`)

**Interfaces:**
- In `Hyphenator.cpp`'s anonymous namespace, ported verbatim from #3787's `lib/Epub/Epub/hyphenation/Hyphenator.cpp`
  with its comment:
  ```cpp
  void appendFallbackIndexes(const std::vector<CodepointInfo>& cps, size_t minPrefix, size_t minSuffix,
                             std::vector<size_t>& out);  // every index but those inside an Indic syllable
  ```
  Both fallback loops call it. A Latin word's fallback breaks do not change.
- Bionic reading leaves a word with a byte of U+0900-0DFF (`indic::containsIndic`) whole, as upstream's `addWord` does
  ("Complex-script words never take a focus split: each half would shape on its own").
- The drop cap: text routed into `pendingDropCap_` that reaches a code point of U+0900-0DFF abandons the capture, as
  its overflow does. Its text goes back to the word flow (upstream's `f0757278`, "a drop cap draws codepoints one by
  one, which would split the cluster"). The capture keeps its visible offset.

- [ ] **Step 1: Write the failing tests:**
  - `IndicFallback.ABreakNeverFallsInsideASyllable`: for `प्रार्थनासंस्कृतम्कृष्णज्ञानम्` and `শ্রীঈশ্বরচন্দ্রকর্তৃক`, with no
    language patterns,
    - `Hyphenator::breakOffsets(word, true)` gives at least three breaks;
    - every break falls between code points `a` and `b` with `indic::syllableBreakAllowed(a, b)`.
  - `IndicFallback.ALatinWordsFallbackBreaksAreUnchanged`: `Donaudampfschiffahrt` gives one break per index from
    `kDefaultMinPrefix` to `size - kDefaultMinSuffix`.
  - `IndicRules.BionicReadingLeavesAShapedScriptWordWhole`: bionic reading on, words `हिन्दी` and `reading`.
    - The line holds `हिन्दी` as one regular word.
    - `reading` is split, its prefix bold.
  - `IndicRules.NoDropCapIsCapturedIntoAnIndicLetter`:
    - a book whose first paragraph opens with a drop-cap span over `हि` (the markup of
      `NativeSizeLayoutFixture.ADropCapIsSetAtItsOwnSize`), then `न्दी`;
    - page 0 has no drop-cap line, and its first word is `हिन्दी`;
    - the same markup over a Latin `T` still takes its drop cap.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R "IndicFallback|IndicRules"`. Expected: FAIL.
- [ ] **Step 3: Implement.** Credit the three ports (Global Constraints).
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "Hyphenation|IndicFallback|IndicRules|EpubPipeline|NativeSize"`.
  Expected: all pass but the five known, and every golden is unchanged.
- [ ] **Step 5: Format check, then commit** `feat(epub): syllable-safe fallback breaks, bionic reading and drop caps
  (#3787's rules)`.

## Storage and draw

### Task 3: Glyph forms in the TextBlock (bit 2), drawn and measured

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`: the arena layout comment (`:30-52`), the bidi constants (`:163-166`),
  `WordRange` (`:243-267`) and the public accessors
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`:
  - the range constructor's bidi sizing (`:293-312`) and write (`:425-448`);
  - `displayForm` (`:522`), `bidiValid` (`:537`), `wordBox` (`:598`);
  - `render`'s `drawRun` and `runWidth` (`:686-706`) and its emphasis loop (`:843`)
- Modify: `src/activities/reader/DictionaryWordSelectActivity.cpp`: `drawPiece` (`:206`) and its caller (`:1241`)
- Modify: `test/epub_pipeline/PipelineRunner.cpp` (`dumpWords`, `:34`)
- Modify, the host renderer stubs: `test/epub_pipeline/GfxRenderer.h`, `test/word_size_layout/GfxRenderer.h`,
  `test/text_block_range/GfxRenderer.h`
- Modify: the include paths of every target that includes `TextBlock.h` without the real renderer:
  `test/word_size_layout`, `test/cjk_layout`, `test/text_block_range` and `test/vertical_text`. Each gains
  `${REPO_ROOT}/lib/GfxRenderer` last, for `GlyphRun.h` only, so the stub `GfxRenderer.h` still wins.
- Modify: `docs/file-formats.md:263` (the bidi region)
- Create: `test/text_block_range/TextBlockGlyphTest.cpp` (added to its CMake target)

**Interfaces:**
- Consumes: `GlyphRun`, `glyph_run::decode`, `glyphRunAdvance` (`lib/GfxRenderer/GlyphRun.h`);
  `GfxRenderer::drawGlyphRun`, `glyphRunWidth`.
- Produces, in `TextBlock`:
  ```cpp
  static constexpr uint8_t kBidiGlyphForms = 0x04;  // parts bit 2: glyph forms (writing modes step 2, decision 4)
  static constexpr uint8_t kGlyphFormLead = 0xFF;   // a glyph form's first byte, which no UTF-8 form starts with
  // WordRange: `count` runs, indexed from 0: what each word is drawn as when it is a shaped run, in drawn pixels. Null,
  // or a run with count 0: none. A word with a glyph form has no display form.
  const GlyphRun* glyphForms = nullptr;
  bool hasGlyphForm(uint16_t i) const;
  GlyphRun glyphForm(uint16_t i) const;  // count 0 when word i has none
  ```
- **The bytes.** A glyph form shares bit 0's offset array (the design's "a form led by 0xFF"). At its offset come 0xFF,
  a `u16` glyph count (little-endian), the records, then 0x00.
  - The offset array is present when bit 0 or bit 2 is set.
  - Bit 0 says the line has a UTF-8 form, bit 2 a glyph form.
  - A line with neither writes the bytes it does today.
  - `displayForm(i)` is null for a 0xFF-led form.
- **`bidiValid`** accepts parts in bits 0-2 only, and not 0. It also requires:
  - each bit set has at least one form of its kind;
  - a 0xFF-led form only with bit 2, and a UTF-8 form only with bit 0;
  - a glyph form holds exactly `count` whole records (`glyph_run::decode`), followed by its 0x00, inside the texts;
  - no glyph form on a vertical line (PR 3 never writes one).
- **Drawing:**
  - `render` draws a glyph word with `renderer.drawGlyphRun(font, wordX, wordY, run, true, style, scale)`, at the
    position and scale `drawRun` gives `drawText`.
  - `runWidth` and `wordBox`'s width use `renderer.glyphRunWidth(font, run, scale)`.
  - The emphasis loop skips glyph words: their marks would follow the text's advances, not the glyphs'.
- **The dictionary's word selection** draws a glyph word by its run, in the highlight's colour (`drawPiece` takes the
  run when the word has one). Its lookup text stays `wordText`.
- **The dump** writes `" WG n=<count> g=<glyph>:<advance>[:<dx>,<dy>] ..."` after a glyph word's `W` record. It does
  not match the word-stream pattern.
- **The stubs** each gain:
  - `drawGlyphRun`, recorded in a `glyphRunDraws` vector of `{fontId, x, y, count, scale}`;
  - `int glyphRunWidth(int, const GlyphRun& run, float scale = 1.0f) const`, returning
    `glyphRunAdvance(run, scale < 0.99f || scale > 1.01f)`.

- [ ] **Step 1: Write the failing tests** (`TextBlockGlyphForms`):
  - `AGlyphFormRoundTripsThroughTheFile`:
    - three words: `हिन्दी` with a 5-record run, one record with an offset; `,` with none; `कि` with a UTF-8 form;
    - after `serialize` and `deserialize`, `glyphForm(0)` decodes to the same records;
    - `hasGlyphForm(1)` is false, `wordDrawText(2)` is the UTF-8 form, and `wordText(0)` is `हिन्दी`.
  - `AGlyphWordKeepsItsLogicalText` (Review Focus 2): `wordText`, `wordTextLen` and `wordDrawText` of a glyph word are
    its logical text.
  - `ALineWithoutAGlyphFormKeepsItsBytes`: a Latin line, and a Hebrew line with UTF-8 forms, serialize to the same
    bytes with `glyphForms` null and with every run's count 0.
  - `LoadRejects`, one case each:
    - bit 2 with no glyph form;
    - a 0xFF form without bit 2;
    - a record cut short;
    - a missing 0x00;
    - a count past the bytes;
    - a glyph form on a vertical line;
    - parts bit 3.
  - `RenderDrawsTheRunAtTheWordsPosition`:
    - at scale 1 and at a word size of 50%, `glyphRunDraws` holds one draw;
    - its x is `xpos + x`, its y is the y `drawText` gets for the same word, and its scale is the word's.
  - `TheBoxAndTheUnderlineAreTheRunsWidth`: `wordBox(...).width` equals `glyphRunAdvance`, and an underlined glyph
    word's line runs that far.
- [ ] **Step 2: Run** `cmake --build build/test --target TextBlockRangeTest -j 8`. Expected: a compile error (no
  `glyphForms`).
- [ ] **Step 3: Implement** as the Interfaces block says.
  - The doc paragraph at `docs/file-formats.md:263` says bit 2 is the glyph forms, with their bytes, and what fails
    the read.
  - The version note stays with Task 10.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "TextBlock|EpubPipeline|WordSize|CjkLayout|BidiLine"`. Expected:
  all pass but the five known, and every golden is unchanged.
- [ ] **Step 5: Format check, then commit** `feat(epub): glyph forms in a line's bidi region, drawn by glyph ID
  (decision 4)`.

## The shaping layer

### Task 4: A face's own plans, and the build's release of faces

**Files:**
- Modify: `lib/EpdFont/ShapingFace.h`, `lib/EpdFont/ShapingFace.cpp`
- Modify: `lib/GfxRenderer/GfxRenderer.h`, beside `setBeforePushHook` (`:446`), and `lib/GfxRenderer/GfxRenderer.cpp`
- Modify: `src/SdCardFontSystem.cpp`: `begin` (`:612-628`)
- Create: `test/text_shaping/FixtureShapingFace.h`, a test double shared by Tasks 4-12. `${REPO_ROOT}/test/text_shaping`
  goes on the include path of each target that uses it: `TtfFontTest` here, then `TextShapingTest`,
  `WordSizeLayoutTest` and `EpubPipelineTest` in their tasks.
- Test: `test/ttf_font/TtfShapingTest.cpp` (suite `ShapingFacePlans`)

**Interfaces:**
- Produces, in `class ShapingFace`:
  ```cpp
  /// The plan for `script` in the language system `languageTags` (ot::languageTagsFor's array, compared by address;
  /// null: the script's default), built on first ask with makeUniqueNoThrow (about 1-2 KB) and kept with the face, by
  /// unique_ptr, never moved: no plan outlives the face it points into. nullptr when the face is not mapped now, the
  /// plan does not build, or there is no memory; asked again, it tries again. Under RenderLock, as current() is.
  const ot::Plan* plan(ot::Script script, const uint32_t* languageTags);
  /// Plans a face keeps; a new one past them frees the one asked for least recently.
  static constexpr uint8_t kMaxPlans = 2;
  /// Plans built since the face was made: for tests and the build's log.
  uint16_t planBuilds() const;
  ```
  - The slots are declared after `face_`, so they are destroyed before it.
  - The class comment no longer says "plans built on it go first": they are its own.
- Produces, in `GfxRenderer` (static, as `setBeforePushHook` is):
  ```cpp
  /// Frees every shaping face and its plans (TtfFontSet::releaseShapingFaces, installed by SdCardFontSystem); nothing
  /// without the hook. A section build's end and the dictionary's pages call it, under RenderLock.
  static void setShapingFaceRelease(void (*release)());
  static void releaseShapingFaces();
  ```
- `SdCardFontSystem::begin` installs a function that calls `ttfSystem->ttfSet()->releaseShapingFaces()` when a set is
  loaded.
- `FixtureShapingFace` is a `ShapingFace` over font bytes held in memory: `map()` returns them, and `tables()` is
  `ot::tablesFromSfnt` with `CPPL` cleared (`keepCpac` false also clears `CPAC`). It also has `std::vector<uint8_t>
  readFont(const char* dir, const char* name)`.

- [ ] **Step 1: Write the failing tests** (`ShapingFacePlans`):
  - `APlanIsBuiltOnceAndShapesAsHarfBuzz`:
    - a `FixtureShapingFace` over each `.layout` fixture (`kShapingFixtures`);
    - `plan(script, languageTagsFor(word.language))` asked twice gives the same pointer, and `planBuilds()` counts one
      per language;
    - every word shapes as `ExpectedShaping.h` says (`ottest::shapeAsStored` with the plan).
  - `EachLanguageHasItsOwnPlan`: लक्ष shapes differently under `""` and `"mr"` (upstream's
    `ShapesInTheDocumentLanguageAndReshapesWhenItChanges`).
  - `PastTwoPlansTheLeastRecentGoes`: `""`, `"mr"`, then `"ne"` builds 3; `"ne"` again builds nothing; `""` again
    builds a 4th.
  - `APlanBuiltBeforeAMoveShapesAfterIt`: the `ScriptedFace` of `TheFirstCallBuildsAndAMoveRepoints`, bases A then B,
    with A poisoned. The plan from base A shapes every Devanagari word after the move.
  - `AFaceFreedBetweenTwoTokensLeavesNoPlanBehind` (Review Focus 1), on `TtfShapingFace`'s installed subset:
    - `shapingFace(REGULAR)->plan(...)` raises `heapBytesHeld`;
    - `releaseShapingFaces()` brings it back to where it was before the face;
    - a fresh `shapingFace()` and `plan()` shape every `ExpectedGlyphRuns.h` run as the oracle says.
  - `ARefusedPlanIsAskedAgain`: with `ot::setHeapCheck` refusing, `plan()` is nullptr; with the check reset, the next
    call builds.
- [ ] **Step 2: Run** `cmake --build build/test --target TtfFontTest -j 8`. Expected: a compile error (no `plan`).
- [ ] **Step 3: Implement.** The hook in `GfxRenderer.cpp` and `SdCardFontSystem.cpp` is not host-built; Task 10's
  firmware build compiles it.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "ShapingFace|TtfShaping|Ttf"`. Expected: all pass.
- [ ] **Step 5: Format check, then commit** `feat(shaping): a face keeps its own plans; a build frees faces and plans`.

### Task 5: The shaping layer's text rules: merges, runs, and IndicReorder into a bounded buffer

**Files:**
- Modify: `lib/TextShaping/TextShaping.h`, `lib/TextShaping/TextShaping.cpp`
- Modify: `lib/TextShaping/HebrewShaper.h`, `lib/TextShaping/HebrewShaper.cpp` (one function beside `matchNameAt`)
- Create: `lib/TextShaping/IndicShaper.h`, `lib/TextShaping/IndicShaper.cpp` (ours; Task 6 adds to them)
- Modify: `lib/Utf8/IndicReorder.h`, `lib/Utf8/IndicReorder.cpp`: an overload, with `appendCluster` and the main loop
  made generic over their output, each changed line marked `// witchhunt: deviation 9`
- Modify: `test/text_shaping/CMakeLists.txt` (`IndicShaper.cpp`, `IndicReorder.cpp`)
- Test: `test/text_shaping/TextShapingTest.cpp` (suites `TextShapingMerge`, `IndicRuns`),
  `test/indic_text/IndicTextTest.cpp` (suite `IndicReorderBounded`)

**Interfaces:**
- Produces, in `TextShaping.h`:
  ```cpp
  // One token of a group of glued tokens (no space and no break between them), in logical order.
  struct GroupToken {
    std::string_view text;  // logical UTF-8
    uint32_t styleKey;      // equal keys: the same font, style, size and scale
  };
  // Whether `token`, glued after `previous` (a token merged so far, when earlier merges made it), joins it before
  // anything is measured or shaped:
  // - deviation 7, when `indicGroup` (the group holds a letter of U+0900-0DFF and its paragraph shapes): the two share
  //   a style key, or the glue falls inside a syllable (indic::syllableBreakAllowed false across it);
  // - the Hebrew name, always: `token` holds transparent marks only, and `previous` ends with the divine name.
  bool mergesWithPrevious(const GroupToken& previous, const GroupToken& token, bool indicGroup);
  ```
- Produces, in `HebrewShaper.h`: `bool endsWithName(const char* text, size_t len);`. It is true when a yod-he-vav-he
  match (`matchNameAt`, its trailing marks included) ends at `len`.
- Produces, in `IndicShaper.h`:
  ```cpp
  namespace IndicShaper {
  // One run of a token, upstream's scanRun: one script's letters with the shared code points around them (dandas,
  // Vedic signs, joiners, the dotted circle), bytes [begin, end); `script` null for shared code points alone.
  struct Run { uint16_t begin; uint16_t end; const indic::ScriptInfo* script; };
  // The most runs a token of `bytes` bytes can hold: a run takes 3 bytes at least, and at least a byte lies between
  // two.
  constexpr size_t maxRuns(size_t bytes) { return bytes / 4 + 1; }
  // The token's runs, in order, into `out` (room for maxRuns(text.size())); text outside them is not listed.
  size_t scanRuns(std::string_view text, Run* out);
  // Whether `text` holds a byte sequence of U+0900-0DFF (indic::startsIndic), bounded by the view.
  bool holdsIndic(std::string_view text);
  }  // namespace IndicShaper
  ```
- Produces, in `IndicReorder.h` (upstream's `std::string` function stays, for its tests):
  ```cpp
  // witchhunt: deviation 9 - the same form into `out` (`room` bytes, NUL-terminated), allocating nothing: true when the
  // form differs from the text and fits; false, `out` unspecified, otherwise. Room for 2 * len + 1 bytes always holds
  // the form: a two-part vowel at most doubles its bytes.
  bool indicReorderForDisplay(const char* text, size_t len, char* out, size_t room, size_t* written);
  ```

- [ ] **Step 1: Write the failing tests:**
  - `TextShapingMerge.ASpanSplitOfOneStyleMerges`: `हि` then `न्दी`, same key.
  - `TextShapingMerge.InsideASyllableAnotherStyleMerges`:
    - `क` with `ि`;
    - `क्` with `ष` (after a virama);
    - a reph token `र्` with `क`;
    - `क्` with `\u200Dष` (before the consonant a ZWJ binds).
  - `TextShapingMerge.BetweenSyllablesAnotherStyleStaysApart`: `क` with `म`, with different keys.
  - `TextShapingMerge.AGroupWithoutAShapedLetterMergesNothing`: `foo` with `bar` (same key) and `indicGroup` false.
    This keeps the Latin goldens.
  - `TextShapingMerge.TheDivineNameTakesAMarkOnlyTokenGluedToIt`: `יְהוָה` with U+0591, under either key, even with
    `indicGroup` false.
  - `TextShapingMerge.AMarkOnlyTokenAfterAnotherWordStaysApart`: `שלום` with U+0591.
  - `IndicRuns.ARunIsOneScriptAndTheSharedCodePointsAroundIt`:
    - `कि।` is one Devanagari run of 9 bytes;
    - `(कि),` is one run, bytes [1, 7);
    - `কিकि` is two runs;
    - `।` alone is one run with no script;
    - `abc` has none.
  - `IndicRuns.MaxRunsHoldsEveryToken`: a 200-byte token alternating `क` and `a` has at most `maxRuns(200)` runs.
  - `IndicReorderBounded.MatchesTheStringForm`: every reordering case of `IndicTextTest` gives the same form both ways.
  - `IndicReorderBounded.TwiceTheBytesHoldsTheForm`: 40 × `কো` fits in `2 * len + 1` bytes.
  - `IndicReorderBounded.TooLittleRoomIsRefusedNotCut`: room of `len` returns false, and `written` is untouched.
- [ ] **Step 2: Run** `cmake --build build/test --target TextShapingTest IndicTextTest -j 8`. Expected: compile errors.
- [ ] **Step 3: Implement.**
  - `scanRuns` follows #3787's `scanRun` (`ComplexShaper.cpp:310-325` at `0fbd1575`) over a bounded view. Credit it.
  - The `IndicReorder` overload's output type has `append(const char*, size_t)` and `appendCodepoint(uint32_t)`; a
    refusal sets a flag the overload returns.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "TextShaping|IndicText|BidiLine"`. Expected: all pass.
- [ ] **Step 5: Format check, then commit** `feat(shaping): which glued tokens merge, Indic runs, and IndicReorder
  without allocating`.

### Task 6: Shaping a token: the Indic module over a face (deviations 6, 7 and 9)

**Files:**
- Modify: `lib/TextShaping/IndicShaper.h`, `lib/TextShaping/IndicShaper.cpp`
- Modify: `lib/TextShaping/TextShaping.h`, `lib/TextShaping/TextShaping.cpp` (the build's heap block)
- Modify: `test/text_shaping/CMakeLists.txt`:
  - sources: `${OT_SHAPER_SOURCES}`, `lib/EpdFont/ShapingFace.cpp`;
  - includes: `${OT_SHAPER_INCLUDE_DIR}`, `lib/EpdFont`, `lib/GfxRenderer` (last), `test/ot_shaper_port`,
    `test/complex_shaper`, `test/ttf_font`;
  - defines: `SHAPING_FIXTURE_DIR`, `TTF_FIXTURE_DIR`;
  - `test/book_metadata_cache/AllocationRefusal.cpp`, whose `AllocationRefusal(bytes)` refuses every allocation of at
    least `bytes` while it lives
- Create: `test/text_shaping/IndicShaperTest.cpp`

**Interfaces:**
- Consumes: `ShapingFace::current()`, `plan()` (Task 4); `scanRuns`, `maxRuns`, the bounded `indicReorderForDisplay`
  (Task 5); `ot::shape`, `ot::maxRunLength`, `ot::Buffer`, `ot::Scale`, `ot::RunContext` (PR 1); `GlyphRecord`,
  `glyph_run::kMinAdvance`, `kMaxAdvance` (PR 2).
- Produces, in `IndicShaper.h`:
  ```cpp
  // What a token's text is drawn as, piece by piece; the pieces cover the text in order.
  enum class PieceKind : uint8_t { Text, Glyphs, Reordered };
  // Where shapeToken writes a token's pieces (ParsedText's paragraph store). Each call returns false when it has no
  // room: the token is then laid out as its code points.
  class PieceSink {
   public:
    virtual ~PieceSink() = default;
    virtual bool beginPiece(PieceKind kind, uint16_t begin, uint16_t end) = 0;  // bytes of the token's text
    virtual bool addGlyph(const GlyphRecord& record) = 0;    // a Glyphs piece's records, in visual order
    virtual bool addBytes(const char* bytes, size_t length) = 0;  // a Reordered piece's UTF-8 form
  };
  // The longest run one ot::shape call takes (its buffer has 2n + 16 slots); a longer one is shaped in pieces.
  constexpr unsigned kMaxRunCodepoints = 48;  // or 32: Step 1 decides
  // The syllable of a glued neighbour, kept apart (deviation 7), that a token is shaped with as context, at most.
  constexpr unsigned kContextCodepoints = 8;
  // The memory one token is shaped in: the Buffer, its storage (ot::Buffer::bytesFor(2 * runCodepoints + 16)), the
  // token's runs and its code points with their context, in one block of `arena` for this call alone, else in the
  // build's heap block (made on first use, grown nothrow to the longest token, freed by
  // TextShaping::releaseBuildMemory()). Released before any glyph is looked up: LIFO with the TrueType miss scope.
  class CallMemory {
   public:
    CallMemory(BuildArena* arena, size_t tokenBytes, unsigned runCodepoints = kMaxRunCodepoints);
    ~CallMemory();
    CallMemory(const CallMemory&) = delete;
    CallMemory& operator=(const CallMemory&) = delete;
    bool valid() const;
  };
  struct Request {
    ShapingFace* face = nullptr;               // null: the font cannot shape: the token is reordered whole
    uint32_t ppem26_6 = 0;                     // the size the glyphs are drawn at
    const uint32_t* languageTags = nullptr;    // the book's (TypographyProfile::shapingLanguage); null: the default
    std::string_view before, after;            // glued neighbours kept apart: context, never output
  };
  // Plain: nothing needs a form (no Indic run, or every form equals its text); nothing written.
  // Formed: the pieces are written. Degraded: a run could not be shaped (no memory, the font failed it, a record
  // outside its fields); what was written is to be dropped and the token laid out as its code points.
  enum class Outcome : uint8_t { Plain, Formed, Degraded };
  Outcome shapeToken(std::string_view text, const Request& request, CallMemory& memory, PieceSink& sink);
  ```
- Produces, in `TextShaping.h`:
  ```cpp
  // Frees the build's heap block for shaping calls (IndicShaper::CallMemory). A section build's end calls it, as it
  // calls BidiLine::releaseBuildMemory(); the next call makes it again.
  void releaseBuildMemory();
  bool buildMemoryHeld();  // for tests
  ```
  The heap block itself lives in `TextShaping.cpp`, behind a layer-internal `void* buildHeapBlock(size_t bytes)` that
  grows it nothrow. So `TextShaping.cpp` needs nothing of `IndicShaper.cpp`, and the targets that link it for the line
  pass (`BIDI_LAYOUT_SOURCES`) still link before Task 8 adds the module to them.
- **The rules of `shapeToken`:**
  - **No face:** one Reordered piece over the whole token, when the bounded `indicReorderForDisplay` changes it; else
    Plain.
  - **A face, per run:**
    - shared code points alone: Text;
    - a script the face does not map (`nominalGlyph(script->probe)`, upstream's coverage): Reordered when the run's
      form differs, else Text;
    - otherwise Glyphs. Text between runs is a Text piece.
  - **A Glyphs run:**
    - `plan = face->plan(script, languageTags)`; `face->current()` before each `ot::shape`;
    - `Scale::set(ppem, (ppem + 32) >> 6, upem)`;
    - shaped whole; when that fails with `buffer.successful` still set (the run is past the storage), upstream's
      piecewise loop: the breaks at `syllableBreakAllowed`, the context levels {2,2}, {1,1}, {1,0}, {0,1}, {0,0};
    - the token's first run takes up to `kContextCodepoints` of `before`'s last syllable as `RunContext::before`, when
      it starts at byte 0 and that syllable is the same script; the last run takes `after` the same way.
  - **Records:** the Global Constraints' rounding.
    - An advance outside `kMinAdvance..kMaxAdvance`, a `dx` or `dy` outside `int8_t`, or a glyph ID above 0xFFFF:
      Degraded. Refused, never clamped (the final review on PR 2).
    - Every glyph of a run carries the run as its cluster (deviation 6). The run is the piece; no per-glyph clusters.
  - **Memory:** nothing is allocated outside `CallMemory`, but by `ShapingFace::plan`.
  - **Not here:** deviation 6's cut at bidi class changes. In PR 3 only paragraphs without bidi shape, so every run is
    at one level; the cut comes with PR 4's line pass.

- [ ] **Step 1: The per-call cap** (left to the builder by the design: "from the share of tokens past it in a
  Malayalam and a Tamil book").
  - Download the most-downloaded Malayalam and Tamil EPUBs from Project Gutenberg's listings
    (`https://www.gutenberg.org/ebooks/search/?query=l.ml`, `l.ta`) into the scratchpad. When a language has none, use
    a public-domain novel exported from `ml.wikisource.org` or `ta.wikisource.org`.
  - With a scratchpad Python script, count each book's whitespace-separated tokens whose longest Indic run (the
    `scanRun` rule) has more than 32 and more than 48 code points.
  - Set `kMaxRunCodepoints` to 32 when at most 1% of either book's tokens pass 32, else to 48.
  - Record each book's ebook number or URL, its sha256 and the two shares for the commit body.
- [ ] **Step 2: Write the failing tests** (`IndicShaper`; faces are `FixtureShapingFace`):
  - `MatchesHarfBuzzInEveryScript`: every `kShapingFixtures` word, at `kFixturePpem26_6` in its language, is one Glyphs
    piece over the whole word whose records are `ExpectedShaping.h`'s glyphs.
  - `MatchesHarfBuzzOnTheTrueTypeSubset`: every run of `kDevanagariRunSets` (both sizes), on the subset files of
    `TTF_FIXTURE_DIR`, gives `ExpectedGlyphRuns.h`'s records.
  - `LeavesTextWithoutIndicPlain`: `hello` and a lone danda are Plain, and the sink is not called.
  - `CutsPiecesAtRunEdges`: on the Bengali face, `(কি),` gives Text `(`, Glyphs `কি`, Text `),`.
  - `ReordersAScriptTheFaceDoesNotMap`: on the Bengali face, `किকি` gives Reordered `िक`, then Glyphs `কি`
    (upstream's `LeavesScriptsTheFontDoesNotCoverAsText`, with our reordering).
  - `AFaceWithoutLayoutStillReorders` (the design's step 1: "a face without GSUB for the script still gets the
    shaper's reordering"): the Devanagari `.layout` face with GSUB and GPOS cleared gives `कि` as one Glyphs piece,
    the i-matra's glyph first.
  - `WithoutAFaceTheWholeTokenIsReordered`: `कि,` gives one Reordered piece, `िक,`.
  - `ShapesInTheBooksLanguage`: ल under `languageTagsFor("mr")` and under the default give different records.
  - `ARunLongerThanTheCallShapesInSyllables`: upstream's word, `kBengaliShaping[5..8]` joined with `তাঁদের`.
    `CallMemory` with `runCodepoints` 8 gives the records it gives with 64.
  - `AGluedNeighbourIsContextNotOutput`: `म` with `before = "क"` gives the records `ot::shape` gives for `कम` with
    `RunContext{1, 0}`.
  - `AnOffsetPastAByteIsDegradedNotClamped`: `kBengaliShaping[5]` (a `dx` of -13 at the fixture's size) at 12 × the
    fixture's ppem is Degraded.
  - `ACallWithoutMemoryIsDegraded`: an arena with 64 B free, and the heap block refused (`AllocationRefusal(4096)`
    around the call), give an invalid `CallMemory`, and `shapeToken` is Degraded.
  - `TheCallsBlockIsTheArenasForTheCallAlone`: with a 16 KB `BuildArena`, `used()` returns to its start after the
    call, and `buildMemoryHeld()` is false.
  - `WithoutAnArenaTheBuildsHeapBlockServesEveryCall`: arena null. The first call makes the block, a second of the
    same size makes nothing, and `releaseBuildMemory()` frees it.
- [ ] **Step 3: Run** `cmake --build build/test --target TextShapingTest -j 8`. Expected: a compile error, then
  failures.
- [ ] **Step 4: Implement.** The file's first comment names its sources (Global Constraints, "Credit").
- [ ] **Step 5: Run** `ctest --test-dir build/test -R "IndicShaper|TextShaping|OtShaper"`. Expected: all pass.
- [ ] **Step 6: Format check, then commit** `feat(shaping): the Indic module shapes a token into pieces over a face`.
  The body gives Step 1's books, shares and cap.

## Layout

### Task 7: The paragraph store

**Files:**
- Create: `lib/Epub/Epub/ShapedStore.h`, `lib/Epub/Epub/ShapedStore.cpp` (ours)
- Create: `test/shaped_store/CMakeLists.txt`, `test/shaped_store/ShapedStoreTest.cpp`; add the subdirectory to
  `test/CMakeLists.txt`. The target links `test/book_metadata_cache/AllocationRefusal.cpp`, for its refusals and its
  `heapLiveBytes()`.

**Interfaces:**
- Consumes: `IndicShaper::PieceSink`, `PieceKind` (Task 6); `GlyphRecord`, `glyph_run::encode`, `GlyphRun` (PR 2);
  `HashUtils::fnvHash64` (`lib/Epub/Epub/HashUtils.h`).
- Produces:
  ```cpp
  // The paragraph's shaped and reordered tokens (writing modes step 2, "How shaping runs" 5): per token, its pieces and
  // their forms, in step with ParsedText's word vectors, valid for the text and key they were made from. The forms
  // sit in chunks of kChunkBytes, from makeUniqueNoThrow (a piece larger than a chunk gets one of its own size); a
  // piece's bytes never straddle two. Reserved from the paragraph's estimate; its chunks survive the paragraphs
  // (rule 1) until release().
  class ShapedStore final : public IndicShaper::PieceSink {
   public:
    struct Piece {
      uint16_t begin = 0, end = 0;  // bytes of the token's text
      IndicShaper::PieceKind kind = IndicShaper::PieceKind::Text;
      uint16_t width = 0;  // px, set by setWidth()
      uint16_t count = 0;  // glyph records (Glyphs)
      uint16_t bytes = 0;  // its form's bytes
      uint8_t chunk = 0;
      uint16_t at = 0;
    };
    static constexpr size_t kChunkBytes = 1024;
    // Past them a piece is refused (logged once at ERR) and the token is laid out as code points. A 96-token pass
    // holds about 3-6 KB, so the cap sits five times above it.
    static constexpr size_t kMaxChunks = 32;
    // Opens token `token`'s entry for (text, key); shapeToken then writes through the sink; commit() keeps the pieces
    // (an entry with none for a Plain token), abandon() drops them. `token` may be one past the last: a scratch entry,
    // for a candidate the breaker measures, which abandon() always ends.
    bool open(size_t token, std::string_view text, uint32_t key);
    void commit();
    void abandon();
    // Token `token`'s pieces when its entry was made for this text and key; nullopt otherwise (none, or stale).
    std::optional<std::span<const Piece>> pieces(size_t token, std::string_view text, uint32_t key) const;
    void setWidth(size_t token, size_t piece, uint16_t width);
    GlyphRun glyphs(const Piece& piece) const;
    std::string_view form(const Piece& piece) const;
    // In step with the word vectors. insert() and reserve() grow nothrow behind a heap check, as
    // ParsedText::reserveWords does; false when refused.
    bool insert(size_t token);
    void erase(size_t token);
    void dropFront(size_t count);  // the consumed tokens' bytes are reclaimed; the kept tokens keep their pieces
    bool reserve(size_t tokens, size_t bytes);
    void reset();    // empty, chunks kept
    void release();  // everything freed
    size_t chunkBytes() const;  // held, for tests
  };
  ```
- An entry's validity is the text's length, its `fnvHash64` and the key. Lockstep makes reuse likely; validation makes
  it correct.
- `dropFront` moves the kept tokens' bytes down into the first chunks, in token order. A paragraph of any length then
  holds about one pass's records.

- [ ] **Step 1: Write the failing tests** (`ShapedStore`):
  - `APiecesRecordsReadBackAsWritten`: a Text, a Glyphs (5 records, one with an offset) and a Reordered piece.
    `glyphs()` decodes the records, and `form()` is the bytes.
  - `AnEntryIsValidOnlyForItsTextAndKey`: another text of the same length, or another key, gives nullopt.
  - `InsertAndEraseKeepTheOtherTokensEntries`.
  - `AScratchEntryLeavesNothingBehind`: open at `size`, write, abandon. Every entry and `chunkBytes()` are as before.
  - `DropFrontReclaimsTheConsumedTokens`: 50 rounds of 20 tokens of 40 B of records, each dropping 16. Every kept
    token's records read back, and `chunkBytes()` stays at or under 4 chunks.
  - `ResetKeepsTheChunksAndReleaseFreesThem`, by a heap counter.
  - `APieceLargerThanAChunkGetsAChunkOfItsOwn`: 300 records with offsets (1,800 B).
  - `ARefusedChunkFailsTheTokenNotTheStore`: with the 33rd chunk refused, `beginPiece`/`addGlyph` return false. After
    `abandon()`, earlier tokens read back and later small tokens fit.
- [ ] **Step 2: Run** `cmake --build build/test --target ShapedStoreTest -j 8`. Expected: a compile error.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R ShapedStore`. Expected: PASS.
- [ ] **Step 5: Format check, then commit** `feat(epub): the paragraph store for shaped tokens`.

### Task 8: ParsedText shapes its Indic tokens: merges, widths, forced splits, warm glyphs

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h`:
  - the declaration of a boxed `ShapingState`, defined in `ParsedText.cpp`, beside `BidiState` (`:162-179`);
  - `setShapingLatch` beside `setLineArena` (`:444`);
  - the private helpers below
- Modify: `lib/Epub/Epub/ParsedText.cpp`:
  - `layoutAndExtractLines` (`:675`): after `decideBidi` (`:684`), before the snapshot (`:716`); the consumed erase
    (`:991-1004`); the restore (`:1010-1020`); the split undo (`:913-924`);
  - `calculateWordWidths` (`:1171`), `kernBefore` (`:581`), `dropBidiControls` (`:653`), `hyphenateWordAtIndex`
    (`:1977`), `releaseLayoutScratch` (`:1094`), `reset` (`:1155`)
- Modify: `lib/Epub/Epub/TypographyProfile.h`, `lib/Epub/Epub/TypographyProfile.cpp` (`shapingLanguage`)
- Modify: `test/CMakeLists.txt`:
  - `BIDI_LAYOUT_SOURCES` gains `${OT_SHAPER_SOURCES}`, `IndicShaper.cpp`, `lib/Utf8/IndicReorder.cpp`,
    `lib/EpdFont/ShapingFace.cpp`, `lib/Epub/Epub/ShapedStore.cpp`;
  - `BIDI_LAYOUT_INCLUDES` gains `${OT_SHAPER_INCLUDE_DIR}`, `lib/EpdFont`, and `lib/GfxRenderer` last;
  - and `test/typography/CMakeLists.txt`, for `languageTagsFor`
- Modify, the three stubs of Task 3. Each gains:
  - `shapingFace(int, EpdFontFamily::Style, uint32_t*)`, answered by a static seam
    `ShapingFace* (*shapingFaceSource)(int fontId, EpdFontFamily::Style, uint32_t* ppem26_6)`, null by default;
  - `fontBaseScale(int)`, returning 1;
  - `prefetchGlyphRun`, counted in `prefetchedRuns`
- Modify: `test/bidi_line/BidiLineTest.cpp` (the rename of `ANameGluedToALeadingMarkFallsBackForNow`, below)
- Create: `test/word_size_layout/ShapedLayoutTest.cpp`, added to `WordSizeLayoutTest`. The target also gains
  `test/text_shaping`, `test/ot_shaper_port` and `test/complex_shaper` on its include path, the
  `SHAPING_FIXTURE_DIR` and `TTF_FIXTURE_DIR` defines, and `test/book_metadata_cache/AllocationRefusal.cpp`.

**Interfaces:**
- Consumes: Tasks 4-7. `GfxRenderer::shapingFace`, `fontBaseScale`, `glyphRunWidth`, `prefetchGlyphRun`.
- Produces, in `TypographyProfile`:
  ```cpp
  // ot::languageTagsFor(the tag): the OpenType language systems a shaped word tries; null: the script's default.
  const uint32_t* shapingLanguage = nullptr;
  ```
- Produces, in `ParsedText` (public):
  ```cpp
  // A run laid out unshaped for want of memory sets *latch (Section's kStatusShapingDegraded). Survives reset().
  void setShapingLatch(bool* latch) { shapingLatch_ = latch; }
  bool shapingStateHeld() const;  // for tests
  uint32_t tokensShaped() const;  // tokens handed to IndicShaper since construction: for tests and the build's log
  ```
- Private. `ShapingState` is only declared in `ParsedText.h` and defined in `ParsedText.cpp`, so the header includes
  nothing new (it is included far beyond layout). `~ParsedText()` and the move operations move out of line for it.
  ```cpp
  struct ShapingState;  // ParsedText.cpp: { ShapedStore store; std::string measureScratch; Task 9's piece vectors }
  std::unique_ptr<ShapingState> shapingState_;
  bool shapes_ = false;  // this pass forms Indic words: no bidi, and a token holds U+0900-0DFF
  bool* shapingLatch_ = nullptr;
  uint32_t shapeKey(const GfxRenderer& renderer, int fontId, size_t i) const;  // font, style bits, size, drawn ppem
  void mergeGluedGroups(const GfxRenderer& renderer, int fontId);
  // Token i's pieces, shaped now or found in the store, their glyphs warmed and their widths set; false: it is laid out
  // as its code points (and the latch set when for want of memory).
  bool shapeToken(const GfxRenderer& renderer, int fontId, size_t i);
  uint16_t shapedWidth(size_t i) const;  // the sum of its pieces' widths
  ```
- **The rules:**
  - **`shapes_`** is decided per pass: `!bidi_` and a token with `IndicShaper::holdsIndic`. `ShapingState` is made
    with `makeUniqueNoThrow` on the first such pass; refused, the pass lays out as today and sets the latch.
  - **Merges** run after `decideBidi`, before the snapshot, over each maximal run of glued tokens
    (`wordBoundaries[j].continues`). Ruby tokens and combine requests are never merged.
    - `mergesWithPrevious` gets `indicGroup = shapes_ &&` the group holds U+0900-0DFF.
    - A merge appends the token's text to the one before, and erases it from every word vector and from the store.
    - It keeps the first token's style, size, boundary and visible offset (deviation 7).
  - **The face** per token: `renderer.shapingFace(wordFontId(i), style & BOLD_ITALIC, &ppem)`; null on a vertical
    line, which reorders instead.
    - The drawn ppem is `ppem × fontBaseScale(font) × wordScale(i)`, rounded.
    - The language is `typography_.shapingLanguage`.
    - The context is the glued neighbours' texts, when they were kept apart.
  - **The call:**
    - `CallMemory(lineArena_, token bytes)`, then `shapeToken`, then the call's block is released;
    - then `prefetchGlyphRun` for each Glyphs piece;
    - then the widths: Glyphs `renderer.glyphRunWidth(font, run, wordScale(i))`; Text and Reordered measured as
      today, in the word's font, style and scale.
  - **Store keys** hold the drawn ppem. Records shaped at one size are never reused at another.
  - **`calculateWordWidths`** gives a token with pieces the sum of their widths. Every other token is measured as
    today.
  - **Reserve** the store at each pass from the estimate: tokens holding U+0900-0DFF, and 2 B of records per byte of
    their Indic text.
  - **`hyphenateWordAtIndex`** on a shaped token:
    - measures each candidate prefix, its hyphen included, by shaping it in the store's scratch entry;
    - after the split, inserts the remainder's entry, and shapes both parts again;
    - only upstream's fallback breaks reach it (Task 2).
  - **`kernBefore`** is 0 across a glue where either side's edge piece is Glyphs.
  - **In step:**
    - the consumed erase calls `store.dropFront(consumed)`;
    - the split undo and `dropBidiControls` erase the entry;
    - the `preserveSource` restore calls `store.reset()`;
    - `reset()` calls `store.reset()`;
    - `releaseLayoutScratch()` releases the store, and drops `ShapingState` when no pass in progress needs it.
  - **`TypographyProfile::forLanguage`** sets `shapingLanguage = ot::languageTagsFor(tag.c_str())`.

- [ ] **Step 1: Write the failing tests** (`ShapedLayout`).
  - The fixture: a `FixtureShapingFace` over `NotoSansDevanagari-Shaping-Regular.ttf` (`TTF_FIXTURE_DIR`), installed
    through the stub's seam at ppem 1280.
  - Expected records come from `ottest::shapeAsStored` at the same ppem.
  - The tests:
  - `BreaksOnTheShapedWidths`: `हिन्दी पत्रिका` fits one line at `w1 + space + w2` px, and takes two at one px less.
  - `ATokenKeptForTheNextPassIsNotShapedAgain`: a pass with `includeLastLine` false, then a pass with true.
    `tokensShaped()` equals the paragraph's token count.
  - `AGroupAcrossTheFlushIsMergedWhole` (the parser's 96-token flush): a pass with `includeLastLine` false ends with
    `क`; then `ि` in BOLD is added, glued, and the last pass lays out one word `कि`.
  - `RecordsOfAnotherSizeAreNotReused` (Review Focus 1): after a pass at ppem 1280, a pass at 1600 shapes the kept
    tokens again (`tokensShaped()` counts them twice).
  - `MergesASyllableSplitByAStyle`: `क`, then `ि` in BOLD, glued. The line has one word `कि`, regular, at `क`'s
    visible offset.
  - `ASpanSplitOfOneStyleIsByteForByteOneToken`: `हि` then `न्दी` give the same TextBlock bytes as one `हिन्दी`.
  - `TheDivineNameGluedToAMarkIsDrawnAsHeAndGeresh` (the flip of `95d410915`):
    - the words of `BidiLine.ANameGluedToALeadingMarkFallsBackForNow`, through ParsedText with the mark glued;
    - the name and its mark are one word, whose display form is U+05F3 U+05D4;
    - the line holds a logical order of 4 words (no fallback);
    - rename the BidiLine test `ANameSplitFromItsMarkFallsBackInTheLinePass`, its comment saying ParsedText merges the
      pair first (Task 5's rule).
  - `AForcedSplitFallsBetweenSyllables`: a 14-syllable token in a column narrower than it. Every line but the last
    ends with `-`, and no line starts with a dependent sign, a ZWJ or a ZWNJ.
  - `ARefusedCallLaysTheTokenOutAsCodePointsAndLatches` (the spec's review focus 2): a 64 B arena, and the heap block
    refused (`AllocationRefusal(4096)`).
    - The token breaks as its code points' width would, and the latch is set.
    - The next paragraph, with memory, shapes it.
  - `AFontThatCannotShapeBuildsNoPlan`: with the seam null, and with the Bengali `.layout` face under Devanagari text,
    no plan is built (`planBuilds()` 0) and the latch stays false.
  - `TheGlyphsAreWarmedOncePerShaping`: `prefetchedRuns` equals the Glyphs pieces shaped.
  - `ABidiParagraphShapesNothingYet`: a Hebrew word and a Hindi word in one paragraph. No `ShapingState` is made, and
    the Hindi word is drawn as its code points.
  - `ALatinParagraphMakesNoShapingState`.
  - The forms these tokens take on their lines are Task 9's tests.
- [ ] **Step 2: Run** `cmake --build build/test --target WordSizeLayoutTest -j 8`. Expected: compile errors, then
  failures.
- [ ] **Step 3: Implement** as the Interfaces block says.
- [ ] **Step 4: Run**
  `ctest --test-dir build/test -R "ShapedLayout|WordSize|CjkLayout|BidiLine|EpubPipeline|Typography"`.
  Expected: all pass but the five known, and every golden is unchanged.
- [ ] **Step 5: Format check, then commit** `feat(epub): ParsedText shapes its Indic tokens once a paragraph
  (deviation 7)`.

### Task 9: Pieces on the line (step 7)

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h` (one private helper)
- Modify: `lib/Epub/Epub/ParsedText.cpp`: `ShapingState`'s piece vectors; `extractLine` (`:2295`), after the
  annotations (`:2622-2629`) and where the range is built (`:2634-2646`)
- Test: `test/word_size_layout/ShapedLayoutTest.cpp` (suite `ShapedLine`)

**Interfaces:**
- Consumes: `ShapedStore::pieces`, `glyphs`, `form` (Task 7); `TextBlock::WordRange::glyphForms` (Task 3).
- Produces, private:
  ```cpp
  // A line of a paragraph that shapes, with a token that has pieces: each token becomes its pieces, in order. The
  // first piece keeps the token's boundary, the others are glued; each takes the token's style and size; a Glyphs piece
  // is drawn as its run, a Reordered piece as its form. lineXPosScratch_ then holds the pieces' positions (the token's
  // x plus the widths before it), and the annotations name pieces. `range` is pointed at ShapingState's vectors.
  void viewShapedPieces(size_t first, size_t count, TextBlock::WordRange& range);
  ```
- `ShapingState` gains `pieceWords`, `pieceForms`, `pieceGlyphs`, `pieceStyles`, `pieceSizes`, `pieceBoundaries` and
  `pieceAnnotations`, all keeping their capacity across lines.
- A line without a token with pieces takes today's path, untouched.
- The bidi path, and `viewWithoutControls`, do not change.

- [ ] **Step 1: Write the failing tests** (`ShapedLine`, Task 8's fixture):
  - `AWordThatIsOneRunStaysOneWord`: `हिन्दी` is one word whose `glyphForm` is the oracle's records.
  - `PunctuationAfterAWordIsAGluedPiece`: `हिन्दी,` is two words.
    - The second is glued, at `x + glyphRunAdvance`.
    - Its text is `,`, with no form.
  - `ATokenOfTwoScriptsBecomesTwoPieces`: `हिन्दीবাংলা` on the Devanagari face gives a Glyphs piece and a Reordered
    piece.
  - `JustificationStretchesTokensNotPieces`: a justified line's pieces keep their distance. Only the gaps between
    tokens grow.
  - `AnEmphasisRunCoversThePiecesOfItsTokens`: `text-emphasis` over `हिन्दी,` covers both pieces.
  - `ALineWithoutAFormKeepsItsBytes`: in a Hindi paragraph, a line of Latin words serializes to the bytes the same
    words, at the same x, give in a Latin paragraph.
  - `GluedPiecesTakeNoKerning`: the stub's kerning seam returns 3 px for every pair, and a glued pair across a Glyphs
    edge still touches.
  - `TheLinesTokenCountIsItsTokens`: `lastLineTokenCount()` counts tokens, not pieces.
  - The forms of Task 8's cases, now that they reach the line:
    - `AStyleChangeBetweenSyllablesIsShapedWithItsNeighbour`: `क`, then `म` in BOLD. The two words keep their styles,
      and `म`'s records are those with `क` as its context.
    - `AWordWiderThanTheLineSplitsBetweenSyllables` (Review Focus 3): Task 8's 14-syllable token. Each part is a glyph
      form whose records are its own text's shaping, its hyphen included.
    - `ASuperscriptIsShapedAtItsDrawnSize`: a SUP word at 50% has the records of ppem 640.
    - `AFontThatCannotShapeReordersAndLatchesNothing` (Review Focus 5): with the seam null, `कि` is one word with the
      display form `िक`; with the Bengali `.layout` face, a Devanagari token is one word with a Reordered form.
    - `AFaceReleasedBetweenTwoPassesShapesAfresh` (Review Focus 1):
      - between a pass with `includeLastLine` false and the last pass, the seam's face is destroyed and a new one
        installed;
      - the kept tokens' records are reused as bytes (no plan is needed for them), and new tokens shape with the new
        face;
      - every glyph form equals the oracle.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R ShapedLine`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "ShapedL|WordSize|CjkLayout|EpubPipeline"`. Expected: all pass
  but the five known, and every golden is unchanged.
- [ ] **Step 5: Format check, then commit** `feat(epub): a shaped token's pieces on its line (step 7)`.

## The build

### Task 10: The section's version, the degraded latch, and the build's shaping lifetime

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (`shapingDegraded()` beside `tableRowDegraded()` `:1071`)
  and `ChapterHtmlSlimParser.cpp`:
  - `setShapingLatch` beside each `setTypography` (`:2257`, `:2670`, `:5668`)
- Modify: `lib/Epub/Epub/Section.h` (`shapingDegraded_`, `isShapingDegraded()` beside `:340`) and `Section.cpp`:
  - `SECTION_FILE_VERSION` (`:37`) and the status bits (`:191-212`);
  - the latch resets (`:695`, `:856`, `:1182`) and the load (`:780`);
  - the parse-end latches (`:1694-1701`) and the status write (`:1871`);
  - the page-count record (`:2155`) and `~BuildState` (`:997-1012`)
- Modify: `src/activities/reader/EpubReaderActivity.cpp`:
  - Background-B's discard (`:1933-1950`);
  - Background-C's fallback (`:2507-2513`);
  - the rebuild on entry (`:4755-4762`)
- Modify: `src/util/DictHtmlPages.cpp` (the release struct, `:84-87`)
- Modify: `test/epub_pipeline/PipelineRunner.h` and `PipelineRunner.cpp` (`Profile::shapingFontFile`), and
  `test/epub_pipeline/CMakeLists.txt` (`${REPO_ROOT}/test/text_shaping` on the include path, for `FixtureShapingFace.h`,
  and `SHAPING_FONT_DIR="${REPO_ROOT}/test/fixtures/fonts"`)
- Modify: `docs/memory-allocation-strategy.md` (invariant 6), `docs/file-formats.md` (the status byte)
- Create: `test/epub_pipeline/ShapingStatusTest.cpp` (added to `EpubPipelineTest`)

**Interfaces:**
- Consumes: `ParsedText::setShapingLatch` (Task 8); `TextShaping::releaseBuildMemory` (Task 6);
  `GfxRenderer::releaseShapingFaces` (Task 4).
- Produces:
  - `constexpr uint8_t SECTION_FILE_VERSION = 109;`, its comment: "v109: Indic lines are shaped: a word's glyph form
    (the bidi region's bit 2) and run-cut pieces (writing modes step 2)".
  - `constexpr uint8_t kStatusShapingDegraded = 1 << 6;`, its comment: "a shaped-script run was laid out as code points
    because the build had no memory for it; the reader rebuilds once per spine per session, as for the table-row bit".
  - `bool ChapterHtmlSlimParser::shapingDegraded() const` and `bool Section::isShapingDegraded() const`. The parse
    end logs `LOG_INF("SCT", "spine=%d shaping degraded: a run was laid out unshaped for want of memory", ...)` when it
    latches.
  - `pipeline_harness::Profile::shapingFontFile`: `const char*`, null by default. When set, the run installs a
    `FixtureShapingFace` over that file in the stub's seam (every font and style, ppem 1280), and removes it after.
- **The build's end** (`~BuildState`): `BidiLine::releaseBuildMemory()`, then `TextShaping::releaseBuildMemory()`, then
  `GfxRenderer::releaseShapingFaces()`. The dictionary's pages do the same on their way out.
- **The reader** treats `isShapingDegraded()` as it treats `isTableRowDegraded()`:
  - Background-B discards the build;
  - Background-C falls back to the released rebuild;
  - a cached section rebuilds on entry once per spine per session (`degradedLayoutRebuildSpine_`);
  - a degraded build records no page count.

- [ ] **Step 1: Write the failing tests** (`ShapingStatus`, a synthetic Hindi book, `Profile::shapingFontFile` =
  the Devanagari subset):
  - `ADegradedBuildIsLatchedAndKeptInTheFile`:
    - with the test's allocation refusal (`test/book_metadata_cache/AllocationRefusal.cpp`) armed to refuse blocks of
      4 KB and up, and no lent arena, `isShapingDegraded()` is true after the build;
    - and after `loadSectionFile`.
  - `ACleanBuildIsNotDegraded`: the same build, unarmed.
  - `TheBuildsEndFreesTheShapingMemory`: after each build, `TextShaping::buildMemoryHeld()` is false, and the stub's
    `shapingReleases` counter has risen by one.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R ShapingStatus`. Expected: FAIL.
- [ ] **Step 3: Implement** as the Interfaces block says.
  - The docs: invariant 6 names `kStatusShapingDegraded` with the three bits that earn a rebuild.
  - `docs/file-formats.md`'s status byte lists bit 6.
  - The stub gains `static void releaseShapingFaces()`, counting `shapingReleases`.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "ShapingStatus|EpubPipeline|SectionAbort|LowHeap"`. Expected:
  all pass but the five known, and every golden is unchanged (the version is not in the dump). Then build `default`.
  Expected: it links, with Task 4's hook.
- [ ] **Step 5: Format check, then commit** `feat(epub): section v109, kStatusShapingDegraded, and the shaping memory
  freed at a build's end`.

### Task 11: The X4's in-place gate refuses a shaped-script book in a TrueType set

**Files:**
- Modify: `lib/Epub/Epub/TypographyProfile.h`, `lib/Epub/Epub/TypographyProfile.cpp`
- Modify: `src/activities/reader/InPlaceBuildGate.h` (`Request`, `Admission`, `admitsResident`, `:84-153`)
- Modify: `src/activities/reader/EpubReaderActivity.cpp`: `heapAllowsInPlaceBuild` (`:4261-4295`)
- Test: `test/in_place_build_gate/InPlaceBuildGateTest.cpp`, `test/typography/TypographyTest.cpp`

**Interfaces:**
- Produces, in `TypographyProfile`:
  ```cpp
  // The language is written in a script the shaper shapes (the ten Indic scripts; PR 4 adds Arabic's). Not a layout
  // rule: the reader asks it whether a chapter's build shapes on the heap (InPlaceBuildGate.h).
  bool shapedScript = false;
  ```
  It is set for the primary tags `hi`, `mr`, `ne`, `sa`, `kok`, `mai`, `bho`, `awa`, `mag`, `raj`, `bn`, `as`, `pa`,
  `gu`, `or`, `ta`, `te`, `kn`, `ml`, `si`.
- `InPlaceBuildGate::Request` gains `bool shapedScript = false;`, and `Admission` gains `bool shapedTrueType = false;`.
  - `shapedTrueType = r.shapedScript && r.trueTypeSet`.
  - `resident` also needs `!shapedTrueType`.
  - The comment above `admitsResident` says why: "an in-place build lends no arena, so every shaping call and plan
    would take the heap" (the design's "The X4's in-place build").
- The gate's log line gains `shapedTtf=%d`.

- [ ] **Step 1: Write the failing tests:**
  - `InPlaceBuildGate.AShapedScriptBookInATrueTypeSetNeverBuildsInPlace`: at any heap, with `shapedTrueType` true.
  - `InPlaceBuildGate.AShapedScriptBookInABitmapFontKeepsTheFloors`: `expectLatinFloors` with `shapedScript` set.
  - `TypographyProfile.TheIndicLanguagesAreShapedScripts`:
    - `hi`, `hi-IN`, `mr`, `bn-BD`, `ta`, `ml` and `si` are;
    - `en`, `he`, `ja` and `ur` are not (`ur` is PR 4's).
- [ ] **Step 2: Run** `ctest --test-dir build/test -R "InPlaceBuildGate|TypographyProfile"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same. Expected: PASS.
- [ ] **Step 5: Format check, then commit** `feat(reader): no in-place build for a shaped-script book in a TrueType
  set`.

## Proof

### Task 12: Four Indic fixture books, their goldens, the rule checker, and the host figures

**Files:**
- Create: `test/epubs/make_test_shaped_indic.py`, writing:
  - `test/epubs/test_shaped_devanagari.epub` (`hi`), `test_shaped_bengali.epub` (`bn`), `test_shaped_tamil.epub`
    (`ta`), `test_shaped_malayalam.epub` (`ml`);
  - with `--long <dir>`, the two device timing books (Task 13), written to that directory, never committed
- Create: `lib/EpdFont/scripts/make_shaped_book_fonts.py`, writing
  `test/fixtures/fonts/NotoSans<Script>-Book-Regular.ttf` for Devanagari, Bengali, Tamil and Malayalam
- Modify: `test/fixtures/fonts/README.md` (rows, sources, regenerating, licences)
- Modify: `test/epub_pipeline/EpubPipelineTest.cpp`: each Indic book's default case shapes with its font, and
  `test_shaped_devanagari` gains an `_unshaped` case without one
- Modify: `test/epub_pipeline/DumpMain.cpp` (`--shaping-font=<file>`, and `--bench`'s page bytes)
- Modify: `lib/Epub/Epub/blocks/TextBlock.h` (`size_t arenaBytes() const`, for the dump)
- Create: `test/epub_pipeline/ShapingRulesTest.cpp` (added to `EpubPipelineTest`), and the five goldens under
  `test/epub_pipeline/goldens/`

**Interfaces:**
- **Each book**, generated as the other `make_test_*.py` scripts generate theirs:
  - **Text:** two chapters of the Universal Declaration of Human Rights in its language: the preamble's first two
    paragraphs, then Articles 1-3, about 250 words. The UN's translations are free to use; the script's header names
    the URL each came from.
  - **Markup:** a `<h2>` heading; a `<b>` and an `<i>` word; `<sup>` over a letter; an inline `font-size: 80%` span;
    punctuation, brackets and a danda.
  - **Deviation 7's cases, in each script that has them:**
    - a vowel sign in a `<b>` of its own (`क<b>ि</b>`);
    - a converter's same-style split (`<span>हि</span><span>न्दी</span>`);
    - a bold letter between syllables (`क<b>म</b>ल`).
  - **Deviation 8's cases:** in Hindi, a half form with ZWJ and a ZWNJ pair; in Malayalam, a chillu with ZWJ at a
    word's end.
  - **Malayalam only:**
    - a compound of more than `kMaxRunCodepoints` code points (the piecewise loop);
    - a 2-column table whose narrow cell holds a compound wider than the cell (a forced split, Review Focus 3).
  - **A Latin sentence**, whose lines must keep their bytes.
- **The fonts script:**
  - **Sources:** `--source-dir` with the unhinted Noto Sans Regular of the four scripts from
    `notofonts/notofonts.github.io` at `5c82cef0a8f08806f932062e18c02be62be278f0`, each checked against the sha256 of
    the measurements' table:
    - Devanagari `9c7d935139ea6a1e6ad9dbac4f6d27ece1e04bca8123c8888d00a0f9df4724cd`;
    - Bengali `5dceda02816fece18ea6796f474f5d3170f1d862f21b1e809cdc94b1caa4b6ec`;
    - Tamil `6634d9cc97a726e670df41281dd32b167a6b9b71f2036e19671ff08fdde0c292`;
    - Malayalam `cc39bb1f7d63b582a2aec1abf2469a43b805a3523a27e0a0eee8cd32702541d5`.
  - **The subset:** the code points of its book's XHTML plus U+0020, U+200C, U+200D and U+25CC, with PR 2's
    `make_shaping_fixtures.py` options.
  - It fails a font of 131,072 B or more. Its output is byte-identical on a second run.
- **The rule checker** (`ShapingRules`, after `BidiRulesTest`). It runs on every page of the four books with their
  fonts, every page line and every table-cell line:
  1. a glyph form only on a word whose text holds U+0900-0DFF;
  2. each shaped word is as wide as it was measured (step 5):
     - a word glued to one with a glyph form starts at that word's x plus its width (the run's advance; a Text
       piece's width is the stub's, scaled);
     - a word with a glyph form followed by a spaced word on its line ends at or before that word's x;
  3. no line starts with a dependent sign (`indic::isDependentSign`), a ZWJ or a ZWNJ, and no line ends with a virama;
  4. each glyph form is what the engine makes of its word: `IndicShaper::shapeToken` over the word's text, at its
     drawn ppem (1280 × its scale), in the book's language, with its glued neighbours as context when they were kept
     apart, gives one Glyphs piece with the stored records;
  5. every word is drawn inside the viewport.
  - Each book prints its counts against floors: glyph words (at least 150 in each), merged words, kept-apart pairs,
    pieces glued to punctuation, and (Malayalam) a word past the cap and a word split across lines.

- [ ] **Step 1: The books.** Write the generator and run it: `python test/epubs/make_test_shaped_indic.py`. A second
  run writes the same bytes.
- [ ] **Step 2: The fonts.** Write the fonts script and run it with
  `python -I -X utf8 C:/tmp/wm2-measure/tools/pyrun.py C:/tmp/wm2-measure/pydeps
  lib/EpdFont/scripts/make_shaped_book_fonts.py --source-dir C:/tmp/wm2-measure/dl/fonts --books test/epubs
  --fonts-out test/fixtures/fonts`. Expected: four fonts, each under 128 KB. Update the README (the OFL, and the
  source's name ID 0 copyright).
- [ ] **Step 3: The failing tests.**
  - `ShapingRules` over the four books.
  - The five golden cases, which fail with no golden on disk.
- [ ] **Step 4: The dump.**
  - `--shaping-font=<file>` sets `Profile::shapingFontFile`.
  - `--bench` adds per page `page_bytes=` (the page's TextBlocks' `arenaBytes()`), `glyph_bytes=` (their glyph forms'
    record bytes) and `token_bytes=` (#3787's tokens for the same glyphs: 8 B a glyph, 12 with an offset).
- [ ] **Step 5: Run** `ctest --test-dir build/test -R ShapingRules`.
  - Expected: PASS with every floor met.
  - A broken rule is a layout bug: fix it in its task's code.
- [ ] **Step 6: The goldens.**
  - `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline` writes the five new files. `git status` must show no
    other golden changed.
  - Read each new golden. Its `WG` records sit on Indic words only, and its Latin lines look like a Latin book's.
- [ ] **Step 7: The host figures (item 5),** with `WH_HOST_STDIO_UNBUFFERED=1`:
  - `epub_pipeline_dump <book> --bench --shaping-font=<font>` for each Indic book;
  - `epub_pipeline_dump test/epubs/test_long_paragraph_spacing.epub --bench`, for the Latin baseline.
  - Record per book: pages, `allocs=`, `page_bytes` (mean and max), `glyph_bytes` against `token_bytes`.
  - The Hindi book's allocations per page must be within 25% of the Latin book's (step 1's rule). A larger gap is a
    finding to fix before the PR: read the `alloc_site` lines.
- [ ] **Step 8: Format and NUL check, then commit** `test(epub): four Indic books, their goldens, and the shaping rule
  checker`. The body gives the figures.

### Task 12a: The engine: consonant positions remembered by the plan, and the planned lookups' digests in RAM

Added 2026-11-06, after Task 13 Step 3 missed its pass line (Hindi 10,753 ms against English 2,560 ms: 4.2x). The X3
profile: `ot::shape` is 2.34 ms a word, 76% of the shaping cost. One word's working set overflows the 16 KB flash
cache, so the cost is reads of the flash-mapped GSUB, not instructions. Two of its parts repeat flash reads whose
answers do not change within a plan:
- Indic initial reordering is 13% of `ot::shape`: `consonantPositionFromFace` runs up to eight `wouldSubstitute`
  walks for every consonant of every word;
- the GSUB loop's own overhead is 10%: `Plan::settings()` reads each planned lookup's digest record and subtable start
  from the CPac in flash, for all 77 planned lookups a word, before the digest skips about 56 of them.

Both changes keep the output byte-identical: a memo of a pure function, and a pre-filter that only rejects what the
flash digest would reject.

**Files:**
- Modify: `lib/OtShaper/OtPlan.h`, `lib/OtShaper/OtPlan.cpp`: the memo and the digest array, counted in
  `Plan::memoryBytes()`
- Modify: `lib/OtShaper/OtIndic.cpp` (`consonantPositionFromFace`, ~line 29), `lib/OtShaper/OtShaper.cpp`
  (`applyTable`, ~line 59)
- Test: `test/ot_shaper/OtShaperTest.cpp`, and the whole suite (every golden unchanged)

**Interfaces:**
- Consumes: `Plan::wouldSubstitute`, `Plan::settings`, `Digest::mayIntersect`, the CPac digest records
  (`readDigest`, `OtBuffer.cpp`).
- Produces: `uint8_t Plan::consonantPosition(const Face&, uint32_t consonant, uint32_t virama) const` (the memo, a
  `mutable` table filled on first use); `bool Plan::mayApply(int table, size_t i, const Digest& buffer) const` (the RAM
  pre-filter). `applyTable` calls `mayApply` before `settings()`.

- [ ] **Step 1: Write the failing tests.**
  - `OtShaper.ConsonantPositionMemoMatchesTheWalk`: for each fixture font (Devanagari, Bengali, Tamil, Malayalam in
    `test/fixtures/fonts/`), every consonant the font maps gets the same position from `consonantPosition` as from
    the uncached walk, asked twice (the second answer from the memo), and with two viramas when the script has them.
  - `OtShaper.MayApplyRejectsOnlyWhatTheDigestRejects`: for every planned lookup of each fixture plan and a set of
    buffers (each fixture book's first 200 tokens), `mayApply == false` implies the flash digest test fails too.
  - `OtShaper.TheTwoMemosLeaveEveryGlyphAlone`: each fixture book's tokens shaped by a plan whose memo and digest
    array are switched off by a test hook, and by a normal plan: glyphs, advances and offsets equal.
- [ ] **Step 2: Implement.**
  - The memo: a direct-mapped table of 64 entries, keyed by `(consonant, virama)`, filled on a miss by the existing
    walk, in the plan (`mutable`; plans are per build and single-threaded under RenderLock: say so in a comment).
    `consonantPositionFromFace` stays the walk; its caller in `indicInitialReordering` (~line 485) asks the plan.
  - The digest array: at `Plan::build`, each planned lookup's digest copied from the CPac into one array in the plan,
    GSUB then GPOS, in `plan.lookups` order. A lookup without a digest record gets an all-ones digest (it never
    rejects). If a host count over the four fixture books shows that one of the digest's three masks alone rejects at
    least 90% of what all three reject, keep that one mask (8 B a lookup) and say so with the counts; otherwise all
    three (24 B a lookup).
  - Report `plan_bytes` before and after for each fixture font (the plan is on the heap during a build).
- [ ] **Step 3: Run the tests, then the whole suite.** Expected: the new tests pass, and the suite shows only the five
  known Windows failures, with every golden unchanged.
- [ ] **Step 4: Measure on the host** with the profile harness of `wip/shaper-profile` (commit b6688a4d3; cherry-pick
  nothing from it into the branch): `ot::shape` per word and per stage on `test_shaped_long_hi`'s chapter 2, before
  and after. Put the table in the report.
- [ ] **Step 5: Commit** `perf(shaper): the plan remembers consonant positions and keeps its lookups' digests`.

### Task 12b: The word cache in the arena's free tail

The design's remedy for a missed time gate: "#3787's direct-mapped run cache comes back as a class C block of the
build". The maintainer's ruling (2026-11-06): it lives in the build arena and must cost no other arena user any room.
A kept block cannot do that: `release()` rewinds newest-first, and the open page block sits above anything reserved at
the parse's start, so the block could never be given back to a mid-parse image walk or glyph miss. So the cache lives
in the arena's **free tail**:
- it occupies `[capacity - S, capacity)`, which the arena still counts as free;
- every room check stays as it is, and any allocation may overwrite the tail;
- a watermark tells the cache when that happened, and it then empties itself.

This is safe only because nothing writes the arena's memory except through `alloc()` (`BuildArena` has no raw
accessor). The measured room (the X3, the long Hindi chapter, cap 52,272): 41,428 B free at the parse's start, 29,973
at its peak. Only a stage-2 image walk, or a CFF glyph miss read from the card at a full page, reaches a 16 KB tail.

**Files:**
- Modify: `lib/Memory/BuildArena.h`: `markPeak()`, `peakSinceMark()`, and `uint8_t* tail(size_t bytes)` (the top
  `bytes` of the region, or null when `bytes > capacity()`)
- Create: `lib/Epub/Epub/WordShapeCache.h`, `lib/Epub/Epub/WordShapeCache.cpp`
- Modify: `lib/Epub/Epub/ParsedText.h`/`.cpp` (`setWordCache`, `shapeToken` ~line 960),
  `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`/`.cpp` (it owns the cache for the parse: armed in `setup()` after
  the SAX state is taken, bound to each `ParsedText` beside `setLineArena` (~lines 1758 and 5879), unbound before
  `dropChunk()`), `lib/Epub/Epub/Section.cpp` (one log line after the `arena:` line)
- Modify: `docs/memory-allocation-strategy.md`: see Step 5
- Test: `test/memory_arena/` (the watermark), a new `test/word_shape_cache/`, `test/epub_pipeline/ShapingRulesTest.cpp`

**Interfaces:**
- Consumes: `ShapedStore` (`open`/`commit`/`abandon`, the `PieceSink` calls `beginPiece`/`addGlyph`/`addBytes`,
  `pieces()`, `glyphs()`, `form()`), `ParsedText::shapeKey`, `TypographyProfile::shapingLanguage`.
- Produces:
  - `class WordShapeCache` with `bool arm(BuildArena&, size_t bytes)`, `void disarm()`,
    `bool replay(uint64_t textHash, uint16_t length, uint32_t key, ShapedStore& store)` (true on a hit, with the
    pieces written through the store's sink), `void insert(uint64_t textHash, uint16_t length, uint32_t key, const
    ShapedStore& store, size_t token)`, and `struct Stats { uint32_t hits, misses, inserts, bypassed, clears; }`.
  - `ChapterHtmlSlimParser::wordCacheStats()` for Section's log line:
    `createSectionFile spine=%d word cache: bytes=%u hits=%u misses=%u inserts=%u bypassed=%u clears=%u`.

- [ ] **Step 1: The watermark, test first.**
  - `BuildArena.PeakSinceMarkFollowsAllocNotRelease`: after `markPeak()` an alloc moves `peakSinceMark()`, a release
    does not lower it, and a new `markPeak()` drops it to the cursor.
  - `BuildArena.TailIsTheRegionsTop`: `tail(n)` is `base + capacity - n`; null past the capacity.
  - Implement: one compare in `alloc()` beside `laneHighWater_`; `reset()` sets the mark to 0.
- [ ] **Step 2: The cache, test first** (`test/word_shape_cache/WordShapeCacheTest.cpp`):
  - `AHitReplaysTheSamePieces`: a token shaped into a store, inserted, then replayed into a fresh store: equal
    pieces, records and bytes.
  - `AnAllocIntoTheTailEmptiesIt`: armed on an arena, insert, then an alloc whose end passes `capacity - S` (a 33 KB
    walk on a 52,272 B arena with the cursor at 16 KB), then released: the next `replay` misses, `clears == 1`, and
    nothing is read from the overwritten bytes. Then the cache works again: insert, replay hits.
  - `ItDoesNotArmWithoutRoom`: `arm` fails when `capacity - used < S + 16,384`: every call is then `bypassed`.
  - `KeysKeepFormsApart`: the same text under two keys (style, size, language), or two texts with one key, never
    replay each other's pieces.
  - `ATooLargeTokenIsBypassed`: an entry larger than a slot is not stored, and is counted.
- [ ] **Step 3: Implement.**
  - **Layout:** direct-mapped fixed slots in the tail. S = 16,384 B (`kWordCacheBytes`). Choose the slot size from
    `test_shaped_long_hi`'s token sizes (pieces plus records plus the entry header), so that at least 95% of its Indic
    tokens fit, and state the slot size and the slot count. An entry holds the identity (the 64-bit text hash, the
    length, the 32-bit `shapeKey`, the language tags' identity) and the token's pieces with their records or bytes.
    The identity is the one `ShapedStore` already trusts (hash, key, length). Widths are not cached: they are measured
    after the replay, as now.
  - **Arming:** `arm` succeeds only when `capacity() - used() >= S + 16,384`, so that the parse lane's own page block
    and call blocks never reach the tail. On the X3 and the X4 lent arenas that holds; on the owned 10 KB arena it
    does not, so no cache there.
  - **Validity, checked at every `replay` and `insert`:** the tail is intact while `peakSinceMark() <= capacity - S`.
    Otherwise empty the slots (`clears++`), and re-arm with `markPeak()` once `used() <= capacity - S - 16,384`; until
    then each call is `bypassed`. Never read a slot before that check.
  - **What is cached:** only tokens with no glued neighbour (`before` and `after` both empty), shaped
    `Outcome::Formed`, and not the `shapedPrefixWidth` candidates. Glued tokens and prefixes take the uncached path.
  - **In `ParsedText::shapeToken`:** after `store.open`, try `replay`. On a hit, `store.commit()` and fall through to
    the existing prefetch and width code. On a miss, shape as now; after `store.commit()`, `insert`.
  - **The lent region stays the build's alone:** confirm that nothing else writes the borrowed framebuffer while a
    build holds it, between slices included. Name every lend and return point in the report. If any writer exists,
    the cache must `disarm()` at that point.
- [ ] **Step 4: The pipeline.**
  - The whole suite, with the cache on: every golden unchanged.
  - `ShapingRules.TheWordCacheChangesNoLine`: `test_shaped_long_hi` dumped through the pipeline on a small arena that
    forces clears (choose the capacity so `clears > 0`, and assert it). Compare against a run with the cache disarmed:
    equal dumps.
  - Report the hit rate on `test_shaped_long_hi` and `test_shaped_devanagari` (the stats line), and `ot::shape` calls
    before and after.
- [ ] **Step 5: The documentation.** In `docs/memory-allocation-strategy.md`:
  - §4 Class C: the parse lane's users in the order a build meets them. That is the open page block, the shaping call
    block, the TrueType miss scope, the bidi workspace, table rows and the mid-build draw. The image probe (8.7 KB)
    and the deferred header walk (16.9 KB, up to 33.3 KB) run mid-parse, inside `startElement` for `<img>`; image
    decodes never run during a build.
  - The extraction ring is released before every parse: every build is two-phase.
  - The tail and its contract.
  - §6: a new invariant: nothing writes a build arena's memory except through `alloc()`. The word cache's tail
    depends on it, so `BuildArena` gets no raw accessor.
  - §8.2: the shaped Hindi chapter on the X3 by moment (setup 95; extraction 41,984 above setup; resident 10,844;
    parse peak 22,299, free 29,973; a stage-2 walk fits only while the open page block is at most 8,116 B; on the X4,
    at most 3,844 B). Add the tail's place in that table.
- [ ] **Step 6: Commit** `perf(epub): a word-shaping cache in the build arena's free tail`. Put the docs in their own
  commit, `docs(memory): the build arena's parse lane, mid-parse image walks and the word cache's tail`.

### Task 13: The devices: the build-time gate, item 6, and the pages before and after

**Files:**
- Modify: `docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`: a new section "PR 3: shaped text on
  the page (items 5 and 6)", and the gate table's rows for item 6 and item 2's running total

The device runs are the controller's to schedule. This task names what they must show. All books and images go to the
scratchpad first.

- [ ] **Step 1: The baseline.**
  - Make a scratch worktree of the branch's merge base with master: `git worktree add --detach C:/tmp/wt-wm3-base
    $(git merge-base HEAD origin/master)`.
  - Build `default` and `gh_release` there, and record `Flash:` and `RAM:`. Keep its `default` `firmware.factory.bin`.
  - Run `make_test_shaped_indic.py --long <scratchpad>/long`. It writes `test_shaped_long_hi.epub` and
    `test_shaped_long_en.epub`.
    - Both have two chapters: a short first, then the UDHR repeated to 2,500 words.
    - The English chapter has the Hindi one's word count and paragraph count, so it is "a Latin chapter of the same
      length".
- [ ] **Step 2: Before, on the X3** (COM6, master's baseline flashed and its web bundle copied):
  - `CMD:GET:*` first, for the restore;
  - upload `test_shaped_bengali.epub` and the two timing books to `/test/` with devctl's `put:`. The X3 already holds
    `/test/test_hindi_shaping.epub` and the merged NotoSansDevanagari and NotoSansBengali families;
  - for each page below: set the family with `CMD:SET:fontFamily` and the size with `CMD:SET:fontSize` (by label), open
    it with `CMD:ACT:READER:<path>@<spine>:<page>`, then `CMD:IDLE:60000:SCREEN` and devctl's `shot:`:
    - `/test/test_hindi_shaping.epub`'s first page of Hindi text, in NotoSansDevanagari, at 14 pt and at 18 pt. Open it
      at `@1:1`; when spine 1 holds no Hindi, take the first spine that does. Record the spine and page: the "after"
      shots use the same ones;
    - `/test/test_shaped_bengali.epub@1:1` in NotoSansBengali, at 14 pt.
- [ ] **Step 3: The gate, on the X3,** with the branch's `default` flashed and its web bundle copied.
  - In NotoSansDevanagari at 14 pt, open each timing book at `@2:1` from a cold cache, three times each. Before each
    open, delete the book's section files: `CMD:FS:LS` its `/.crosspoint/epub_<hash>/spines/` folders, then
    `CMD:FS:RM` each `.bin`.
  - Read `createSectionFile spine=2 done: total=…ms (… parse=…)` from the log.
  - **The pass line:** the Hindi chapter's median `total` is at most twice the English one's.
  - **Missed: stop and report**, with both medians and each chapter's word count. The design's remedy is #3787's
    direct-mapped run cache "as a class C block of the build". Where it lives beside the image-header walk, which
    needs the arena, is
    the maintainer's call, so it is not planned here.
- [ ] **Step 4: Item 6 on the X3**, with the gate passed.
  - **Background-B's lowest heap:**
    - open `test_shaped_long_hi.epub@1:1` from a cold cache, and wait for `EVT:BUILD_DONE spine=2` (B's look-ahead);
    - the lowest `Free` of the `Reader mem[...]` and `Watermark DROP` lines during B's build is the floor;
    - **pass line:** at least 10,240 B on the X3. Missed, stop and report: the design reopens.
  - **The reading heap after a page turn:** `CMD:STATE`'s `heap` and `maxblock` after five page turns, on the Hindi
    chapter and on the English one.
  - **The loop task's stack:**
    - build `default` again with `PLATFORMIO_BUILD_FLAGS=-DENABLE_BOOT_HEAP_DIAGNOSTICS`, for the branch and for the
      base;
    - after a cold build of the Hindi chapter, read `loopTask stack high-water=` from the `[MEM]` lines, against
      16,384 B;
    - under 1,536 B free, stop and report.
  - The build summary's `arena: cap= highWater=` and `failedAlloc=` lines, and no `shaping degraded` line in the
    log.
- [ ] **Step 5: After, on the X3:** the same pages as Step 2, at the same settings.
  - Expected: conjuncts are formed; vowel signs and the reph sit in place; no dotted circle appears; no visible virama
    appears where the font has a conjunct.
  - A page that differs from that is a finding: report it with its screenshot.
- [ ] **Step 6: Ring mode** (Review Focus 4):
  - on the Hindi page, `CMD:TTF:FULL`, then turn forward three pages and back three;
  - each page's screenshot equals the one taken on the way before `FULL`;
  - `STATE heap` is within 2 KB of the English book's page after the same turns.
- [ ] **Step 7: The dictionary's word selection** (Review Focus 2): open it on the Hindi page, move to a shaped word,
  and take a screenshot. The highlight draws the word shaped.
- [ ] **Step 8: The X4** (COM5), when attached:
  - upload the two merged families from `C:/tmp/wm2-measure/merged/` to `/.crosspoint/fonts/<Family>/`, and the books;
  - Steps 2, 4 (no B-heap pass line: that is the X3's) and 5;
  - the in-place gate: opening the Hindi book logs `heapAllowsInPlaceBuild=0 ... shapedTtf=1`;
  - PR 2's owed figures: `CMD:TTF:SHAPE:hi:हिन्दी` and `CMD:TTF:SHAPE:hi:प्रार्थना`, three times each;
  - without the X4, record "not measured", as PR 2 did.
- [ ] **Step 9: The images.**
  - For each page, compose master's screenshot and the branch's side by side with a scratchpad Pillow script:
    - 1:1 pixels, a 24 px white gutter;
    - a 32 px caption band above each: `master 618312bb4` (or the base's short SHA) and `PR 3 <short SHA>`, in black on
      white;
    - a 1 px `#888` frame around each.
  - Name them `x3-hindi-14pt.png`, `x3-hindi-18pt.png`, `x3-bengali-14pt.png` and `x3-dictionary.png`, and `x4-*.png`
    for the X4.
  - Publish them on the orphan branch `assets/pr-shaped-indic`:
    - `git worktree add --detach C:/tmp/wt-assets-wm3`, `git checkout --orphan assets/pr-shaped-indic`, `git rm -rf .`;
    - copy the images to `shaped/`, and commit `assets: screenshots for the writing modes step 2 PR 3 description`;
    - `git push -u origin assets/pr-shaped-indic`, then remove the worktree.
  - The PR links them as
    `https://raw.githubusercontent.com/jpirnay/witchhunt-reader/assets/pr-shaped-indic/shaped/<file>`.
- [ ] **Step 10: Restore.** Both devices get back the settings read in Step 2. The X3 runs master again. The families
  and test books stay. Nothing removes `/web-*.bin`.
- [ ] **Step 11: Flash and RAM.** Build `default` and `gh_release` from the branch, and record each delta against
  Step 1.
  - The gates (Global Constraints): `gh_release`'s delta at most 81,772 B, and `default` at most 6,297,600 B. Missed,
    stop and report.
  - Split `gh_release`'s delta from the map file: the engine (`lib/OtShaper` objects) against the rest.
- [ ] **Step 12: Write the section**, then remove the scratch worktree.
  - **Tables:**
    - item 5's host figures (Task 12's commit body) and the shares past 32 and 48 (Task 6's);
    - item 6 per board: the stack, B's heap floor, the reading heap, and the build times (min/median/max of three);
    - flash and RAM.
  - **The gate table:** item 6's two pass lines, and item 2's running total for PRs 1-3.
  - **Estimate to measured:** the design's rows "Glyph buffer per call", "A plan", "Paragraph glyph store" and
    "Resident page".
  - Commit `docs(writing-modes): step 2 PR 3's figures on the X3 and the X4`.

### Task 14: Docs, credits, the whole suite and the PR

**Files:**
- Create: `docs/text-shaping.md` (reference)
- Modify: `USER_GUIDE.md` ("Supported Languages", `:808-815`; "Current Limitations", `:1109`)
- Modify: `docs/future_work/writing-modes.md` (row 2, `:24`, and "Next step.")
- Modify: `README.md` (the OtShaper entry, `:176`)
- Modify: `src/main.cpp:1813` (the loop task's stack comment: 16 KB, not 8 KB, per the design's "Stack")

- [ ] **Step 1: The reference page** `docs/text-shaping.md`. It describes the code as it is, by symbol names:
  - the layers: the parser's joiners, the shaping layer's modules, the face and its plans, the store, the pieces;
  - the glyph forms in the TextBlock, and the draw;
  - the lifetimes: a call's block, the build's heap block, the faces and plans until the build's end;
  - what fails how: Degraded and its rebuild; fixed limits;
  - what is not shaped yet: bidi paragraphs (PR 4), titles and the TOC (step 5), bitmap fonts (reordered only);
  - the code-point prefetch still covering shaped words' code points: about one extra stored glyph per distinct code
    point, once per glyph store.
- [ ] **Step 2: The user guide.**
  - **Supported Languages:** Hindi, Marathi, Nepali, Sanskrit, Bengali, Assamese, Punjabi, Gujarati, Odia, Tamil,
    Telugu, Kannada, Malayalam and Sinhala are shaped with a TrueType family that maps their script. Until the Font
    Manager offers one (the next PR), put the family's files in `/.crosspoint/fonts/<Family>/`. In a bitmap or
    built-in font, their vowel signs are placed but conjuncts are not formed.
  - **Limitations:** book titles, chapter names and the TOC are not shaped yet. Indic words inside a right-to-left
    paragraph are not shaped yet either.
- [ ] **Step 3: The roadmap and credits.**
  - Row 2's state: `in progress: PR 1 #603, PR 2 #624 merged; PR 3 #<n> (Indic books: shaped text laid out and drawn);
    PR 3b (the Font Manager's Indic families); PR 4 (Arabic joining)`, with this plan linked. "Next step." follows.
  - The README entry adds:
    - the glue's port of #3787's `scanRun`, piecewise loop and glyph rounding (`lib/TextShaping/IndicShaper.cpp`);
    - the Hyphenator, focus-reading and drop-cap rules;
    - the book subsets of Noto Sans (OFL, `test/fixtures/fonts/README.md`).
- [ ] **Step 4: The whole host suite** on the final tree, with `origin/master` merged in again if it moved. Expected:
  only the five known Windows-only failures, and every golden byte-identical but the five new ones.
- [ ] **Step 5: cppcheck.** `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high`,
  in the background.
  - A finding in our code is fixed.
  - A finding in an upstream file gets an inline `// cppcheck-suppress <id>` with its reason, marked
    `// witchhunt: build glue`, and listed in the commit body.
- [ ] **Step 6: Format and NUL check** on every touched file, then commit `docs(writing-modes): step 2 PR 3, shaped
  Indic text on the page`.
- [ ] **Step 7: The PR.**
  - `gh pr view --json state` on the branch: expect no PR.
  - Push, and open a PR against master titled `feat(shaping): Indic text shaped on the page (writing modes step 2,
    PR 3)`.
  - **The body:**
    - **Summary:** the design, this plan and the measurements.
    - **What a reader sees:** Task 13's images inline, each with a caption.
      - The left side is master: unshaped, viramas visible, conjuncts apart, i-matra after its consonant.
      - The right side is this branch, on the same device, page, font and size.
      - Say that the assets branch can be deleted after the merge.
    - **Deviations 6 (without bidi), 7, 8 and 9,** and where each lives.
    - **Rulings to check:**
      - the scope split (PR 3b);
      - plans in the face;
      - no shaping in bidi paragraphs until PR 4;
      - vertical sections reorder;
      - the per-call cap and its data;
      - glyph forms sharing bit 0's offsets;
      - the bounded `IndicReorder` overload;
      - the in-place gate's language list;
      - the doubled code-point prefetch.
    - **The review focus** above.
    - **Device figures and sizes:** Task 13's tables.
    - **Tests:** the suite's count; the new suites; the five goldens.
  - Fill row 2's `#<n>` in a commit `docs(writing-modes): step 2 PR 3 is #<n>`, and push. Merging is the maintainer's.
