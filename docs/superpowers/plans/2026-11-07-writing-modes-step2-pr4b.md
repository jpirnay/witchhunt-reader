# Writing Modes Step 2, PR 4b: Arabic Books — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Arabic, Persian or Urdu book in a TrueType family that maps its script reads joined and shaped. An Indic
word inside a bidi paragraph (Hebrew or Arabic) is shaped too. The Font Manager offers Noto Naskh Arabic and Noto Sans
Arabic, and an Arabic book uses an installed one of them by itself.

**Architecture:**
- **The rules:** a new `lib/Utf8/ArabicScripts.h` holds the run members and the cluster breaks. minibidi's
  classes learn the Arabic Supplement and Extended blocks.
- **The shaping layer:** `IndicShaper` is generalized in place. A token's runs are scanned for both scripts, and each
  run is shaped under its own `ot::Script` with one face, within one call. Arabic context is one base cluster on
  each side.
- **The line pass:** `BidiLine` learns cut points inside a token, so a formed token becomes glued pieces in visual
  order, in the fallback past 128 slots too. `ParsedText` lifts PR 3's `!bidi_` gate. It shapes before the bidi
  workspace is taken, and `placeBidiLine` substitutes glyph pieces.
- **The book:** one predicate makes every Arabic-script language right to left and shaped. The X4's gate and the
  automatic family follow from it.
- **The proof:** four fixture books with six new goldens, and a bidi-aware rule checker. On the device: the 2x
  build-time line, the heap floor, and before/after pages.

