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
- **Plain text:** a `.txt` gets none of this yet, and only UTF-8 is read: [txt-pipeline.md](txt-pipeline.md).
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
| 8 | Ruby and emphasis dots | shipped: B1 (#554, review fixes #556), B2 emphasis dots (#566, 2026-10-28) ([plan](../superpowers/plans/2026-10-21-cjk-ruby-and-emphasis.md), Tasks 8-10) |
| 9 | The dictionary on CJK pages ([what remains](#dictionary-on-cjk-pages)) | shipped (#570, #569, #574) |
| 10 | Line spacing: a CJK em-pitch scale, Extra wide and Widest, Book's style, a per-book override ([below](#line-spacing)) | shipped (2026-10-26, on master directly: `3bdeb993c`, `2c14d8ae9`, `4d0c4c526`, `54b88fe7f`) ([spec](../superpowers/specs/2026-10-26-line-spacing-design.md), [plan](../superpowers/plans/2026-10-26-line-spacing.md)) |

Synthetic bold comes before the font manager because the CJK families ship in their Regular weight only (spec
section 4). Step 6 is what makes a CJK library navigable once a font is installed.

## Follow-ups of the layout work

**Open.** What shipped, and how it is built, is in `docs/design/cjk-typography.md`, section 11: token boundaries
(#481), line breaking (#497), punctuation widths and the grid (A3a, #520), push-in and the CJK–Latin gap (A3b, #575),
ruby (B1, #554, review fixes #556) and emphasis dots (B2, #566). The page glyph arena (#500) is under its follow-ups
below, and the dictionary on CJK pages (step 9) under its own section.

**Ruby (B1)**, known limits from its final review:
- A reading is measured with kerning (`getTextAdvanceX`) but drawn one character at a time without it, so a Latin gloss with kern pairs can sit 1-2 px past the margin. Fix: measure the unkerned sum, or draw the reading in one call and spread per character only for 1:2:1.
- A reading over a word in another font family (publisher fonts on) is placed by that word's ascender, but the band is half the block font's ascender: with a taller family it can rise above the band into the line above. Fix: clamp `rubyY` to the line's top.
- `room()` takes Latin curly quotes (U+2018, U+2019, U+201C, U+201D) as punctuation a reading may overhang, though `applyPunctuationWidths` leaves them out as narrow Latin glyphs: a pixel or two over an apostrophe. Fix: ask `markClass_` instead of `CjkPunctuation::classOf`.

**Emphasis dots (B2):**
- `<em class="sesame_dot">` with `font-style: normal`, Aozora Bunko's own markup, stays italic: the `<i>`/`<em>` tag path sets italic whatever the CSS says (`ChapterHtmlSlimParser::startElement`). Honouring a CSS `font-style: normal` there is a maintainer policy call, since `<b>` deliberately lets the tag decide its weight. Span-based markup (AozoraEpub3's `em-sesame`) is unaffected.
- Fullwidth ＃％＆＊／＠＼＿ (U+FF03, FF05, FF06, FF0A, FF0F, FF20, FF3C, FF3F), the CJK Compatibility Forms U+FE30-FE4F and ～ U+FF5E still take a mark, while their ASCII forms and 〜 do not (`TextEmphasis::takesMark`). ＊ in a "＊　＊　＊" break inside a marked span is the likeliest to show.

**Push-in and the CJK–Latin gap (A3b):**
- Guide dots are drawn at Han–Latin gaps. `TextBlock::render` dots every gap of at least the dot size + 2 px, and the CJK–Latin gap is 4-5 px at reading sizes, so a reader with guide dots on sees a dot between 漢字 and iPhone. Guide dots mark word spaces; the gaps between CJK characters are too narrow for one. Next step: dot only the gaps that were a source space.
- A re-flowed line can end with ！ or ？ followed by an ideographic space U+3000 (case 1 of the fixture, "！　"). Typesetting practice sets no space there, and no section-5 rule covers it: a line-end U+3000 after ！？ could be dropped or hung.
- The rule checker's justified line-end check stays either-way (pushed in or not) on lines with ruby, word spaces or glued Latin, about a quarter of the corpus's justified lines. Restating those lines' widths exactly would let the check be strict on every line (`CjkRulesTest`).
- **Latin scratch:** the four mark vectors (`markClass_`, `edgeTrim_`, `shrink_`, `edgeShrink_`) are allocated for Latin paragraphs too: +524 B on test_table_streaming's peak, from table cells keeping their scratch. The fix: gate all four together on "the block holds a CJK mark", or `releaseLayoutScratch()` after each cell's layout.
- **The gap's lookups:** `cjkLatinGapBefore` decodes two codepoints at every breakable CJK boundary and calls `cjkEm` per gap. A per-block "has a Han/kana-Latin boundary" flag or a per-(font, style, scale) em cache would remove them.

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
at a chapter or book open, instead of wiping the store. Format 5 since #624 (a shaping font's style block ends with its
`CPac`): [file-formats.md](../file-formats.md#the-ttf-glyph-store-cptt).

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
  it, whatever its scripts (a character it lacks falls through to the UI faces), kept from one such screen to the
  next, and closed on any other (`syncUiBookFonts`):
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

Since #670 the last book's family opens whatever its scripts (before, only one with CJK did); a CJK character it lacks
still draws as the CJK faces' stand-in square (□). Before the CJK faces these strings drew nothing
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

Shipped in step 9.

**What step 9 built** (#570, #569, #574).
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

What remains, measured on the X4 with CC-CEDICT, JMdict_e and CC-KEDICT (`docs/dictionary.md` has the conversions):

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
- **The headword back in the header.** A CJK lookup leaves its headword empty (`DictionaryWordSelectActivity`), so
  the definition's header shows no title (`DictionaryDefinitionActivity::render`). Still open after the UI text
  fallback (step 6, #563) merged on 2026-10-28. That fallback is `CjkUiFonts::setReaderFont` and the per-string
  `beginText`; the names this item gave (`BookTextScope`, `readerFontAttached()`, `prefetchBookText()`) were never built.
- **Not run, by the maintainer's decision:** the X3 dense-page measurement and the touch check on the X4 Pro and
  T5S3 (spec row 12). The X4 with `CMD:HEAP:HOLD` ballast stood in for the X3.

**Where.** `Dictionary` (`resolvePrefixes`, `collectPrefixes`, `mergeSynonymHits`, the sidecars),
`CjkLookupRun`, `DictionaryWordSelectActivity`, `DictionaryDefinitionActivity`, `DictHtmlPages`.

## Line spacing

**Shipped** on 2026-10-26, on master directly (`3bdeb993c`, `2c14d8ae9`, `4d0c4c526`, `54b88fe7f`;
[spec](../superpowers/specs/2026-10-26-line-spacing-design.md), [plan](../superpowers/plans/2026-10-26-line-spacing.md)), after an X3 study of a Japanese, a Chinese and a Korean book
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

**What #500 built.**
- An SD font's per-page glyph arena (25-31 KB on a dense CJK page) is freed once the page is on the panel.
- The pre-render of the next page prewarms 1-bit ink masks instead of 2-bit glyphs: half the bytes, the same black-and-white pixels.
- **Effect on the X3:** a dense Chinese page turns in ~503 ms instead of ~678 ms, and reading leaves ~53 KB free instead of 15-21 KB.

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
- vertical text, `writing-mode` and `page-progression-direction`: no longer out of scope: vertical text is built (writing-modes steps 3 and 4, [../design/writing-modes.md](../design/writing-modes.md)), and what is left of it is in [writing-modes-handover.md](writing-modes-handover.md).

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
