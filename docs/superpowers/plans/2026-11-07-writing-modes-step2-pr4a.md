# Writing Modes Step 2, PR 4a: The Arabic Shaper in the Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `ot::shape` shapes an Arabic, Persian or Urdu run right to left, glyph for glyph as HarfBuzz 14.5.0 does.
This is proven by an oracle on the two merged Arabic families and on a font with cursive attachment. No book is shaped
yet: the layout's use of the engine is PR 4b.

**Architecture:**
- **The tables.** `scripts/gen_ot_shaper_tables.cc` extends `OtUnicodeData.h` past U+0900-0DFF to the Arabic blocks:
  - categories, modified combining classes, default-ignorables, decompositions and compositions;
  - a joining-type table read off HarfBuzz's `hb-ot-shaper-arabic-table.hh`;
  - the Arabic-script language systems.
- **The plan.** `Script::Arabic` selects `ShaperKind::Arabic`, whose features and stages are `collect_features_arabic`
  as measure-first item 7 read them. The plan also carries the seven form masks and the `rtlm` mask.
- **The shaper:**
  - a new `OtArabic.cpp` holds the joining state machine and the modifier-mark reordering;
  - `OtShaper.cpp`, `OtGpos.cpp` and `OtBuffer` gain a direction: the `rtlm` mask, cursive attachment's right-to-left
    branch, the backward offset propagation, and the final reverse of glyphs and positions.
- **The proof:**
  - CI tests against HarfBuzz output recorded from committed fixture fonts;
  - an offline oracle over a generated Arabic, Persian and Urdu corpus, at two sizes, through `CPac`;
  - the 20 Indic families' oracle re-run unchanged.

