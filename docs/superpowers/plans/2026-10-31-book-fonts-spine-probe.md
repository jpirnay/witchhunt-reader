# Book fonts spine probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `BookFonts` finds a book's accent families from its whole spine, ranks them by their text (inline uses
included), gives negligible families no slot, and copies only the faces whose style carries text.

**Architecture:** All of it in `BookFonts` (probe, ranking, copy) plus one `Epub` read helper; no glyph store, engine,
layout or section format change. The probe samples the head of every spine item (64 at most), weights each item's
counts to estimate the whole book, and tallies text per (family, style); `examine()` ranks, prunes faces and copies.

**Tech Stack:** C++17 (ESP32-C3 firmware, PlatformIO), host gtest suite (CMake + Ninja, MSYS2 UCRT64 gcc).

**Spec:** `docs/design/book-fonts-probe.md` (read it first; section numbers below are its; section 8 records what was
built, which departs from the budget and constants below after the device runs).

## Global Constraints

- No change to `fonts.bin`'s layout, the glyph store format, or the section format. `BookFonts::kVersion` 7.
- Constants, in `BookFonts.h` beside today's, documented as today's are: `kProbeBudgetBytes = 512 * 1024`,
  `kProbeMaxItems = 64`, `kProbeMinItemBytes = 8 * 1024`, `kProbeMaxItemBytes = 64 * 1024`, `kBlockMinBytes = 64`,
  `kMinFamilyBytes = 256`, `kMinFamilyPerMille = 1` (0.1 %), `kMinStyleBytes = 256`, `kMinStylePercent = 1`.
  `kProbeChapters`, `kProbeChapterBytes`, `kProbeEnoughText` are removed.
- Heap: the probe object may grow by about 1 KB (it is on the heap only during `prepare`); nothing new at reading time.
- Counts are estimated visible text bytes (`VisibleText::visibleBytes`), `uint32_t`, saturating; weights are fixed point
  with 8 fractional bits (`weight256`, 256 = 1.0), products in 64 bits.
- Comments in `BookFonts.*` follow the file's existing prose style (full sentences, the reason, not the history).
- Log tag `"BKF"`, `LOG_INF` for the probe summary and per-family lines (spec 4.6).
- Worktree: `git worktree add C:/tmp/wt-probe -b feat/book-fonts-spine-probe master`, then in it
  `git submodule update --init --reference C:/_development/witchhunt-reader/freeink-sdk freeink-sdk`.
- Host build, **from Bash only** (PowerShell lacks the UCRT64 DLLs): `cmake -S test -B build/test -G Ninja
  -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1 -DGIT_EXECUTABLE="C:/Program
  Files/Git/cmd/git.exe"`, then `cmake --build build/test --target EpubPipelineTest -j 8`; run with
  `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter=...`.
- Full suite: `cmake --build build/test -j 8 -- -k 0 && (cd build/test && ctest -j 8)`. Known Windows-only failures
  (8): FirstChapter x2, LongChapterPageTables x2, KeptReadingState, DictionaryTest.LongFolder…, KOReaderProgressReplyTest
  (NOT_BUILT), HeapPeakRegression (passes alone). Anything else failing is ours.
