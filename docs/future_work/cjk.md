# CJK support

Status: open items as of 2026-10-21.

Line breaking for Chinese, Japanese and Korean is done, and so are punctuation widths, flush justification and the
whole-em grid (A3a). The font engine is done too: TrueType and CFF (`.otf`) outlines, families too large for flash
read from the card, and a glyph store that makes room. What remains for full CJK support:
- **Fonts:** synthetic bold, then the CJK families as `.otf` downloads in the font manager.
- **UI text:** the UI itself is translated; book titles, TOC entries and status-bar chapter names in CJK
  still need the reader's CJK font as a fallback.
- **Typography:** push-in and the CJK–Latin gap (A3b), and ruby with emphasis dots (B).
- **The dictionary** on CJK pages.
- **Small edge cases** the reviews found.

## Roadmap

In order. Each step is its own plan and PR. Steps 1-5 are the CJK fonts spec's order of work
(`docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md`, section 13), in the order they run: synthetic bold moved
behind CFF.

| Step | What | State |
|---|---|---|
| 1 | Serial test API for the font work (`CMD:TTF`, `CMD:FS`) | shipped (#531) |
| 2 | The glyph store makes room (spec section 8) | shipped (#532) |
| 3 | CFF outlines (spec section 9), and their speed-ups | shipped (#542, #544) |
| 4 | Synthetic bold (spec section 7): regular-only outline families draw bold, CFF included | next: plan `docs/superpowers/plans/2026-10-20-synthetic-bold.md` |
| 5 | The CJK families as `.otf` in the font manager (spec sections 4-6); the bitmap CJK families removed | plan to write after 4 |
| 6 | CJK book titles, authors and TOC in the UI: the reader's CJK font behind the UI faces ([UI text in CJK](#ui-text-in-cjk)) | needs a design |
| 7 | Push-in and the CJK–Latin gap ([A3b](#push-in-and-the-cjklatin-gap-pr-a3b)) | plan to write |
| 8 | Ruby and emphasis dots ([B](#ruby-furigana-pr-b)) | plan to write |
| 9 | The dictionary on CJK pages ([below](#dictionary-on-cjk-pages)) | open |

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
- **The design:** `docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md`, and its plan in `docs/superpowers/plans/`.

**Tests and books.**
- `test/epub_pipeline/CjkRulesTest.cpp`, the rule checker, lays out every line and asserts kinsoku, no hyphen, and no stray space between CJK characters.
- The books it runs on are in `test/epubs/`:
  - `test_cjk_line_breaking.epub`, the fixture, generated by `make_test_cjk_line_breaking.py`;
  - `test_cjk_ja_soseki`, `test_cjk_zh_honglou` and `test_cjk_ko_hyeon`, public-domain excerpts generated by `make_test_cjk_excerpts.py`;
  - `test_cjk_block_seams`, list items and `dl` after CJK text.
- Unit tests are in `test/cjk_line_break/` and `test/cjk_layout/`.

**The design.** The full design for everything below is `docs/superpowers/specs/2026-10-16-cjk-layout-design.md` on branch `docs/cjk-layout-design` (commit `7e1136008`). It is working material, not on master.
- Section 7 is B, Appendix A the character sets. Section 6, the first A3 design, is replaced by the A3 spec (`docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md`, on master).
- Section 3 explains each upstream-versus-KOReader choice.
- Upstream is crosspoint-reader `upstream/develop`. Port by hand: the histories are unrelated.

## Push-in and the CJK–Latin gap (PR A3b)

**Open.** Two parts of the A3 design are not built yet (spec sections 5.3 and 5.4, `docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md`):
- **Push-in.** When the next token does not fit, a line breaks earlier and is stretched (push-out). JLREQ prefers to shrink the line first: mid-line punctuation down to its minimum width, then the CJK–Latin gaps.
- **The CJK–Latin gap.** A Han or kana character next to a Latin letter or digit, with no space between them in the source, gets a quarter em between them. Chinese and Japanese books only, not Hangul.

**Why it matters.** Push-in keeps lines from being stretched to make room for one character. The gap stops Latin words and digits from touching the CJK text around them.

**Where.**
- **Push-in:** in all four breakers (`computeLineBreaks` and the three greedy ones) and in `extractLine`.
  - **Fit test:** a line fits when its width, with every shrinkable part at its minimum, is within the text width.
  - **The minimum widths** are in the tables already (`CjkPunctuation::Width::minimum`).
  - **In the DP,** a shrunk line costs more than an exact one and less than a badly stretched one. Pure-CJK paragraphs without punctuation must lay out exactly as before.
- **The gap:** at token boundaries without a space, in the profile's Chinese and Japanese rules. It stretches with justification and shrinks under push-in. It is not a break opportunity of its own.

**Next step.**
1. Write the A3b plan from spec sections 5.3 and 5.4.
2. Extend the rule checker: no line is wider than the text width; a shrunk line could not have been laid out at full widths.
3. Bump `SECTION_FILE_VERSION` (`lib/Epub/Epub/Section.cpp`).

## Ruby (furigana) (PR B)

**Open.** `<rt>` readings flow inline after their base. In the Soseki excerpt they show as 吾輩（わがはい）: the `<rp>` parentheses plus the reading at full size.

**Why it matters.** Japanese books mark rare kanji with ruby throughout. Inline readings double the text in places and break the reading flow.

**Where.** Spec section 7.
- **Parser:**
  - `<ruby>` flushes and continues glued;
  - `<rt>` collects the ruby of the base tokens since the group start;
  - `<rp>` is skipped in layout only;
  - no 96-token split inside a group.
- **Layout:**
  - a layout-only group marker per token in `ParsedText`; `TextBlock`'s style byte has no free bit;
  - JLReq overhang as upstream's `calculateWordWidths`;
  - line-edge protection in all four breakers and `extractLine` (upstream protects one);
  - ruby at half the base size through `getTextWidthScaled` / `drawTextScaled`.
- **Storage:** `TextBlock`'s `hasSizes` byte becomes a flags byte (bit 1 = ruby). Ruby groups `{firstWord, wordCount, text}` are stored only on lines that have ruby, so books without it cost nothing.
- **Drawing and line height:**
  - a ruby line is taller by half an ascender: in `addLineToPage`, table rows and cells, `TextBlock::wordBox` (dictionary) and link areas;
  - `dumpPage` must print the ruby groups, or the goldens cannot see them.

**Constraint.** KOSync must not move. `VisibleText`'s non-visible tags stay unchanged, so `<rt>` and `<rp>` text keep counting toward visible offsets, as crengine counts them. Add a host test that pins the offsets of a ruby fixture.

**Two additions** (A3 spec, section 9):
- **Ruby spacing:** when ruby and base differ in length, the shorter is spread across the longer with JLREQ's 1:2:1 spacing, instead of being centred.
- **Emphasis dots** (傍点, CSS `text-emphasis`): dots or sesame marks above each character of a span, as Aozora Bunko books use them. They share ruby's storage and its taller line.

**Next step.**
1. Write the B plan from spec section 7.
2. Port upstream #2665 and its follow-ups by hand (Phạm Bình An's, with `Co-authored-by`).
3. Add mono-ruby, group ruby, `<rp>` and ruby-at-line-edge cases to the fixture.
4. Bump `SECTION_FILE_VERSION`.

## A CJK font readers can install

**Decided, open** ([Roadmap](#roadmap) step 5). The CJK families ship as per-language `.otf` subsets that the font
manager downloads, drawn by the TTF engine: Japanese, Simplified Chinese, Traditional Chinese and Korean, sans and
serif, about 27.5 MB for all eight (spec sections 4-6, Appendix B). The bitmap families below are removed then. The
analysis that led there follows.

**Open.** No CJK font ships, and `lib/EpdFont/scripts/sd-fonts.yaml` keeps NotoSansCJK, NotoSerifCJK and NotoSansHangul commented out.

**What the sizes cost.** The fonts are 3.5 MB at 12 pt and 4.5 MB at 14 pt, per style (NotoSansCJK SC, 22,389 glyphs). The flash font partition is 3.375 MB (`partitions.csv`, `spiffs`):
- **14 pt** does not fit (`FlashFontPartition::appendFile` logs "would overflow partition"). It loads from SD: a 25-31 KB per-page arena and ~130 ms of SD reads per page to prewarm.
- **12 pt** just fits, and is then served from the flash mapping with no arena and no SD reads.

**The Korean split.** Korean needs NotoSansHangul (the `hangul` interval), a different family from Chinese and Japanese (the `cjk` interval has no Hangul). Hanja inside Korean text, such as names in parentheses, draws blank with the Hangul font.

**Why it matters.** Without a downloadable font a reader cannot read a CJK book at all. A font that fits the flash partition makes every page cheaper.

**Where.**
- The font list: `sd-fonts.yaml`, `build-sd-fonts.py`, `fontconvert_sdcard.py` (interval presets `cjk`, `hangul`).
- `FlashFontPartition`, and `partitions.csv`.
- The font download manager (`FontInstaller`, `/api/fonts/*`).

**What was tried.** Building takes time:
- NotoSansCJK at one size takes over 30 minutes on a desktop, so the script's default timeout fails;
- `--timeout 14400`, with sizes built in parallel through separate configs, works.

**Options.**
1. **A common-characters subset** at 14 pt: GB2312 + JIS X 0208 + kana + Hangul, about 8-10k glyphs, ~2-2.5 MB. It fits flash and covers Chinese, Japanese and Korean in one family. Rare characters then need a fallback to a full SD font.
2. **Grow the font partition.** There is little room: the app image uses ~89 % of each 6.4 MB OTA slot.
3. **Ship 12 pt only.** It fits today, but 12 pt CJK is small on the X3.
4. **TTF Stage 2c-4** (next item): CJK TrueType fonts loaded from SD.

**Decided:** option 4, through the CJK fonts spec.

## TTF Stage 2c-4: TrueType families too large for flash mode

**Shipped** (#506): a family too large for flash mode keeps its tables in the glyph store and reads its outlines from
the card (SD mode); CFF families do the same since #542. Font-selector previews for TTF families were assigned to this
stage (TTF spec section 7.6); check the font selector before planning them.

What follows is the stage as it was planned.

**Where.** The design is `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (on master):
- section 4, the glyph store;
- section 5.1, `FontBytes` in flash versus SD mode (a 4 KB page cache);
- section 7.6, previews;
- section 8, the 2c-4 row.

Already decided:
- no kerning for CJK pairs, with a range check before the GPOS store probe (A2 short-circuits `EpdFont::getKerning` for built-in and `.cpfont` fonts only);
- SD-mode families start without kerning or ligatures (`FontBytes::data()` is null);
- copying GPOS kern data into the mapped table area for large Latin and Cyrillic families can come later.

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
(`FlashGlyphStore::rebuildIndex`), then take 1 and 2.

## CFF outlines (`.otf`)

**Shipped** (branch `feat/cff-outlines`). The TTF engine draws CFF outlines as well as TrueType: a CFF parser and charstring interpreter in `lib/EpdFont/ttf/`, in flash mode and in SD mode (the charstring data read from the card). Font discovery lists `.otf` files and reads the content, so a file with an `OTTO` header is a CFF face whatever its name. Publisher Fonts takes a book's embedded CFF faces too, copied under their usual `<style>.ttf` names. Tested with CJK `.otf` files: subsets and a desubroutinized NotoSansTC (3.45 MB), in SD mode on the X4. Full stock files (NotoSansCJKjp-Regular.otf, which would need an SD-mode block of about 2.2 MB) are untested.

Still refused: variable CFF2 fonts and font collections.

## UI text in CJK

**Partly done** (branch `feat/cjk-ui-fonts`). Chinese, Japanese and Korean UI translations ship with
their own small UI faces: each holds the characters its translation uses, at 10 and 12 pt, attached
behind every UI face as fallback glyphs (`src/CjkUiFonts.cpp`, `docs/i18n.md`). Wrapped UI text breaks
between Chinese and Japanese characters. The build fails when a translation uses a character no UI
face has.

The faces ship as 1-bit DEFLATE groups, 97,132 B where they took 175,916 B flat
(`compress_ui_face.py`). The active UI language's two faces are inflated into its language pack
slot and drawn from flash with no decode (`docs/i18n.md`); the other two languages' glyphs, for the
language picker and CJK book titles, decode one at a time, ~1.9 ms each on the X3.

**Still open: text that is not a UI string.** Book titles and authors, TOC entries, chapter titles in
the status bar, file names and the finished-book screen draw the characters they share with a
translation and a box (□) for each of the rest. Before the CJK faces they drew nothing (X3,
2026-10-18, 紅樓夢: no title or author on the Home card, no caption under the Library cover). Without
the boxes the faces made it worse: the Home card showed a lone 雪, the one character of the author
曹雪芹 a translation happens to use, as if it were the title.

**Why it matters.** Even with a CJK reading font, a CJK reader cannot find or navigate their books.

**Next step.** Chain the reader's SD CJK font behind the CJK UI faces, as upstream does (crosspoint-reader
#2521, Tsz Ho Sze, and follow-ups #3026, #3071, #3484, #3646): `FallbackFaceProvider` already serves a
UI face's misses, so the SD font would be its last source. Questions that decide the design:
- An SD font is 2-bit and the UI faces are 1-bit, and the renderer draws at the depth of the font it
  was asked for: the 2-bit glyph has to be thresholded on the way, or drawn as its ink mask (#500's
  1-bit pre-render does exactly that).
- Its glyphs live in a heap page arena filled by a prewarm, which UI screens do not run; upstream
  prewarms per string (#3026) and bounds it for lists (#3071).
- Heap: upstream loads extra SD font sizes for the UI only on boards with PSRAM (#3646).

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

**Open.**
- **Selection is per token.** On a CJK page that means one character, with no segmentation into words. A Han character looked up in a Latin dictionary finds nothing, and a CJK dictionary needs multi-character words.
- **Plain definitions.** On a dense CJK page the definition shows as plain text: the styled renderer wants a larger block than is free while the word screen's arrays are held (`DHTML` "Low heap for styled definition").
- **Noisy logs.** The word screen's full repaints on dense pages log "Failed to allocate mini bitmap" before the arena retry serves them. It works, but it is noise.

**Where.** `DictionaryWordSelectActivity` (`extractWords`, `performLookup`, `LOOKUP_CONTIGUOUS_BYTES`), `DictionaryDefinitionActivity`, `Dictionary`, `DictZip`.

**Next step.**
1. Extend a selection across adjacent glued CJK tokens, for example by holding to grow it.
2. Test with a CJK StarDict dictionary such as CC-CEDICT.
3. Consider freeing the word arrays before the definition renders.

## Page glyph arena follow-ups (after #500)

**Open.**
- **The pre-render floor can underestimate once.** It is keyed on the last page's arena, so a sparse-to-dense page transition underestimates the next pre-render (the safe failure is a skipped pre-render). A running maximum of the arena per font would be conservative.
- **The capture restore keeps an arena.** Screenshot and light drawer (`restoreCurrentPageToBufferIfPreRendered`) rebuild a 2-bit arena and keep it until the next page.
- **The 1-bit pre-render covers plain unscaled pages only.** A page with a heading multiplier, a sized span, sup/sub or small caps pre-renders with 2-bit glyphs, and on a dense CJK page the floor then usually refuses it.
- **No test** covers `SdCardFont::pageArenaBytes()` on the flash-mmap path; it must stay 0.

**Where.** `EpubReaderActivity` (`releasePageGlyphArena`, `preRenderMinFreeHeap`, `pageDrawsUnscaled`), `SdCardFont` (`setInkOnlyPrewarm`, `releasePageArenaIfAtLeast`).

## Smaller edge cases from the A2 reviews

None of these is visible in the test books. Each is a one-line note of the case and the symbol.

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

These were set aside in the layout spec (section 3) and the A3 spec (section 10), and are candidates once A3b and B land:
- CSS `line-break` / `word-break` parsing;
- `lang` attributes inside a book: the book's language decides, and there is no detection from the content;
- last-line grid continuity, and even line height in ruby paragraphs;
- the special indent of an opening bracket at a paragraph start;
- a CJK default for line spacing, to be judged from screenshots first;
- hyphenating Latin words inside CJK books;
- `<rtc>` and ruby below the text;
- hanging punctuation;
- upstream's Latin dash split (#3883);
- right-to-left and vertical text, `writing-mode` and `page-progression-direction`. KOReader has no vertical text either. The typography profile already carries each language's inline direction (A3 spec, section 4).

## Testing on a device

**Fonts.** Build them locally:
1. Copy `sd-fonts.yaml` to a scratch file and keep only the CJK entries, uncommented.
2. Run `build-sd-fonts.py --config <scratch> --only NotoSansCJK --timeout 14400`, one config per size, so the sizes build in parallel.
3. Install over Wi-Fi: on the File Transfer web server, `POST /api/fonts/upload?family=<Family>` with the `.cpfont` as multipart `file`. Use relative file paths with the Windows curl. Books go in with `POST /upload?path=/books`.

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