**Tech Stack:**
- C++20 (`-fno-exceptions` on the device), with a GoogleTest host suite (CMake/Ninja, MSYS2 UCRT64 g++);
- Python 3.14 with fontTools 4.63.0 and uharfbuzz 0.56.2 (HarfBuzz 14.5.0), from `C:/tmp/wm2-measure/pydeps`;
- PlatformIO for the flash figures.

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step2-design.md`:
- deviation 11, as read off HarfBuzz 14.5.0 in measure-first item 7 (3dadfc961);
- "The pieces", item 4, split on 2026-11-07;
- the Constraints.

The measurements are in `docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`, "Item 7: the Arabic
shaper and the two Arabic families". Item 7's file-by-file notes, with line numbers, are in its report:
`C:/Users/jens/AppData/Local/Temp/claude/b6fb8d1046822d55/a26b6c48-93d8-439e-bdb1-b594ab437f3c/scratchpad/hb145-report.md`
(sections 1-3). The HarfBuzz 14.5.0 sources it read are in the `hb145/` folder beside it. They are downloaded data: read
them, never build or run them.

**The maintainer's rulings (2026-11-07):**
- PR 4 splits into this PR (engine and oracle) and PR 4b (the Arabic module, bidi paragraphs, the line pass, the two
  Font Manager families, the docs).
- PR 4's flash floor: at least 235,000 B of the 6,553,600 B slot free on `default`.
- A mark-only token merges into its base (PR 4b's).
- Carried from PR 3: the X4 is not required. The X3 gives this PR's device figures.

## Global Constraints

- **The engine bar** (design, Constraints): "The engine's output matches HarfBuzz 14.5 glyph for glyph on the oracle
  corpus (upstream's bar: 0 differences), for every font PR 3 and PR 4 ship, at two sizes, and with `CPac` from the
  install." The sizes are (2133, 33) and (1195, 19).
- **No output change elsewhere:**
  - every golden is byte-identical;
  - the 20 Indic families' oracle (`test/ot_shaper/oracle_shipped_fonts.py`) still gives 272 runs and 0 differences;
  - upstream's `.layout` fixtures still load: `MaskedFeature`, `MF_COUNT` and the CPpl offsets are unchanged (item 7's
    report, `OtPlan.h`).
- **Nothrow:** no exceptions, and fixed-capacity storage (deviation 1). A run the buffer cannot hold fails cleanly.
- **Flash:** `default` keeps at least 235,000 B free of 6,553,600 B, so at most 6,318,600 B. Measured against master
  `3ae95c07b` (6,297,547 B).
- **Ported code:**
  - each ported file keeps HarfBuzz's MIT notice;
  - every place that departs from upstream's engine is marked `witchhunt: deviation 11`, so a resync of #3787's files
    can merge around it.
- **Not ported** (deviation 11): `stch`, the fallback shaping with presentation forms, `fallback_position`'s mark
  placement and legacy `kern` for a font without GPOS, and Syriac. Their pauses stay, so the stages are HarfBuzz's.
- **Pinned tools:** fontTools 4.63.0 and uharfbuzz 0.56.2. The generator is pinned to the HarfBuzz 14.5.0 tarball it
  already pins.
- **Downloads are untrusted data:** each goes into its own new directory, and Python that reads them runs with `-I`.
- **No literal bidi controls** (U+200E/F, U+202A-202E, U+2066-2069, U+061C) and no literal ZWJ/ZWNJ in sources, data
  files or commit messages. Use `\u` escapes; data files write Cf code points as `\uXXXX` text.

## Review Focus

1. **A vocalised word** (shadda with a vowel in either order, a superscript alef, a hamza above or below with a
   harakat). The marks are reordered as HarfBuzz does (modified classes 27-35, then the modifier marks moved to the
   front as 25 and 26), and placed as HarfBuzz places them. Pinned in Task 3
   (`OtArabic.ModifierMarksMoveToTheFront`) and Task 4 (the recorded words).
2. **Lam-alef with a harakat between**, and lam-alef in the middle of a word. The ligature forms, and its marks attach
   to the right components (MarkLig). Pinned in Task 4's recorded words.
3. **ZWNJ and ZWJ inside a word** (Persian's half-space, a forced join). ZWNJ breaks the joining, ZWJ forces it, and
   both behave as HarfBuzz's manual-ZWJ features expect. Pinned in Task 3 (`OtArabic.JoiningActionsFollowTheMachine`)
   and Task 4.
4. **A run shaped in pieces with its neighbours as context** (`RunContext`, as PR 4b's glued tokens will shape it).
   The pieces' glyphs equal the whole word's, in visual order. Pinned in Task 4
   (`ArabicShaping.PiecesShapedWithTheirNeighboursMatchTheWholeWord`).
5. **A run the buffer cannot hold.** The buffer must not overflow when `ccmp` splits a dot off each letter of a long
   word. `shape` returns false, writes nothing past the storage, and the next call shapes normally. Pinned in Task 4
   (`ArabicShaping.ARunPastTheStorageIsRefused`).

---

### Task 1: The generator's Arabic tables

**Files:**
- Modify: `scripts/gen_ot_shaper_tables.cc`, and `scripts/gen_ot_shaper_tables.py` if the range list lives there
- Modify (generated): `lib/OtShaper/OtUnicodeData.h`
- Modify: `lib/OtShaper/OtNormalize.cpp` (`charData`), `lib/OtShaper/OtShaperInternal.h`
- Test: `test/ot_shaper/OtShaperTest.cpp`

**Interfaces:**
- Produces:
  - `ot::CharData ot::charData(uint32_t cp)`: unchanged signature, now covering the Arabic ranges.
  - `enum class ot::JoiningType : uint8_t { U = 0, L = 1, R = 2, D = 3, Alaph = 4, DalathRish = 5, T = 7 };` in
    `OtShaperInternal.h`. These are HarfBuzz's values; C folds into D, as HarfBuzz's `JOINING_TYPE_C` does.
  - `ot::JoiningType ot::joiningType(uint32_t cp)`. HarfBuzz's X is resolved at generation: T when Mn, Me or Cf,
    else U (`get_joining_type`).
  - `languageTagsFor` gains `ar` ARA, `fa` FAR, `ur` URD, `ps` PAS, `ug` UYG, `ku` KUR and `ckb` KUR. `sd` SND and
    `ks` KSH stay as they are.

- [ ] **Step 1: Write the failing tests.**
  - `OtUnicodeData.TheIndicBlocksAreUnchanged`:
    - an FNV-1a hash over `charData(cp)`'s six fields for U+0900-0DFF and U+200C, 200D, 25CC equals a constant;
    - record the constant on the base tree, before any change, and say in the test where it came from.
  - `OtUnicodeData.ArabicCharactersCarryHarfBuzzsProps`:
    - general categories: U+0628 Lo, U+0640 Lm, U+064B Mn;
    - U+061C is Cf and ignorable; U+0600 is Cf and not ignorable;
    - modified classes: U+064B 28, 064C 29, 064D 30, 064E 31, 064F 32, 0650 33, 0651 27, 0652 34, 0670 35;
    - U+0654 is 230 and U+0655 220 (item 7's table).
  - `OtUnicodeData.ArabicJoiningTypesAreHarfBuzzs`:
    - U+0627 R, 0628 D, 0621 U, 0640 D (C), 064B T, 06CC D, 0698 R;
    - 200D D (C), 200C U, 25CC U.
  - `OtUnicodeData.ArabicDecomposesAndComposesAsUnicode`: U+0622 = 0627 0653, 0623 = 0627 0654, 0625 = 0627 0655,
    0624 = 0648 0654, 0626 = 064A 0654, 06C0 = 06D5 0654, 06C2 = 06C1 0654, 06D3 = 06D2 0654, each pair composing back.
    Use whichever decompose/compose helper `OtNormalize.cpp` exposes, or test through it.
  - Extend `OtShaperLanguage.MapsBcp47TagsToLanguageSystems` with the rows above.
- [ ] **Step 2: Run them.** Build and run `ctest --test-dir build/test -R "OtUnicodeData|OtShaperLanguage"`. Expected:
  the Arabic tests fail, and `TheIndicBlocksAreUnchanged` passes (it pins the base).
- [ ] **Step 3: Implement.**
  - **The generator's ranges:** U+0600-06FF, 0750-077F, 0870-089F and 08A0-08FF next to U+0900-0DFF, plus the existing
    extras. Presentation forms (FB50-FDFF, FE70-FEFF) are left out.
  - **The header's layout:** a range table `{first, last, indexOffset}` and one PROPS_INDEX array, with `charData` a short
    range search. Also a 4-bit joining-type array over the same ranges, included in the generator from HarfBuzz's
    `hb-ot-shaper-arabic-table.hh`.
  - **The header's comment** names the source: "joining types from HarfBuzz 14.5.0's hb-ot-shaper-arabic-table.hh
    (Unicode 18.0 ArabicShaping.txt)".
  - **Limits:** PROPS stays at most 256 distinct values. The decomposition "within the set" checks keep failing loudly
    when a pair leaves the set.
  - **The language rows** use `HB_SCRIPT_ARABIC`.
  - **Regenerate:** run `python scripts/gen_ot_shaper_tables.py`. It downloads the pinned tarball into its own
    temporary directory.
- [ ] **Step 4: Run** the same tests, then `OtShaper*`, `ComplexShaper*`, `OtShaperPort*`, `IndicShaper*` and every
  golden. Expected: all pass, and `TheIndicBlocksAreUnchanged` still passes.
- [ ] **Step 5: Commit** `feat(shaper): the generator's Arabic tables (deviation 11)`.