**Tech Stack:**
- C++20 (`-fno-exceptions` on the device).
- GoogleTest host suite (CMake/Ninja, MSYS2 UCRT64 g++).
- Python 3.14 with fontTools 4.63.0 and uharfbuzz 0.56.2 (HarfBuzz 14.5.0), from `C:/tmp/wm2-measure/pydeps`.
- PlatformIO.

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step2-design.md`:
- "The pieces", item 4, as split on 2026-11-07 into 4a (#651, merged) and this PR;
- deviations 6, 7 (with the 2026-11-07 mark-only refinement) and 8;
- "How shaping runs", step 7 ("in a bidi paragraph, by step 1's line pass");
- decision 6's two Arabic families;
- the Constraints, Review focus items 3, 4 and 6, and the pass lines.

**Research:** `C:/_development/witchhunt-reader/.superpowers/research/2026-11-07-writing-modes-step2-pr4b/pr4b-research.md`.
It holds the code as it is at `b5209cf46`, file by file with line numbers, and every change below in detail. Each task
names the report's sections to read. PR 4a's ledger and item 7's report are in the folder beside it
(`2026-11-07-writing-modes-step2-pr4a/`).

**The maintainer's rulings (2026-11-07):**
- PR 4b takes the rest of item 4: the fonts stay in this PR.
- A mark-only token after an Arabic letter merges into its base, and its style is lost.
- The flash floor is at least 235,000 B free on `default`.
- The X4 is not required. The X3 gives the device figures, or the X4 when the X3 is busy, as in PR 4a.

**Carried from PR 4a's rulings** (the run scan's requirements):
- Bidi_Mirrored code points (brackets) stay out of an Arabic run.
- Bidi controls and non-Arabic Mn, Me and Cf stay out too.
- The letter-or-mark classifier moves into `ArabicScripts.h`, and `SerialTestApi.cpp` stops including the engine's
  `OtShaperInternal.h`.
- The Arabic corpus lists U+088F.

**My calls** (the research's open questions; the maintainer reviews them with this plan):
1. **A ligature across a style glue** (lam-alef across a `<span>`) is drawn once, in the first token's style, as the
   design's review focus 4 says. The second token's run is then shaped with context and keeps no glyph. That is a legal
   empty piece of width 0: it is left off the line and not latched. Without context, 0 glyphs stays Degraded.
2. **A forced split of an Arabic word** falls only between base clusters: never before a haraka or a joiner, and never
   after a ZWJ. The letters at the cut take final and initial forms.
3. **One predicate for every Arabic-script language:**
   - it sets right to left and `shapedScript`;
   - it covers ar, fa, prs, ur, ps, sd, ug, ckb, ks, pnb, azb, bal, skr, glk, mzn, lrc, bqi, ota, and any tag with
     `-Arab`;
   - it excludes bare `ku`, and any tag with `-Deva` or `-Latn`.
4. **AL punctuation and symbols are inside a run:** ؟ ؛ ۔ ٭ and U+0608, 060B, 060D, 061D, 061E, 06FD, 06FE, 0888.
   Digits, the Arabic comma, the number signs, ON symbols, ALM and presentation forms stay out.
5. **The mark-only merge and the face gate:** the merge applies only in a paragraph that shapes. An Arabic paragraph
   shapes only when one of its Arabic tokens has a face. A bitmap-font Arabic book is then byte for byte step 1's.
6. **The dictionary's word text on a right-to-left line** joins a word's glued pieces in reading order.
7. **The 2x build-time line:** a miss is measured and reported to the maintainer with the figures, as for Hindi. No
   run cache comes back in this PR without the maintainer's word.
8. **Smaller calls:**
   - `kMaxRunCodepoints` stays 32;
   - the probe code point is U+0628;
   - the section version is 115;
   - the name `IndicShaper` stays, with its header saying it shapes Arabic runs too.

## Global Constraints

- **No output change elsewhere:**
  - every existing golden (Latin, CJK, Hebrew, Indic) stays byte-identical. Check with `git status` after
    `UPDATE_GOLDENS=1`; only the six new goldens appear;
  - `test_shaped_arabic_unshaped` (no face) is byte for byte the layout master's dump gives;
  - `ShapingRules.TheLatinLinesKeepTheirBytes` holds for the new books.
- **The engine's bar** (design, Constraints): "The engine's output matches HarfBuzz 14.5 glyph for glyph on the oracle
  corpus (upstream's bar: 0 differences), for every font PR 3 and PR 4 ship, at two sizes, and with `CPac` from the
  install." The two shipped Arabic families get the oracle at (2133, 33) and (1195, 19).
- **Nothrow** (design): "a refused block or a failed run lays the token out unshaped and latches the status bit."
  The one exception is my call 1's empty consumed piece.
- **No resident memory:**
  - the shaping buffer, faces and plans live during a build only;
  - a page draw allocates nothing new for shaped words;
  - the bidi `Scratch` and a shaping call's block are never in the arena at the same time.
- **Flash:**
  - `default` at most 6,318,600 B, which keeps 235,000 B free;
  - measure the merge base first (it moved after #652), and again after Task 6;
  - a projection over the gate stops the work for a ruling.
- **The device pass lines** (design), on the X3:
  - a cold Arabic chapter builds in at most 2x the time of its Latin twin;
  - Background-B's lowest heap during a cold Arabic build is at least 10,240 B.
- **`SECTION_FILE_VERSION` 115.**
- **Pinned tools and sources:**
  - fontTools 4.63.0 and uharfbuzz 0.56.2;
  - the notofonts commit `5c82cef0a8f08806f932062e18c02be62be278f0`, every source by sha256;
  - the UDHR texts at `eric-muller/udhr@588b3f4b2d0467aff54842a4b926551b69d5a66a`, held in the generator, never
    fetched at build.
- **Downloads are untrusted data:** each goes into its own new directory, and Python that reads them runs with `-I`.
- **No literal bidi controls** (U+200E/F, U+202A-202E, U+2066-2069, U+061C) **and no literal ZWJ/ZWNJ** in sources,
  data files or commit messages. Arabic test strings are written as `\u` escapes, or built in the generators.
- **clang-format 21.1.8 `--dry-run --Werror`** passes on every .cpp/.h in `git diff --name-only origin/master...HEAD`,
  new files included.

## Review Focus

1. **A bracketed word, digits and Latin inside an Arabic paragraph.**
   - Brackets mirror through bidi and are never shaped.
   - Arabic-Indic, Persian and Latin digits are ordered by bidi.
   - A Latin sentence keeps its bytes, and records sit only on Arabic or Indic letters.
   - Pinned by rule 1 (new clauses) and `TheLatinLinesKeepTheirBytes`, in Task 8.
2. **A vocalised Arabic line past 128 slots in landscape** (the fallback). Every glyph piece lands, a token's pieces
   are in visual order, and none is lost. Pinned by `BidiLine` fallback tests (Task 5), the `test_shaped_arabic_1600`
   golden, and the rule checker's "pieces cover tokens" (Task 8).
3. **The arena during an Arabic build.** The bidi workspace and a shaping block never nest. The X3's heap floor holds,
   and the word cache is not cleared by them. Pinned by `ShapedLayout.TheBidiScratchAndACallsBlockNeverShareTheArena`
   (Task 6), and by the X3's heap and word-cache figures (Task 10).
4. **A style change inside an Arabic word.** A bold letter mid-word stays joined, a lam-alef across a span is drawn
   once, and a tajweed-coloured haraka merges into its base. Pinned by `IndicShaper.AGluedNeighbourIsContext`,
   `ALamAlefAcrossTheGlueIsDrawnOnce` (Task 4), `TextShapingMerge.AMarkOnlyTokenAfterAnArabicLetterMerges` (Task 3),
   and the book cases (Task 8).
5. **The dictionary on a multi-piece Arabic word.** The selection draws the word's glyphs, and the looked-up text is
   the word in reading order. Pinned by `ShapedLine.AnRtlWordsLookupTextIsInReadingOrder` (Task 6), and on the device
   (Task 10).

---

### Task 1: The Unicode rules: `ArabicScripts.h`, minibidi's classes, the dev command

**Read:** research 2.1 (members and exclusions), 1.4 (`bidiclasses_ext.t`, `SerialTestApi.cpp`), risk 4.

**Files:**
- Create: `lib/Utf8/ArabicScripts.h` (beside `IndicScripts.h`)
- Modify: `scripts/gen_bidi_classes_ext.py` (`FILE_BLOCKS`), regenerating `lib/MiniBidi/bidiclasses_ext.t`
- Modify: `src/SerialTestApi.cpp` (its `isArabicLetterOrMark` uses the new header; drop the `OtShaperInternal.h` include),
  `docs/serial-test-api.md` if a sentence changes
- Test: a new `test/arabic_text/ArabicScriptsTest.cpp` (with its CMake), and minibidi's `BidiUtilsTest`

**Interfaces:**
- Produces, in `namespace arabic`:
  - `constexpr bool inBlocks(uint32_t cp)`: U+0600-06FF, 0750-077F, 0870-08FF;
  - `bool isRunMember(uint32_t cp)`: the 13 ranges of research 2.1, AL punctuation included (my call 4);
  - `bool isMark(uint32_t cp)`: the NSM members;
  - `bool isLetterOrMark(uint32_t cp)`;
  - `bool isClusterBreak(uint32_t prev, uint32_t cp)`: true when `cp` is not a mark, not ZWJ and not ZWNJ, and
    `prev` is not ZWJ;
  - `bool holdsArabic(const char* utf8, size_t len)`: the byte pre-test of research 2.1;
  - `bool endsWithLetter(const char* utf8, size_t len)`: the last code point that is not a mark is a run letter,
    tatweel included;
  - `constexpr uint32_t kProbe = 0x0628`.

- [ ] **Step 1: Re-measure the base.** Build `default` and `gh_release` at the merge base, then record `Flash:` and
  `RAM:` in the task report. This is the reference for every flash figure in the PR. Build `gh_release` first and
  `default` last.
- [ ] **Step 2: Write the failing tests.**
  - `ArabicScripts.RunMembersAreTheThirteenRanges`: every range edge of research 2.1, inside and one past.
  - `ArabicScripts.NoMirroredCodePointIsAMember`: sweep 0-0x10FFFF against minibidi's pairs table.
  - `ArabicScripts.TheBytePreTestMatchesTheBlocks`: every code point of the blocks and of 0590-05FF, 0700-074F,
    0900-0DFF; `DD 80-8F` (Syriac) is not Arabic.
  - `ArabicScripts.ClusterBreaks`: beh|beh, not before fatha, not before ZWJ/ZWNJ, not after ZWJ.
  - `ArabicScripts.U088FIsALetter`.
  - `BidiUtils.ArabicSupplementAndExtendedAreStrongRtl`: U+0750, 08A0 and 088F are AL; 08CA, 08F0 and 0897 are NSM;
    0890 and 08E2 are AN.
- [ ] **Step 3: Run them.** Expected: FAIL (the header does not exist; minibidi gives ON).
- [ ] **Step 4: Implement.** Write the header. Add the blocks (U+0750-077F, 0870-08FF, FC00-FDFF) to the generator's
  `FILE_BLOCKS` and regenerate. Switch `SerialTestApi.cpp` to the header.
- [ ] **Step 5: Run** the new tests and the `MiniBidi*`/`Bidi*` suites. Expected: PASS, with every Hebrew golden
  unchanged.
- [ ] **Step 6: Commit** `feat(text): the Arabic run rules, and minibidi's Arabic blocks`.

