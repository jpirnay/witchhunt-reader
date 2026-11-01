# Writing modes step 4, PR 2: 、。 and small kana placed in the column — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a vertical page, 、。，． sit in the top-right corner of their cell, small kana are nudged up and right,
Traditional Chinese marks are centred, and a mark that A3b narrowed keeps its ink inside its box.

**Architecture:** One decision per code point (`uprightPlacementOf`) beside `drawsUpright`. The renderer's ink
query grows a vertical half (`GlyphInk`). `drawTextVertical` moves an upright placed piece by offsets computed from
its ink box and the cell. `columnInk` reports where that ink lands along the column, and ParsedText's `inkShift`
uses it for a vertical line. A line carries a "centred marks" bit (flags bit 3) set from the book's punctuation
profile. Section version 106.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3); host tests with GoogleTest/CMake (`test/`); the Python fixture
generator; the serial test API on the X4.

**Spec:** `docs/superpowers/specs/2026-10-30-writing-modes-step4-design.md`, "PR 2: placed marks and A3a along the
column", and Decision 1 with its measurements.

## Global Constraints

- Nothing changes on a horizontal page. Every horizontal golden stays byte-identical, and the horizontal `inkShift`
  keeps its results.
- The draw path allocates nothing; `VerticalDrawTest.DrawingAllocatesNothing` keeps passing. There is no new
  resident RAM: `sizeof(TextBlock)` must not grow (pack the new bit beside `vertical_`).
- The advance of a placed mark stays one em, so no measurement changes.
- A font without the glyph (`hasGlyph` false) draws the piece as step 3 does, unplaced.
- Section version 106 (PR 1 #594 is 105). Bump it once, in Task 2.
- Comments and commits follow the repo's style: plain sentences, conventional prefixes, no Co-Authored-By trailer,
  never skip hooks.

## Review Focus

1. **A 。 narrowed at a column's end, or before 」.** Its ink ends before the next character's pen and before the
   column's end (Task 3's rule-checker check, over every vertical fixture book).
2. **A mark in a font that lacks it.** It is not moved (Task 2, a test with `hasGlyph` false).
3. **A scaled run** (`drawTextScaled`, scale ≠ 1). The offsets scale with the em (Task 2, a scale-2 test).
4. **A cached v105 section.** It is rebuilt, not misread, and an old line without bit 3 reads as not centred
   (Task 2's round-trip test; Task 4 on the X4).
5. **Traditional Chinese in a Japanese font.** The marks are centred, not reflected (Task 2's centred test; Task 3's
   zh-Hant book).

---

### Task 1: the building blocks — placement classes and the ink box

**Files:**
- Modify: `lib/Epub/Epub/VerticalOrientation.h/.cpp`
- Modify: `lib/GfxRenderer/GfxRenderer.h/.cpp` (`getGlyphInk`)
- Modify: `test/word_size_layout/GfxRenderer.h` and `test/epub_pipeline/GfxRenderer.h` (their `getGlyphInk`)
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`inkShift`, the one caller)
- Test: `test/vertical_text/VerticalOrientationTest.cpp`

**Interfaces:**
- Produces: `enum class UprightPlacement : uint8_t { None, Reflect, SmallKana };` and
  `UprightPlacement uprightPlacementOf(uint32_t cp);` in `VerticalOrientation.h`.
  - Reflect: U+3001, U+3002, U+FF0C, U+FF0E.
  - SmallKana: U+3041 3043 3045 3047 3049 3063 3083 3085 3087 308E 3095 3096, U+30A1 30A3 30A5 30A7 30A9 30C3 30E3
    30E5 30E7 30EE 30F5 30F6, and U+31F0-31FF.
  - Everything else: None.