### Task 2: The Arabic plan, and fixtures recorded from HarfBuzz

**Files:**
- Modify: `lib/OtShaper/OtPlan.h`, `lib/OtShaper/OtPlan.cpp`, `lib/OtShaper/OtPlanBuild.cpp`
- Create: `test/ot_shaper/make_arabic_fixtures.py`, `test/ot_shaper/ArabicExpected.h` (generated)
- Create: `test/fixtures/fonts/NotoNaskhArabic-Shaping-Regular.ttf`, `NotoSansArabic-Shaping-Bold.ttf` and
  `ArabicRtlTest-Regular.ttf`, with their lines in `test/fixtures/fonts/OFL.txt` and `README.md`
- Test: `test/ot_shaper/OtShaperTest.cpp` (or a new `OtArabicTest.cpp` in the same target)

**Interfaces:**
- Consumes: Task 1's tables.
- Produces:
  - `ShaperKind::Arabic` and `Script::Arabic`. `Script::Arabic` comes after `Sinhala`, so the Indic indices keep their
    values. `SCRIPT_TAGS` gets `{tag("arab"), 0, 0}`.
  - `Plan` gains `uint32_t arabicMasks[8]` (in HarfBuzz's order: isol, fina, fin2, fin3, medi, med2, init, with index
    7 meaning none and holding 0), `uint32_t rtlmMask` and `bool backward`, which is true for `Script::Arabic`.
  - The recorded data in `ArabicExpected.h`:
    - `struct ArabicPlanExpectation { const char* font; const char* language; const uint16_t* gsub; uint8_t gsubCount;
      const uint16_t* gpos; uint8_t gposCount; uint8_t gsubStages; };`
    - `struct ArabicShapingExpectation { const char* font; const char* utf8; const char* language; const ExpectedGlyph*
      glyphs; uint8_t count; };`, where `ExpectedGlyph` is `test/complex_shaper/ExpectedShaping.h`'s;
    - the arrays `kArabicPlans[]` and `kArabicShaping[]`.

