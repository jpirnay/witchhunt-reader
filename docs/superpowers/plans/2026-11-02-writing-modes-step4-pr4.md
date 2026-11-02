# Writing modes step 4, PR 4: the dictionary on vertical pages — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a vertical page, the reader can open the dictionary's word screen, move the selection down the
columns and across them, see the highlight on the right words, and look a word up, as on a horizontal page.

**Architecture:**
- **Geometry stays in the turned frame.** The word screen keeps its word boxes in the turned (content) frame, where a
  vertical page is an ordinary horizontal page: a row is a column, and `x` is the position down it. Its rows,
  navigation, CJK run and span logic then work unchanged.
- **The mapping happens at the edges:**
  - the boxes are built from content-frame offsets (`turnedframe::contentOffsets`, as `publishPageLinkTargets` does);
  - a tap is mapped into the content frame;
  - the highlight saves, fills and restores rectangles mapped to the reader's frame (`turnedframe::toReader`);
  - its text is redrawn white in the turned frame (`PageFrameScope`), by `drawTextVertical` or `drawCombinedVertical`;
  - the buttons are remapped to the column directions.
- The reader's guards against opening the dictionary on a vertical page go.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3); host tests with GoogleTest/CMake for the pure pieces; the serial
test API on the X4 and the X3 for the word screen, which is not host-linked.

**Spec:** `docs/superpowers/specs/2026-10-30-writing-modes-step4-design.md`, "PR 4: the dictionary on vertical pages",
and Decision 3.

## Global Constraints

- **Horizontal pages are untouched.** Nothing changes on a horizontal page: the word screen's horizontal behaviour,
  its button mapping and its highlight.
- **No resident RAM.** `Fragment` stays 16 bytes on the firmware (its `static_assert`). The new per-word flags live
  in the interned `DrawStyle`. No new allocation in the highlight path.
- **No format change and no section version bump.**
- **Navigation on a vertical page:**
  - Up and Down step through the words in reading order, down the column and on into the next one
    (`selected - 1`, `selected + 1`).
  - Left goes to the next column, which lies to the left, at the word nearest the same position down the column
    (`moveVertical(+1)`). Right goes to the previous column (`moveVertical(-1)`).
  - The first selection is the middle column's word nearest the page's vertical centre.
- **The highlight on a vertical page:**
  - every word is redrawn as the page draws it: placed marks (`centredMarks`) and combined words
    (`drawCombinedVertical`), in white;
  - the snapshot's two-run, 2,048-byte budget is unchanged; a span over more columns repaints in full, as over
    more rows today.
- Comments and commits follow the repo's style. No Co-Authored-By trailer; never skip hooks.

## Review Focus