### Task 2: The parser's joiners and upstream's three rules, for Arabic

**Read:** research 1.4 (`ChapterHtmlSlimParser.cpp`, bionic, `Hyphenator.cpp`), 5.4 (`JoinerParseTest`, Hyphenator,
drop cap, bionic).

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (the held joiner after an Arabic letter, the drop cap),
  `lib/Epub/Epub/ParsedText.cpp` (bionic), `lib/Epub/Epub/hyphenation/Hyphenator.cpp` (the forced split)
- Test: `JoinerParseTest`, the drop-cap and bionic tests, the Hyphenator tests

**Interfaces:** Consumes Task 1's `arabic::isRunMember`, `isMark` and `isClusterBreak`.

- [ ] **Step 1: Write the failing tests.**
  - `JoinerParse.APersianZwnjIsKept`.
  - `JoinerParse.AZwnjAfterAnArabicLetterAcrossAnElementEdgeIsKept`.
  - `JoinerParse.ARunOfTwoJoinersIsDropped`.
  - `JoinerParse.TheVisibleOffsetsAreUnchanged`.
  - `DropCap.ADropCapOverAnArabicLetterIsAbandoned`.
  - `Bionic.AnArabicWordIsNotSplit`.
  - `Hyphenator.AnArabicForcedSplitFallsBetweenBaseClusters`: never before a haraka, never after a ZWJ (my call 2).
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement**, as research 1.4 lists.
- [ ] **Step 4: Run** them, the parser suites and every golden. Expected: PASS, goldens unchanged.
- [ ] **Step 5: Commit** `feat(epub): Arabic joiners kept, and no drop cap, bionic split or forced split inside a cluster`.

