# Writing modes step 4, PR 3: tate-chu-yoko — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a vertical page, a short run the book's CSS marks `text-combine-upright` (the acceptance book's 56
two-digit chapter numbers, 10-37) is set upright as one block: one em down the column, its glyphs side by side
across it.

**Architecture:**
- **The flag:** the parser carries `CssStyle::textCombine` on the style stack, as it carries `text-emphasis`, and
  into each token's boundary (a free bit of `Boundary::annotation`).
- **The decision:** for a vertical line, ParsedText decides whether a flagged token combines. The token must be at most
  4 code points with nothing drawn upright, and for `digits N` all ASCII digits, at most N. It must also fit one em
  at no less than half size.
- **A combined token:** it measures one em, takes no CJK–Latin gap, and its line gets a `Combine` annotation record
  that reserves no ruby band.
- **The draw:** `TextBlock` draws a combined word with a new `drawCombinedVertical`: upright in the reader's frame,
  centred in its cell, shrunk to the em's width when wider.
- **Section version 107.**

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3); host tests with GoogleTest/CMake; the fixture generator in
Python; the serial test API on the X4, and on the X3.

**Spec:** `docs/superpowers/specs/2026-10-30-writing-modes-step4-design.md`, "PR 3: tate-chu-yoko" and Decision 2
(follow the book's CSS only; no setting, no automatic digits).

## Global Constraints

- **Vertical only.** On a horizontal page the flag changes nothing: every horizontal golden stays byte-identical,
  and a horizontal line gets no `Combine` record.
- **No resident RAM.** `sizeof(TextBlock)` must not grow; the 32-bit `static_assert` in `TextBlock.cpp` and the
  host pin in `VerticalTextBlockTest` stay. The draw path allocates nothing.
- **No band.** A `Combine` record reserves no annotation band: a column holding only combined words is as wide as
  one without them.
- **The decision rule:**
  - All: at most 4 code points, none drawn upright (`drawsUpright` false for each).
  - DigitsN: 1 to N ASCII digits (0-9) and nothing else.
  - Both: the run's horizontal width at most twice the em (`kMinCombineScale = 0.5`).
  - A token that fails is set as before.
- **Section version 107** (master is 106). Bump it once, in Task 2.
- Comments and commits follow the repo's style. No Co-Authored-By trailer; never skip hooks.

## Review Focus

1. **A combined run inside an internal link** (the acceptance book's `<a href="c10.xhtml#…" class="class_s9">10</a>`)
   is combined and keeps its link target. Task 1 covers the link path; Task 3 covers the target box.
2. **A flagged run that may not combine:** `digits 2` over "123", or `all` over 5 characters or over CJK. It is set
   exactly as without the flag (Task 1).
3. **A column with a combined word and ruby on another word.** The band is reserved once, by the ruby (Task 2).
4. **A bionic-reading or hyphenation split** never cuts a combined token, and a copied boundary keeps the bit
   (Task 1).
5. **A cached v106 section** is rebuilt, not misread (Task 2's round trip; Task 4 on the devices).

---

### Task 1: the flag reaches the tokens, and ParsedText decides

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h/.cpp`:
  - `StyleStackEntry` gets `hasTextCombine` and `textCombine`;
  - `effectiveTextCombine_`;
  - an `applyCssTextCombineToEntry` call beside every `applyCssTextEmphasisToEntry` call: inside
    `applyCssEmphasisToEntry` (~:708, which serves block entries and the generic span), the internal link (~:3088),
    b/strong (~:3402), underline (~:3436), italic (~:3470) and sup/sub (~:3489). `text-combine-upright` is inherited
    in CSS, so a block's value reaches its words;
  - the block entry's guard (~:727) and the generic span's guard (~:3511-3513) also admit `cssStyle.hasTextCombine()`;
  - the two `addWord` calls (~:1263, ~:1296) pass it.
- Modify: `lib/Epub/Epub/TokenBoundary.h`: bit 6 of `annotation` is "combine requested", with accessors beside
  `emphasisOf`/`withEmphasis`.
- Modify: `lib/Epub/Epub/ParsedText.h/.cpp`:
  - `addWord` gains `uint8_t textCombine = 0` (the `CssTextCombine` value);
  - the boundary copies keep the bit (the ruby follower ~:810-815, the hyphenation remainder ~:1731-1733, the
    cuts in `addWord` ~:383);
  - bionic (~:1363) skips a token with the bit;
  - the decision;
  - the measure (~:901);
  - the CJK–Latin gap;
  - the line's records (~:2014-2051).
- Test: `test/epub_pipeline/VerticalLayoutTest.cpp`; `test/epub_pipeline/PipelineRunner.cpp` (`dumpAnnotations` prints
  a `C first=… count=…` row).

**Interfaces:**
- Produces:
  - `TextBlock::AnnotationKind::Combine = 3`, emitted by ParsedText (Task 2 makes TextBlock accept it);
  - `bool ParsedText::combines(size_t wordIdx) const`, true only on a vertical line;
  - the constant `kMinCombineScale = 0.5f` in `ParsedText.h`.

- [ ] **Step 1: Write the failing tests** in `VerticalLayoutTest`, with CSS
  `.tcy { text-combine-upright: all } .d2 { text-combine-upright: digits 2 }`, under `VerticalText::On`:
  - `AFlaggedNumberIsCombinedAndTakesOneEm`: `<p>前<a class="tcy">13</a>後</p>` dumps a `C` row for "13", and 後's x is
    13's x plus one em (16 in the pipeline shim). Compare against the same text without the class, where "13" is
    a sideways run 2 × the shim's digit advance wide, with a CJK–Latin gap on each side.
  - `AnInternalLinkCombines`: `<p>第<a href="#n" class="tcy">10</a>章</p>` with `<p id="n">x</p>` later: the same
    `C` row.
  - `DigitsTwoCombinesOnlyTwoDigits`: `<p class="d2">第12章と第123章</p>` gives exactly one `C` row, for "12".
  - `AllOverLongOrUprightTextDoesNotCombine`: `<span class="tcy">abcde</span>` and `<span class="tcy">第1</span>`
    dump no `C` row and the same x positions as without the class.
  - `ACombinedWordReservesNoBand`: the line height (the next line's y) with `<a class="tcy">13</a>` equals the one
    without.
  - `HorizontalIgnoresTheFlag`: the same document under `VerticalText::Off` dumps byte-identical to the one
    without the class.
- [ ] **Step 2: Run them, and see them fail** (no `C` rows).
- [ ] **Step 3: Implement.**
  - **The parser:** as listed under Files.
  - **The token:** the bit is set on every token `addWord` makes from the word. A combined run is already one token,
    since the element boundary flushes the buffer and digits make no CJK cut. Do not change the gluing.
  - **The decision** (`combines`), made once per layout pass where the widths are computed. On a vertical line, a
    token with the bit and the rule of Global Constraints met.
  - **The measure:** a combined word's width is `columnEm(renderer, wordFontId(i, fontId), wordStyles[i],
    wordScale(i))`.
  - **The gap:** the CJK–Latin gap (A3b) is not inserted on either side of a combined word; it sets as one upright
    character. Find where A3b decides the gap, and treat a combined word as unspaced CJK there.
  - **The records:**
    - A combined word gets a record `{firstWord, 1, x, width = em, Combine, mark 0}`.
    - The records are sorted by `firstWord` whenever more than one kind is present.
    - The band flag passed to `beforeLine_` (~:2077) is true only when a Ruby or Emphasis record exists.
- [ ] **Step 4: Run.** `ctest --test-dir build/test -R "VerticalLayout|Cjk|EpubPipeline|Vertical" -j 8`.
  - The new tests pass.
  - No existing golden changes: no fixture uses the property yet.
- [ ] **Step 5: Commit.** `feat(epub): text-combine-upright reaches the words; a combined word measures one em in a column`

### Task 2: TextBlock keeps and draws a combined word

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h/.cpp`:
  - `AnnotationKind::Combine`;
  - `knownKind`, `annotationsValid`;
  - a band bit;
  - `annotationBand`, `hasAnnotations` users, render, `wordBox`.
- Modify: `lib/Epub/Epub/VerticalDraw.h/.cpp` (`drawCombinedVertical`).
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION` 107).
- Test: `test/vertical_text/VerticalDrawTest.cpp`, `test/vertical_text/VerticalTextBlockTest.cpp`.

**Interfaces:**
- Consumes: Task 1's `Combine` records.
- Produces:
  - `void drawCombinedVertical(const GfxRenderer&, int fontId, int x, int y, const char* text, EpdFontFamily::Style,
    float scale);` in `VerticalDraw.h`. `x` is the pen along the column and `y` the line's top, as for
    `drawTextVertical`.
  - `bool TextBlock::isCombined(uint16_t wordIdx) const`.

- [ ] **Step 1: Write the failing tests.** The shim has em 10, ascender 16, digits 10 px each, `b = 9`, and the axis
  at reader x 479 - 62 for a line at y 50, as in `VerticalDrawTest`'s first test.
  - `ACombinedPairIsCentredUpright`: `drawCombinedVertical(r, 1, 100, 50, "13", REGULAR, 1.0f)`.
    - Width 20 > em 10, so the scale is 10 / 20 = 0.5: one `drawTextScaled` call in the reader's frame (Portrait).
    - Across: the scaled width 10 is centred on the axis, `x = 479 - 62 - 10 / 2 = 412`.
    - Along: the scaled em box is centred in the cell, `baseline = pen + (em - emS) / 2 + (emS * 88 + 50) / 100`
      with `emS = 5`, so `100 + 2 + 4 = 106`, and the line top is `106 - ascenderScaled(0.5) = 106 - 8 = 98`.
    - Expect (412, 98) at scale 0.5. Re-derive these in the test comment from the shim's scaled ascender; a value
      that does not reproduce is a finding.
  - `ANarrowRunIsNotScaled`: "7" (10 px, equal to the em) draws one unscaled Portrait call at
    `x = 479 - 62 - 5 = 412`, `y = 100 + 9 - 16 = 93`.
  - `CombinedDrawingAllocatesNothing`.
  - In `VerticalTextBlockTest`:
    - `ACombinedWordDrawsAsOneUprightBlock`: a vertical line whose word 1 has a `Combine` record. Render records
      one `drawTextScaled` call for it, and `wordBox(1).width == em`.
    - `ACombineRecordReservesNoBand`: `annotationBand == 0` for a line with only a Combine record.
    - `ARubyLineStillReservesItsBand`: a Ruby record plus a Combine record keeps the band.
    - `TheCombineRecordSurvivesTheCache`: serialize, deserialize, `isCombined(1)`.
  - `sizeof(TextBlock)` is unchanged: the existing pin.
- [ ] **Step 2: Run them, and see them fail.**
- [ ] **Step 3: Implement.**
  - **TextBlock:**
    - Accept kind 3 in `knownKind` and `annotationsValid`.
    - Add a bit `annotationBand_ : 1` to the bitfield byte beside `vertical_` and `centredMarks_`. It is true when a
      Ruby or Emphasis record exists, set wherever the records are set (the constructor and deserialize).
    - `annotationBand()`, and the `baseY` choices in `render` and `wordBox` that use `hasAnnotations()`, read that
      bit instead.
    - `drawsUnscaled()` keeps `hasAnnotations()`.
    - `isCombined(i)` scans the records. In `render`, walk the records once in word order, not by calling
      `annotation(i)` per word, which is linear in `i`.
    - A combined word is drawn with `drawCombinedVertical`, and `runWidth` and `wordBox` give it the em.
  - **`drawCombinedVertical`:**
    - Measure the run with `getTextAdvanceX`, then scale it by `scale`.
    - If the width exceeds the em, multiply by `em / width`.
    - Draw it in the reader's frame (`setDrawOrientation(held)`), centred across the axis.
    - Along the column, centre the scaled em box in the cell `[pen, pen + em]`, by the formula in Step 1.
    - Restore the turned frame.
    - A scan pass draws it plainly, as `drawTextVertical`'s scan branch does.
  - **`SECTION_FILE_VERSION`** = 107, with the comment
    `// v107: a vertical line's Combine annotation record (tate-chu-yoko), which reserves no band`.
- [ ] **Step 4: Run.** `ctest --test-dir build/test -R "VerticalDraw|VerticalTextBlock|Vertical|EpubPipeline|Cjk" -j 8`.
  All pass; no golden changes.
- [ ] **Step 5: Commit.** `feat(epub): a combined word is drawn upright as one block in its cell (section v107)`

### Task 3: fixtures, goldens, the link target, the rule checker

**Files:**
- Modify: `test/epubs/make_test_vertical_text.py`, and regenerate `test_vertical_text.epub`.
- Goldens: the vertical ones.
- Test: `test/epub_pipeline/VerticalRulesTest.cpp`.

- [ ] **Step 1: The fixture.**
  - Append to the vertical book's spine 1 a heading `<h2><a href="#c13" class="tcy">13</a>　章</h2>` and a short
    paragraph with `<span class="tcy">12</span>`, plus `digits 2` text holding "123" (which must not combine).
  - Add the anchor target.
  - Regenerate with the script, and check that it reproduces byte for byte.
- [ ] **Step 2: Write the failing rule.** `VerticalRules.ACombinedWordIsOneUprightDrawInItsCell`, over every vertical
  book.
  - Each word with a `Combine` record draws as exactly one call in the reader's frame. Its box along the column lies
    within `[xpos, xpos + em]`.
  - `wordBox` of the word, mapped with `turnedframe::toReader`, contains that call's position.
  - Copy values out inside the `forEachPage` callback; no page pointer may outlive it.
- [ ] **Step 3: Run, regenerate the vertical goldens, and check the diff.**
  - Only the appended text and its `C` rows may change; report each changed golden.
  - Every horizontal golden stays byte-identical.
- [ ] **Step 4: Commit.** `test(epub): tate-chu-yoko in the vertical fixture, its goldens and a rule-checker rule`

### Task 4: suite, firmware, the X4 and the X3, docs

- [ ] **Step 1: Suite and format.**
  - The whole host suite: only the known Windows failures.
  - clang-format 21 clean on every touched C++ file.
  - No NUL byte in any touched text file.
- [ ] **Step 2: Firmware.** Build `gh_release`, then `default`, against master 8c559519b's sizes (build master's
  `default` in a base worktree, or take #595's: gh_release 6,030,795, default 6,143,297; RAM 57,272 / 57,776).
- [ ] **Step 3: The X4** (COM5).
  - Before flashing, use ListAgents and message the peers (memory shared-device-other-sessions).
  - Flash with esptool, then back up with GET.
  - NotoSansJP 14 pt, portrait.
  - The acceptance book:
    - spine 4 (the contents, 28 links) and the first pages of spines 9 and 22, each with an `<h2>` chapter number:
      screenshots showing the numbers upright and side by side, one em tall;
    - a tap or link check that a combined number still opens its target;
    - page counts of spines 4, 9, 22 and 39, Automatic and Off, against #595 (Automatic 12/76/62/136, Off
      8/69/61/129);
    - v106 sections rebuilt.
  - The fixture: screenshots of the new heading and paragraph.
  - Restore every setting.
- [ ] **Step 4: The X3** (COM6; the maintainer attached it and offered it on 2026-11-01).
  - Message the peers first.
  - Flash this branch's `default` image with esptool `write-flash 0x0`. Never pulse RTS, send `CMD:REBOOT` or
    `CMD:SLEEP`; if it goes silent on USB, stop and report.
  - Back up the settings with GET, then use NotoSansJP 14 pt, portrait, `sleepTimeoutMinutes` at the X3's own value or
    longer.
  - The acceptance book, spines 4, 9, 22 and 39 under Automatic:
    - pages;
    - build ms;
    - the lowest free heap during a cold build;
    - screenshots of spine 9 page 1 and spine 4, showing the placed 、。 and small kana of #595 and the combined
      numbers of this PR.
    - This is also the first X3 look at #594 and #595.
  - Restore every setting, and leave the X3 on this build.
- [ ] **Step 5: Docs.**
  - `docs/future_work/writing-modes.md`:
    - row 4: PR 1 #594 and PR 2 #595 shipped, PR 3 `#<this PR>`;
    - "Next step." names PR 4;
    - the inherited-limits list drops tate-chu-yoko; `text-orientation` stays.
  - `docs/file-formats.md`: annotation kind 3 (Combine).
  - `USER_GUIDE.md`: the Vertical Text limits.
  - Commit `docs(writing-modes): step 4 PR 3`.