- [ ] **Step 1: The fixtures and their recorded output.**
  - **`make_arabic_fixtures.py <sources dir> <out dir>`** builds three fonts.
    - Two are subsets of the merged families (`lib/EpdFont/scripts/merge_noto_latin.py`'s `merge()`, with the pinned
      sources in `C:/tmp/wm2-measure/dl/fonts`): Noto Naskh Arabic Regular with Noto Serif's Latin, and Noto Sans
      Arabic Bold with Noto Sans's. Each is cut with `fontTools.subset` (all layout features kept) to the fixture words'
      code points.
    - The third, `ArabicRtlTest-Regular.ttf`, is synthetic, built with `fontTools.fontBuilder` and `feaLib`. It covers
      what neither Noto family has:
      - `curs` with and without the RightToLeft lookup flag;
      - `isol`, `init`, `medi` and `fina` on a few letters, and `rlig` lam-alef;
      - `calt`, under a `dflt` language system without `rclt` and a `URD` one with it;
      - mark-to-base and mark-to-mark chains;
      - U+0627 and U+0653 mapped, and U+0622 left unmapped, so Task 3 can test the decomposition.
  - **The script records HarfBuzz's planned lookups** per font and language (none, `ar`, `fa`, `ur`, `sd`; plus `ur`
    for the synthetic font). It uses uharfbuzz's message function ("start lookup N"), item 7's method, which item 7
    validated on Devanagari. It counts the GSUB stages from the plan's pauses.
  - **The script records HarfBuzz's output** for each word with `direction = "rtl"` and `script = "Arab"`, at scale
    2133 and ppem 33.
  - **The words** are given as `\u` escapes in the script. They cover:
    - every Arabic letter isolated, initial, medial and final (next to beh);
    - lam-alef (0644 with 0622, 0623, 0625 and 0627), alone, mid-word and with a harakat between;
    - shadda with fatha and with kasra, in both orders; a superscript alef;
    - 0654 and 0655 with a vowel, in both orders;
    - beh ZWNJ beh, beh ZWJ, ZWJ beh;
    - tatweel between letters;
    - Persian and Urdu letters (067E 0686 0698 06AF 06A9 06CC 06C1 06D2 06BA 0679 0688 0691 06BE);
    - Arabic-Indic digits;
    - for the synthetic font, words that reach each of its lookups.
  - **Pinning:** the merged sources go through the pinned sha256 (`download_font`'s check, or a direct hash check).
    Two runs must write the same bytes.
  - **Run it** through `C:/tmp/wm2-measure/tools/pyrun.py` with the pinned deps. Commit the three fonts and
    `ArabicExpected.h`, with the fonts' sizes in the commit message.
- [ ] **Step 2: Write the failing tests.**
  - `OtPlanArabic.PlannedLookupsMatchHarfBuzz`: for each `kArabicPlans` entry, our `Plan::build(face,
    Script::Arabic, languageTagsFor(language))` gives the same GSUB and GPOS lookup indices, in order, and the same
    number of GSUB stages.
  - `OtPlanArabic.StagesFollowRclt`: the synthetic font gives 14 GSUB stages under `dflt` (no `rclt`) and 13 under
    `URD` (with it), and 1 GPOS stage.
  - `OtPlanArabic.FormMasksAreHarfBuzzs`:
    - `isol`, `fina`, `medi` and `init` each get a distinct non-zero bit, even on a font that lacks the feature
      (fallback);
    - `fin2`, `fin3` and `med2` get 0 on a font without them;
    - `arabicMasks[7] == 0`.
  - `OtPlanArabic.ChosenWhateverTheScriptTag`: a font whose GSUB has only `DFLT` still gets `ShaperKind::Arabic` for
    `Script::Arabic`.
  - `OtPlanCompiled.LoadedPlansMatchBuiltPlans` stays green: CPpl is unchanged.
- [ ] **Step 3: Run them.** Expected: they fail to compile (no `Script::Arabic`).
- [ ] **Step 4: Implement.**
  - **`chooseShaper`** returns Arabic for `Script::Arabic`, before the DFLT/latn test.
  - **`collectDefaultFeatures`** is direction-aware: a backward plan enables `rtla` and adds `rtlm` masked, in place of
    `ltra` and `ltrm`.
  - **`collectArabicFeatures(FeatureBuilder&, bool hasRclt)`** follows deviation 11's list and item 7's table exactly:
    features, flags, stages and pauses. The Arabic pauses are `Pause::None`. If named pauses are added, extend
    `cppl::MAX_PAUSE`/`pauseAllowed` too.
  - **`hasFeature(tag)`:** lift `mapFeatures`' `findFeatureIndex` into a member that searches GSUB, then GPOS, in the
    chosen language systems, as `hb_ot_map_builder_t::has_feature` does.
  - **After `planLookups`:** fill `arabicMasks` and `rtlmMask` with `oneMask`.
  - **Keep** `MASKED_FEATURE_TAGS`/`MF_COUNT` unchanged.
- [ ] **Step 5: Run** the new tests and the existing `OtPlan*`/`OtShaper*` ones. Expected: PASS.
- [ ] **Step 6: Commit** `feat(shaper): the Arabic plan, checked against HarfBuzz's planned lookups`.

### Task 3: Joining, mark reordering and normalization

**Files:**
- Create: `lib/OtShaper/OtArabic.cpp`, with HarfBuzz's MIT notice
- Modify: `lib/OtShaper/OtShaperInternal.h`, `lib/OtShaper/OtNormalize.cpp`, `lib/OtShaper/OtBuffer.h`
- Modify: the CMake source lists that enumerate `lib/OtShaper` files (`OT_SHAPER_SOURCES`) and the firmware build, if
  it lists files
- Test: the test file of Task 2

**Interfaces:**
- Consumes: Task 1's `joiningType`; Task 2's `Plan::arabicMasks`.
- Produces:
  - `void ot::arabicSetupMasks(const Plan& plan, Buffer& buffer)`. It runs the state machine over `buffer.info`,
    context code points included, stores each glyph's action in `GlyphInfo::position` (HarfBuzz's shared aux byte),
    and ORs in `plan.arabicMasks[action]`.
  - `void ot::reorderMarksArabic(Buffer& buffer, unsigned start, unsigned end)`.
  - `void GlyphInfo::setModifiedCombiningClass(uint8_t)`, which writes the high byte of `unicodeProps` (marks only).