### Task 3: The shaping layer's text rules: Arabic runs, byte sizes, merges

**Read:** research 2.1, 2.4, 2.5, 1.4 (`IndicShaper.{h,cpp}`: `Run`, `maxRuns`, `scanRuns`, `holdsShaped`,
`CallMemory`), risk 5.

**Files:**
- Modify: `lib/TextShaping/IndicShaper.{h,cpp}` (the scan and the sizes), `lib/TextShaping/TextShaping.{h,cpp}`
  (the merge rule)
- Test: `test/text_shaping/TextShapingTest.cpp`

**Interfaces:**
- Produces:
  - `Run` carries its script: an `ot::Script` index, or `{const indic::ScriptInfo* indic; bool arabic;}`. Pick one
    and name it in the report; Task 4 uses it.
  - `scanRuns` returns Arabic runs beside Indic ones. An Arabic letter ends an Indic run, and the reverse holds too.
  - `maxRuns(bytes)` and the code-point room are correct for 2-byte letters.
  - `holdsShaped(text)` is true for Indic or Arabic.
  - `TextShaping::mergesWithPrevious` takes a `shapedGroup` flag (renamed if the current name says Indic) and
    gains the mark-only rule: a token of marks only after a token for which `arabic::endsWithLetter` holds.

- [ ] **Step 1: Write the failing tests.**
  - `ArabicRuns.BracketsAndCommaStayOutside`: `(كتاب)،` gives one run, bytes [1, 9).
  - `ArabicRuns.DigitsEndARun`: `كتاب١٩` gives a run, with the digits outside.
  - `ArabicRuns.AZwnjStaysInside`: ZWNJ between ک and ه gives one run.
  - `ArabicRuns.ALeadingMarkStartsNoRun`.
  - `ArabicRuns.AnRlmCutsAWord`.
  - `ArabicRuns.PresentationFormsAreNoRun`.
  - `ArabicRuns.ARunEndsAtADanda`.
  - `ArabicRuns.MaxRunsHoldsAlternatingScripts`.
  - `TextShapingMerge.AMarkOnlyTokenAfterAnArabicLetterMerges` (any style).
  - `TextShapingMerge.AMarkAfterALatinOrHebrewWordDoesNot`.
  - `TextShapingMerge.NothingMergesOutsideAShapedGroup`.
  - Update `IndicRuns.MaxRunsHoldsEveryToken`'s `static_assert` to the new formula. The Indic suites stay otherwise
    unchanged.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `TextShaping*`, `IndicRuns*` and `IndicShaper*`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(shaping): Arabic runs in the run scan, and the mark-only merge`.

### Task 4: The Arabic module: shaping a token's Arabic runs over a face

**Read:** research 2.2 (context, the zero-glyph case), 2.3 (the split), 2.4 (dispatch), 2.6 (generalize in place),
1.4 (`IndicShaper.cpp`'s `shapeRun` and `shapeToken`).

**Files:**
- Modify: `lib/TextShaping/IndicShaper.{h,cpp}` (a per-script policy: `ot::Script`, the probe, the break predicate,
  the context test)
- Modify: `test/text_shaping/CMakeLists.txt` (include `test/ot_shaper` for `ArabicExpected.h`)
- Test: `test/text_shaping/IndicShaperTest.cpp`, or a new `ArabicShaperTest.cpp` in the same target

**Interfaces:**
- Consumes: Task 3's `Run` script, and PR 4a's `ot::Script::Arabic` and `ArabicExpected.h` (`kArabicShaping`, the
  fixture fonts `NotoNaskhArabic-Shaping-Regular.ttf` and `NotoSansArabic-Shaping-Bold.ttf`).
- Produces: `shapeToken` shapes each run under its own script. Dispatch per run, within one call, is research 2.4's
  table: Arabic without a face, or with a face that lacks U+0628, stays Text and is not reordered. A run shaped with
  context that keeps 0 glyphs gives an empty Glyphs piece of width 0 (my call 1).

- [ ] **Step 1: Write the failing tests.**
  - `IndicShaper.MatchesHarfBuzzOnTheRecordedArabicWords`: every `kArabicShaping` word over the fixture faces, glyph
    for glyph.
  - `IndicShaper.CutsArabicPiecesAtRunEdges`: brackets, digits and the Arabic comma make pieces; AL punctuation stays
    inside.
  - `IndicShaper.AFontWithoutArabicLeavesItText`.
  - `IndicShaper.NoFaceLeavesArabicPlain`.
  - `IndicShaper.ShapesInTheBooksLanguage`: `fa` against `ur` differ where the font's `locl` does.
  - `IndicShaper.ARunLongerThanTheCallShapesInClusters`: with `runCodepoints` 8, the output equals the whole run's.
  - `IndicShaper.AGluedNeighbourIsContext`: a bold letter mid-word keeps its joining forms.
  - `IndicShaper.ALamAlefAcrossTheGlueIsDrawnOnce`: the first token holds the ligature, the second an empty consumed
    piece, not latched.
  - `IndicShaper.AnOffsetPastAByteIsDegraded`.
  - `IndicShaper.ATokenWithBothScriptsShapesEachRun` (a pan-Unicode face or two runs).
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
  - Generalize in place: one policy struct per script, so `shapeRun` takes the break predicate and the `ot::Script`.
  - The Arabic context is the neighbour's nearest base cluster, at most `kContextCodepoints` (8).
  - Name in the header that the shaper does Arabic runs too.
- [ ] **Step 4: Run** `IndicShaper*`, `TextShaping*`, `ShapingRules*` and the Indic goldens. Expected: PASS, Indic
  unchanged.
- [ ] **Step 5: Commit** `feat(shaping): Arabic runs shaped over a face, with one cluster of context`.

### Task 5: The line pass's run-cut pieces and its fallback (`BidiLine`)

**Read:** research 1.1, 1.2, 1.3 and 1.4 (`BidiLine.{h,cpp}`: `Token::cuts`, `collect`, `markCuts`, `fallbackLine`),
risk 1.

**Files:**
- Modify: `lib/Epub/Epub/BidiLine.{h,cpp}`
- Test: `test/bidi_line/BidiLineTest.cpp` (or wherever the 33 existing BidiLine tests live)

**Interfaces:**
- Produces: `BidiLine::Token` gains its cut points: byte offsets within the token where a piece starts. `collect`
  marks slots at cuts. A token without cuts behaves exactly as today. The fallback splits a token into its segments
  and orders them visually.

- [ ] **Step 1: Write the failing tests.**
  - `BidiLine.ATokenWithCutsGivesPiecesInVisualOrder`.
  - `BidiLine.AnRtlTokensPunctuationPieceLandsLeft`.
  - `BidiLine.AnLtrIndicTokenInAnRtlLineKeepsItsPiecesOrder`.
  - `BidiLine.AMarkAcrossACutTakesItsOwnSlot`.
  - `BidiLine.AnUndrawnSegmentIsAbsorbed`.
  - `BidiLine.TheFallbackCutsATokenIntoItsSegments`, at level 0 and 1, and for a token past 128 code points.
  - `BidiLine.TheFallbackGluesOneTokensPieces`.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.** This is pure code: no fonts, no shaping.
- [ ] **Step 4: Run** the `BidiLine*` and `Bidi*` suites. Expected: PASS, the 33 existing tests unchanged.
- [ ] **Step 5: Commit** `feat(bidi): token cut points and the fallback's pieces`.

