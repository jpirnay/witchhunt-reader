# Complex scripts: shaping, bidi and their speed

Open work on right-to-left text and the shaped scripts (the ten Indic ones, Arabic and Myanmar). What shipped, and
why, is in [../design/writing-modes.md](../design/writing-modes.md); how shaping works now, its timings and what it does
not shape yet are in [../text-shaping.md](../text-shaping.md). The limits of vertical text are in the design record's
section 5, and CJK's out-of-scope list is in [../design/cjk-typography.md](../design/cjk-typography.md), section 10. How to time a chapter build and profile the X3 is in
[../debugging.md](../debugging.md#for-developers-timing-and-profiling-a-chapter-build). The shaped store's heap is a
[memory-and-parsing item](memory-and-parsing/the-shaped-store-is-on-the-heap-and-the-paragraph-gate-ignores-its-size.md).

Checked against master on 2026-11-23 (1a9f93b08).

## The shaping engine's speed

**Where it stands.** X3, chapter 2 of the bench books at 14 pt portrait, warm glyph store, median of six cold builds
(#874): Hindi 5,640 ms, Arabic 6,015 ms, their Latin twin 3,004 ms, so 1.88x and 2.00x the twin, against the design's
2x line. #683 (deviations 16-21, the engine's hottest code in IRAM) and the layout half of #872 brought it there. The
processor work alone (the build less its wait for the first page) has not been measured since #683: re-baseline it
before choosing work.

**Not the lever on the X3.** The engine half of #872 (deviations 23 and 24: lookup dispatch, recursion filters, early
exits) was within the code-placement noise: Hindi -0.2% and Arabic +1.8% alone, 0% and -0.7% on top of the layout
half. #874 reverted it, so there are two fewer deviations from HarfBuzz to carry on every resync. It can come back with
an S3 measurement; #874 timed no S3 board. The C3 is bound by fetching the engine's code, not by its reads of the
font (deviation 21 in [text-shaping.md](../text-shaping.md#the-face-and-its-plans)), so time any candidate on the X3
first.

**Candidates**, HarfBuzz accelerators not ported:
- mark-set digests: `ot::Face::markSetCovers` reads the mark glyph set's coverage table from flash on every call;
- a GDEF glyph-props cache: `ot::Face::glyphProps` reads GlyphClassDef, and MarkAttachClassDef for a mark, on every
  call;
- HarfBuzz 14.5's second-glyph digests beyond the LigatureSubst seconds digest per lookup that deviation 19 ports.
  That these are PairPos's is an inference.

**Upstream HarfBuzz since 14.5.0** (surveyed 2026-11-09: nothing then changed Indic, Arabic or Myanmar output):
- HarfBuzz #6544's Indic table fix (U+11B0A, U+11DF0-11DF1): take it at the next table regeneration (`VERSION` in
  `scripts/gen_ot_shaper_tables.py`, still 14.5.0).
- With any port of HarfBuzz's coverage caches, not only the digests: their fixes 2994c4d0a (HarfBuzz #6321),
  b98ffb1fb (#6419) and c3fd6a885 (#6453).
- HarfBuzz #6558 (open then) makes the 64-glyph syllable cut the same in every pass. Our `forEachSyllable` has no cut,
  and a call holds at most 32 code points (`kMaxRunCodepoints`).
- The oracle scripts pin uharfbuzz 0.56.2 (HarfBuzz 14.5.0); uharfbuzz 0.56.4 bundles HarfBuzz 15.0.0.

## The right-to-left path

Hebrew takes it whole, and Arabic shares it. X3, `parse=`, warm store, median of three, after #674: Bialik's Hebrew
chapter in NotoSansHebrew 2,210 ms against its Latin twin's 1,151 (1.92x). In a sampling profile of the build after its
first page (master 74b8fc803, before #674), line extraction took 42% of Hebrew: the bidi line pass 18%, its placement
14%; the reordering 10%; `bidi_class` alone 7.3%. The per-item figures below are from the device timers of
`wip/layout-profile`, taken before #662 and #669.

- **The line pass** (466 ms a chapter, 1.2 ms a line): one UAX #9 pass per line (`BidiLine`), with every token's
  `HebrewShaper::shape` repeated and the forms encoded again. `HebrewShaper::shape` walks a token twice (count, then
  write) looking for the divine name, which starts with a yod (U+05D9). A byte scan for the yod's UTF-8 (`D7 99`)
  could skip both walks for a token without one.
- **The form bytes.** A Hebrew line writes each word's display form beside its text (`WD` in the
  `epub_pipeline_dump` output): 26,091 B against the twin's 15,983, and the emit step 395 ms against 220. A plain
  token's form is its text reversed and could be derived when it is drawn. That is a section format change
  (`SECTION_FILE_VERSION`).
- **The plain path** (305 ms for 2,574 tokens, 118 µs each): `bidi_class` per code point (`lib/MiniBidi/minibidi.c`:
  a binary search in each of two tables for a code point the first lacks), the `HebrewShaper` walk, and the lock that
  `BidiUtils::workspaceAvailable` takes. #793 made that lock a static FreeRTOS mutex on the device, so re-measure
  before working on it.
- **The Liang patterns.** In the Latin twin's profile the line breaker took 19%: its split search 14%, of which
  measuring the candidates 5.3% (cut in #675) and the Liang patterns 4.5% (`LiangHyphenation`,
  `lib/Epub/Epub/hyphenation/`). Since then a layout pass reuses one hyphenation scratch, and the tries come from the
  card's packs (`PagedTrieReader`): profile again first.

## Arabic

Each of these is also listed in [text-shaping.md](../text-shaping.md#what-is-not-shaped-yet), "What is not shaped yet".
- **A soft hyphen (U+00AD) inside an Arabic word** ends its run and cuts the joining; HarfBuzz joins across it
  (joining type T). The fix: a transparent run member, dropped from the shaper's input. It touches the run scan
  (`IndicShaper::scanRuns`) and the soft-hyphen paths (`BidiLine`, which keeps U+00AD out of every run, and
  `ParsedText`'s soft-hyphen helpers).
- **A word past 32 code points split by a style span** can lose or double a long ligature: the piecewise loop reaches
  only two clusters (`kMaxRunCodepoints`, `IndicShaper.h`). The fix: size the buffer for the run plus twice
  `kArabicContextCodepoints` (16) for an Arabic call.
- The two above sit in the run scan and the buffer of `IndicShaper`, and the maintainer held them for a full code audit
  of the shaper (2026-11-08). The engine's speed work, held with them, went ahead in #683, so ask whether the hold
  still stands.
- **Fonts without GPOS.** HarfBuzz's fallback mark placement and legacy `kern` are not ported, so such a font draws its
  marks where its advances put them.
- **A consumed letter whose partner degraded is measured at width 0.** A run that a glued neighbour's ligature emptied
  (the alef of a lam-alef across a style glue) is drawn as its own code points, at their width, when that neighbour is
  drawn as code points (`ParsedText::formBidiPieces` and `viewShapedPieces`, their `ligatureDrawn` checks). The line
  breaker still takes the token's shaped width, in which that run is 0, so such a line can run past its measure by the
  letter. Rare: it needs a ligature across a style glue and a neighbour whose shaping degraded. Next step: a case in
  `test/word_size_layout/ShapedLayoutTest.cpp`, beside the drawing cases there, before any fix.
- **Optional: Noto Nastaliq Urdu**, the style Urdu readers expect, with heavy cursive positioning (GPOS `curs`). The
  engine already shapes it as HarfBuzz does (`oracle_arabic.py`,
  [step 2 measurements](../superpowers/specs/2026-11-02-writing-modes-step2-measurements.md)). Measure its flash and
  speed before offering it; its source file is 176,600 B.

## Markdown bidi across spans

A Markdown line is drawn span by span (`MdReaderActivity::drawSpanAt`), and `GfxRenderer::drawText` makes each string
with a right-to-left character visual on its own (`BidiUtils::visualUiLine`). So a line whose spans differ in style
keeps the spans left to right, each reordered alone, and no line takes a paragraph direction (read from the code, not
seen on a device). A TXT line is one string, and the parked TXT pipeline ([txt-pipeline.md](txt-pipeline.md)) would
give TXT paragraph bidi; Markdown stays out of it. No design yet.

## Myanmar

The follow-ups are in the Myanmar design (`docs/superpowers/specs/2026-11-09-myanmar-design.md`, "Later, if wanted",
the decisions and open questions 4 and 8): clusters in the engine, Stribley's two weights, the justified-space cap, a
dictionary, ZWSP as a break, `CMD:TTF:SHAPE` for Myanmar (`shapeWord` in `src/SerialTestApi.cpp` finds Indic and
Arabic runs only), Mon and Shan text under their own language systems.

## Flash (low priority)

#868 reports the `default` build at 6,178,901 B of the 6,553,600 B app slot. Candidates from the shaping reviews, each
small:
- one shared controls and joiners filter (150-300 B);
- `ParsedText::viewWithoutControls` folded into `viewShapedPieces(..., true)` (300-400 B);
- one helper for the soft-hyphen handling around `copyWithoutSoftHyphens` in `formBidiPieces` and `viewShapedPieces`;
- one struct for the piece vectors of `ParsedText::BidiState` and `ShapingState`.

## Not planned

Each waits for a request:
- **Khmer and Tibetan**: each needs a shaper module of its own; `OtUse` covers the Indic blocks only.
- **Shaped runs in a vertical section**: a load refuses one, so a vertical section reorders Indic words only
  ([text-shaping.md](../text-shaping.md#what-is-not-shaped-yet)).
