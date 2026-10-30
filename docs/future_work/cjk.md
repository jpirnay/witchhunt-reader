# CJK support

Status: open items as of 2026-10-29.

Line breaking for Chinese, Japanese and Korean is done, and so is the typography: punctuation widths, flush
justification and the whole-em grid (A3a), push-in and the CJK–Latin gap (A3b), ruby (B1) and emphasis dots (B2).
The font engine is done too: TrueType and CFF (`.otf`) outlines, families too large for flash read from the card, and
a glyph store that makes room. The font manager installs eight CJK families as `.otf`. What remains for full CJK
support:
- **UI text:** shipped (#508 the UI faces, #563 book titles, TOC and the status bar in the reader's font); a
  scratch block for the reader's own passes, the held set's heap and a CJK keyboard remain.
- **The dictionary** on CJK pages: shipped; deinflection, word-by-word selection and the search time on
  two-file dictionaries remain.
- **Small edge cases** the reviews found.

## Roadmap

In order. Each step is its own plan and PR. Steps 1-5b are the CJK fonts spec's order of work
(`docs/design/cjk-ttf-fonts.md`, section 13), in the order they run: synthetic bold moved
behind CFF.

| Step | What | State |
|---|---|---|
| 1 | Serial test API for the font work (`CMD:TTF`, `CMD:FS`) | shipped (#531) |
| 2 | The glyph store makes room (spec section 8) | shipped (#532) |
| 3 | CFF outlines (spec section 9), and their speed-ups | shipped (#542, #544) |
| 4 | Synthetic bold (spec section 7): regular-only outline families draw bold, CFF included | shipped (#548) |
| 5 | The CJK families as `.otf` in the font manager (spec sections 4-6); the bitmap CJK families removed | shipped (#550) |
| 5b | Synthetic italic (spec section 7.4): families without an italic file draw italic slanted, as KOReader does | shipped (#551) |
| 6 | CJK book titles, authors and TOC in the UI: the reader's CJK font behind the UI faces ([UI text in CJK](#ui-text-in-cjk)) | shipped (#563) ([spec](../superpowers/specs/2026-10-21-cjk-ui-text-fallback-design.md)) |
| 7 | Push-in and the CJK–Latin gap (A3b) | shipped (#575) |
| 8 | Ruby and emphasis dots | B1 shipped (#554, review fixes #556); B2 emphasis dots in #566 ([plan](../superpowers/plans/2026-10-21-cjk-ruby-and-emphasis.md), Tasks 8-10) |
| 9 | The dictionary on CJK pages ([what remains](#dictionary-on-cjk-pages)) | shipped (#570, #569, #574) |
| 10 | Line spacing: a CJK em-pitch scale, Extra wide and Widest, Book's style, a per-book override ([below](#line-spacing)) | shipped (#562) ([spec](../superpowers/specs/2026-10-26-line-spacing-design.md), [plan](../superpowers/plans/2026-10-26-line-spacing.md)) |

Synthetic bold comes before the font manager because the CJK families ship in their Regular weight only (spec
section 4). Step 6 is what makes a CJK library navigable once a font is installed.

## Already shipped

Listed here so nobody redoes it.

**#481 (A1): token boundaries.** Every word carries a boundary record (`TokenBoundary::Boundary`: continues, no space before), so a break can be a zero-width CJK gap.

**#497 (A2): CJK line breaking.**
- **Tokens and kinsoku.**
  - A CJK word becomes one token per allowed break (`ParsedText::addWord`, `CjkLineBreak::breakByteOffsets`).
  - Kinsoku works by gluing tokens. The sets are upstream's plus crengine's defaults, plus the straight quotes (never broken next to CJK, UAX #14 LB19), U+00B7 and U+3000 (`CjkLineBreak::isNoLineStart` / `isNoLineEnd`).
- **Hyphens and kerning.** No hyphen and no kerning next to CJK (`Hyphenator`, `EpdFont::getKerning`).
- **Korean.** Words stay whole, and split between syllables only at a line end with hyphenation on (`CjkLineBreak::hangulLineEndBreaks`).
- **The parser** (`ChapterHtmlSlimParser`):
  - a 32-token run flush;
  - a per-token table budget charge with an early degrade for long CJK cells;
  - the CSS Text 3 segment-break rule. A source newline between CJK characters, or next to a quote in CJK text, vanishes. It is decided when the word is flushed, and only inside inline formatting.
- **Section version** 92.
- **The dictionary** releases the font caches when a CJK page leaves no room (`DictionaryWordSelectActivity`).

**#500: page glyph arena.**
- An SD font's per-page glyph arena (25-31 KB on a dense CJK page) is freed once the page is on the panel.
- The pre-render of the next page prewarms 1-bit ink masks instead of 2-bit glyphs: half the bytes, the same black-and-white pixels.
- **Effect on the X3:** a dense Chinese page turns in ~503 ms instead of ~678 ms, and reading leaves ~53 KB free instead of 15-21 KB.

**#520 (A3a): punctuation widths, flush justification, the whole-em grid.**
- **A profile per book language** (`TypographyProfile::forLanguage`, from the OPF `dc:language`): how lines break and justify, the punctuation table, the grid, the inline direction and the hyphenator. It travels `Section` → `ChapterHtmlSlimParser` → every `ParsedText`.
  - Traditional Chinese is `zh-Hant`, `-TW`, `-HK`, `-MO` and `lzh`; an explicit `-Hans` wins.
  - The direction is data only: every book is laid out left to right, horizontally (spec section 4).
- **Punctuation widths** (`CjkPunctuation`), written from W3C JLREQ and CLREQ; crengine's tables were a cross-check, not the source.
  - Every adjustable mark is a token of its own, glued where kinsoku holds it (`ParsedText::addWord`).
  - Mid-line widths are set before line breaking (`applyPunctuationWidths`). The line-edge widths are applied by all four breakers and `extractLine` (`markClass_`, `edgeTrim_`: two bytes a token).
  - A narrowed mark is drawn where its ink sits (`GfxRenderer::getGlyphInk`).
  - A mark is narrowed only when its font draws it full-width, at least 4/5 of the em of 一, so a Latin reader font changes nothing.
  - Simplified Chinese and Japanese narrow most marks next to other punctuation and at line edges; traditional Chinese keeps its centred marks full width.
- **Flush justification.** A justified line with CJK gaps spreads its remainder over the gaps and ends at the margin. Latin lines keep their rounding.
- **The whole-em grid** in Chinese and Japanese books.
  - A block's width rounds down to whole ems at the block's own font size, with half the cut on each side.
  - A first-line indent rounds down to whole ems, at least 1.
  - A block beside a float gets no grid.
- **Section version** 96.
- **Tests and books.**
  - `test/cjk_layout/CjkTypographyTest.cpp` and `test/typography/`.
  - The rule checker places every unspaced CJK character where the widths before it put it, and checks that justified lines with CJK gaps end flush on a whole-em grid (`CjkJustifiedRules`).
  - `test_cjk_zh_punctuation.epub`, generated by `make_test_cjk_zh_punctuation.py`: simplified-Chinese punctuation in runs and at line edges.
- **The design:** `docs/design/cjk-typography.md`.

**#554 (B1): ruby (furigana).**
- **Parser** (`ChapterHtmlSlimParser`): `<ruby>` flushes and continues glued, `<rt>` collects the reading for the base tokens since the group start, `<rp>` is skipped in layout. A reading is capped at 255 bytes. No token-run split inside a group. `<rt>` and `<rp>` text still count toward visible offsets, so KOSync is unchanged.
- **Tokens and groups.** A group is `{firstWord, wordCount, text}` on token boundaries (`ParsedText`, `TextBlock`). `TextBlock`'s `hasSizes` byte is a flags byte (bit 1 = ruby); groups are stored only on lines that have ruby, so books without it cost nothing.
- **Widths.**
  - Ruby is drawn at half the base size (`getTextWidthScaled` / `drawTextScaled`).
  - A reading wider than its base overhangs, centred, over neighbouring kana and punctuation only, never over kanji.
  - JLREQ 1:2:1 spreading applies only when both the reading and the base are all CJK; any other pair is centred.
  - The margins (line-edge protection) are applied by all four breakers and `extractLine`.
- **Taller lines.** A line with ruby is taller by half an ascender: in `addLineToPage`, in table rows and cells, in `TextBlock::wordBox` (the dictionary boxes the base, not the ruby) and in link areas. Other lines keep their height.
- **Section version** 99.
- **Tests.** `dumpPage` prints the ruby groups, so the goldens see them; the rule checker checks ruby spacing and margins; stray ruby readings keep their spacing, pinned by x positions.
- **Ruby: known limits** (from the final review).
  - A reading is measured with kerning (`getTextAdvanceX`) but drawn one character at a time without it, so a Latin gloss with kern pairs can sit 1-2 px past the margin. Fix: measure the unkerned sum, or draw the reading in one call and spread per character only for 1:2:1.
  - A reading over a word in another font family (publisher fonts on) is placed by that word's ascender, but the band is half the block font's ascender: with a taller family it can rise above the band into the line above. Fix: clamp `rubyY` to the line's top.
  - `room()` takes Latin curly quotes (U+2018, U+2019, U+201C, U+201D) as punctuation a reading may overhang, though `applyPunctuationWidths` leaves them out as narrow Latin glyphs: a pixel or two over an apostrophe. Fix: ask `markClass_` instead of `CjkPunctuation::classOf`.

**#566 (B2): emphasis dots (傍点).**
- **CSS** (`CssParser`): `text-emphasis-style` and its `-epub-` and `-webkit-` forms, and the `text-emphasis` shorthand. The style is a mark: dot, circle, double circle, triangle or sesame, filled or open; `none` clears it. `CSS_CACHE_VERSION` 31.
- **Parser to page.** The mark travels as bits of the token boundary (`TokenBoundary::withEmphasis`), through splits and erases, and the parser marks every token of a span, links and a multi-word ruby base's kept space included. A run of tokens with one mark becomes one `Emphasis` annotation on its line, outside ruby groups (ruby wins over a mark on the same base).
- **Drawing** (`TextBlock`): one mark above each character that takes one (`TextEmphasis::takesMark`: not punctuation, not spaces, not a soft hyphen), centred, at half size, through the ruby box path, with no allocation. A run with no such character gets no record and no band. A wrapped span is marked on both lines.
- **Taller lines.** A line with a mark gets ruby's band, so marks never touch the line above.
- **Section version** 102.
- **Tests.** `test/cjk_layout/CjkEmphasisTest.cpp`, `RubyTextBlock.DrawingMarksAllocatesNothing`, the emphasis cases of `RubyLayoutTest`, the CSS tests, and case 16 of `test_cjk_line_breaking.epub` (the golden shows the `E` records).
- **Follow-up:** `<em class="sesame_dot">` with `font-style: normal`, Aozora Bunko's own markup, stays italic: the `<i>`/`<em>` tag path sets italic whatever the CSS says (`ChapterHtmlSlimParser::startElement`). Honouring a CSS `font-style: normal` there is a maintainer policy call, since `<b>` deliberately lets the tag decide its weight. Span-based markup (AozoraEpub3's `em-sesame`) is unaffected.
- **Follow-up:** fullwidth ＃％＆＊／＠＼＿ (U+FF03, FF05, FF06, FF0A, FF0F, FF20, FF3C, FF3F), the CJK Compatibility Forms U+FE30-FE4F and ～ U+FF5E still take a mark, while their ASCII forms and 〜 do not (`TextEmphasis::takesMark`). ＊ in a "＊　＊　＊" break inside a marked span is the likeliest to show.

**#570, #569, #574 (step 9): the dictionary on CJK pages.**
- **The run** (`CjkLookupRun`). From the cursor's character: up to 12 code points across words and line ends, stopping at the first one that is not unspaced CJK or that `cleanWord` strips as punctuation; 々, 〆, ー and kana kept. On Hangul, the token itself. Each code point remembers its word, so a match length maps back to a span of words.
- **The search** (`Dictionary::resolvePrefixes`).
  - Up to three headwords that are prefixes of the run, longest first, from the `.idx` and the `.syn` (a tie goes to the `.idx`), each with its run of same-headword entries (at most 8). None of `resolve()`'s fallbacks. It gives up between probes when a button press is waiting (speculation only).
  - One descent per index file; a later, shorter target steps back from the window it left (`gallopSamples`). Sidecar samples are read in 512-byte blocks through whichever scan buffer is idle. A probe reads its window's first headword from its own scan, no `.syn` probe runs whose every hit the merge would drop, and a followed synonym finds its run in its own window (`followSynonym`).
- **Definitions** (`readPrefixDefinitions`). Every entry of the longest match, then the shorter matches, in one buffer sized once within `MAX_DEFINITION_BYTES`. Drawn in the reader's font (`definitionFontId()`), each entry under its headword; the header keeps the dictionary name and page counter. Latin lookups are unchanged.
- **The word screen.** The highlight grows over the match with no extra press (speculation), Confirm looks it up, and the span is drawn on the differential path (2 runs, 2,048 bytes) or by the full repaint. A tap inside the span looks it up.
- **#569 (H1):** the word arrays and the snapshot are freed while a definition is open and rebuilt on return (8 ms for 232 words), +9.2 KB free heap at the definition's entry on the X4 with 12 KB of ballast. **#570 (L1):** the prewarm arena retry logs no error when it recovers.
- `CMD:ACT:DICT` reports the run, the hits, their entries, probes, reads and both timings (`docs/serial-test-api.md`).
- **No version changes.** `gh_release` flash +9,044 B, static RAM +0 B (`firmware_size_history.py`, against master).
- **Tests.** `test/dictionary/DictionaryTest.cpp` (the prefix search and its read counts, the definitions), `test/dictionary/CjkLookupRunTest.cpp`.
- **The user's side** is in `docs/dictionary.md` ("Chinese, Japanese and Korean"): how it works, its limits, the PyGlossary conversions, sizes and first-open times. The design spec was deleted once its lasting parts were there; the plan stays (`docs/superpowers/plans/2026-10-23-cjk-dictionary.md`).

**#575 (A3b): push-in and the CJK–Latin gap.**
- **Push-in** (spec 5.3). A line one token too long first shrinks, in this order: the marks at its two edges, each all or nothing (spec 5.2: "half or full"), then every other mark down to its minimum width, then the CJK–Latin gaps. Only if that is not enough does it break earlier and stretch. Western word spaces do not shrink, so Latin lines do not move.
  - All four breakers test a line at its pushed-in width (`pushedInWidth`): the DP breaker (`computeLineBreaks`) and the three greedy ones, with the quantities `extractLine` uses, so a line a breaker accepts pushed in is one `extractLine` pushes in. `extractLine` takes the need tier by tier, shares each tier in line order, and aligns and justifies the line by its pushed-in width.
  - **The DP cost.** A line that fits only pushed in, the last line too, costs (1 + badness of its overrun)² + 1 (`kPushInPenalty`). An exact line costs 1, a pushed-in one at least 2, a stretched one with any badness at least 4: push-in wins against a loose line (about 1.3 em of slack on 28) and loses to a near-exact one. A paragraph with nothing that can give way breaks as before.
  - What each mark may give, mid-line and at a line edge, is layout scratch, two bytes a token (`shrink_`, `edgeShrink_`, set by `applyPunctuationWidths`). Nothing new in the section file.
- **The CJK–Latin gap** (spec 5.4). A quarter of the CJK side's em, `(em × 2 + 4) / 8` px, between Han or kana and a Latin letter or digit with no source space between them (`CjkPunctuation::takesCjkLatinGap`, `ParsedText::cjkLatinGapBefore`), in Chinese and Japanese books only (`TypographyProfile::cjkLatinGap`).
  - None next to punctuation, full-width letters, half-width katakana or Hangul, and none in a font without CJK glyphs.
  - It stretches with justification, keeps half of itself under push-in, and is dropped at a line break. It is not a break opportunity of its own.
- **The Japanese minimums** (JLREQ). 。 and ！？ keep their full width mid-line under push-in; 、 may halve; at a line end 。、 and a closing bracket are half or full.
- **Ruby gives nothing to push-in.** A ruby's overhangs and line-edge reserves stay as they are, a mark inside a ruby group does not shrink, and a mark a ruby overhangs shrinks only as far as the overhangs leave it room (`applyRubyWidths`).
- **Section version** 103.
- **Measured cost.** Cold section build on the X4 against master: total 0..+2 %, layout (`parse`) +2..+8 %; page turns unchanged.
- **Tests and books.**
  - `CjkPushIn` and `CjkLatinGap` in `test/cjk_layout/CjkTypographyTest.cpp`, both breakers; the minimums in `test/typography/`.
  - The rule checker lays every line out again from its dump, push-in included (`CjkRulesTest`, `LineLayout`): no line is wider than its text width, a line that shrank could not have kept its natural widths, and a line that was not pushed in holds the CJK–Latin gap exactly.
  - Cases 17 (CJK–Latin boundaries) and 18 (a line that fits only pushed in) of `test_cjk_line_breaking.epub`.
- **Follow-up:** guide dots are drawn at Han–Latin gaps. `TextBlock::render` dots every gap of at least the dot size + 2 px, and the CJK–Latin gap is 4-5 px at reading sizes, so a reader with guide dots on sees a dot between 漢字 and iPhone. Guide dots mark word spaces; the gaps between CJK characters are too narrow for one. Next step: dot only the gaps that were a source space.
- **Follow-up:** a re-flowed line can end with ！ or ？ followed by an ideographic space U+3000 (case 1 of the fixture, "！　"). Typesetting practice sets no space there, and no section-5 rule covers it: a line-end U+3000 after ！？ could be dropped or hung.
- **Follow-up:** the rule checker's justified line-end check stays either-way (pushed in or not) on lines with ruby, word spaces or glued Latin, about a quarter of the corpus's justified lines. Restating those lines' widths exactly would let the check be strict on every line (`CjkRulesTest`).
- **Follow-up, Latin scratch:** the four mark vectors (`markClass_`, `edgeTrim_`, `shrink_`, `edgeShrink_`) are allocated for Latin paragraphs too: +524 B on test_table_streaming's peak, from table cells keeping their scratch. The fix: gate all four together on "the block holds a CJK mark", or `releaseLayoutScratch()` after each cell's layout.
- **Follow-up, the gap's lookups:** `cjkLatinGapBefore` decodes two codepoints at every breakable CJK boundary and calls `cjkEm` per gap. A per-block "has a Han/kana-Latin boundary" flag or a per-(font, style, scale) em cache would remove them.

**Tests and books.**
- `test/epub_pipeline/CjkRulesTest.cpp`, the rule checker, lays out every line and asserts kinsoku, no hyphen, and no stray space between CJK characters.
- The books it runs on are in `test/epubs/`:
  - `test_cjk_line_breaking.epub`, the fixture, generated by `make_test_cjk_line_breaking.py`;
  - `test_cjk_ja_soseki`, `test_cjk_zh_honglou` and `test_cjk_ko_hyeon`, public-domain excerpts generated by `make_test_cjk_excerpts.py`;
  - `test_cjk_block_seams`, list items and `dl` after CJK text.
- Unit tests are in `test/cjk_line_break/` and `test/cjk_layout/`.

**The design.** The full design for everything below is `docs/superpowers/specs/2026-10-16-cjk-layout-design.md` on branch `docs/cjk-layout-design` (commit `7e1136008`). It is working material, not on master.
- Section 7 is B, Appendix A the character sets. Section 6, the first A3 design, is replaced by the A3 design record (`docs/design/cjk-typography.md`).
- Section 3 explains each upstream-versus-KOReader choice.
- Upstream is crosspoint-reader `upstream/develop`. Port by hand: the histories are unrelated.

## A CJK font readers can install

**Shipped** (#550). The font manager installs eight CJK families as `.otf`: NotoSans and NotoSerif in JP, SC, TC and KR,
1.9 to 7.0 MB each (9.7 MB sans, 17.3 MB serif), drawn by the TTF engine. They are cut by
`lib/EpdFont/scripts/cjk_subset.py`, listed in `sd-fonts.yaml` and `assets/sd-fonts/fonts-v3.json` (manifest version 3).
"Download all" leaves them out. The design is `docs/design/cjk-ttf-fonts.md`, sections 4-6.

## CJK `.cpfont` fonts converted by hand

**Open.** Two bugs in the bitmap path, found while building the CJK families. Neither touches the shipped `.otf`
families; both hit a CJK font that a reader converts to `.cpfont` and installs.

1. **The interval table is copied to the heap.**
   - **What:** `SdCardFont::loadFromMmap` copies the interval table out of the flash mapping. For Japanese that is 49 KB
     of heap, and it turned pre-rendering off.
   - **Why:** `loadFromMmap` has no in-place path for the table: it copies it into a heap array per style
     (`fullIntervals`) because the table may sit unaligned in the mapping.
   - **Where:** `SdCardFont::loadFromMmap`. `MAX_INTERVALS` is 4096; the Japanese set has 4,083.
   - **Next step:** read the table in place: when it sits 4-byte aligned in the mapped partition, point
     `fullIntervals` at it, and keep the heap copy for an unaligned one. Then check that a Japanese `.cpfont` keeps
     pre-rendering on.
2. **A `.cpfont` larger than the partition erases the whole partition.**
   - **What:** on the first book open after every restart, the partition is erased (4-5 s), then the append fails.
   - **Why:** the single-file path in `SdCardFontManager` calls `FlashFontPartition::beginWrite`, which erases, before
     `appendFile` checks that the file fits.
   - **Where:** the function that caches a family into flash, `lib/EpdFont/SdCardFontManager.cpp`: its single-file
     fallback after the whole-family check;
     `FlashFontPartition::beginWrite` and `appendFile` (`lib/hal/FlashFontPartition.cpp`).
   - **Next step:** compare the file with `FlashFontPartition::fontUsableSize()` before `beginWrite`, and remember the
     refusal so the next open does not try again.

### 龘 in Noto Serif TC

**Open.** U+9F98 has 621 points, over the CFF scratch cap of 512 points, and draws as missing in either outline format.
A larger block for every family is not worth one glyph. A Hong Kong or full-set family would need it.

**Next step.** Raise the cap only if such a family is added.

## Font Manager follow-ups

**Open.** Left out of the CJK families' round (`docs/design/cjk-ttf-fonts.md`, section 6.5):
- **Uploading a `.ttf` or `.otf` through the web page as it is.** The upload takes `.cpfont` only, and the Convert card
  needs its own choice for it.
- **A family installed but not listed.** The web page's installed list comes from the registry, so such a family is
  missing from it and only the device can delete it.
- **The update rule has two accepted limits** (`FontDownloadRules::isUpdate`). A reader's own file with a manifest file
  name but other bytes counts as an update and is replaced. A hosted family whose files are all renamed is never offered
  an update, so hosted files are never renamed: a renamed set ships as a new family.
- **Resume works per file.** An interrupted one-file family (every `.otf`) restarts from zero.
- **The card's free space is cached for 20 s.** The SDK caches the used bytes
  (`freeink-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp`) and a remove or a write does not invalidate the
  cache. A Retry right after deleting a family, or the second family of a "Download all", can be judged on the old
  figure; a batch can then run the card full mid-download (a loud failure, recoverable by resume).

**Where.** `FontInstaller`, `FontDownloadActivity`, the `/api/fonts/*` handlers and the font page.

**Next step.** Take them one at a time. For the last, invalidate the cache in the SDK's remove, removeDir and write paths.

## TTF Stage 2c-4: TrueType families too large for flash mode

**Shipped** (#506): a family too large for flash mode keeps its tables in the glyph store and reads its outlines from
the card (SD mode); CFF families do the same since #542. Font-selector previews for TTF families were assigned to this
stage (TTF spec section 7.6); check the font selector before planning them.

**Shipped: the glyph store makes room.** The store's log is a ring of segments. A full log reclaims its oldest segment
(keeping the glyphs drawn most) and a worn index is rebuilt, in idle time (`EpubReaderActivity::stepGlyphStoreRoom`) and
at a chapter or book open, instead of wiping the store. Format 4: [file-formats.md](../file-formats.md#the-ttf-glyph-store-cptt).

**Measured** on the X4 at 12 pt, Traditional Chinese in SD mode (`test_cjk_zh_honglou`): a cold chapter lays out in
1,114 ms as TrueType and 1,159 ms as CFF; with its glyphs stored, in 315 ms either way (#544).

## Glyph store: a faster index rebuild

**Open.** An index rebuild takes about 1.8 s on the X4 (Noto Sans TC at 12 pt: about 9,400 records, 16,384 slots), and
in idle time it holds the loop task that long, so a key pressed then waits. With CJK at 12 pt the index fills before the
log, so while many new glyphs arrive (the store-fill test book, `tools/make_cjk_store_fill_epub.py`) a rebuild comes about
every five reclaims; a reclaim itself takes about 150 ms.

**Ideas**, cheapest first:
1. **Size the index for CJK records.** `planLayout` sizes an SD-mode index for 250 B a record (`kExpectedRecordSd`); at
   12 pt a CJK record is about 150 B, so 70% of the index is reached before the log is full. Sized for 150 B, the log
   fills first and reclaims make the room. Cost: index pages from the mapping budget (one 64 KB page per 8,192 slots),
   so a little less log.
2. **Program the index a sector at a time.** Each re-inserted slot is one 8-byte program call today (about 100 µs of
   overhead each on the X3, so about 1 s of the 1.8 s). A RAM bitmap of occupied slots (2 KB for 16,384) gives every
   record's slot without writing; one log walk per index sector fills a 4 KB buffer that is programmed whole: 32 program
   calls instead of about 9,400. Estimated 0.5-0.8 s.
3. **Rebuild more at opens.** Lower the open-only stale share (`kRebuildStalePercent`, 12%) so more rebuilds run behind
   the chapter-open popup and fewer in idle time.
4. **An A/B index rebuilt in slices.** A second index region filled across idle ticks while the first serves lookups,
   switched by a journal entry: no long block at all. Cost: a second index's flash and mapping pages.

**Next step.** Measure how the 1.8 s splits between the index erase, the log walks and the slot programs
(`FlashGlyphStore::rebuildIndex`), then take 1 and 2; the plan is
`docs/superpowers/plans/2026-10-21-glyph-store-faster-rebuild.md`.

## CFF outlines (`.otf`)

**Shipped** (#542 and #544). The TTF engine draws CFF outlines as well as TrueType: a CFF parser and charstring interpreter in `lib/EpdFont/ttf/`, in flash mode and in SD mode (the charstring data read from the card). Font discovery lists `.otf` files and reads the content, so a file with an `OTTO` header is a CFF face whatever its name. Publisher Fonts takes a book's embedded CFF faces too, copied under their usual `<style>.ttf` names. Tested with CJK `.otf` files: subsets and a desubroutinized NotoSansTC (3.45 MB), in SD mode on the X4. Full stock files (NotoSansCJKjp-Regular.otf, which would need an SD-mode block of about 2.2 MB) are untested.

Still refused: variable CFF2 fonts and font collections.

## UI text in CJK

**Shipped** (#508 the UI faces, #563 book text). Chinese, Japanese and Korean UI translations ship with
their own small UI faces: each holds the characters its translation uses, at 10 and 12 pt, attached
behind every UI face as fallback glyphs (`src/CjkUiFonts.cpp`, `docs/i18n.md`). Wrapped UI text breaks
between Chinese and Japanese characters. The build fails when a translation uses a character no UI
face has.

The faces ship as 1-bit DEFLATE groups, 97,132 B where they took 175,916 B flat
(`compress_ui_face.py`). The active UI language's two faces are inflated into its language pack
slot and drawn from flash with no decode (`docs/i18n.md`); the other two languages' glyphs, for the
language picker and CJK book titles, decode one at a time, ~1.9 ms each on the X3.

**Text that is not a UI string** (book titles and authors, TOC entries, the status-bar chapter title, file names, the
finished-book screen) draws what the UI faces lack from the reader's TrueType font, at the CJK UI faces' em
(`CjkUiFonts::setReaderFont`, `FallbackFaceProvider::setReaderFace`):
- in the reader, from the loaded set's first family with CJK (else its first), lent by `SdCardFontManager` on every
  load and body size, and taken back before its native sizes go;
- outside it, on the screens that draw book text (`Activity::drawsBookText`), from the last book's set, opened
  read-only by the first string that needs it (never installed or wiped from the UI), only when the glyph store holds
  it and its body family has CJK, kept from one such screen to the next, and closed on any other (`syncUiBookFonts`):
  a screen's cover work before it draws never has it on the heap;
- per string: every text call of `GfxRenderer` tells the provider which string comes next (`beginText`). One with a
  character neither the UI face nor the active language's face has is book text (the build gives every translation's
  characters a face): the reader's font goes first for all of it, so a title draws in one font, and what the UI face
  lacks is rendered in one batch;
- a book-text screen's first-sight glyphs draw in the secondary framebuffer when the heap could not hold their scratch
  with 24 KB to spare (the reader menu): lent on the pass's first miss with the black-and-white sequence (RED seeded,
  single-buffer fast diff) and returned as the frame is about to be pushed (`lendFramebufferForGlyphs`,
  `GfxRenderer::setBeforePushHook`). A lend costs the X3 a slower refresh, so screens with room keep the heap;
- its 2-bit glyphs drawn as their ink; its UI-size faces made on the first glyph a screen needs.

Without a CJK TrueType family nothing opens, and the boxes (□) stay. Before the CJK faces these strings drew nothing
(X3, 2026-10-18, 紅樓夢); without the boxes the Home card showed a lone 雪 of the author 曹雪芹 as if it were the title.

**Still open:**
- the reader's own passes (its status-bar title) keep their miss scratch on the heap: the reader holds the secondary
  for its grayscale passes. A scratch block planned for the UI's sizes (~9.4 KB against 17.4 KB) would halve it;
- the set held while a book-text screen is current costs ~4 KB (Home after the Library), more than the spec's 2-3 KB;
  opening it per screen instead cost 35-100 ms a visit, which weighed more;
- the reader menu's 2.42 s opening refresh on the X3 is its anti-ghosting HALF (a full resync and a conditioning pass
  on the UC8253), not this work: a FAST would do when no grey is on the glass.

**Smaller items:**
- a CJK keyboard: none of the three has one, so text entry stays Latin;
- bold UI text draws CJK in the regular weight (a synthetic bold would be a 1 px double strike);
- the large UI step draws 12 pt CJK under 14 pt Latin headings; a 14 pt CJK size costs ~125 KB flat,
  ~60 KB compressed, and its active-language faces would need about another 64 KB page of the
  language pack slot;
- the interval tables (15,648 B for the three languages) are nearly one interval per glyph; a sorted
  codepoint list would save ~12 KB;
- 67 Han characters at 10 pt and 92 at 12 pt are byte-identical in the Chinese and Japanese faces
  (3,080 B and 5,910 B flat); dropping them from the Japanese faces would draw the same pixels from
  the Chinese ones, but a Japanese UI would then decode those per glyph, outside its language pack;
- the translations are machine-made and unreviewed (`docs/translators.md`).

## Dictionary on CJK pages

Shipped in step 9 ([above](#already-shipped)). What remains, measured on the X4 with CC-CEDICT, JMdict_e and
CC-KEDICT (`docs/dictionary.md` has the conversions):

- **Search time on dictionaries with a large `.syn` (follow-up: a JMdict speed-up).** The maintainer's gate is the
  median of `ms / exact_ms` over 50 words per book (`CMD:ACT:DICT`), at most 1.5. Hong Lou Meng with CC-CEDICT: 1.46
  (four runs, 1.46-1.47; 2.69 before the five speed-ups of #574). Soseki with JMdict: 1.95 (3.66 before them;
  median 37 ms against 20 ms for the exact lookup), not met; the maintainer accepted it for #574 (2026-10-29),
  JMdict being best effort, and a JMdict speed-up is the follow-up. A CJK search reads both index
  files: two descents, two window scans, the `.syn` and `.sidx` opened (6 ms), and a follow per synonym hit; the
  exact lookup of the matched headword reads the `.idx` once. JMdict files every kana reading in its 15.8 MB `.syn` (550,503 entries,
  2,151 samples, ~7 KB windows), so kana text always takes both. Measured costs on the X4: a 4-byte sample read
  0.46 ms, a 512-byte refill 0.47 ms, a sample block 0.65 ms, opening the `.syn` and `.sidx` 6.1 ms, 5.5 ms fixed.
  What would close the gap needs a sidecar change: a map from each `.qidx` sample to its `.sidx` sample (no second
  descent, ~6 ms off JMdict's median), or a skip test for runs the `.syn` cannot extend (estimated medians: CC-CEDICT
  0.89, JMdict 1.75). Against today's lookup of the matched text as the page writes it (夢幻 rather than 梦幻), the
  estimated medians are ~1.0 (CC-CEDICT) and ~1.55 (JMdict).
- **Deinflection.** None, by the maintainer's decision: 食べた finds 食, 먹었습니다 finds nothing. The PyGlossary
  JMdict build's `.syn` holds kana readings, other kanji spellings and "kanji・kana" pairs, no inflected forms, so
  the `.syn` route does not cover it either. A rule table (kana suffix in, suffix out) would add candidates to the search; it has to be
  clean-room (Yomitan is GPL-3.0, KOReader AGPL-3.0) and reviewed by someone who reads Japanese.
- **Moving the cursor word by word, and a manual range.** The cursor steps one character; the match is always the
  longest. Upstream went with a manual range (#3932).
- **Greedy matches.** Forward maximum matching over the dictionary's headwords, against a segmenter on the same runs
  (word boundaries, F1): Hong Lou Meng with CC-CEDICT 0.74 against `jieba` (0.88 without its HMM, 0.90 without it on the
  text mapped to simplified), Soseki with JMdict 0.77 against `fugashi` and UniDic. JMdict's long entries (吾輩は猫である,
  これと言う) and kana synonyms (はま from はまだ) make most of the Japanese misses; the shorter matches in the
  definition soften them.
- **One headword per synonym.** A `.syn` hit names one `.idx` entry, so a reading several headwords share shows the
  one of them, and which one is not defined (はし showed 嘴 of eight). Listing every ordinal of a hit changes `DictPrefixHit` and the contiguous-entries
  read.
- **The other script in the definition.** CC-CEDICT writes each word in both scripts; a single regional CJK font
  draws the other script's missing characters as U+FFFD (學, 書 with a Simplified Chinese font). The NotoSansCJK
  `.cpfont` family carries both scripts but not pinyin's ē, ǎ, ū (U+0113, U+01CE, U+016B: `SDCF` "onGlyphMiss"), which
  drop out of the reading (dàxuéshng). A fallback face for definition text, as the UI text fallback has, would fix both.
- **Styled definitions in a TrueType reader font, first time round.** In a session whose glyph store did not yet
  hold the definitions' glyphs, two of three CC-CEDICT definitions ran the free heap from 60 KB to 16 KB during the
  styled layout and stopped on its retain floor (`DHTML` "Styled definition stopped on free heap", an error line on a
  path that recovers): they fell back to plain text. With the glyphs stored, and with the `.cpfont` family, all three
  laid out styled (110-240 ms for 1.5-2.6 KB entries).
- **A long CJK definition in an SD-card font, laid out without a prewarm, is untimed.** Only entries of 1.5-2.6 KB
  were measured.
- **The headword back in the header** once the UI text fallback (step 6) has merged: `BookTextScope`,
  `CjkUiFonts::readerFontAttached()` and `prefetchBookText()`.
- **Not run, by the maintainer's decision:** the X3 dense-page measurement and the touch check on the X4 Pro and
  T5S3 (spec row 12). The X4 with `CMD:HEAP:HOLD` ballast stood in for the X3.

**Where.** `Dictionary` (`resolvePrefixes`, `collectPrefixes`, `mergeSynonymHits`, the sidecars),
`CjkLookupRun`, `DictionaryWordSelectActivity`, `DictionaryDefinitionActivity`, `DictHtmlPages`.

## Line spacing

**Shipped** in #562 ([spec](../superpowers/specs/2026-10-26-line-spacing-design.md),
[plan](../superpowers/plans/2026-10-26-line-spacing.md)), after an X3 study of a Japanese, a Chinese and a Korean book
at 1.5, 1.7 and 2.0 em against the old Normal and Wide.
- **The scale** (maintainer, 2026-10-26). Chinese, Japanese and Korean books get a line pitch in ems: Tight 1.5, Normal
  1.7, Wide 1.85, Extra wide 2.0, Widest 2.2. JLREQ and CLREQ set the gap at half to one character (1.5 to 2 em); KLREQ
  gives no figure (its example, and Hancom Hangul's default, is 160%). The old Normal was Noto CJK's own line, about
  1.45 em. Other books keep factors on the font's line height (0.95 to 1.3; 0.90 to 1.2 for the built-in Noto Sans).
- **Book's style** reads each block's `line-height`, inherited as CSS has it, floored at Tight. EBPAJ books declare
  1.75; calibre conversions carry 1.2, which the floor lifts to 1.5 em in a CJK book.
- **A per-book override** in Quick overrides and the reader menu.
- **Vertical text** (writing-modes step 3): the column pitch is the line pitch in the turned frame, so the same steps set it.

**Left for later.**
- The TXT and Markdown readers keep the font's line height (maintainer: not in this round).
- In Book's style, lines that are not a block's (a `<br>` separator's blank line, an `<hr>`'s margin, table rows) keep
  Normal's pitch, and `em` and `%` values are inherited as numbers, which differs from CSS only where a child changes
  font size.

## Page glyph arena follow-ups (after #500)

**Open.**
- **The pre-render floor can underestimate once.** It is keyed on the last page's arena, so a sparse-to-dense page transition underestimates the next pre-render (the safe failure is a skipped pre-render). A running maximum of the arena per font would be conservative.
- **The capture restore keeps an arena.** Screenshot and light drawer (`restoreCurrentPageToBufferIfPreRendered`) rebuild a 2-bit arena and keep it until the next page.
- **The 1-bit pre-render covers plain unscaled pages only.** A page with a heading multiplier, a sized span, sup/sub or small caps pre-renders with 2-bit glyphs, and on a dense CJK page the floor then usually refuses it.
- **No test** covers `SdCardFont::pageArenaBytes()` on the flash-mmap path; it must stay 0.

**Where.** `EpubReaderActivity` (`releasePageGlyphArena`, `preRenderMinFreeHeap`, `pageDrawsUnscaled`), `SdCardFont` (`setInkOnlyPrewarm`, `releasePageArenaIfAtLeast`).

## Smaller edge cases from the reviews

None of these is visible in the test books. Each is a one-line note of the case and the symbol.

**Font files** (found while planning synthetic italic, 2026-10-22).
- `ttf::probeFile` takes a file's style from `head.macStyle` alone, so `OpenDyslexic-Italic.otf` (macStyle 0) is a regular: in a card folder that also holds `OpenDyslexic-Regular.otf`, it serves the regular style, being first by name (`assignStyles`). `post.italicAngle` would tell it apart, as `Face::italic` does.
- `probeFile` reads OS/2's weight class from a table of 6 bytes or more, while `parseFace` ignores an OS/2 table FreeType would (under 78 bytes, or short for its version): a lone heavy face with such a table is emboldened (spec 7.1's third row).

**Line breaking.**
- Combining dakuten U+3099/U+309A, U+302A-302F and ideographic variation selectors U+E0100-E01EF can be split from their base (`CjkLineBreak::boundaryAllowsBreak`). Rare: NFD kana, IVS texts.
- `utf8IsCjkBreakable` misses Ext D-I, the Compatibility Supplement, Katakana Phonetic Extensions, Enclosed CJK, CJK Compatibility and Bopomofo. Halfwidth ｡｢｣､ are not "unspaced", so a source newline after them stays a space. These are faithful to upstream's ranges.
- Project Gutenberg Chinese uses a backtick U+0060 as an opening single quote, and it is in neither kinsoku set.
- A word containing CJK can split at an inner hyphen (`foo-bar`); this is upstream behaviour.

**The parser.**
- CJK text right after a non-inline end tag that does not flush the word buffer (`字</section>\n字`, text directly inside `section`) still glues, because the flush brings `lastTextCodepoint_` back after the end-tag reset.
- A Latin quotation or apostrophe that ends a source line glues to the CJK line after it (`He said "hello"\n字`). The quote-seam rule could require CJK before the quote.
- A table-row degrade drops one pending segment-break decision, so one source wrap in that cell becomes a space.
- The nested `</table>` handler clears the word buffer without resetting `partWordCjkCodepoints_`. Extremely rare: a stale count glues the next CJK character.
- A dash inside a non-CJK run can add a token the run flush does not count.
- A ruby group wider than a whole line (forced split) overhangs the right margin (narrow table cells, floats).
- The table SSO surcharge is computed on the unsplit word, about a 6 % overcount (the safe direction).

**Tokens.** In-word token offsets are computed after `utf8NfcNorm`, so tokens after a composed jamo or a skipped U+FEFF sit a few bytes short. This affects KOSync offsets inside one word only.

**Tests to add.**
- A table-driven kinsoku test over all of Appendix A.1 (the current test samples ~13 of ~70 codepoints).
- `Hyphenator` on a mixed word ("ab漢字").
- `addWord` cases: a single CJK character, an all-punctuation word, and the heap-gate refusal.
- A Hangul word next to the new quote entries.
- `CjkRulesTest`'s `raw.substr(raw.find_first_not_of(' '))` throws on a blank dump line.
- The `CjkBlockSeams` dl and figure assertions run only when both words share a line.

## Out of scope so far

These were set aside in the layout spec (section 3) and the A3 design (`docs/design/cjk-typography.md`, section 10). Now that A3b has landed, each is a candidate for its own plan:
- CSS `line-break` / `word-break` parsing;
- `lang` attributes inside a book: the book's language decides, and there is no detection from the content;
- last-line grid continuity, and even line height in ruby paragraphs;
- the special indent of an opening bracket at a paragraph start;
- hyphenating Latin words inside CJK books;
- `<rtc>` and ruby below the text;
- `text-emphasis-position`: Chinese marks below the text (marks are always above);
- hanging punctuation;
- upstream's Latin dash split (#3883);
- vertical text, `writing-mode` and `page-progression-direction`: no longer out of scope: vertical text is built (writing-modes step 3), the rest is planned in [writing-modes.md](writing-modes.md).

## Testing on a device

**Fonts.** Install a CJK family from the Font Manager. For a branch that is not merged, use the scratch-manifest
recipe of Task 10, Step 1 in `docs/superpowers/plans/2026-10-21-cjk-fonts-font-manager.md`. Set the build flag from
PowerShell: Git Bash rewrites the `http://...:8000/...` value as a path list. The working form is
`$env:PLATFORMIO_BUILD_FLAGS = '-DFONT_MANIFEST_URL=\"http://<PC address>:8000/fonts-v3.json\"'`, then
`pio run -e default -t upload --upload-port <port>` from the same PowerShell. Rebuilding the families
needs `pip install fonttools==4.63.0`.

**Driving the X3.** Use the serial test API (`docs/serial-test-api.md`, `tools/devctl.py`):
- **Choose the font:** `CMD:SET:fontFamily=<index or name>`. On the boot straight into USB Transfer the family keys are refused; leave that screen first.
- **Open a page:** `CMD:ACT:READER:/books/test_cjk_zh_honglou.epub@1:1`, then `CMD:IDLE`, `CMD:BTN:RIGHT`.
- **Measure:** `CMD:STATE` reports the heap and the largest block.

**Page-turn timing.** Read it from the log:
- **The turn starts:** `PageTurn stats`.
- **The panel update starts:** `triggerDisplay`, or for a pre-rendered page `displayBuffer ... took` (subtract the time it took).
- **The page is complete:** `Deferred AA`, or "after the AA replay" for a pre-rendered page.
- **Pre-render outcome:** "Pre-rendered page ... lowest" gives the pre-render's heap low point; "PreRender skipped: free=... < floor=..." says why one did not run.

**Who reviews.** The maintainer does not read CJK. Screenshots (`CMD:SCREENSHOT`) are checked against the kinsoku and spacing rules, and the host rule checker is the gate.