1. **A selection that crosses from one column to the next** (Down at a column's last word): the highlight moves to
   the next column's first word, and the old box is restored, not left inverted (Task 3, on the device).
2. **A CJK span over two columns:** both column boxes are lit and restored; over three it repaints in full (Task 3).
3. **A combined number or a placed 。 under the highlight** is redrawn white in its place, not sideways or unplaced
   (Task 3 screenshots).
4. **A tap on a word of a vertical page** selects that word, not the one at the transposed point (Task 2's
   `toContentPoint` test; Task 3 on a touch board if one is attached, else noted).
5. **A horizontal book's word screen** is unchanged (Task 3, one horizontal check on the device).

---

### Task 1: the building blocks (colour, a point into the turned frame, the frame scope in a header)

**Files:**
- Modify: `lib/Epub/Epub/VerticalDraw.h/.cpp`: `drawTextVertical` and `drawCombinedVertical` gain a last parameter
  `bool black = true`, threaded through `drawIn` and its four call sites.
- Modify: `lib/Epub/Epub/TurnedFrame.h`:
  `constexpr Point toContentPoint(int readerWidth, int readerX, int readerY)` with `struct Point { int x; int y; }`,
  the inverse of `toReader` for a point: `{readerY, readerWidth - 1 - readerX}`.
- Create: `lib/Epub/Epub/PageFrameScope.h`. Move `Page.cpp`'s anonymous-namespace `FrameScope` here unchanged in
  behaviour, as `class PageFrameScope`. `Page.cpp` includes it and uses it at its four sites.
- Modify: `test/word_size_layout/GfxRenderer.h`, if its `DrawCall` does not record `black`: add the field.
- Test: `test/vertical_text/VerticalDrawTest.cpp`, the TurnedFrame test (find where `toReader` is tested, e.g.
  `test/touch_transform/TurnedFrameTest.cpp`).

- [ ] **Step 1: Write the failing tests.**
  - `VerticalDraw.AWhiteColumnDrawsWhite`: `drawTextVertical(..., "日a", ..., false, /*black=*/false)`. Every recorded
    call has `black == false`, with the same positions as the black draw.
  - `VerticalDraw.AWhiteCombinedRunDrawsWhite`: the same for `drawCombinedVertical`.
  - `TurnedFrame.AContentPointMapsBackToTheReadersPoint`: for `readerWidth` 480 and several content points `(x, y)`,
    `toContentPoint(480, toReader(480, {x, y, 1, 1}).x, toReader(...).y) == {x, y}`.
- [ ] **Step 2: Run them, and see them fail to compile.**
- [ ] **Step 3: Implement** as listed. `Page.cpp` keeps its behaviour: every golden is byte-identical.
- [ ] **Step 4: Run.**
  `cmake --build build/test -j 6 -- -k 0 && ctest --test-dir build/test -R "VerticalDraw|TurnedFrame|Vertical|EpubPipeline" -j 8`.
  All pass; no golden changes.
- [ ] **Step 5: Commit.** `refactor(epub): white vertical drawing, a reader point into the turned frame, and the page frame scope in a header`

### Task 2: the word screen works on a vertical page

**Files:**
- Modify: `src/activities/reader/DictionaryWordSelectActivity.h/.cpp`.
- Modify: `src/activities/reader/EpubReaderActivity.cpp`:
  - remove the vertical guard in `openDictionary` (~:2653-2657);
  - set the menu's `wordLookupAvailable` (~:7574) back to its horizontal meaning.
- Test: none on the host; the activity is not host-linked. The firmware build and Task 3's device run are the test.
  Keep each geometric decision in a small free function in the .cpp's anonymous namespace, so it reads clearly.

**Interfaces:**
- Consumes: Task 1's `black` parameter, `toContentPoint`, `PageFrameScope`; `TextBlock::isVertical()`,
  `centredMarks()`, `isCombined(i)`, `wordBox`; `turnedframe::contentOffsets`, `toReader`.

- [ ] **Step 1: Implement.**
  - **The page's frame.** In the constructor or `extractWords`, record `vertical_ = page->isVertical()`. For a
    vertical page, compute `origin_` with `turnedframe::contentOffsets(renderer.getScreenWidth(held), marginLeft,
    marginTop, page->verticalBlockExtent)`, `held` being `renderer.getHeldOrientation()`. A horizontal page's origin
    is `{marginLeft, marginTop}`.
  - **`extractWords`:**
    - call `wordBox(renderer, i, fontId, line->xPos + origin_.x, line->yPos + origin_.y)`. Fragments then hold
      content-frame boxes; a horizontal page is unchanged;
    - for a dash-split piece on a vertical line, measure with `verticalTextAdvance` in place of `getTextWidth` and
      `getTextAdvanceX`.
  - **`DrawStyle`** gains `uint8_t flags`: bit 0 vertical line, bit 1 the line's `centredMarks()`, bit 2 the word
    `isCombined(i)`. It takes part in the interning comparison. A fragment gets the flags of its word and line.
  - **The first selection** on a vertical page: `closestInRow(rowCount / 2, renderer.getScreenHeight(held) / 2)`. A
    content `x` is a reader `y`.
  - **Buttons on a vertical page** (`loop`): Up `selected - 1`, Down `selected + 1`, Left `moveVertical(+1)`, Right
    `moveVertical(-1)`, with the same bounds and the same speculation and redraw calls the horizontal branches make.
  - **Touch:** `consumeWordTouch` maps a reader point with `toContentPoint(renderer.getScreenWidth(held), x, y)`
    before `wordAt` on a vertical page.
  - **The highlight** (`drawHighlightWithSnapshot`, `forEachRow`):
    - The row boxes are unions in the content frame, as now. On a vertical page, map each with `toReader` before the
      2 px growth, the clamp, `readFramebufferRegion`, `fillRect` or `drawRect`, and the saved run.
    - Redraw the text inside one `PageFrameScope` (constructed from copies of the margins; it switches the renderer
      to the turned frame):
      - `drawCombinedVertical` for a fragment with flag bit 2;
      - otherwise `drawTextVertical(..., centredMarks = bit 1, black = knownMissing)`;
      - at the fragment's content `x` and `y`, with its scale.
    - Restoring with `writeFramebufferRegion` in the differential path uses the saved reader-frame runs, unchanged.
  - **`prewarmHighlight`:** a vertical fragment's glyphs are prewarmed with the same scan the page uses. Call the
    vertical drawers inside a scan scope, or record the em glyph as `drawTextVertical`'s scan branch does. Then a
    TTF or SD font draws without a miss.
  - **`drawHints`:** keep the labels unless one names a direction that is now wrong. Report what you checked.
  - **The reader:** remove the guards. Both the long-tap gesture (`BTN_DICTIONARY`) and the menu row then open the
    word screen on a vertical page.
- [ ] **Step 2: Build.**
  - The host suite: `cmake --build build/test -j 6 -- -k 0`, then
    `ctest --test-dir build/test -j 8 -E HeapPeakRegression`. Only the known Windows failures.
  - clang-format 21.
  - `pio run -e default` after checking that no other pio build runs. Record Flash and RAM.
- [ ] **Step 3: Commit.** `feat(reader): the dictionary's word screen on a vertical page`

### Task 3: devices, docs

- [ ] **Step 1: The X4** (COM5) and **the X3** (COM6).
  - **Before flashing:** ListAgents and message the peers. The X3 rules: one esptool flash, no RTS, no
    `CMD:REBOOT`, no `CMD:SLEEP`; if it is silent, stop.
  - **Setup:**
    - Flash this branch's `default` image, then back up with GET.
    - NotoSansJP 14 pt, portrait, Vertical Text Automatic.
    - The device needs a Japanese dictionary (JMdict, as in #574). If the card has none, report it and run the
      geometry checks with the dictionary's "not found" result, which still shows the word screen.
  - **The acceptance book, spine 9 page 1 (vertical):**
    - Open the word screen (the reader menu's Dictionary row, or `cmd:BTN` on the dictionary button). Read STATE:
      `sel`, `label`, `span`, `match`.
    - Screenshot the first selection: the middle column, near the vertical centre.
    - Down ×3 and Up ×1 move through the words in reading order. `label` follows the column, and a screenshot shows
      the box on the word `label` names.
    - Left and Right move to the next and previous columns, at about the same height.
    - Down at a column's last word moves to the next column's first word, with no inverted box left behind (Review
      Focus 1).
    - A CJK span over two columns, if one occurs on the page: both boxes lit and restored (Review Focus 2).
    - Confirm looks the word up and shows its definition; Back returns to the page, unchanged.
  - **A combined number and a placed 。 under the highlight:** spine 22 page 1's chapter number, and a 。. Screenshots
    show each redrawn white in place (Review Focus 3).
  - **A horizontal book:** the word screen opens and moves as before, with one screenshot (Review Focus 5).
  - **A touch board, if attached:** a tap on a word selects it (Review Focus 4). If none is attached, say so.
  - **Restore every setting** on both devices, and leave both on this build.
  - **The X4's owed PR 3 numbers:** if the X4 is back on USB, also run them now and report them separately. They are
    the page counts against #595 (Automatic 12/76/62/136, Off 8/69/61/129) and the v106 rebuild; the scripts are in
    the session scratchpad `wm4-pr3-dev`.
- [ ] **Step 2: Docs.**
  - `docs/future_work/writing-modes.md`:
    - row 4: PR 3 #597 shipped, PR 4 `#<this PR>`, and step 4 done once this merges;
    - "Next step." names step 1 (bidi and Hebrew);
    - the inherited-limits list drops the dictionary.
  - `USER_GUIDE.md`: the Vertical Text limits drop "no dictionary"; the dictionary section describes the vertical
    keys.
  - Commit `docs(writing-modes): step 4 PR 4`.