### Task 6: `ParsedText` shapes in bidi paragraphs

**Read:** research 1.1-1.4 (`decideShaping`, `calculateWordWidths`, `layOutBidiLine`, `placeBidiLine`,
`bidiLineIsPlain`, `withoutBidi`, `releaseLayoutScratch`, `viewShapedPieces`, the dictionary's word text), risks 1-3.

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.{h,cpp}`
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION` 115, with its comment)
- Modify: the dictionary's word-text join, wherever `logicalPosition` orders it (research 4)
- Test: `test/word_size_layout/ShapedLayoutTest.cpp` (`ShapedLayout`, `ShapedLine`)

**Interfaces:**
- Consumes: Tasks 3-5.
- Produces:
  - `decideShaping` shapes a bidi paragraph that holds Indic or Arabic tokens with a face (my call 5).
  - Tokens are shaped in a loop before the bidi `Scratch` is taken.
  - `placeBidiLine` substitutes formed pieces.
  - The side paths (`bidiLineIsPlain`, `withoutBidi`) draw them, or degrade and latch.

- [ ] **Step 1: Write the failing tests** (with the Naskh fixture face through the stub seam).
  - `ShapedLayout.AnArabicWordOnAnRtlLineIsOneGlyphWord`.
  - `ShapedLayout.AWordWithItsCommaIsTwoTouchingWords`: the comma is left of the word and glued.
  - `ShapedLayout.ABidiParagraphShapesItsIndicWords` (renamed from `ABidiParagraphShapesNothingYet`, which it
    replaces).
  - `ShapedLayout.GluedPiecesTakeNoKerning`.
  - `ShapedLayout.JustificationStretchesTokensNotPieces`.
  - `ShapedLayout.AWithoutBidiLineDrawsOrDegrades`: refuse the heap `Scratch`.
  - `ShapedLayout.TheBidiScratchAndACallsBlockNeverShareTheArena`: watch `arena.used()` across `calculateWordWidths`.
  - `ShapedLayout.AnArabicParagraphWithoutAFaceKeepsItsBytes`.
  - `ShapedLine.AnRtlWordsLookupTextIsInReadingOrder`.
  - `ShapedLine.AnEmptyConsumedPieceIsLeftOffTheLine`.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement**, as research 1.4 lists. Then set the section version to 115.
