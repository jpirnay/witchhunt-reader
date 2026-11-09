# Myanmar script (Burmese): shaping, fonts and line breaking (design draft)

Status: draft for the maintainer, 2026-11-09. Nothing is built. It builds on writing modes step 2
(`docs/superpowers/specs/2026-11-02-writing-modes-step2-design.md`; its deviations 1-13 hold) and on the handover's
items 3 and 6 (`docs/future_work/writing-modes-handover.md`), which list Myanmar as "their own shaper modules", not
planned until asked for. The maintainer has now asked: "we should add at least Myanmar".

**How each figure was obtained.** Every statement is one of three kinds, and says which:
- **read**: from source. Ours at master `74b8fc803`; HarfBuzz at tag `14.5.0` (GitHub raw); crengine and libunibreak
  at master; SILE at `v0.15.4`.
- **measured**: run on this Linux host, in a scratch directory, never in the repo. Tools: HarfBuzz 14.5.0 through
  uharfbuzz 0.56.2 and fontTools 4.63.0 (the repo's pins); ICU `gendict` 78.2; copies of the repo's
  `merge_noto_latin.py` and `gen_ot_shaper_machines.py`; `riscv32-esp-elf-g++ -Os` on copies of our `lib/OtShaper`
  files. The text corpora are listed under "The corpora".
- **estimate**: given with its basis.

No device figure exists. No time on the device is claimed anywhere.

## Intent

A Burmese book should read as printed:
- the vowel sign ေ (U+1031) and the medial ြ (U+103C) sit before their consonant;
- stacked consonants (U+1039), kinzi (င်္) and the medials take the font's forms;
- marks sit where the font's GPOS puts them;
- lines end between syllables, not only at the spaces, because Burmese writes no spaces between words.

The same engine serves the other languages of the script: Mon, Shan, S'gaw Karen and the rest (question 4).

Non-goals:
- converting or detecting Zawgyi (question 5);
- a word dictionary (question 2);
- Burmese titles in the UI (the handover's item 8: the UI faces are bitmaps);
- bitmap fonts: only TrueType shapes, as for Indic and Arabic;
- vertical sections;
- Myanmar Extended-C (U+116D0-116FF, Unicode 16): neither Noto file maps it (measured).

## What a Burmese book does today (read)

- **No shaping.** `IndicShaper::scanRuns` finds runs of U+0900-0DFF and of the Arabic blocks only. A Myanmar word is
  plain text: ေ is drawn after its consonant, stacks are not formed, marks sit by their advances.
- **No family.** `SdCardFontSystem::selectLanguageFamily` knows CJK, Indic, Arabic and Hebrew. A Burmese book stays in
  the reader font. No `sd-fonts.yaml` family covers Myanmar, so with the shipped fonts the letters draw as replacement
  glyphs.
- **The parser drops ZWSP and the variation selectors.** `utf8IsDefaultIgnorable` (`lib/Utf8/Utf8.h`) covers
  U+200B-200F and U+FE00-FE0F. `ChapterHtmlSlimParser.cpp` around line 4142 says so. A joiner is kept only after
  U+0900-0DFF or an Arabic letter (`keepsJoinerAfter`, line 453).
- **Lines break only at spaces.** An overflowing word goes to `Hyphenator::breakOffsets`' fallback.
  `appendFallbackIndexes` asks `indic::syllableBreakAllowed`, whose `isDependentSign` covers only the Indic blocks, so
  it may cut a Myanmar word before a vowel sign. The cut also draws a hyphen: only a CJK side is exempt
  (`Hyphenator.cpp`, the `cjkSide` test).

## Recommendations at a glance

| # | Question | Recommendation |
|---|---|---|
| 1 | Shaper | Port HarfBuzz 14.5.0's Myanmar shaper into `lib/OtShaper` as a fourth `ShaperKind`. Script tags `mym2`, then `mymr` (`mymr`, `DFLT` and `latn` take the default shaper). Most of it reuses the Indic and USE machinery. Estimate: about 4-5.5 KB of flash for the engine. |
| 2 | Line breaking | A rule: a break before each orthographic syllable, no data and no dictionary. In the first PR, phrases stay one shaped token. The syllable breaks are line-end split points without a hyphen, through the path Hangul words already take. |
| 3 | Fonts | NotoSansMyanmar and NotoSerifMyanmar, Regular and Bold, from the notofonts commit every merged family pins. Merged with Noto Sans/Serif Latin by the current recipe: the shaping is unchanged and the Latin is whole (measured). 236-312 KB a file. |
| 4 | Language tags | `language_tag::isMyanmarScript`, the same scan as `isHebrewScript`: my, mnw, shn, ksw and the Karen, Shan and Palaung codes below, or any tag whose script subtag is `Mymr`. |
| 5 | Zawgyi | Out of scope for these PRs. The user guide says to convert such books first. Detection is a later, separate decision. |
| 6 | Memory and speed | No resident memory. Build memory as for Indic. A plan is about 1-1.7 KB (estimate). Build time is unknown until measured; expect it to miss the 2x line, as Hindi and Arabic do. |
| 7 | Tests | The HarfBuzz oracle over a generated corpus plus public-domain prose. The merge checks. A fixture book with goldens. A Judson Bible timing book with a KJV twin. X3 and X4 runs. |
| 8 | PRs | 1: the engine. 2: Burmese books (fonts, language pick, the shaping glue). 3: line breaking. 2 and 3 ship in one release. |

## The corpora (measured)

| Corpus | Source and licence | Phrases (space-separated) | Code points |
|---|---|---|---|
| Judson Bible, Genesis 1-19 | eBible.org `myajvb`, "Judson version of 1840", public domain (its about page) | 9,598 | 54,670 |
| my.wikipedia, four articles | Wikipedia (CC BY-SA). For measurement only; never committed | 11,446 | 149,091 |
| UDHR in Burmese | the "UDHR in Unicode" text, as copied into google/myanmar-tools | 1,220 | 14,602 |

None of the three has a U+1031 at a syllable's start, and none has a ZWSP. All three are Unicode, not Zawgyi.

| | Judson | my.wikipedia | UDHR |
|---|---|---|---|
| Code points a phrase: median / p90 / max | 5 / 10 / 54 | 12 / 22 / 65 | 11 / 20 / 42 |
| Syllables a phrase: mean / p90 / max | 1.99 / 4 / 17 | 4.30 / 7 / 20 | 3.82 / 6 / 15 |
| Phrases over 32 code points (the shaper's call cap) | 0.1% | 1.6% | 0.7% |
| Distinct phrases (an unbounded cache's limit) | 30% | 74% | 73% |
| Distinct syllables | 5% | 4% | 11% |

The eBible Judson text puts a space between most words, which is not usual. Wikipedia and the UDHR put spaces between
phrases, as modern Burmese prose does. In the Wikipedia corpus a paragraph has 51 syllables on average; p99 is 475
and the longest 775. In phrases the same figures are 12, 107 and 204.

## 1. The shaper

### What HarfBuzz 14.5.0 does (read)

From `hb-ot-shaper-myanmar.cc`, `hb-ot-shaper-myanmar-machine.rl`, `hb-ot-shaper.hh` and `hb-ot-tag.cc`:
- **Which shaper.** Script Myanmar tries the script tags `mym2`, then `mymr`. There is no `mym3`. When the chosen GSUB
  script is `DFLT`, `latn` or `mymr`, the default shaper runs; any other choice takes the Myanmar shaper. `mymr` is the
  old tag, from before the shaping spec. A font with no GSUB script at all also takes the Myanmar shaper
  (`hb_ot_layout_table_select_script` answers `HB_TAG_NONE`).
- **Zawgyi** has its own shaper, `myanmar_zawgyi`, with all processing off. It is reached only when the caller sets
  the private script `Qaag`; text never selects it.
- **Settings.** Normalization `COMPOSED_DIACRITICS_NO_SHORT_CIRCUIT`, as for Indic and USE. Marks are zeroed
  `BY_GDEF_EARLY`, as in USE. No fallback positioning. No `preprocess_text`, so there are no vowel constraints. The
  default decompose and compose are used: the block's only decomposition is U+1026 to U+1025 U+102E, whose first part
  is a letter, so USE's "nothing composes onto a mark" rule changes nothing.
- **Masks.** None. `setup_masks` stores each character's category from `hb_indic_get_categories`. That is the same
  table the Indic shaper reads, so our generator already gets Myanmar's categories once its ranges include the blocks.
- **Stages.** First a pause, `setup_syllables`. Then `locl` and `ccmp` per syllable, and a pause, `reorder`. Then
  `rphf`, `pref`, `blwf` and `pstf`, each enabled globally with manual ZWJ, per syllable, in its own stage. Then a
  pause (`hb_syllabic_clear_var`). Then `pres`, `abvs`, `blws` and `psts` with manual ZWJ. The common features follow.
- **Reorder.** Dotted circles go into broken clusters (no repha category, no position). Then, for each consonant or
  broken syllable:
  - the base: a kinzi (Ra, Asat, H) at the start is skipped, and the base is the first consonant-like character (C,
    CS, Ra, IV, GB, the dotted circle);
  - positions: medial ra (MR) goes before the base, a pre-base vowel (VPre) to `PRE_M`, a VS keeps its neighbour's
    position, and a small state machine places below and post forms;
  - a stable sort by position;
  - a run of several left matras is reversed, with the VS kept after its vowel (HarfBuzz issue 3863).
- **Syllables.** The grammar has 25 categories, Myanmar's from 32 up (As, MH, MR, MW, MY, PT, VS, ML). Its types are
  consonant syllable, broken cluster and non-Myanmar cluster.

### What our engine shares (read)

- `scanSyllable` and the generated DFA format (`OtShaperInternal.h`).
- `insertDottedCircles`, with a repha category that never matches and no position.
- `forEachSyllable`, OtSort's stable sort, `Buffer::reverseRange`.
- The planner's `FeatureBuilder` with `MANUAL_ZWJ | PER_SYLLABLE`, as USE uses it.
- Normalization: `OtNormalize.cpp:133` short-circuits only for Default and Arabic, so Myanmar takes the Indic path
  without a change.
- Both generators.

### What must be new

- `ShaperKind::Myanmar` and `Script::Myanmar`, after Arabic, so the values a `CPpl` stores keep their meaning.
  `Plan::read` rejects every shaper above Use (`OtPlan.cpp:143`); TrueType plans are built at run time anyway.
- A `SCRIPT_TAGS` row `{mym2, mymr, 0}` (`OtPlan.cpp:17`). That also makes `isShapedScriptTag` true for them, so the
  install writes a `CPac` for a Myanmar font.
- `chooseShaper` (`OtPlanBuild.cpp:409`): for Myanmar, `DFLT`, `latn` or `mymr` gives Default, anything else Myanmar.
- `collectMyanmarFeatures` (about 20 lines) and two pauses, `MyanmarSetupSyllables` and `MyanmarReorder`, with their
  `runPause` cases and `cppl::pauseAllowed`.
- In `shape()`, the masks dispatch. In `position()` (`OtShaper.cpp:100`), Myanmar must join Use's early zeroing. Today
  a kind that is neither Use, Default nor Arabic is never zeroed.
- `OtMyanmar.cpp`: the masks, the syllables and the initial reordering. Estimate: 150-200 lines.
- An `mcat` namespace in `OtCategories.h`.
- **Generators.**
  - `gen_ot_shaper_tables.cc`: three more ranges (U+1000-109F, A9E0-A9FF, AA60-AA7F: 224 more `PROPS_INDEX`
    bytes) and the Myanmar languages, asked of HarfBuzz with `HB_SCRIPT_MYANMAR`.
  - `gen_ot_shaper_machines.py`: a `myanmar_grammar()` of about 25 lines, written from the `.rl` as the other two
    are. Compiled with the repo's own DFA builder (measured), it gives 57 states x 23 classes: 1,311 B of
    transitions, 57 B of accept states and 64 B of classes, before the alphabet is cut to the categories that occur.
  - The INDIC and USE tables must come out byte-identical; that is a test of the regeneration.

### Size (estimate, with its basis)

- **What the existing modules compile to** (measured): `.text` from `riscv32-esp-elf-g++ -Os` on copies of master's
  sources:
  - `OtUse.cpp` (172 lines): 2,477 B;
  - `OtIndic.cpp` (513 lines): 6,781 B;
  - `OtArabic.cpp` (112 lines): 1,276 B.
- **The Myanmar module** is about USE's size: 2-3.5 KB of code. With the DFA (at most 1.4 KB) and the data (about
  0.3 KB), **PR 1 is about 4-5.5 KB**.
- **The other PRs:** the glue of PR 2 about 2-4 KB, the break rule of PR 3 about 1-2 KB. In all about 7-12 KB of the
  about 230 KB free on the C3. That is the brief's figure; the handover had 235,000 B after #656.
- **Lines:** 300-400 lines of C++ across the three PRs, with tests and generator changes beside them.

### Deviations (named, as step 2's are)

- **14. The Myanmar shaper** (above): HarfBuzz 14.5.0's, in our nothrow engine. It shapes left to right and needs no
  clusters.
- **15 (open, the maintainer's call; see question 5).** A face shapes Myanmar only when its GSUB names `mym2` or
  `mymr`; any other face draws the code points in their stored order. HarfBuzz would run the Myanmar shaper on a face
  with no GSUB script. The reason: a Zawgyi font is drawn without shaping. Whether common Zawgyi fonts lack `mym2` is
  not checked.

## 2. Line breaking

### What breaking at spaces leaves (measured)

Phrases were laid out greedily at their HarfBuzz widths in Noto Sans Myanmar, on lines of 16 and 24 em. These two
widths are assumed to bracket a portrait and a landscape line; they were not measured on a device. The input was the
first 300 Wikipedia paragraphs. A phrase averages 6.20 em (median 5.74, p90 10.63, the longest 29.4 em). A syllable
averages 1.48 em (1.39 in the Serif).

| Line | Mean slack at a line's end | p90 slack | Spaces a line | Lines with no space at all | Phrases wider than the line |
|---|---|---|---|---|---|
| 16 em | 3.5 em (22%) | 7.8 em | 1.0 | 32% | 75 of 4,315 |
| 24 em | 4.2 em (18%) | 9.0 em | 2.2 | 7% | 8 of 4,315 |

So with spaces only, a justified line puts 3.5 em into about one space, and a third of the portrait lines have no
space to stretch. Breaks inside phrases are needed.

### The options

- **Spaces only, as today, and as KOReader does (read).** crengine passes text to libunibreak. Its `linebreak.c`
  resolves class SA, which every Myanmar letter has, to AL or CM (rule LB1), so a Burmese run never breaks inside.
  crengine's `textlang.cpp` turns SA into ID only under CSS `word-break: break-all`, which breaks between any two
  characters. KOReader therefore breaks Burmese at spaces and ZWSPs only.
- **A word dictionary, as ICU does (measured).**
  - ICU's `burmesedict.txt` (main) holds 41,120 words in 1,164,408 B. Compiled by `gendict` 78.2 with ICU's own
    options (`--bytes --transform offset-0x1000`), it is 254,326 B. It could live on the card.
  - It needs ICU's dictionary break engine ported (its Burmese engine in `dictbe.cpp`; a few hundred lines,
    estimate).
  - It needs trie walks over a 254 KB file on the card during every build. Their cost on the device is not
    measured.
  - What it buys: breaks at word boundaries instead of syllable boundaries.
- **A syllable rule (read: Keith Stribley's "Syllable Based Dual Weight Algorithm for Line Breaking in Myanmar
  Unicode"; SILE's `languages/my.lua`).**
  - Stribley: "Myanmar usually does not have spaces typed after every word, but only at sections and other
    grammatically appropriate break points."
  - His weight 2 is a syllable boundary. Treating it like weight 1 "will give good performance in narrow columns, but
    will cause some multiple-syllable words to be split".
  - SILE implements only the unambiguous pairs: before a consonant after anusvara, asat, dot below or visarga. On the
    Wikipedia corpus that is 50% of the full rule's breaks, and 99.8% of SILE's breaks are in the full rule
    (measured).

**The rule** (written for the measurements; to be the PR's). A line may break before a consonant, an independent
vowel, a digit or a sign of U+104C-104F. The consonants are U+1000-1021, 103F, 1050-1051, 105A-105D, 1061, 1065-1066,
106E-1070, 1075-1081 and 108E; the independent vowels U+1023-102A; the digits U+1040-1049. It may not break:
- after U+1039, the stacker;
- before a consonant followed by U+103A (asat) or U+1039, after any U+1037 (dot below). The dot below precedes asat
  in canonical order, as in င့်. This keeps a killed final consonant, and kinzi, with the syllable before it;
- before ၊ or ။ (U+104A/B).

It needs no data. Checked against HarfBuzz on the first 4,000 phrases of each corpus (measured):
- none of its 16,666 breaks a font (3,940 Judson + 12,726 Wikipedia) falls inside a HarfBuzz cluster;
- `ကျောင်း` stays whole, while the shaping machine splits it into two syllables, ကျော|င်း.

**How the breaks reach the breakers.** Three ways were costed on the 300 Wikipedia paragraphs. The table counts
shaping calls and code points shaped (measured with HarfBuzz widths). It is not time.

| Way | Calls | Code points shaped | Against spaces only |
|---|---|---|---|
| P: phrases shaped whole, breaks at spaces only | 4,315 | 52,094 | 1x |
| S: each syllable a token (as CJK is one token a character), glued but breakable, shaped with one neighbour syllable on each side as context | 17,354 | 128,773 | 2.47x |
| H: phrases shaped whole; at a line's end the overflowing phrase's prefixes are shaped, in order, until one is too wide; then both parts are shaped again | 8,859 (24 em) - 11,292 (16 em) | 91,360 - 110,321 | 1.75-2.12x |

How exact each way is (measured, 4,000 phrases a corpus and font, both Noto files):
- **Syllables shaped alone** differ from the whole phrase in 0.2-1.5% of phrases. Most are GSUB differences across
  syllables: in the Wikipedia corpus, 50 of 59 phrases in the Sans and 43 of 43 in the Serif.
- **With one neighbour syllable on each side shaped along and dropped**, which is what the existing context path does
  for glued tokens: 0 differences in 16,000 phrase shapings. The same 0 also confirms that a long run shaped in pieces
  between syllables, upstream's loop, is exact for Myanmar.
- **HarfBuzz's `unsafe_to_break` flag** is set at 23-40% of the breaks, yet only 1.5% of phrases differ when they
  are cut. The flag is conservative here.

Word cache hits, simulated with the cache's geometry (128 sets x 2 ways, LRU, one way an entry) over chapters of
2,500 phrases:

| Key | Wikipedia | UDHR |
|---|---|---|
| phrases | 10-15% | 21% |
| syllables alone | 70-72% | 76% |
| syllables keyed with their neighbours | 17-18% | 25% |

Today glued tokens are not cached at all.

### Recommendation for the first line-breaking PR: H

- **Phrases stay tokens.** Their syllable breaks join `hangulLineEndBreaks` in `hyphenateWordAtIndex`
  (`ParsedText.cpp:2733`) as split points without a hyphen. A shaped prefix is measured by `shapedPrefixWidth`, which
  exists.
- **The candidate loop stops** at the first prefix that does not fit. Ascending order then gives the widest fitting
  one, as long as a longer prefix is never narrower (assumed, not proven for every font). The simulation's call counts
  assume the stop. Today's loop measures every candidate.
- **The greedy breaker runs** for a paragraph with Myanmar text, whatever the hyphenation setting. Today the hyphenated
  breaker runs only when hyphenation is on (`ParsedText.cpp:1487`), and the DP breaker splits only a word wider than
  the line. With hyphenation off, Latin words in such a paragraph must still get no pattern breaks.
- **The overflow fallback** (`appendFallbackIndexes`) uses the same rule and draws no hyphen beside a Myanmar letter.
  This part goes into PR 2, since a phrase wider than the line occurs without PR 3 (75 of 4,315 at 16 em).

Why H rather than S:
- **Tokens.** S has 4.3 times the tokens in Wikipedia prose; a p99 paragraph has 475 tokens against 107. Every token
  takes a `ShapedStore` index entry of 16 + 14 B, and the store's index was the X3's heap problem in handover item 1.
- **Shaping.** S also shapes more (2.47x against 1.75-2.12x). Its cache would need keys with context, and that key
  hits only 17-25%.
- **Code.** H changes one breaker path that Hangul already uses. S touches the token split, the glue merge
  (`mergesWithPrevious` would merge same-style syllables back), the word cache key and the link matcher (the CJK
  link-tap fix had to learn CJK breaks).
- **At a line end** H shapes both parts on their own, which is what a line break means typographically. S draws the
  prefix as shaped with the next line's syllable as context.

**What the reader sees with H:**
- lines filled to within one syllable of the margin; a syllable averages 1.48 em in the Sans;
- 58-61% of lines end inside a phrase (simulated);
- no hyphen at those breaks;
- some words of several syllables split across lines, as Stribley says of weight-2 breaks;
- never a break inside a syllable, a stack or a kinzi, and never before ၊ or ။;
- justification stretches only the spaces between phrases. A line ending inside a phrase has slack under one
  syllable, so the stretch is small.

**Later, if wanted:**
- **C, clusters in the engine.** Each glyph keeps its source cluster, so a phrase shaped once is cut at a syllable
  break with no reshaping. That would cost the same as P, with output identical to S, because measured, a syllable
  shaped with its neighbours equals its share of the whole phrase. It reverses step 2's deviation 6 (one cluster a
  run) in every buffer operation, a large engine change. It belongs with item 3's speed work.
- **Stribley's two weights.** Prefer a space, or a break after asat, anusvara, dot below or visarga, when it is "close
  to the line end" (his example threshold: under 20% of the column). That means fewer split words, against wider
  slack.
- **The dictionary** as weight 1 over the syllable rule, if readers ask.
- **ZWSP as a break** (none in the three corpora).

## 3. Fonts

Source: `notofonts/notofonts.github.io` at `5c82cef0a8f08806f932062e18c02be62be278f0`, the commit every merged family
in `sd-fonts.yaml` pins. Path `fonts/<Family>/unhinted/ttf/<Family>-<Style>.ttf` (read and measured).

| File | Bytes | sha256 | Version | Glyphs |
|---|---|---|---|---|
| NotoSansMyanmar-Regular.ttf | 148,628 | `f4be6ef43871516d8347c5217a2033991b8ecda2dd1200007c2fb431a66d64db` | 2.107 | 620 |
| NotoSansMyanmar-Bold.ttf | 146,412 | `c40a000f8093de13bdf1379a7c589faa2ea989319f33f244eaaa907033c6889f` | 2.107 | 620 |
| NotoSerifMyanmar-Regular.ttf | 205,748 | `bb3a381c17751aa169595ae8be13fde604463e3587ae466c31f34a833e10e2a5` | 2.106 | 751 |
| NotoSerifMyanmar-Bold.ttf | 207,508 | `2407c27e57a219f37cfb77285c50e19ad8f1d2948654a750a29a217c6bac9863` | 2.106 | 751 |

What the files hold (measured):
- **Licence:** SIL Open Font License 1.1 (name ID 13).
- **cmap:** 239 code points each: all 160 of U+1000-109F, 31 of A9E0-A9FF, all of AA60-AA7F; also U+200B-200D,
  U+25CC, U+FE00, the curly quotes, the ellipsis and U+A92E.
- **Little ASCII.** Of U+0020-007E they map only the space and `?`. So in a merged family, unlike most Indic ones,
  the digits and the other punctuation are Noto Sans's or Serif's.
- **GSUB and GPOS:** both name only `mym2`, with language systems `KSW ` and `MON `.
  - The Sans: GSUB `abvs blwf blws ccmp locl`; GPOS `kern mark mkmk`, the kern a chained lookup over 252
    single-adjustment lookups.
  - The Serif: GSUB adds `rlig`; GPOS `abvm mark mkmk`, no kern.
- **GDEF:** 1.2, with mark glyph sets.
- **No Latin letters**, so `prepare_script` leaves the cmap as it is.

**The merge** was run with the repo's recipe (a copy of `merge_noto_latin.py`, fontTools 4.63.0, no change), with
Noto Sans and Noto Serif at the same commit (measured):

| Merged file | Bytes | Glyphs | GSUB / GPOS lookups | Scripts |
|---|---|---|---|---|
| NotoSansMyanmar-Regular | 240,256 | 1,628 | 109 / 21 | DFLT cyrl grek latn mym2 |
| NotoSansMyanmar-Bold | 235,856 | 1,628 | 109 / 21 | same |
| NotoSerifMyanmar-Regular | 310,460 | 1,734 | 158 / 19 | same |
| NotoSerifMyanmar-Bold | 312,344 | 1,734 | 158 / 19 | same |

- **Myanmar shaping unchanged.** HarfBuzz shaped 1,884 Myanmar runs from the Wikipedia corpus, every mapped code
  point alone, and ka with each sign, under no language and my, mnw and ksw. The merged and the original font gave
  0 differences, in all four styles.
- **Latin kept whole.** The script's own `check()`, its Latin half, found 0 differences in all four styles.
- **Size.** Four files of 1,098,916 B in all. NotoSerifMyanmar at 312 KB a style is 10 KB above the largest style
  shipped, NotoSerifDevanagari's 302,032 B. Whether the family fits the glyph store in flash mode is `fitsStore`'s
  answer at install; it was not run.
- **The 252 kern lookups.** The Sans merge keeps 21 GPOS lookups of the original 256, and the kerning is unchanged
  (the 0 above). Why the merger drops them was not looked into.
- **Glyph counts** are far below any limit.

**Line height (measured; the engine reads OS/2's typo metrics when `USE_TYPO_METRICS` is set: `TtfFace.cpp`,
210-236).** NotoSansMyanmar is 2.184 em a line (1,324 / -860) and NotoSerifMyanmar 2.499 em (1,239 / -1,260). As
shipped, NotoSansDevanagari is 1.477 em and NotoNaskhArabic 1.703. So a Burmese page holds about two thirds of a
Devanagari page's lines at the same size and spacing (derived). The depth is for stacks and below marks.

**`CPac` and plans (estimate, by the design's 28 B a lookup and 24 B a subtable).** About 6.9 KB of flash a style for
the Sans and 10.4 KB for the Serif. A plan takes what the default `mym2` language system names (measured):
- the Sans: GSUB 27 lookups, GPOS 4;
- the Serif: GSUB 43, GPOS 11.

At 8 B a planned lookup plus the 24 B digest copy (deviation 13), a plan is about 1.0 KB (Sans) or 1.7 KB (Serif).
Noto Sans Devanagari's has up to 185 lookups.

**Recipe changes (read):**
- `merge_noto_latin.py`: `script_of` answers `Mymr` for the three blocks, shaped left to right, and `LANGS["myanmar"]`
  lists the languages to check;
- a `generate_corpus_myanmar.py`;
- `build-sd-fonts.py`'s `write_corpus` gains that generator;
- two `sd-fonts.yaml` entries (`format: ttf`, `inDownloadAll: false`);
- `fonts-v3.json`, `OFL.txt` and the README;
- the font tests' pinned counts move from 24 to 26 families and from 48 to 52 files.

## 4. Language tags

`lib/Utf8/IndicScripts.h` cannot take Myanmar. Its `static_assert(scriptsAreConsecutiveBlocks())` indexes
U+0900-0DFF by block, and Thai, Lao and Tibetan lie between those blocks and U+1000. So Myanmar gets its own header,
as Arabic has `ArabicScripts.h`. Its name: `lib/Utf8/MyanmarScript.h`, namespace `myanmar`. It holds `isRunMember`,
`kProbe = 0x1000` (ka) and the break rule.

**`language_tag::isMyanmarScript(tag)`**, with the same scan as `isHebrewScript`. The first script subtag decides:
`mymr` says yes, any other says no. Without one, the primary language decides. The OpenType tags are HarfBuzz 14.5.0's
(read from `hb-ot-tag-table.hh`, and from its rule that an unlisted three-letter code is used upper-cased).

| Tag | Language | OpenType language systems |
|---|---|---|
| my | Burmese | BRM |
| mnw | Mon | MON, MONT (Noto has MON) |
| shn | Shan | SHN |
| ksw | S'gaw Karen | KSW, KRN (Noto has KSW) |
| kjp, pwo, blk | Eastern Pwo, Western Pwo and Pa'o Karen | KJP, PWO, BLK, each with KRN |
| kht | Khamti Shan | KHT, KHN |
| aio, phk | Aiton, Phake | AIO, PHK (not in HarfBuzz's table: its upper-case fallback) |
| pll, rbb, pce | Palaung | PLG |
| rki | Rakhine | ARK |
| tvn, int | Tavoyan, Intha | TVN, INT (the upper-case fallback) |

- **Pali** (`pi`) is written in many scripts. Only `pi-Mymr` counts.
- **Kachin and the Chin languages** are written in Latin and stay out.
- `kyu` (Western Kayah) has its own Kayah Li script; leave it out unless tagged `-Mymr`.

Where the tag is read:
- `ot::languageTagsFor` gets these rows from the generator.
- `TypographyProfile::shapedScript` becomes true for them, so the X4's in-place gate keeps such a book out of an
  in-place build in a TrueType set.
- `SdCardFontSystem` gains `readerMyanmarScript_`, the probe U+1000 and the suffix "Myanmar". So `NotoSansMyanmar` is
  first for a reader font whose name holds "Sans", else `NotoSerifMyanmar`, as for the Indic scripts.

## 5. Zawgyi

**What it is.** A pre-Unicode font encoding on the Myanmar code points. It stores ေ before its consonant and reuses
U+1060-1097 for stacked and ligature forms. This is general knowledge; the measurement below bears it out.

**What the reader would show (measured).** The Burmese UDHR in Zawgyi (google/myanmar-tools' conversion of the same
text) was shaped by HarfBuzz 14.5.0 with Noto Sans Myanmar:
- 839 of 1,220 phrases (69%) get dotted circles, 1,801 in all;
- it holds 499 code points of U+1060-1097, which a Unicode font draws as Mon, Shan and Karen letters, and 116 vowel
  signs ေ at a syllable's start.

The Unicode original gives 0 circles. A Zawgyi book is unreadable in these families, and our port would draw what
HarfBuzz draws.

**What detection and conversion cost (read).**
- The usual detector is a Markov model: google/myanmar-tools' `zawgyiUnicodeModel.dat`, 27,988 B.
- Conversion is a transliterator. ICU's `Zawgyi_my.txt` holds 8,334 B of rules for ICU's rule engine, which the
  firmware does not have; a hand-written converter is a port of its own.

**Recommendation: out of scope for these PRs.** SCOPE.md puts language support in scope, but the gate is RAM and flash
against reading benefit. Detection alone helps nobody read: the text stays broken. Conversion is a second text engine.
None of the three Unicode corpora shows a Zawgyi sign (0 initial ေ, 0 circles). Myanmar's official move to Unicode in
2019 should make new books Unicode; that is general knowledge, not checked here. Two cheap follow-ups are left to the maintainer (open questions 2 and 3):
- **a heuristic notice.** A U+1031 at a syllable's start never occurs in Unicode text (0 in about 218,000 code points
  here) and occurs 116 times in the Zawgyi UDHR. It costs a scan and one translated string in 13 languages;
- **deviation 15** (question 1), so a reader who installs a Zawgyi font reads Zawgyi books in it, unshaped.

The user guide says: convert a Zawgyi book to Unicode on a computer before transferring it.

## 6. Memory and speed

**Nothing is resident (read).** Shaping lives in the build, as step 2 has it: the call's block, the faces and plans,
the paragraph's store. A page draw allocates nothing new.

**Per call.** The block is unchanged: a buffer for 2n + 16 glyphs at n = 32, about 6.3 KB by the design's estimate.
Wikipedia phrases over 32 code points (1.6%) take the piecewise loop between syllables, which is exact for Myanmar
(above). Orthographic syllables average 3.0 code points, the longest 18; 0.2% exceed the 8 code points that a glued
neighbour's context keeps (measured).

**Per token (estimate from measured glyph counts).**
- **Glyphs:** 0.98 a code point in the Sans, 0.96 in the Serif.
- **Records:** 4 B a glyph, 6 with an offset. At 33 px, 15.6-19.3% of glyphs carry an offset of a pixel or more in the
  Sans and 3.6-3.7% in the Serif, so about 4.1-4.4 B a glyph.
- **A median phrase** (12 code points, 36 B of UTF-8) takes about 52 B of records plus 30 B of store index. The store
  reserves 2 B of records a byte of Indic text, which covers Myanmar's 1.4 B a byte.

**Word cache.** For phrases, 10-21% hits in the simulation. Hindi's Gaban measured 51% on the X3.
- A p90 phrase of 22 glyphs needs about 95 B of records, near the 126 B entry limit, so some long phrases are not
  cached (estimate).
- No phrase reaches the 255 B token limit: the longest is 65 code points, 195 B.

**Page.** About 3 B of text plus about 4.3 B of glyph form a code point, against about 1 B a character for Latin
(estimate). Fewer lines fit a page (the line height above), which offsets part of it.

**Plans and faces.** About 1-1.7 KB a plan (above), 2 plans a face, on the heap for the build. A user's own Myanmar
TTF installed before the update has no `CPac` until it is installed again. Its run-time filters cover 31-54 planned
lookups, a few KB of build heap (estimate). No `kRenderVersion` bump is proposed: one would reinstall every TrueType
family.

**Speed (no number; the device decides).** The X3's Indic and Arabic chapters build in 2.61x and 3.17x their Latin
twins' time, against the design's 2x. What is known about Burmese:
- **Fewer planned lookups:** 31 to 54, against up to 185 for Devanagari. The X3 profile found the cost is flash-cache
  misses in the lookup walks.
- **Fewer cache hits:** 10-21% simulated, against 51% measured for Hindi.
- **Longer tokens:** a median of 12 code points.
- **H's line-end shaping** adds 0.75-1.12 times the phrases' code points (simulated).

My expectation, a guess: Burmese misses 2x until the engine's speed work (handover item 3) lands.

## 7. Test plan

**Host, PR 1 (engine):**
- A `generate_corpus_myanmar.py`, seeded:
  - every assigned code point of the three blocks alone;
  - each consonant with each medial and medial pair in canonical order, each vowel sign, each tone and asat;
  - dot below before and after asat; kinzi before each consonant; each stack C+U+1039+C;
  - the Mon, Shan and Karen letters with their signs;
  - ZWJ and ZWNJ placements; marks without a base;
  - 2,000 random sequences;
  - the public-domain Judson Genesis lines as prose. The Wikipedia text cannot be committed.
- `ot_shape_cli` against uharfbuzz 0.56.2 (HarfBuzz 14.5.0), as `compare_harfbuzz.py` runs it:
  - fonts: the four original and the four merged files;
  - sizes: two;
  - languages: none, my, mnw, ksw, shn;
  - pass line: 0 differences.
- HarfBuzz's three in-house Myanmar tests (`myanmar-misc`, `myanmar-syllable`, `myanmar-zawgyi`), if their fonts may
  be taken.
- The Indic and Arabic oracles unchanged; `OtSyllableMachines.h`'s INDIC and USE tables byte-identical after
  regeneration.
- The allocation-refusal test over a Myanmar run; the plan memo's with-and-without test.

**Host, PR 2 (books):**
- `merge_noto_latin` checks in the build: the script corpus equal, the Latin whole.
- `LanguageTag` tests: `my`, `my-MM`, `pi-Mymr`, `my-Latn` (no), and `ksw` under each check.
- `SdCardFontSystem` family pick; `IndicShaper` runs and context for Myanmar; the fallback rule (no cut inside a
  syllable, no hyphen).
- A fixture book `test_shaped_myanmar.epub`, made by `make_test_shaped_indic.py` with Burmese, Mon and Shan pages, in
  a subset of Noto Sans Myanmar as `test/fixtures/fonts` holds the others. Goldens, and `ShapingRules`: every glyph
  form sums to its width, no break inside a syllable.

**Host, PR 3 (breaks):**
- unit tests of the rule: kinzi, stacks, asat, dot below in both orders, ၊ ။, digits, Latin inside;
- the candidate loop's early stop;
- the greedy breaker for a Myanmar paragraph with hyphenation off;
- goldens at 460 and 1,600 px.

**Timing book** (`tools/writing-modes-bench/make_judson_books.py`, local only, never committed):
- **Text:** the Judson Bible (1840, public domain) from eBible.org `myajvb`, with the KJV (public domain, eBible.org)
  as a true parallel text.
- **Fetching:** not through ws-export: eBible's readaloud zip is plain text, a chapter a file (measured: 1,193 files).
- **Twin length:** the existing twins match word counts a paragraph, which means nothing for an unspaced script.
  Proposed: the twin is cut to the same page count at the same settings, and both ratios are reported, a chapter and
  a page (open question 4).
- **Its limit:** eBible's Judson text is word-spaced (mean 2.0 syllables a phrase), so it under-tests line breaking.
  A second, phrase-spaced book for the breaker: the Burmese UDHR with its English (local only).

**Device (X3 and X4), the house procedure:**
- **Setup:** the fonts on the card before flashing; back up and restore.
- **Flash:** flash on `default` and `gh_release` for each PR.
- **Install:** install time and `CPac` size of both merged families, and whether each maps in flash mode.
- **Timing:** cold build at `@2:1`, median of three alternating with the twin; the `word cache:` and `arena:` lines.
- **Heap:** Background-B's lowest heap; the allocator's minimum with `CMD:HEAP:HOLD` ballast; the loop task's stack
  high-water.
- **Screenshots:** ေ and ြ before their consonants, kinzi, stacks, medial combinations, the line ends of PR 3, and a
  Zawgyi book to document what it shows.

## 8. The PRs

Each opens against master after the one before it merges, and each is testable on its own.

1. **The engine** (`feat(shaping): HarfBuzz 14.5's Myanmar shaper in the engine`).
   - Contents: deviation 14, the generators' ranges, machine and languages, `OtMyanmar.cpp`, the oracle and corpus.
   - Nothing calls it but the tests and `CMD:TTF:SHAPE`, so it reads against HarfBuzz file for file.
   - Flash from a scratch build. Reader-visible: nothing.
2. **Burmese books** (`feat(reader): Burmese books shape in the Noto Myanmar families`).
   - Fonts: the two Font Manager families and the merge recipe.
   - Picking a family: `isMyanmarScript`, `TypographyProfile`, `SdCardFontSystem`.
   - Shaping the text: `MyanmarScript.h`; the run scan, `ScriptPolicy` and context in `IndicShaper`;
     `mergesWithPrevious` with the Myanmar rule (else a style change before a vowel sign splits a syllable); the
     joiner keep; the drop-cap and focus-reading rules.
   - Breaking an overflowing phrase: the fallback by syllable without a hyphen.
   - The fixture book and the section version.
   - Reader-visible: Burmese draws shaped. Lines break at spaces only (the table under question 2).
3. **Line breaking** (`feat(layout): Burmese lines break between syllables`).
   - H as above, and the user guide.
   - It can be built on the host in parallel with PR 2.
   - Ship it in the same release as PR 2, because PR 2 alone gives a third of portrait lines no space to justify.

Later and separate: clusters in the engine (C), a Zawgyi notice, the dictionary, Burmese UI titles (item 8).

## Open questions for the maintainer

1. Deviation 15: shape Myanmar only in a face whose GSUB names `mym2` or `mymr`, so Zawgyi fonts draw unshaped? (First
   check a common Zawgyi font for those tags.)
2. Zawgyi: nothing, a log line, or the heuristic notice with a translated string?
3. Line height: keep the families' 2.18 and 2.50 em, or cap them? A cap may clip deep stacks.
4. The timing twin for an unspaced script: the same page count, or a Latin word for each Burmese syllable? And is the
   2x line kept for Burmese while item 3 is open?
5. The greedy breaker for every paragraph with Myanmar text, whatever the hyphenation setting: acceptable?
6. Stribley's "close to the line end" preference for a space: in PR 3, later, or never?
7. The language list (question 4): which minority languages, and should the non-BCP-47 `mya` and `bur` count, as the
   withdrawn `iw` does for Hebrew?

## Sources

- HarfBuzz 14.5.0: [hb-ot-shaper-myanmar.cc](https://github.com/harfbuzz/harfbuzz/blob/14.5.0/src/hb-ot-shaper-myanmar.cc),
  [hb-ot-shaper-myanmar-machine.rl](https://github.com/harfbuzz/harfbuzz/blob/14.5.0/src/hb-ot-shaper-myanmar-machine.rl),
  [hb-ot-shaper.hh](https://github.com/harfbuzz/harfbuzz/blob/14.5.0/src/hb-ot-shaper.hh),
  [hb-ot-tag.cc](https://github.com/harfbuzz/harfbuzz/blob/14.5.0/src/hb-ot-tag.cc),
  [hb-ot-tag-table.hh](https://github.com/harfbuzz/harfbuzz/blob/14.5.0/src/hb-ot-tag-table.hh)
- Noto: [notofonts.github.io @ 5c82cef0](https://github.com/notofonts/notofonts.github.io/tree/5c82cef0a8f08806f932062e18c02be62be278f0/fonts/NotoSansMyanmar)
- Keith Stribley, [Syllable Based Dual Weight Algorithm for Line Breaking in Myanmar Unicode](https://thanlwinsoft.github.io/www.thanlwinsoft.org/ThanLwinSoft/MyanmarUnicode/Parsing/my2weightLineBreakAlg1_1.pdf)
- SILE: [languages/my.lua](https://docs.rs/crate/sile/0.15.4/source/languages/my.lua)
- crengine: [textlang.cpp](https://github.com/koreader/crengine/blob/master/crengine/src/textlang.cpp); libunibreak:
  [linebreak.c](https://github.com/adah1972/libunibreak/blob/master/src/linebreak.c)
- ICU: [burmesedict.txt](https://github.com/unicode-org/icu/blob/main/icu4c/source/data/brkitr/dictionaries/burmesedict.txt),
  [BUILDRULES.py](https://github.com/unicode-org/icu/blob/main/icu4c/source/data/BUILDRULES.py),
  [translit/Zawgyi_my.txt](https://github.com/unicode-org/icu/blob/main/icu4c/source/data/translit/Zawgyi_my.txt)
- google/myanmar-tools: [repository](https://github.com/google/myanmar-tools) (the Zawgyi model and the UDHR pair)
- eBible.org: [Judson Bible, myajvb](https://ebible.org/study/content/texts/myajvb/about.html)