- Format with clang-format **21.1.8** (CI's): `python -m pip install --target <scratch>/cf21 "clang-format>=21,<22"`,
  then `PATH="<scratch>/cf21/clang_format/data/bin:$PATH" bash bin/clang-format-fix`.
- Firmware: `export PLATFORMIO_CORE_DIR=C:\\pio`; `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default` (and
  `-e gh_release` before the PR; build `default` last), in the background (a cold build passes 600 s).
- Commits: conventional (`feat(fonts): …`, `test(fonts): …`); the PR title too (`feat: …`), or Title Check fails.

## Review Focus

1. A spine item that is not XHTML text (an image wrapper with binary bytes, a malformed document): read as far as the
   parser goes, counted as read, no failure, no family invented from it. Test in Task 1 (`ABinarySpineItemIsNoText`).
2. A very large book (thousands of items, multi-MB chapters): weights and sums must saturate, never wrap, so a body
   family is not lost to overflow. Test in Task 1 (`BookFontsWeights.*`).
3. A long spine read by name per item would walk the ZIP central directory 64 times (King's Avatar: minutes). Items
   are read through their spine stat. Test in Task 1 (`ALongSpineIsSampledWithinTheBudget` counts walks).
4. A hand-filled letter in a family the reader does not load: the word must stay one word (glued pieces, no gap), in
   the block's font. Test in Task 4 (`ALetterInAFamilyTheBookLacksStaysInItsWord`).
5. A family or style with faces but no text in the sample: the regular alone is copied (never every face, never none).
   Test in Task 3 (`keptStyles` cases with all-zero scores).

---

### Task 1: The probe reads the whole spine, weighted

Spec 4.1, 4.2, and the read-failure rule of 4.1. Ranking stays by blocks in this task.

**Files:**
- Modify: `lib/Epub/Epub.h`, `lib/Epub/Epub.cpp` (new `readSpineItemToStream`, beside `readItemContentsToStream`)
- Modify: `lib/Epub/Epub/BookFonts.h` (constants, `Found` stats, two helpers)
- Modify: `lib/Epub/Epub/BookFonts.cpp` (`BodyFontProbe::beginParser`, `count`, `stopAtLimit`, `detectFamilies`)
- Test: `test/epub_pipeline/BookFontsTest.cpp`

**Interfaces:**
- Produces:
  - `bool Epub::readSpineItemToStream(int spineIndex, Print& out, const bool* stop = nullptr, BuildArena* scratch = nullptr) const;`
    the item through `getSpineItemStat` and `ZipFile::EntryReader::open(const FileStatSlim&)` (as
    `readItemContentsToStreamWithArena` does, arena or heap), so no lookup by name when book.bin has the stat.
  - In `BookFonts`: `uint32_t addWeighted(uint32_t sum, uint32_t bytes, uint32_t weight256);` returns
    `sum + (bytes * weight256 + 128) / 256`, saturating at `UINT32_MAX`.
  - `uint32_t probeWeight256(size_t itemBytes, size_t readBytes, int spineCount, int sampledCount);` returns
    `256 * (itemBytes / readBytes, or 1 when itemBytes is 0) * spineCount / sampledCount`, saturating.
  - `Found::itemsRead` (`uint16_t`, items whose read succeeded) and `Found::bytesRead` (`uint32_t`, the parser's
    byte offset summed over items).
  - `size_t probeItemCap(int sampledCount)`: `clamp(kProbeBudgetBytes / sampledCount, kProbeMinItemBytes,
    kProbeMaxItemBytes)` (internal is fine).

- [ ] **Step 1: Write the failing tests** (in `BookFontsTest.cpp`; `kProbeCss` as the existing body tests use it)

```cpp
TEST(BookFontsWeights, AddWeightedRoundsAndSaturates) {
  EXPECT_EQ(200u, BookFonts::addWeighted(0, 100, 512));
  EXPECT_EQ(5u, BookFonts::addWeighted(0, 3, 384));                 // 4.5 rounds up
  EXPECT_EQ(UINT32_MAX, BookFonts::addWeighted(UINT32_MAX - 10, 100, 256));
  EXPECT_EQ(UINT32_MAX, BookFonts::addWeighted(0, 1u << 30, 1u << 20));
}
TEST(BookFontsWeights, AWeightScalesAnItemReadInPartAndASubsampledSpine) {
  EXPECT_EQ(6400u, BookFonts::probeWeight256(200 * 1024, 8 * 1024, 1, 1));
  EXPECT_EQ(256u, BookFonts::probeWeight256(0, 8 * 1024, 1, 1));       // size unknown: as read
  EXPECT_EQ(512u, BookFonts::probeWeight256(4096, 4096, 128, 64));
}
```
- `TheBodyIsFoundPastTheSixthSpineItem`: seven short items `<body><p class='block'>Front matter.</p></body>`, then one
  item with ~4 KB of `<p class='indent'>` prose; `detect()` == `family("EBGaramond")` (today: Roboto).
- `ALargeChapterReadInPartOutweighsSmallItemsReadWhole`: 58 items of ~8 KB `<p class='block'>` text, then 2 items of
  ~300 KB `<p class='indent'>` text; `detect()` == `family("EBGaramond")`.
- `ALongSpineIsSampledWithinTheBudget`: 100 items of ~20 KB prose; `found.itemsRead == 64`,
  `found.bytesRead <= 64 * (8 * 1024 + 4 * 1024)`, and `zipScanStats().walks` (from `ZipFile.h`) grows by at most 2
  across `detectFamilies`.
- `AnUnreadableItemIsSkipped`: chapters `{prose, kMissing, prose}` in EBGaramond with an embedded face; `found()`
  reports not failed and the body family; `prepare()` returns `State::Family`.
- `TheTextReferenceDoesNotHideEarlierItems`: `book()` gains an optional `guide` argument (inserted after `</spine>`);
  a guide `<reference type='text' href='Text/c2.xhtml'/>`, items 0-1 long EBGaramond prose, item 2 one short Roboto
  paragraph; `detect()` == `family("EBGaramond")` (today the probe starts at item 2).
- `ABinarySpineItemIsNoText`: chapter 0 is `<body>` + 2 KB of bytes 0x00-0xFF + `</body>`, chapter 1 prose in
  EBGaramond; not failed, body EBGaramond.
- Rename `OnlyTheFirst64KbOfAChapterIsRead` to `OnlyTheHeadOfAChapterIsRead` (one item: the cap is 64 KB; same
  assertion). `AChapterThatCannotBeReadIsRetriedTwiceMore` stays as it is (nothing readable: Pending, Pending, None).

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='BookFonts*'`
Expected: compile errors for `addWeighted`/`probeWeight256`/`itemsRead`; once stubbed, the new body tests FAIL.

- [ ] **Step 3: Implement**

`detectFamilies`: sampled = `min(count, kProbeMaxItems)`; item k is `k` when `count <= kProbeMaxItems`, else
`k * count / kProbeMaxItems` (64-bit product); cap from `probeItemCap`; the item's size from
`epub.getSpineItem(i).zipUncompressedSize` (0: unknown); `probe->beginParser(parser, weight256, cap)`; read with
`readSpineItemToStream`. A failed read: `LOG_ERR`, skip, go on. `*failed` only when items were attempted and none was
read, on probe OOM, or on the existing low-heap check. `stopAtLimit` uses the item's cap; `count()` uses `addWeighted`
with the item's weight (the body tally `usage_` and the block counts alike). Start at spine 0 (drop
`getSpineIndexForTextReference` here). Fill `itemsRead`/`bytesRead`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: same as Step 2. Expected: every `BookFonts*` test PASSES, plus `--gtest_filter='FirstOpenIndexing*'` PASSES.

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub.h lib/Epub/Epub.cpp lib/Epub/Epub/BookFonts.h lib/Epub/Epub/BookFonts.cpp test/epub_pipeline/BookFontsTest.cpp
git commit -m "feat(fonts): the book-font probe samples the whole spine, weighted to the book"
```

### Task 2: Text by family and style, ranked, negligible families left out

Spec 4.3, 4.4.

**Files:**
- Modify: `lib/Epub/Epub/BookFonts.h` (`Found` style arrays, constants), `lib/Epub/Epub/BookFonts.cpp` (`BodyFontProbe`:
  frame style, block shortfall, the family/style table replacing `blocks_`, `accents()`)
- Test: `test/epub_pipeline/BookFontsTest.cpp`

**Interfaces:**
- Consumes: Task 1's `addWeighted`, the weighted `count()`.
- Produces: `Found::bodyStyles[4]` and `Found::accentStyles[kMaxAccents][4]` (`uint32_t`, estimated text bytes per
  style, EpdFontFamily numbering: bit 0 bold, bit 1 italic) for the body family and each chosen accent.

Decisions the spec left open, made here: a frame's style is inherited; `b`, `strong`, `h1`-`h6` set bold, `i`, `em` set
italic; a defined CSS `font-weight` (bold or not) or `font-style` overrides. A block's shortfall
(`kBlockMinBytes - its raw bytes`, when its text is in one family) goes to the style of the block's **first** text run,
weighted like its text. Ranking ties keep table order (first seen first). A candidate is negligible when
`score < kMinFamilyBytes && score * 1000 < total * kMinFamilyPerMille` (total: every family's score, the reader's
included). A negligible family is logged with its name (from the face records) and score.

- [ ] **Step 1: Write the failing tests**
- `AnInlineOnlyFamilyIsAnAccent`: `p { font-family: Body }`, `.hand { font-family: Hand }`, faces for both; ~20 body
  paragraphs each with `Du<span class='hand'>n</span>thorpe` five times; `found(...).accents[0] == family("Hand")`.
- `TheAccentWithMoreTextOutranksOneWithMoreShortBlocks`: 12 `<p class='ad'>` of 10 bytes (Ads) and 2 `<p class='epi'>`
  of 600 bytes (Epigraph) beside body prose; `accents[0] == family("Epigraph")`, `accents[1] == family("Ads")`.
- `HeadingsAcrossChaptersOutrankAFewInlineWords`: 10 chapters, each an `<h2>` of 8 bytes in Display and two 20-byte
  spans in Script within body prose; `accents[0] == family("Display")`, `accents[1] == family("Script")`.
- `ANegligibleFamilyTakesNoSlot`: ~400 KB of body prose over 8 items and one `<p class='rare'>Rare</p>`;
  `accents[0] == 0`.
- `ASmallFamilyInAShortBookKeepsItsSlot`: ~2 KB of body prose and the same `Rare` block; `accents[0] == family("Rare")`.
- `FoundCarriesTheStylesOfTheChosenFamilies`: one chapter `<p>Plain</p><p><i>Slant</i></p><p><b>Heavy</b></p>` with
  `p { font-family: Body }` and a Body face; `bodyStyles` == `{64, 64, 64, 0}` (each block's 5 bytes plus its 59-byte
  shortfall, in its first run's style; one item read whole, weight 1).
- `AMiseryShapedBookGetsItsTypewriterAndHandFilledLetters`: spine: cover (`<body><img src='c.jpg'/></body>`); an ad
  item with 4 `<p class='ad'>` short blocks (Ads); a praise item in Body; five more short front-matter items in Body; a
  part item with `<p class='part'>PART I</p>` and 600 bytes of `<p class='part'><i>…</i></p>` (Narrow); a chapter with
  20 KB of Body prose and five `<p class='typed'>` blocks (Typed), each repeating
  `Du<span class='hand'>n</span>thorpe i<span class='hand'>n</span> the rai<span class='hand'>n</span>. ` 100 times
  (Hand: 300 bytes a block, 1,500 in all; Typed about 11 KB; Narrow 664 = the `PART I` block's 64 + 600). Expect body
  Body, `accents == {Typed, Hand, Narrow}`.
- Update the three ranking tests whose names or comments say "blocks": `TheAccentIsTheFamilyTheMostOtherBlocksAreSetIn`
  becomes `TheAccentIsTheFamilyWithTheMostOtherText` (Display 192 = three 64-byte minimums; Script 140 = 64 + 12 + 64),
  `AHeadingSetInASpanCountsForTheSpansFamily` (Display 64, Script 140), and
  `TheAccentFamiliesAreRankedByTheirBlocksAndTheFirstThreeCopied` becomes `…RankedByTheirText…` (Code 75 = 11 inline +
  the `pre`'s 64, Rare 64). Their assertions stay.

- [ ] **Step 2: Run them to verify they fail**

Run: `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='BookFonts*'` after building.
Expected: the new tests FAIL (inline-only family not ranked; `bodyStyles` missing).

- [ ] **Step 3: Implement** the frame style, the family/style table (`kMaxFamilies` entries of
`{uint32_t family; uint32_t bytes[4];}`, replacing `blocks_`), the shortfall at `onEnd`, `accents()` by score with the
negligible rule, and `Found`'s style arrays. `usage_` and `family()` (the body) are unchanged but for Task 1's weights.

- [ ] **Step 4: Run the tests to verify they pass**

Run: same. Expected: every `BookFonts*` test PASSES.

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/BookFonts.h lib/Epub/Epub/BookFonts.cpp test/epub_pipeline/BookFontsTest.cpp
git commit -m "feat(fonts): rank a book's accent families by their text, inline uses included"
```

### Task 3: Copy only the faces whose style carries text

Spec 4.5.

**Files:**
- Modify: `lib/Epub/Epub/BookFonts.h` (`keptStyles`), `lib/Epub/Epub/BookFonts.cpp` (`pickFaces` split, `examine`)
- Test: `test/epub_pipeline/BookFontsTest.cpp`

**Interfaces:**
- Consumes: `Found::bodyStyles`, `Found::accentStyles` (Task 2).
- Produces: `uint8_t BookFonts::keptStyles(const uint32_t (&scores)[4], uint8_t present);` the styles to copy as a
  bitmask (bit n = style n), `present` the styles with a face. Rules: the primary is the present style with the highest
  score, ties in the order regular, italic, bold, bold italic; another present style is kept when
  `score >= kMinStyleBytes || (score > 0 && score * 100 >= familyTotal * kMinStylePercent)`; bold italic only when bold
  and italic are both kept.
- `pickFaces` no longer moves a face into the regular; `moveNearestToRegular(CssParser::FontFace (&faces)[4])` does,
  after pruning (italic, then bold, then bold italic). `detectFamilies`' "usable" check becomes "any face present".

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(BookFontsStyles, TheFacesKeptAreThoseWithText) {
  EXPECT_EQ(0b0101, BookFonts::keptStyles({427731, 121, 32091, 6}, 0b1111));  // Misery's body: R, I
  EXPECT_EQ(0b0101, BookFonts::keptStyles({362, 0, 1471, 0}, 0b0101));         // BkNarrow: both
  EXPECT_EQ(0b0100, BookFonts::keptStyles({10, 0, 2000, 0}, 0b0101));          // italic alone
  EXPECT_EQ(0b0101, BookFonts::keptStyles({5000, 10, 3000, 900}, 0b1111));     // bold italic goes with bold
  EXPECT_EQ(0b0001, BookFonts::keptStyles({0, 0, 0, 0}, 0b1111));              // no text: the regular alone
  EXPECT_EQ(0b0010, BookFonts::keptStyles({0, 0, 0, 0}, 0b0010));              // a bold-only family: its bold
  EXPECT_EQ(0b0011, BookFonts::keptStyles({50, 10, 0, 0}, 0b0011));            // a short book keeps its bold
}
```
(Scores and masks are indexed by EpdFontFamily style: 0 regular, 1 bold, 2 italic, 3 bold italic; `0b0101` is regular
and italic.)
- `ABoldUsedForAFewWordsIsNotCopied`: Body with regular (`kTrueType`) and bold (`kTrueTypeBold`) faces, ~30 KB of
  prose with `<b>two words</b>`; after `prepare`, `0.ttf` exists, `1.ttf` does not.
- `AnItalicDominantAccentKeepsItsItalicAsItsRegular`: Narrow with a regular (`kTrueType`) and an italic face
  (`kNotoSans` standing in) set in blocks `<p class='part'>PART</p>` and ~8 KB of `<p class='part'><i>…</i></p>`, beside 20 KB of Body prose;
  the accent's `0.ttf` equals `kNotoSans` and its `2.ttf` does not exist.
- `AnEmbeddedTrueTypeBodyIsCopiedWithItsBoldFace`: give its chapter bold text (`<p>Body text with <b>bold
  words</b>.</p>`) so the face it checks is justified; assertions unchanged. Run every `BookFonts*` test: any other
  fixture that declares faces without text for them gets that text, not a weaker assertion.

- [ ] **Step 2: Run them to verify they fail**

Run: `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='BookFonts*'` after building.
Expected: `keptStyles` undefined; once stubbed, the pruning tests FAIL.

- [ ] **Step 3: Implement** `keptStyles`, the `pickFaces` split and `examine`'s copy loop (faces not kept are not
copied; the rest as today, refused styles falling back as today).

- [ ] **Step 4: Run the tests to verify they pass**

Run: same. Expected: every `BookFonts*` test PASSES.

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/BookFonts.h lib/Epub/Epub/BookFonts.cpp test/epub_pipeline/BookFontsTest.cpp
git commit -m "feat(fonts): copy only the book-font faces whose style carries text"
```

### Task 4: Version, logs, mid-word layout, docs, full suite

Spec 4.6, the layout test of section 5.

**Files:**
- Modify: `lib/Epub/Epub/BookFonts.h` (`kVersion` 7 and its comment line), `lib/Epub/Epub/BookFonts.cpp` (log lines)
- Modify: `test/epub_pipeline/FontFamilyLayoutTest.cpp` (`Line` gains `std::vector<bool> continues` from
  `TextBlock::wordContinues(w)`)
- Modify: `docs/file-formats.md` (the `fonts.bin` row: `BookFonts::kVersion` (7))

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Write the failing tests** (in `FontFamilyLayoutTest.cpp`, with the fixture's `accents()`)
- `AHandFilledLetterInsideAWordHasTheAccentFace`: `layout("<p>Du<span class='dedication'>n</span>thorpe</p>", &families)`;
  the line's tokens are `Du`, `n`, `thorpe`; `wordFonts[1].fontId == kAccentBody`; `wordFonts[0].fontId == 0`,
  `wordFonts[2].fontId == 0`; `continues[1]` and `continues[2]` are true.
- `ALetterInAFamilyTheBookLacksStaysInItsWord`: the same with `class='georgia'`; every `wordFonts[i].fontId == 0` and
  `continues[1]`, `continues[2]` true.
- `ABookExaminedBeforeIsExaminedAgain` (BookFontsTest) already covers a version bump: check it passes with 7.

- [ ] **Step 2: Run them**

Run: `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='FontFamilyLayout*:BookFonts*'` after building.
Expected: the two layout tests PASS already (the reviewer of the spec found mid-word spans glued with their own slot);
if either fails, that is a layout bug to fix here, in `ChapterHtmlSlimParser`, before going on.

- [ ] **Step 3: Implement** `kVersion` 7 with the comment line "v7: the probe reads the whole spine and ranks families
by their text; a face whose style has little text is not copied", the log lines of spec 4.6 (probe summary: items read
of the spine, bytes, ms via `millis()`; per chosen family: name, score, kept-styles mask; per negligible family: name,
score), and the `file-formats.md` row.

- [ ] **Step 4: Full suite and format**

Run: the full suite (Global Constraints). Expected: only the 8 known Windows-only failures. Then clang-format 21 and
`git diff --stat` shows only the files of this plan.

- [ ] **Step 5: Commit**

```bash
git add -A lib/Epub test/epub_pipeline docs/file-formats.md
git commit -m "feat(fonts): book fonts version 7, probe logs, mid-word accent tests"
```

### Task 5: Device validation on the X4 (the controller runs this, not a subagent)

Spec section 5 (device) and the 1 s limit of section 2.

- [ ] **Step 1: Builds.** `pio run -e gh_release`, then `-e default` (last), on the branch; and `-e default` on master in
  `C:/_development/witchhunt-reader` for the baseline. Check `.pio/build/default/firmware.factory.bin` exists.
- [ ] **Step 2: The device.** `ListAgents`: no other session is using the X4 (COM5); devctl needs pyserial + Pillow in a
  `--target` dir (memory `dev-tool-locations`). Pick an ordinary novel with `@font-face` rules and 20-60 spine items
  (on the card, else one of the EPUBs under `C:/_development`), and upload *Misery* (`.idea/Misery, a novel - Stephen
  King.epub`).
- [ ] **Step 3: Baseline.** Flash master. Warm the flash font cache first with one untimed open: it may be empty (the
  last session's CJK font selection erased it), and its refill on the first bitmap-SD-font open would skew one run.
  Delete the novel's cache (`epub_<hash>` over USB transfer), open it, and record
  the time between the "Lent secondary framebuffer for first-open indexing" and "Returned secondary framebuffer" log
  lines. Twice.
- [ ] **Step 4: The branch.** Flash the branch; the same first open of the novel, twice. **Gate:** the branch's time is
  no more than 1 s above master's. Over it: lower `kProbeBudgetBytes` (then `kProbeMaxItems`), update the tests' numbers,
  rebuild, measure again.
- [ ] **Step 5: Misery.** Publisher Fonts = Accents, Bookerly. First open: the log shows body Garamond3LTStd (R+I),
  accents AmericanTypewriterStd, JamesHandscript, ITCGaramondStd-BkNarrow (R+I), TradeBio left out, and the probe line.
  Screenshots (`shot:`) of a part02a page (typed text, hand-filled letters in JamesHandscript) and a part title page;
  `CMD:STATE` heap in the book. `families_test.epub` in each mode as spec 11.1.3 lists, if on the card.
- [ ] **Step 6: Record** the measured times, heap and log lines in a short "As built" section at the end of the spec,
  restore every setting changed, commit (`docs(fonts): spine probe as built`), push, and open the PR
  (`feat: book fonts found across the whole spine, ranked by their text`) with the evidence.