- [ ] **Step 4: Run** `ShapedLayout*`, `ShapedLine*`, `EpubPipeline*`, `CjkLayout*`, `BidiLine*`, `WordSize*`,
  `ShapingRules*` and `BidiRules*`. Expected: PASS, with every existing golden byte-identical.
- [ ] **Step 5: The early flash gate.** Build `default` and record `Flash:`. If it is over 6,318,600 B, or if the
  projection for Tasks 7-9 (about 1.5 KB) crosses it, stop and report.
- [ ] **Step 6: Commit** `feat(layout): Indic and Arabic words shaped in bidi paragraphs (step 7)`.

### Task 7: The book's language: typography, the X4's gate, the automatic family

**Read:** research 3.3, and 1.4 (`LanguageTag.h`, `TypographyProfile`, `SdCardFontSystem`, `InPlaceBuildGate`).

**Files:**
- Modify: `lib/Epub/Epub/LanguageTag.h`, `lib/Epub/Epub/TypographyProfile.{h,cpp}`, `src/SdCardFontSystem.{h,cpp}`
- Test: `test/typography/TypographyTest.cpp`, and the in-place gate's test

**Interfaces:**
- Produces:
  - `bool language_tag::isArabicScript(const std::string& rawTag)` (my call 3);
  - `TypographyProfile` sets right to left and `shapedScript` from it;
  - `SdCardFontSystem` takes `NotoSansArabic` when the reader font's name holds "Sans", else `NotoNaskhArabic`, else
    whichever is installed. It probes U+0628, and waits for the language as the Indic and CJK families do.