- [ ] **Step 1: Write the failing tests.** A test helper normalizes a code point string under an Arabic plan and runs
  `arabicSetupMasks`; it reads each glyph's action, or its mask bit.
  - `OtArabic.JoiningActionsFollowTheMachine`:
    - 0628 064A 062A gives init, medi, fina;
    - 0628 0627 gives init, fina; 0627 0628 gives isol, isol; 0628 alone gives isol;
    - 0628 064E 062A gives init, none (a transparent mark), fina;
    - 0628 200C 062A gives isol, none, isol; 0628 200D gives init, none;
    - the run 064A 062A with one context code point 0628 before gives medi, fina.
  - `OtArabic.ModifierMarksMoveToTheFront`:
    - 0628 0650 0655 becomes 0628 0655 0650, with 0655's class 25;
    - 0628 064E 0654 becomes 0628 0654 064E, with 0654's class 26;
    - 0628 064E 0651 and 0628 0651 064E both become 0628 0651 064E.
  - `OtNormalize.ArabicComposesAsUnicodeDoes`:
    - 0627 0653 becomes the single glyph of 0622 in the Naskh fixture, which maps it;
    - 0622 becomes 0627 and 0653 in the synthetic font, which does not.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement.**
  - **`OtArabic.cpp`:** HarfBuzz's 7x6 state table verbatim; the machine as `arabic_joining` runs it; the MCM list and
    the class-220-then-230 loop of `reorder_marks_arabic`. There are no clusters to merge.
  - **`OtNormalize.cpp`:**
    - `mightShortCircuit` holds for Arabic;
    - the "nothing composes onto a mark" rule applies to Indic and USE only;
    - after each mark run's stable sort, call `reorderMarksArabic` for an Arabic plan.
  - **Marking:** every change is marked `witchhunt: deviation 11`.
