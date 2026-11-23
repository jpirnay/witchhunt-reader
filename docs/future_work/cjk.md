# CJK support

Status: open items as of 2026-11-23.

Line breaking, typography, fonts, the UI text, the dictionary and line spacing for Chinese, Japanese and Korean have
shipped. What was built, and why, is in the design and reference docs:
- [../design/cjk-typography.md](../design/cjk-typography.md): line breaking, punctuation widths, the grid, push-in,
  the CJK–Latin gap, ruby, emphasis dots and line spacing (section 11); what is not done yet (section 10);
- [../design/cjk-ttf-fonts.md](../design/cjk-ttf-fonts.md): the CJK families, CFF outlines, synthetic bold and italic,
  the glyph store, the page glyph arena, the roadmap (section 13) and how to test on a device (section 15);
- [../i18n.md](../i18n.md#chinese-japanese-and-korean): the CJK UI faces, and book titles in the UI;
- [../dictionary.md](../dictionary.md#chinese-japanese-and-korean): the dictionary on CJK pages.

Elsewhere: plain text gets none of the CJK layout ([txt-pipeline.md](txt-pipeline.md)), a CJK keyboard
([ui-languages-and-keyboards.md](ui-languages-and-keyboards.md)), the UI faces moving to the card
([cjk-ui-faces-on-card.md](cjk-ui-faces-on-card.md)), vertical text and the other scripts
([complex-scripts.md](complex-scripts.md)).

## Follow-ups of the layout work

**Open.** Found in the final reviews of each step.

**Ruby (B1):**
- **A reading is measured with kerning but drawn without it.** Layout measures the whole reading
  (`ParsedText::rubyWidth`, through `measureWordWidth`); `TextBlock::render` sums each character's own advance and
  draws the characters one at a time. A Latin gloss with kern pairs can sit 1-2 px past the margin. Fix: measure the
  unkerned sum, or draw the reading in one call and spread per character only for 1:2:1.
- **A reading over a word in another font family** (Publisher Fonts on) is placed by that word's ascender
  (`rubyY` in `TextBlock::render`), but the band is half the block font's ascender: with a taller family it can rise
  above the band into the line above. Fix: clamp `rubyY` to the line's top.
- **`room()` in `ParsedText::applyRubyWidths` lets a reading overhang Latin curly quotes** (U+2018, U+2019, U+201C,
  U+201D): it asks `CjkPunctuation::classOf`, which classes them, though `applyPunctuationWidths` leaves them out as
  narrow Latin glyphs. A pixel or two over an apostrophe. Fix: ask `markClass_` instead.

**Emphasis dots (B2):**
- **`<em class="sesame_dot">` with `font-style: normal` stays italic** (decide). This is Aozora Bunko's own markup.
  The `<i>`/`<em>` branch of `ChapterHtmlSlimParser::startElement` sets italic whatever the CSS says, as `<b>` lets
  the tag decide its weight. Options: honour a CSS `font-style: normal` on the italic tags, or keep the tag's word.
  Span-based markup (AozoraEpub3's `em-sesame`) is unaffected.
- **Fullwidth ＃％＆＊／＠＼＿** (U+FF03, FF05, FF06, FF0A, FF0F, FF20, FF3C, FF3F), the CJK Compatibility Forms
  U+FE30-FE4F and ～ U+FF5E take a mark, while their ASCII forms and 〜 do not (`TextEmphasis::takesMark`). ＊ in a
  "＊　＊　＊" break inside a marked span is the likeliest to show.

**Push-in and the CJK–Latin gap (A3b):**
- **Guide dots are drawn at Han–Latin gaps.** `TextBlock::render` dots every gap of at least the dot size + 2 px, and
  the CJK–Latin gap is 4-5 px at reading sizes, so with guide dots on a dot sits between 漢字 and iPhone. Guide dots
  mark word spaces. Next step: dot only the gaps that were a source space.
- **"！　" at a line end** (decide). A re-flowed line can end with ！ or ？ followed by an ideographic space U+3000
  (case 1 of the fixture). Typesetting practice sets no space there, and no rule of cjk-typography.md section 5
  covers it. Options: drop a line-end U+3000 after ！？, or hang it.
- **The rule checker accepts either line end on some justified lines.** A line with ruby or a word space, whose
  natural width the dump does not give, may end pushed in or not (`test/epub_pipeline/CjkRulesTest.cpp`,
  `CjkJustifiedRules`): about a quarter of the corpus's justified lines. Restating those lines' widths exactly would
  let the check be strict on every line.
- **Latin scratch.** `applyPunctuationWidths` assigns the four mark vectors (`markClass_`, `edgeTrim_`, `shrink_`,
  `edgeShrink_`) for every paragraph, Latin ones too. They cost `test_table_streaming` +524 B of heap peak when each
  table cell kept its own scratch. Since #729 one `ParsedText::LayoutScratch` per build is lent to every block and
  cell, so the vectors now hold one set sized for the longest paragraph. Next step: measure the peak again, and gate
  the four on "the block holds a CJK mark" only if it still shows.
- **The gap's lookups.** `ParsedText::cjkLatinGapBefore` decodes two code points at every boundary without a space
  and calls `cjkEm` per gap, in every Chinese and Japanese book (`TypographyProfile::cjkLatinGap`). A per-block "has a
  Han/kana-Latin boundary" flag, or an em cache per (font, style, scale), would remove them.

## CJK `.cpfont` fonts converted by hand

**Open.** Two bugs in the bitmap path, found while building the CJK families (cjk-ttf-fonts.md section 5). Neither
touches the hosted `.otf` families; both hit a CJK font that a reader converts to `.cpfont` and installs.

1. **The interval table is copied to the heap.**
   - **What:** `SdCardFont::loadFromMmap` copies the interval table out of the flash mapping. For Japanese that is
     49 KB of heap, and it turned pre-rendering off.
   - **Why:** the table may sit unaligned in the mapping, so `loadFromMmap` copies it into a heap array
     (`fullIntervals`), once per distinct table: a style whose table matches an earlier style's shares that copy
     (`findIdenticalIntervals`). The comment on `loadFromMmap` in `SdCardFont.h` says the intervals are read in place;
     the code copies them.
   - **Where:** `SdCardFont::loadFromMmap`. `MAX_INTERVALS` is 4096; the Japanese set has 4,083.
   - **Next step:** read the table in place when it sits 4-byte aligned in the mapped partition, and keep the heap
     copy for an unaligned one. Then check that a Japanese `.cpfont` keeps pre-rendering on.
2. **A `.cpfont` larger than the partition erases the whole partition.**
   - **What:** on the first book open after every restart, the partition is erased (4-5 s), then the append fails.
   - **Why:** the single-file fallback of `writeFamily` (`lib/EpdFont/SdCardFontManager.cpp`) calls
     `FlashFontPartition::beginWrite`, which erases, before `appendFile` checks that the file fits.
   - **Where:** `writeFamily`'s single-file fallback, after the whole-family check; `FlashFontPartition::beginWrite`
     and `appendFile` (`lib/hal/FlashFontPartition.cpp`).
   - **Next step:** compare the file with `FlashFontPartition::fontUsableSize()` before `beginWrite`, and remember the
     refusal so the next open does not try again.

### 龘 in Noto Serif TC

**Parked.** U+9F98 has 621 points, over the rasterizer's scratch cap of 512 (`ttf::kMaxScratchPoints`), and draws as
missing in either outline format (cjk-ttf-fonts.md 9.2). A larger block for every family is not worth one glyph. A Hong
Kong or full-set family would need it.

**Next step.** Raise the cap only if such a family is added.

## Font Manager follow-ups

**Open.** Left out of the CJK families' round (cjk-ttf-fonts.md 6.5). The update rule's accepted limits are in its
6.6.
- **A family installed but not listed.** Discovery leaves out an outline family the glyph store cannot install in
  either mode (`SdCardFontRegistry`, its `ttfFits_` check), and the web page's installed list is the registry's
  (`CrossPointWebServer::handleFontList`). Such a family is missing from the page, and only the device can delete it.
- **Resume works per file.** An interrupted download keeps the files already staged and checked
  (`FontDownloadActivity`, the staging folder), so an interrupted one-file family (every `.otf`) restarts from zero.
- **The card's free space is cached for 20 s.** The SDK caches the used bytes (`SDCardManager`,
  `USED_BYTES_CACHE_TTL_MS`), and a remove or a write does not invalidate the cache. A Retry right after deleting a
  family, or the second family of a "Download all", can be judged on the old figure; a batch can then run the card
  full mid-download (a loud failure, recoverable by resume).

**Where.** `FontInstaller`, `FontDownloadActivity`, the `/api/fonts/*` handlers and the font page.

**Next step.** Take them one at a time. For the last, invalidate the cache in the SDK's remove, removeDir and write
paths, landed on the SDK's `witchhunt` branch first.

## Font selector previews of outline families

**Open.** The font selector previews a `.cpfont` family that is not in flash by reading it from the card
(`FlashCachePolicy::ReadOnly`), but not a TrueType or OpenType one: `SdCardFontManager::loadFamily` returns false for
a read-only outline load ("TTF previews come later (spec 7.6)"). The TTF engine spec planned a temporary `TtfFont` in
ring mode, reading from the card and writing nothing to the store
(`docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, section 7.6).

**Next step.** Check what the selector shows for an outline family today, then plan the preview.

## Glyph store: a faster index rebuild

**Open.** An index rebuild takes about 1.8 s on the X4 (Noto Sans TC at 12 pt: about 9,400 records, 16,384 slots), and
in idle time it holds the loop task that long, so a key pressed then waits. With CJK at 12 pt the index fills before the
log, so while many new glyphs arrive (the store-fill test book, `tools/make_cjk_store_fill_epub.py`) a rebuild comes
about every five reclaims; a reclaim itself takes about 150 ms.

**Ideas**, cheapest first:
1. **Size the index for CJK records.** `glyphstore::planLayout` sizes an SD-mode index for 250 B a record
   (`kExpectedRecordSd`); at 12 pt a CJK record is about 150 B, so 70% of the index is reached before the log is full.
   Sized for 150 B, the log fills first and reclaims make the room. Cost: index pages from the mapping budget (one
   64 KB page per 8,192 slots), so a little less log.
2. **Program the index a sector at a time.** `FlashGlyphStore::rebuildIndex` inserts each record with its own 8-byte
   program call (about 100 µs of overhead each on the X3, so about 1 s of the 1.8 s). A RAM bitmap of occupied slots
   (2 KB for 16,384) gives every record's slot without writing; one log walk per index sector fills a 4 KB buffer that
   is programmed whole: 32 program calls instead of about 9,400. Estimated 0.5-0.8 s.
3. **Rebuild more at opens.** Lower the open-only stale share (`kRebuildStalePercent`, 12%) so more rebuilds run
   behind the chapter-open popup and fewer in idle time.
4. **An A/B index rebuilt in slices.** A second index region filled across idle ticks while the first serves lookups,
   switched by a journal entry: no long block at all. Cost: a second index's flash and mapping pages.

**Next step.** Measure how the 1.8 s splits between the index erase, the log walks and the slot programs
(`FlashGlyphStore::rebuildIndex`), then take 1 and 2; the plan is
`docs/superpowers/plans/2026-10-21-glyph-store-faster-rebuild.md`.

## UI text in CJK

**Open.** How UI strings and book text draw in CJK: [../i18n.md](../i18n.md#chinese-japanese-and-korean).
- **The reader's own passes keep their miss scratch on the heap** (its status-bar title): the reader keeps the
  secondary framebuffer for its grayscale passes, so its passes lend none (`ActivityManager`,
  `lendFramebufferForGlyphs`). A scratch block planned for the UI's sizes (~9.4 KB against 17.4 KB) would halve it.
- **Bold UI text draws CJK in the regular weight.** A synthetic bold would be a 1 px double strike.
- **The large UI step draws 12 pt CJK under 14 pt Latin headings** (decide). A 14 pt CJK size costs ~125 KB flat,
  ~60 KB compressed, and its active-language faces would need about another 64 KB page of the language pack slot.
- **The interval tables are nearly one interval per glyph:** 1,304 intervals for 1,542 glyphs a size, 15,648 B for the
  three languages (a language's two sizes share one table). A sorted codepoint list would save ~12 KB.
- **Han characters the Chinese and Japanese faces share** (decide). 67 Han characters at 10 pt and 92 at 12 pt are
  byte-identical in the two faces (3,080 B and 5,910 B flat). Dropping them from the Japanese faces would draw the
  same pixels from the Chinese ones, but a Japanese UI would then decode those per glyph, outside its language pack.

## Dictionary on CJK pages

**Open.** What shipped, and how the search works: [../dictionary.md](../dictionary.md#chinese-japanese-and-korean).
Measured on the X4 with CC-CEDICT, JMdict_e and CC-KEDICT.

- **Search time on dictionaries with a large `.syn` (a JMdict speed-up).** The maintainer's gate is the median of
  `ms / exact_ms` over 50 words per book (`CMD:ACT:DICT`), at most 1.5. Hong Lou Meng with CC-CEDICT: 1.46 (four runs,
  1.46-1.47; 2.69 before the five speed-ups of #574). Soseki with JMdict: 1.95 (3.66 before them; median 37 ms against
  20 ms for the exact lookup), not met; the maintainer accepted it for #574, JMdict being best effort, and a JMdict
  speed-up is the follow-up.
  - A CJK search reads both index files: two descents, two window scans, the `.syn` and `.sidx` opened (6 ms), and a
    follow per synonym hit; the exact lookup of the matched headword reads the `.idx` once. JMdict files every kana
    reading in its 15.8 MB `.syn` (550,503 entries, 2,151 samples, ~7 KB windows), so kana text always takes both.
  - Measured costs on the X4: a 4-byte sample read 0.46 ms, a 512-byte refill 0.47 ms, a sample block 0.65 ms, opening
    the `.syn` and `.sidx` 6.1 ms, 5.5 ms fixed.
  - What would close the gap needs a sidecar change: a map from each `.qidx` sample to its `.sidx` sample (no second
    descent, ~6 ms off JMdict's median), or a skip test for runs the `.syn` cannot extend (estimated medians: CC-CEDICT
    0.89, JMdict 1.75). Against today's lookup of the matched text as the page writes it (夢幻 rather than 梦幻), the
    estimated medians are ~1.0 (CC-CEDICT) and ~1.55 (JMdict).
- **Deinflection** (decide). There is none, by the maintainer's decision: 食べた finds 食, 먹었습니다 finds nothing.
  The PyGlossary JMdict build's `.syn` holds kana readings, other kanji spellings and "kanji・kana" pairs, no inflected
  forms, so the `.syn` route does not cover it either. A rule table (kana suffix in, suffix out) would add candidates
  to the search. It has to be clean-room (Yomitan is GPL-3.0, KOReader AGPL-3.0) and reviewed by someone who reads
  Japanese. matcha-reader (MIT; #5, #6, #691; credit Eszter Schuffert) has one in `lib/Dict/Deinflector.cpp`, but names
  no source for its rules, which have the shape of Yomichan-style tables. Options: keep none; write a clean-room
  table; take matcha's after asking where its rules come from.
- **Moving the cursor word by word, and a manual range** (decide). The cursor steps one character, and the match is
  always the longest. Upstream went with a manual range (#3932).
- **The other script in the definition.** CC-CEDICT writes each word in both scripts; a single regional CJK font
  draws the other script's missing characters as U+FFFD (學, 書 with a Simplified Chinese font). The NotoSansCJK
  `.cpfont` family carries both scripts but not pinyin's ē, ǎ, ū (U+0113, U+01CE, U+016B: `SDCF` "onGlyphMiss"), which
  drop out of the reading (dàxuéshng). A fallback face for definition text, as UI text has, would fix both.
- **A long CJK definition in an SD-card font, laid out without a prewarm, is untimed.** Only entries of 1.5-2.6 KB
  were measured (110-240 ms). Next step: time a long entry on the X4.
- **The headword back in the header.** A CJK lookup leaves its headword empty (`DictionaryWordSelectActivity`, the
  lookup's `headword`), so the definition's header shows no title (`DictionaryDefinitionActivity::render`); the body
  opens on the headword lines instead.

A styled definition that falls back to plain text, CJK or not, is in [dictionary.md](dictionary.md).

**Where.** `Dictionary` (`resolvePrefixes`, `collectPrefixes`, `mergeSynonymHits`, the sidecars), `CjkLookupRun`,
`DictionaryWordSelectActivity`, `DictionaryDefinitionActivity`, `DictHtmlPages`.

## Line spacing

**Open.** What shipped: [../design/cjk-typography.md](../design/cjk-typography.md), section 11.8.
- **The Markdown reader keeps the font's line height** (`MdReaderActivity`, `GfxRenderer::getLineHeight`); left out of
  the first round by the maintainer. TXT: [txt-pipeline.md](txt-pipeline.md).
- **Book's style, lines that are not a block's.** A `<br>` separator's blank line, an `<hr>`'s margin and table rows
  keep Normal's pitch (100%, `lineSpacingFor`), not the block's `line-height`. `em` and `%` values are inherited as
  numbers, which differs from CSS only where a child changes font size.

## Page glyph arena follow-ups (after #500)

**Open.** What #500 built: [../design/cjk-ttf-fonts.md](../design/cjk-ttf-fonts.md), section 14.
- **The pre-render floor can underestimate once.** `EpubReaderActivity::preRenderMinFreeHeap` is keyed on the last
  page's arena (`releasePageGlyphArena`), so a sparse-to-dense page transition underestimates the next pre-render (the
  safe failure is a skipped pre-render). A running maximum of the arena per font would be conservative.
- **The capture restore keeps an arena.** `EpubReaderActivity::restoreCurrentPageToBufferIfPreRendered` (screenshot,
  sleep overlay, light drawer) draws the page again with 2-bit glyphs and keeps that arena until the next page.
- **The 1-bit pre-render covers plain unscaled pages only** (`EpubReaderActivity::pageDrawsUnscaled`). A page with a
  heading multiplier, a sized span, sup/sub or small caps pre-renders with 2-bit glyphs, and on a dense CJK page the
  floor then usually refuses it.
- **No test covers `SdCardFont::pageArenaBytes()` on the flash-mmap path**; it must stay 0 there.
  `test/sd_font_arena/SdFontArenaTest.cpp` tests it after `load()` only.

**Where.** `EpubReaderActivity` (`releasePageGlyphArena`, `preRenderMinFreeHeap`, `pageDrawsUnscaled`), `SdCardFont`
(`setInkOnlyPrewarm`, `releasePageArenaIfAtLeast`).

## Smaller edge cases from the reviews

None of these is visible in the test books. Each is a one-line note of the case and the symbol.

**Font files.**
- `ttf::probeFile` and `ttf::parseFace` disagree on a short OS/2 table. The probe reads the weight class from a table
  of 6 bytes or more, and fsSelection from one of 64 bytes or more; `parseFace` ignores an OS/2 table FreeType would
  (under 78 bytes, or short for its version). A lone heavy face with such a table is emboldened (cjk-ttf-fonts.md
  7.1, the table's third row).

**Line breaking.**
- Combining dakuten U+3099/U+309A, U+302A-302F and ideographic variation selectors U+E0100-E01EF can be split from
  their base: `CjkLineBreak::boundaryAllowsBreak` keeps a mark with its base only as `utf8IsCombiningMark` defines one,
  and those ranges are not in it. Rare: NFD kana, IVS texts.
- `utf8IsCjkBreakable` misses Ext D-I, the Compatibility Supplement, Katakana Phonetic Extensions, Enclosed CJK, CJK
  Compatibility and Bopomofo. Halfwidth ｡｢｣､ are not "unspaced", so a source newline after them stays a space. These
  are upstream's ranges; extending them departs from upstream.
- Project Gutenberg Chinese uses a backtick U+0060 as an opening single quote, and it is in neither kinsoku set.

**The parser.**
- CJK text right after a non-inline end tag that does not flush the word buffer (`字</section>\n字`, text directly
  inside `section`) still glues, because the flush brings `lastTextCodepoint_` back after the end-tag reset.
- A Latin quotation or apostrophe that ends a source line glues to the CJK line after it (`He said "hello"\n字`): the
  segment-break rule drops a newline after any quotation mark. The quote-seam rule could require CJK before the quote.
- A table-row degrade drops one pending segment-break decision, so one source wrap in that cell becomes a space.
- A dash inside a non-CJK run can add a token the run flush does not count.
- A ruby group wider than a whole line (forced split) overhangs the right margin (narrow table cells, floats).
- The table SSO surcharge (`TABLE_BUFFER_SSO_CAPACITY`) is computed on the unsplit word, about a 6 % overcount (the
  safe direction).

**Tokens.** In-word token offsets are computed after `utf8NfcNorm`, so tokens after a composed jamo or a skipped U+FEFF
sit a few bytes short. This affects KOSync offsets inside one word only.

**Tests to add.**
- A table-driven kinsoku test over all of Appendix A.1 (the current test samples ~13 of ~70 codepoints).
- `Hyphenator` on a mixed word ("ab漢字").
- `addWord` cases: a single CJK character, an all-punctuation word, and the heap-gate refusal.
- A Hangul word next to the new quote entries.
- The `CjkBlockSeams` dl and figure assertions (`test/epub_pipeline/CjkRulesTest.cpp`) run only when both words share
  a line.

## Decisions from the matcha-reader triage (2026-11-15)

matcha-reader (MIT; credit Eszter Schuffert) has CJK features we do not. Each needs a decision before any work.
Japanese deinflection from its table is under [Dictionary on CJK pages](#dictionary-on-cjk-pages).

- **Furigana on and off** (named Pinyin/Zhuyin for Chinese), with tighter line spacing when off (matcha #64, #391).
  Ruby is always drawn here (`TextBlock::render`, the ruby loop), and there is no setting. A layout input, so a
  section-header field and a `SECTION_FILE_VERSION` bump. Size S-M.
- **Text settings per script** (matcha #428, #608): separate Latin and CJK profiles. We pick typography by the book's
  language (`lib/Epub/Epub/TypographyProfile.h`); there are no per-script user settings. Next step: list which
  settings a profile would split and how a book picks its profile, then size it.
- **Manga** (#659, #733 and matcha's converter): `.matcha` volumes, Mokuro import, an offline CBZ/webtoon-to-EPUB
  converter, panel zoom. We have no comic reader or CBZ; the converter's output, EPUBs of image pages, opens already.
  [SCOPE.md](../../SCOPE.md) names no comics: a scope question first.