- [ ] **Step 1: Write the failing tests.**
  - `LanguageTag.IsArabicScriptCoversEveryArabicScriptLanguage`: each language of my call 3; `ku`, `sd-Deva`,
    `ks-Deva`, `pa-Latn` and `en` give false; `ku-Arab` and `pa-Arab` give true.
  - `Typography.AnArabicScriptLanguageIsRtlAndShaped`: `ur`, `ps`, `ckb` and `fa`.
  - The gate's test: an Arabic book in a TrueType set is refused in place, as an Indic one is.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `LanguageTag*`, `Typography*` and the gate's suite. Expected: PASS.
- [ ] **Step 5: Commit** `feat(reader): an Arabic-script book is right to left, shaped, and takes an installed Arabic family`.

### Task 8: The fixture books, their goldens, the rule checker, the host figures

**Read:** research 5.1, 5.2 and 5.3.

**Files:**
- Create: `test/epubs/make_test_shaped_arabic.py`, and the books `test_shaped_arabic.epub`, `test_shaped_persian.epub`,
  `test_shaped_urdu.epub` and `test_shaped_bidi_indic.epub`
- Modify: `lib/EpdFont/scripts/make_shaped_book_fonts.py` (`NotoNaskhArabic-Book-Regular.ttf`, at most 128 KB), and
  `test/fixtures/fonts/README.md`
- Modify: `test/epub_pipeline/EpubPipelineTest.cpp` (the cases and profiles: 1,600 px and unshaped),
  `ShapingRulesTest.cpp` (bidi-aware rules, the new rules), `BidiRulesTest.cpp` (`bidiBooks()`)
- Create: the six goldens of research 5.2

**Interfaces:** Consumes Tasks 2-7.

- [ ] **Step 1: The books and the font.**
  - The generator holds the UDHR texts (ar `arb`, fa `pes_1`, ur `urd`, at the pinned commit; verify the file names),
    NFC, and every case of research 5.1. Two runs write the same bytes.
  - The font is subset from the pinned Naskh source with `-I`.
- [ ] **Step 2: The rule checker.**
  - Rules 1-5 become bidi-aware, as research 5.3 says: reading order by `logicalPosition`, and visual neighbours for
    glue.
  - Rule 1 gains: no digit, Bidi_Mirrored code point or bidi control in a glyph-form word.
  - The new rules are "pieces cover tokens" and "an Arabic token reads right to left".
  - Floors for each case.
  - `BidiRulesTest` takes `test_shaped_arabic` at 460 and 1,600 px; `EveryMarkStaysWithItsLetter` skips glyph-form
    words.
- [ ] **Step 3: The goldens.**
  - Write them with `UPDATE_GOLDENS=1`. Then `git status` must show only the six new files.
  - Prove that `test_shaped_arabic_unshaped` equals master's dump. Run the merge base's `epub_pipeline_dump` once on
    the same book, and compare.
  - Read each golden: `WG` records only on Arabic or Indic words.
- [ ] **Step 4: The host figures.** Run `epub_pipeline_dump --bench` on the four books: page bytes, glyph bytes
  against token bytes, and allocations against `test_long_paragraph_spacing`.
- [ ] **Step 5: Run** the whole suite. Expected: only the five known Windows failures.
- [ ] **Step 6: Commit** `test(epub): four Arabic-script fixture books, their goldens, and the rules on bidi lines`.

### Task 9: The Font Manager's two Arabic families

**Read:** research 3.1 and 3.2. PR 3b's plan (`2026-11-06-writing-modes-step2-pr3b.md`), Tasks 2-4, is the pattern.

**Files:**
- Modify:
  - `lib/EpdFont/scripts/sd-fonts.yaml`: two entries, `inDownloadAll: false`, described as "Arabic, Persian, Urdu,
    serif" and "..., sans serif" in the CJK and Indic entries' wording;
  - `build-sd-fonts.py`: the Arabic corpus for the merge check;
  - `merge_noto_latin.py`: `Arab`, `rtl`, and the `LANGS` rows;
  - `test/ot_shaper/generate_corpus_arabic.py`: lists U+088F, and fixes PR 4a's deferred corpus minors.