- [ ] **Step 4: Run** them and the Indic/USE suites. Expected: PASS, with the Indic output unchanged.
- [ ] **Step 5: Commit** `feat(shaper): Arabic joining and the modifier-mark reordering`.

### Task 4: Right to left, end to end

**Files:**
- Modify: `lib/OtShaper/OtBuffer.h`, `lib/OtShaper/OtBuffer.cpp`, `lib/OtShaper/OtShaper.cpp`,
  `lib/OtShaper/OtGpos.cpp`, `lib/OtShaper/OtLayout.h`, `lib/OtShaper/OtLayoutInternal.h`
- Test: the test file of Task 2

**Interfaces:**
- Consumes: Tasks 1-3, and `ArabicExpected.h`.
- Produces:
  - `bool Buffer::backward`, set in `shape()` from `plan.backward`;
  - `void Buffer::reverse()`, which swaps `info[i]` with `info[n-1-i]` and `pos[i]` with `pos[n-1-i]`;
  - `propagateAttachmentOffsets(Buffer&)`, which reads `buffer.backward`.
  - After `shape()` returns, an Arabic run's glyphs are in visual order, as HarfBuzz's are.

- [ ] **Step 1: Write the failing tests.**
  - `ArabicShaping.MatchesHarfBuzzGlyphForGlyph`: every `kArabicShaping` entry gives the recorded glyph IDs, advances
    (12.4) and offsets. Round as `ExpectedShaping` rounds; reuse the Indic expected test's helper.
  - `ArabicShaping.TheSameThroughCpac`:
    - each fixture font shapes every recorded word alike with a `CPac` face and with computed filters;
    - build the `CPac` face as `test/ttf_font/ShippedFamiliesTest.cpp`'s `ShapedFace::open` does.
  - `ArabicShaping.PiecesShapedWithTheirNeighboursMatchTheWholeWord`:
    - an Arabic word of at least six letters is cut after a letter that does not join to the left;
    - each piece is shaped with the other side as `RunContext` context;
    - the pieces' glyphs together equal the whole word's, in visual order;
    - follow `OtShaperContext.PiecesShapedWithTheirNeighboursMatchTheWholeWord`.
  - `ArabicShaping.ARunPastTheStorageIsRefused`:
    - a buffer sized for N slots, and a word of N letters whose `ccmp` splits a dot off each (Noto Sans Arabic Bold's
      U+0636; take the word from the fixture font's behaviour);
    - `shape` returns false, every byte past the storage is untouched (use a canary), and the next call on a short
      word matches its recorded output.
- [ ] **Step 2: Run them.** Expected: FAIL.
- [ ] **Step 3: Implement**, as item 7's report section 2 lists it.
  - **`shape()`:**
    - `preprocessVowelConstraints` runs for Indic and USE only;
    - a backward plan ORs `rtlmMask` into every glyph's mask whose mirror the font lacks. Port
      `hb_ot_rotate_chars`' Bidi_Mirroring_Glyph lookup for the code points it covers, or document why a run never
      holds one, and test that claim;
    - `arabicSetupMasks` runs after normalization, before glyph mapping.
  - **`position()`:**
    - marks are zeroed late by GDEF;
    - `adjustOffsets` applies only when the plan is not backward;
    - offsets propagate backward for a backward buffer;
    - then, before `hideDefaultIgnorables`, `buffer.reverse()`.
  - **`OtGpos.cpp`:**
    - cursive attachment gets the right-to-left branch (with saturation, as the left-to-right one has);
    - `propagateAttachmentOffsets` walks from the last glyph down for a backward buffer, with the backward mark
      branches (#5514);
    - check `reverseCursiveMinorOffset` against 14.5.0's overflow refusal.
  - **Marking:** every change is marked `witchhunt: deviation 11`.
- [ ] **Step 4: Run** them, the whole `OtShaper*`/`ComplexShaper*`/`OtShaperPort*`/`IndicShaper*`/`ShippedFamilies*`
  set, and the goldens. Expected: PASS, and the Indic output byte-identical.
- [ ] **Step 5: Commit** `feat(shaper): right-to-left runs, as hb_ot_position does them (deviation 11)`.

### Task 5: The oracle over Arabic, Persian and Urdu

**Files:**
- Create: `test/ot_shaper/generate_corpus_arabic.py`, `test/ot_shaper/oracle_arabic.py`
- Modify:
  - `test/ot_shaper/OtShapeCli.cpp`: Arabic script detection and `plans[11]`;
  - `test/ot_shaper/compare_harfbuzz.py`: `script_of` returns `Arab`, and `harfbuzz_lines` sets `rtl` for it;
  - `docs/superpowers/specs/2026-11-02-writing-modes-step2-measurements.md`: a section "PR 4a: the Arabic shaper
    against HarfBuzz".

**Interfaces:**
- Consumes: Tasks 1-4; `compare_harfbuzz.harfbuzz_lines`; `ot_shape_cli ... cpac`.

- [ ] **Step 1: The corpus.** `generate_corpus_arabic.py <out dir>` writes `arabic.txt`, deterministic, with
  `random.Random(1757)`. It holds:
  - every assigned code point of the four Arabic ranges alone;
  - each letter isolated, initial, medial and final (around beh);
  - 40 base letters, each with each of U+064B-0652 and 0670, and with shadda plus each vowel in both orders;
  - the 14 modifier marks after beh, each with fatha and with kasra in both orders;
  - lam with each alef (0622 0623 0625 0627 0671 0672 0673 0675), alone, after beh, with a harakat or tatweel
    between;
  - the ZWJ/ZWNJ variants of Task 2;
  - Persian and Urdu letters inside words;
  - 2,000 random sequences of 2-8 code points from the letters, marks, joiners and tatweel.
- [ ] **Step 2: The tools.**
  - **`ot_shape_cli`** shapes a line with Arabic letters under `Script::Arabic`.
  - **`compare_harfbuzz.py`** gives HarfBuzz the right script and direction for an Arabic line. Its Indic results are
    unchanged.
  - **`oracle_arabic.py <cli> <fonts dir> <corpus dir>`:**
    - for each font, at (2133, 33) and (1195, 19), it shapes the corpus with `cpac`, with no language and with each of
      `ar`, `fa`, `ur`, `ps`, `sd`, `ug`, `ku`, `ckb` and `ks`;
    - it prints "font size lang: N/N" per run and exits non-zero on any difference.
- [ ] **Step 3: The fonts, in a new scratch directory:**
  - the four merged Arabic files (Naskh and Sans, Regular and Bold), made by `merge_noto_latin.merge()` from the pinned
    sources;
  - Noto Nastaliq Urdu Regular, for `curs` on a real font. Download it from
    `https://raw.githubusercontent.com/notofonts/notofonts.github.io/5c82cef0a8f08806f932062e18c02be62be278f0/fonts/NotoNastaliqUrdu/unhinted/ttf/NotoNastaliqUrdu-Regular.ttf`
    into its own new directory, and record its sha256 and size. If that path does not exist, use another OFL Arabic font
    with `curs`, record its URL and sha256, and say so.
- [ ] **Step 4: Run** the oracle on the five fonts, and `oracle_shipped_fonts.py` on the 40 Indic files.
  - **Expected:** every run N/N with 0 differences, and the Indic oracle still 272 runs with 0 differences.
  - **A difference** stops the task: report it with its lines (font, size, language, the input as `\u` escapes, both
    outputs). Do not loosen the comparison.
  - **The measurements section** records the corpus size, the table per font, the Nastaliq font's provenance, and the
    Indic re-run.
- [ ] **Step 5: Commit** `test(shaper): the Arabic shaper matches HarfBuzz on Arabic, Persian and Urdu`.

### Task 6: Flash, the device's timing, docs and the PR

**Files:**
- Modify: `src/SerialTestApi.cpp` (`CMD:TTF:SHAPE` takes the first shaped run, Indic or Arabic), `docs/serial-test-api.md`
- Modify: `docs/text-shaping.md` (the engine's Arabic shaper: what it does, what is left out, that layout uses it only
  from PR 4b), `docs/future_work/writing-modes.md` (row 2 and "Next step.")
- Modify: the measurements' "PR 4a" section (flash, the device's figures)

- [ ] **Step 1: `CMD:TTF:SHAPE` with an Arabic word.**
  - An Arabic run is shaped under `Script::Arabic` with the language the command gives.
  - The reply keeps its fields; `script=Arabic`.
  - Host-test the grammar change if `SerialCommandGrammar` takes part.
- [ ] **Step 2: Flash and RAM.**
  - Build `default` and `gh_release`, and record `Flash:` and `RAM:` against master `3ae95c07b`.
  - **The gate:** `default` at most 6,318,600 B (235,000 B free). Missed, the task stops and reports.
- [ ] **Step 3: On the X3** (the controller's run; read `CMD:GET:*` first, for the restore):
  - with merged Noto Naskh Arabic as the reader family, `CMD:TTF:SHAPE` on ten Arabic words, ten Persian and ten Urdu;
  - with NotoSansDevanagari, ten Hindi words;
  - record `plan_us`, `plan_bytes`, `shape_us` and `cpac`. This is PR 4b's early warning for the 2x build-time line.
  - Restore master and the settings.
- [ ] **Step 4: The docs, the whole host suite, cppcheck, and clang-format 21 `--dry-run --Werror`** on every .cpp/.h
  in `git diff --name-only origin/master...HEAD`.
  - **Expected:** only the five known Windows failures, every golden byte-identical, and no cppcheck defect.
  - **Commit** `docs(writing-modes): step 2 PR 4a, the Arabic shaper in the engine`.
- [ ] **Step 5: The PR** (the controller's):
  - Run `gh pr view --json state` first, then push, and open the PR against master. Its title is
    `feat(shaper): the Arabic shaper in the engine (writing modes step 2, PR 4a)`.
  - **The body:**
    - deviation 11 as read off HarfBuzz;
    - the RTL changes;
    - the oracle table;
    - the Indic re-run;
    - the fixtures' provenance;
    - flash against the 235,000 B floor;
    - the X3's figures;
    - what PR 4b does with it.
  - Fill `#<n>` in the roadmap with a commit `docs(writing-modes): step 2 PR 4a is #<n>`.