- Produces: `struct GlyphInk { int left; int width; int top; int height; int advance; };` in `GfxRenderer.h`, and
  `bool getGlyphInk(int fontId, uint32_t cp, EpdFontFamily::Style style, GlyphInk& ink) const;`. It replaces the
  three-out-parameter form. `top` is `glyph.top` (baseline to the ink's top, up positive) and `height` is
  `glyph.height`, both scaled like `left` and `width`.

- [ ] **Step 1: Write the failing tests.**
  - `VerticalOrientationTest.PlacementClasses`: every Reflect and SmallKana code point above, plus None for 一 (U+4E00),
    あ (U+3042), 「 (U+300C), ー (U+30FC), ！ (U+FF01), and `a`.
- [ ] **Step 2: Run them, and see them fail to compile.**
- [ ] **Step 3: Implement `uprightPlacementOf`.** A switch, or a small sorted table.
- [ ] **Step 4: Change `getGlyphInk` to the `GlyphInk` form,** in the renderer and both shims, and update `inkShift` to
  read `ink.left`, `ink.width` and `ink.advance`. The behaviour is the same.
  - In the shims, `top` and `height`, with `adv` as today and `b = (adv * 88 + 50) / 100`:
    - Comma and FullStop: `top = height = (adv * 3 + 5) / 10` (the ink sits on the baseline, in the lower part of the
      cell, as a real 、。 does);
    - everything else: `top = b`, `height = adv` (the whole em box).
  - Keep each shim's existing `left` and `width`.
- [ ] **Step 5: Run.** `cmake --build build/test -j 6 -- -k 0`, then
  `ctest --test-dir build/test -R "VerticalOrientation|VerticalDraw|VerticalTextBlock|Cjk|EpubPipeline|Vertical" -j 8`.
  Expected: all pass, and no golden changes.
- [ ] **Step 6: Commit.** `feat(epub): placement classes for upright marks; the ink query reports the ink's height`

### Task 2: the draw places marks; lines carry "centred marks"

**Files:**
- Modify: `lib/Epub/Epub/VerticalDraw.h/.cpp`
- Modify: `lib/Epub/Epub/blocks/TextBlock.h/.cpp` (the bit, serialization, `render`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (where lines get `setVertical`: ~:4703 and the table
  cell lines ~:5524)
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION` 106)
- Test: `test/vertical_text/VerticalDrawTest.cpp`, `test/vertical_text/VerticalTextBlockTest.cpp`

**Interfaces:**
- Consumes: Task 1's `uprightPlacementOf` and `GlyphInk`.
- Produces:
  - `void drawTextVertical(const GfxRenderer&, int fontId, int x, int y, const char* text, EpdFontFamily::Style,
    float scale, bool centredMarks);`. The new last parameter has no default; update every caller.
  - `bool columnInk(const GfxRenderer&, int fontId, uint32_t cp, EpdFontFamily::Style, bool centredMarks,
    int& inkStart, int& inkSize, int& cell);` in `VerticalDraw.h`. For an upright code point it gives where its ink
    lands along the column as drawn, from the cell's leading edge (the pen). `cell` is the em. It returns false for a
    sideways code point or when there is no glyph.
  - `TextBlock::setCentredMarks(bool)` and `centredMarks() const`.

- [ ] **Step 1: Write the failing tests** in `VerticalDrawTest`. The shim has em 10, ascender 16, `b = 9`, and an
  upright pen at x 412 and y `pen - 7`, as in the file's first test.
  - `AFullStopIsReflectedIntoTheTopRightCorner`: `drawTextVertical(r, 1, 100, 50, "。", REGULAR, 1.0f, false)`.
    The ink is `left 0, width 5, top 3, height 3`, so `dx = 2 * (10 / 2) - 2 * 0 - 5 = 5` and
    `dy = 10 - 2 * 9 + 2 * 3 - 3 = -5`. Expect one Portrait call at (417, 88).
  - `ASmallKanaMovesUpAndRight`: "っ" at pen 100, offsets `(10 * 10 + 50) / 100 = 1`. Expect (413, 92).
  - `ACentredMarkIsCentred`: "。" with `centredMarks = true`. The offsets are the reflection's halved,
    `(5 / 2, -5 / 2) = (2, -2)`. Expect (414, 91).
  - `AnUnplacedCharacterDoesNotMove`: "日" and "！" stay at (412, 93).
  - `AMarkTheFontLacksIsNotMoved`: use the shim's no-glyph font (the one whose `hasGlyph` is false). Expect the
    unplaced position.
  - `PlacementScalesWithTheEm`: "。" at scale 2.0. Derive em 20, the scaled ink, and the offsets in the comment.
  - `ColumnInkIsWhereTheDrawPutsIt`: `columnInk` for "。" gives `start = (b - top) + dy = 9 - 3 - 5 = 1`, `size 3`,
    `cell 10`. With `centredMarks`, the start is `9 - 3 - 2 = 4`. For "日": `start 0, size 10`. For "a" (sideways)
    it returns false.
- [ ] **Step 2: Write the failing serialization tests** in `VerticalTextBlockTest`:
  - a vertical line with `setCentredMarks(true)` round-trips with the bit;
  - one without it reads back false;
  - a line serialized by the current code (bit 3 clear) still reads.
  - Also `static_assert`, or EXPECT, that `sizeof(TextBlock)` is unchanged from master: measure it first, and write
    the figure into the test's comment.
- [ ] **Step 3: Run, and see the failures.**
- [ ] **Step 4: Implement.**
  - **Upright pieces** in `drawTextVertical`. When `uprightPlacementOf` of the piece's first code point is not None,
    `hasGlyph(fontId, cp, style)` is true (a lookalike or U+FFFD is never placed) and `getGlyphInk` succeeds:
    - Reflect: `dx = 2 * (adv / 2) - 2 * ink.left - ink.width` and `dy = em - 2 * b + 2 * ink.top - ink.height`, where
      `b = (em * 88 + 50) / 100` (the draw's own baseline offset). With `centredMarks`, use `dx / 2` and `dy / 2`.
    - SmallKana: `dx = (em * 10 + 50) / 100` and `dy = -dx`, whatever `centredMarks` says.
    - Scaled ink: scale `left`, `width`, `top` and `height` by `scale` with the file's `scaled()`.
    - Add `dx` to the reader-frame x and `dy` to the reader-frame y.
    - The scan branch needs nothing new: it draws the same glyphs, and `getGlyphInk` records nothing. Keep a single
      ink lookup per placed piece.
  - **`columnInk`:** the same arithmetic, returning `start = (b - ink.top) + dy` and `size = ink.height`.
    - For an upright code point with placement None, `dy = 0`.
    - `cell` is the em.
    - It returns false when the code point is drawn sideways or there is no glyph.
  - **TextBlock:**
    - Make `vertical_` and a new `centredMarks_` one-bit fields of the byte `vertical_` uses today.
    - Serialize the new bit as flags bit 3, `0x08`, and drop it from the rejected-bits check.
    - `render`'s `drawRun` passes `centredMarks_`.
  - **The parser**, beside each `setVertical(vertical_)` on a line (TextBlock) it creates:
    `setCentredMarks(vertical_ && typography_.punctuation == TypographyProfile::Punctuation::TraditionalChinese)`.
  - **`SECTION_FILE_VERSION` = 106,** with the comment
    `// v106: a vertical line's flags bit 3 says its marks are centred (Traditional Chinese); placed marks shift ink`.
- [ ] **Step 5: Run.**
  `ctest --test-dir build/test -R "VerticalDraw|VerticalTextBlock|VerticalOrientation|EpubPipeline|Vertical" -j 8`.
  Expected:
  - the new tests pass, and `DrawingAllocatesNothing` passes;
  - no golden changes (placement is draw-time, and the dump does not record draw positions);
  - a changed golden is a finding to report.
- [ ] **Step 6: Commit.** `feat(epub): 、。 and small kana placed in a vertical column; Traditional Chinese marks centred (section v106)`

### Task 3: A3a along the column, the rule checker, the fixtures

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`inkShift`)
- Modify: `test/epubs/make_test_vertical_text.py`; a new `test/epubs/test_vertical_text_zh_hant.epub`; and its
  goldens
- Modify: `test/epub_pipeline/VerticalRulesTest.cpp` (a check and the book list) and the goldens' book list in
  `EpubPipelineTest.cpp`, as `test_vertical_text` is registered
- Test: as listed

**Interfaces:**
- Consumes: Task 2's `columnInk` and the parser's `typography_.punctuation`.

- [ ] **Step 1: Write the tests.**
  - **The focused one, which must fail first:** `VerticalLayout.ACentredMarkPushedInShiftsByHalf`.
    - After Task 2's placement, a Japanese 、。 sits in the leading half of its cell, and the horizontal ink rule
      happens to give the same shift. The case where the rules differ is a Traditional Chinese mark: centred along
      the column, so it shifts by `removed / 2`, while the shim's horizontal ink (left half) says 0.
    - Take a push-in case from the A3b tests in `test/cjk_layout` (a line that overflows by a little, so a 。 is
      narrowed below its natural width).
    - Lay the same text out vertically in a `lang="zh-Hant"` document, with `VerticalText::On`.
    - Expect the 。's `x` to be its box start minus `removed / 2`.
    - Write the derivation in the test's comment.
    - If no push-in case narrows a 。 under the Traditional Chinese table (`CjkPunctuation.cpp` keeps 、。 at least
      6/8 em there), use the nearest mark it does narrow, and say which in the report.
  - **The regression net:** `VerticalRules.APlacedMarksInkStaysInItsColumnPlace` (TEST_P over the vertical books).
    It may already pass after Task 2; that is expected. For every vertical line, and every word `i` that is one code
    point with `CjkPunctuation::classOf != None` and `uprightPlacementOf != None`:
  - take `columnInk(...)` with the line's `centredMarks()`;
  - expect `wordXpos(i) + inkStart + inkSize <= wordXpos(i + 1)` when a next word exists, and `<=` the column length
    (the content frame's width) otherwise;
  - expect `wordXpos(i) + inkStart >=` the previous word's `wordXpos` when one exists.
  Remember the page lifetime rule: copy values out inside the callback (memory: pipeline-test-page-lifetime).
- [ ] **Step 2: Extend the fixture generator.**
  - In the vertical book: a paragraph of small kana (ぁぃぅぇぉっゃゅょゎ ァィゥェォッャュョ), and 、。 at a column's
    end and before 」』）, beside the existing のか。」. Keep it short, so the existing pages do not shift more than they
    must.
  - A second book, `test_vertical_text_zh_hant.epub`: `lang="zh-Hant"`, `writing-mode: vertical-rl`, about 30
    columns of text with 、。，and「」.
  - Regenerate both books with the script, and register the new one with the vertical goldens and the rule checker
    as `test_vertical_text` is.
- [ ] **Step 3: Run, and see `ACentredMarkPushedInShiftsByHalf` fail.** It gives a shift of 0 while `inkShift`
  still uses horizontal ink. The rule-checker check passes or fails as it may; report which.
- [ ] **Step 4: Implement.** In `inkShift`, for a line where `vertical_` is set and `columnInk` succeeds for the code
  point (with `centredMarks` = the profile is Traditional Chinese):
  - use `inkStart`, `inkSize` and `cell` in place of `inkLeft`, `inkWidth` and `advance`, with the same three-way
    rule;
  - a sideways code point (columnInk false) keeps the horizontal ink, which is right for a turned glyph.
- [ ] **Step 5: Run and regenerate.** `ctest --test-dir build/test -R "Vertical|EpubPipeline|Cjk" -j 8`.
  - Regenerate the vertical goldens: `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`.
  - Their diff may only move narrowed marks' x along the column and add the new books and text. Report each changed
    golden.
  - Every horizontal golden stays byte-identical.
- [ ] **Step 6: Commit.** `fix(epub): a narrowed mark in a vertical column keeps its ink inside its box (A3a along the column)`

### Task 4: suite, firmware, the X4, docs

- [ ] **Step 1:** Run the whole host suite (`cmake --build build/test -j 6 -- -k 0`, then
  `ctest --test-dir build/test -j 8 -E HeapPeakRegression`, then HeapPeakRegression alone with `--timeout 3000`).
  Only the known Windows failures may fail.
- [ ] **Step 2:** clang-format 21 is clean on every touched C++ file.
- [ ] **Step 3:** Firmware: build `gh_release`, then `default`, against PR 1's sizes (gh_release 6,029,665 / RAM
  57,272; default 6,142,243 / 57,776).
- [ ] **Step 4: The X4** (COM5; first ListAgents and message the peers, per memory shared-device-other-sessions).
  Flash with esptool. NotoSansJP 14 pt, portrait.
  - `test_vertical_text.epub` spine 1: screenshots. Check:
    - 、。 in the top-right corner of their cell;
    - small kana nudged;
    - のか。」 and である。） with no overlap;
    - a 。 at a column's end inside the column.
  - The acceptance book spine 9 page 1, and the pages step 3 noted for the 。」 overlap: screenshots.
  - Page counts of spines 4, 9, 22 and 39 under Automatic and Off, against PR 1 (Automatic 12/76/62/136, Off
    8/69/61/129).
  - v105 sections are rebuilt.
  - If no Traditional Chinese font is on the card, note that the zh-Hant book was checked on the host only.
  - Restore every setting from the backup.
- [ ] **Step 5: Docs.**
  - `docs/future_work/writing-modes.md`: row 4 becomes `in progress: PR 1 #594, PR 2 #<this PR>`; the "Punctuation
    stays rough" bullet is done for 、。 and small kana (the wave dashes stay rotated, design: mirrored form not
    built).
  - `docs/file-formats.md`: TextBlock flags bit 3.
  - Commit `docs(writing-modes): step 4 PR 2`.