- Create: `assets/sd-fonts/NotoNaskhArabic/*` and `NotoSansArabic/*`, Regular and Bold
- Modify: `fonts-v3.json` and `fonts.json` (regenerated with `--manifest`), and `assets/sd-fonts/OFL.txt`
- Modify (tests): `ShippedFamiliesTest` (counts; `EachArabicFamilyShapesAlikeThroughItsCpac` over a committed sample,
  `test/ttf_font/data/arabic_corpus.txt`, made by `make_corpus_sample.py` with `\u` escapes),
  `FontManifestReaderTest`, `test_build_sd_fonts.py`, `test_generate_font_manifest.py`
- Modify: the measurements: a section "PR 4b: the shipped Arabic families against HarfBuzz"

**Interfaces:** Consumes the `format: ttf` path and the recipe of PR 3b, and the oracle (`oracle_arabic.py`) of PR 4a.

- [ ] **Step 1:** the yaml entries and the merge check, with their Python tests.
- [ ] **Step 2: Build** the 4 files and both manifests.
  - Run twice; the second run must change no byte.
  - Compare each file's sha256 with PR 4a's merges (scratch `merged/`). Equal is expected, since the recipe is the
    same.
- [ ] **Step 3:** `OFL.txt`, and the pinned-set tests.
  - Families: 28 outline families become 30.
  - `blockBytes` stays at most 16 KB.
  - Each Arabic family fits the X3's store.
- [ ] **Step 4: The oracle** on the four shipped files at the two sizes, through `CPac`, with every language.
  Expected: 0 differences. Record the table in the measurements.
- [ ] **Step 5: Commit** `feat(fonts): the Font Manager offers Noto Naskh Arabic and Noto Sans Arabic`.

### Task 10: The devices (the controller's)

**Read:** research 6.

- [ ] **Step 1: The setup.**
  - Read `CMD:GET:*` first, for the restore.
  - Put the families and the test books on the card **before** flashing, because families are discovered at boot.
  - Flash, and copy the web bundle.
  - On a device with older firmware, back up its flash first (PR 4a's X4 method).
- [ ] **Step 2: Build time** (the 2x line). Use a real Arabic chapter and its Latin twin (research 6.1):
  - three cold builds each, with the median;
  - master's unshaped figure beside it;
  - the `word cache:` line.
- [ ] **Step 3: The heap floor, stack and reading heap** (research 6.2-6.4), including `CMD:TTF:FULL`.
- [ ] **Step 4: The gate and the automatic family** (research 6.5), and the install time (6.6).
- [ ] **Step 5: Before and after** (research 6.7): pages on an orphan assets branch, including the dictionary
  selection.
- [ ] **Step 6: Restore** the firmware, the settings and the open book. Record everything in the measurements' section
  "PR 4b: Arabic on the page".

### Task 11: Docs, credits, the whole suite, cppcheck, the PR

**Files:**
- `docs/text-shaping.md`:
  - "layout does not use it yet" and "paragraphs that need bidi" become what they are now;
  - the run scan;
  - the limits: `i8` offsets, ayah signs, presentation forms, fonts without GPOS.
- `USER_GUIDE.md`:
  - Supported Languages: Arabic, Persian and Urdu are joined in a TrueType family, and the two families;
  - Limitations;
  - "Download All" now skips 30 families.
- `docs/future_work/writing-modes.md`, row 2 and "Next step".
- `README.md` credits.
- `docs/memory-allocation-strategy.md`: the shaping-then-workspace order in the arena.

- [ ] **Step 1:** the docs, each claim checked against the code.
- [ ] **Step 2:** merge `origin/master` if it moved; run the whole host suite (only the five known Windows failures);
  run cppcheck; run the clang-format check.
- [ ] **Step 3:** build `default` and `gh_release` on the final tree, and record flash against the merge base and the
  gate.
- [ ] **Step 4: Commit** `docs(writing-modes): step 2 PR 4b, Arabic books`.
- [ ] **Step 5: The PR** (the controller's).
  - Run the state check, gated on OPEN.
  - Push, and open the PR against master: `feat(layout): Arabic books (writing modes step 2, PR 4b)`.
  - The body gives the rulings, the device figures, the goldens and the flash.
  - Fill `#<n>` in the roadmap.
