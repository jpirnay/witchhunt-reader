# CJK layout: line breaking, justification and ruby: design

Status (2026-10-16): written for review.

It is working material. When the work merges, lasting parts move to reference docs, open items move to
`docs/future_work/`, and this file is deleted (see the docs layout convention).

## 1. Goal

A reader opens a Chinese, Japanese or Korean book and gets text laid out the way CJK typography expects:
- **Line breaks** between characters, with no hyphens.
- **Kinsoku:** no 。、」 at the start of a line and no 「 at the end.
- **Straight right edges** on justified lines, with punctuation narrowed at line edges and in runs.
- **Furigana** (ruby) drawn above the text it annotates.

This comes before TTF Stage 2c-4 (SD-mode TTF, chiefly CJK fonts), so CJK fonts arrive on correct layout. It
also helps today's `.cpfont` CJK readers at once.

**Success:**
- the host rule checker (9.1) passes on the fixture and on real public-domain books;
- X3 screenshots of those books show correct line edges, punctuation and ruby;
- heap during a CJK section build stays within today's Latin figures;
- KOSync positions are unchanged for every book, ruby books included.

## 2. Today (code reading, `lib/Epub/Epub`)

Our layout treats CJK like a language it has no hyphenation patterns for:
- **No word boundaries:** text without spaces is cut into 200-byte "words" (`MAX_WORD_SIZE`, about 66 CJK
  characters), and each cut becomes an ordinary word gap: a visible space every ~66 characters.
- **Hyphens:** a word wider than the line is broken at any character with a hyphen inserted (`Hyphenator`
  fallback breaks always set `requiresInsertedHyphen`). Every CJK line therefore ends in "-".
- **No CJK rules:** no kinsoku, no CJK justification, no ruby. `<rt>` text flows inline.
- **Korean** wraps at spaces, but an over-wide Hangul word gets a "-".

## 3. Sources and decisions

The work ports **upstream crosspoint-reader's** CJK layout (`upstream/develop`, 2026-06 to 2026-10). It
then adds four improvements taken from **KOReader** (crengine), which the user named as the reference. Our tree
and upstream have unrelated histories, so this is a port by hand of upstream's end state, not a replay of its
PRs.

| Decision | Source | Why |
|---|---|---|
| Break between CJK characters, no hyphen at CJK breaks | upstream #2288 | The core of CJK layout |
| Kinsoku by gluing tokens, "loose" rules | upstream #2288 | Small kana and ー may start a line, as in KOReader's default |
| Kinsoku sets extended with crengine's defaults (A.1) | KOReader | It covers 々ゝゞヽヾ・〜…‼ at line start, and the halfwidth forms, which upstream does not |
| Korean: words whole; syllable break at a line end only with hyphenation on; justified at word spaces | upstream #2288, #3700 | A Korean contributor's choice. KOReader breaks between any syllables instead; both are accepted Korean styles. The user chose whole words |
| Zero-width stretchable gaps between CJK characters | upstream #2288 | |
| Spread the justification remainder | ours | Upstream and crengine both lose or bias it. Upstream's justified CJK lines are ragged by up to one glyph |
| Punctuation squeeze, forced entries of crengine's simplified-Chinese table | KOReader | The biggest visual gain: half-width punctuation at line edges and in runs |
| Whole-em text width and indent for zh/ja books | KOReader ("cjk-tailored" tweak) | Pure CJK lines fill exactly, and characters sit on a grid |
| A source newline between Han, kana or CJK punctuation collapses | ours (CSS Text 3 segment-break rule) | Better than KOReader, which renders a space there and says it cannot tell |
| No kerning for CJK pairs | upstream #2288 (`EpdFont::getKerning`) | Also keeps TTF kerning off the CJK path (2c-4) |
| `<br>` in CJK books | upstream #2710: nothing to port | Our neutral `<br>` style already strips vertical margins |
| Ruby: upstream's parser and JLReq overhang; line-edge protection in every breaker; storage only on lines with ruby | upstream #2665 and follow-ups; our storage | Upstream protects edges in one breaker only, and stores 4 bytes per word for every book |
| Not ported: the Latin dash split | upstream #3883 | Merged upstream on 2026-10-15; it changes every Latin book's layout. A later decision |

**Out of scope:**
- CJK UI fallback fonts (upstream #2521 ff.);
- CSS `line-break` / `word-break` parsing;
- CJK–Latin ¼ em spacing;
- optional squeeze with "push-in" fitting, and the Japanese and traditional-Chinese squeeze tables;
- last-line grid continuity, and even line height in ruby paragraphs;
- `<rtc>` and ruby below the text;
- hanging punctuation;
- vertical text (KOReader does not support it either).

These are candidates for `docs/future_work/` once the work lands.

## 4. Delivery

| PR | Content | Section version |
|---|---|---|
| **A: CJK line breaking** | Sections 5 and 6. It lands in steps; the first changes nothing (5.2) | 84 (master is at 83) |
| **B: Ruby** | Section 7 | 85 (format change) |

Each port commit cites the upstream PR and commit, with a `Co-authored-by` trailer where the code is mainly
theirs (the porting convention):
- Justin Mitchell (#2288);
- Phạm Bình An (#2652, #2665, #2781, #3102);
- Uri Tauber (#2768);
- Sung-jin Brian Hong (#3700).

## 5. PR A: breaking and tokens

### 5.1 Character classes (`lib/Utf8`, header-only)

- **`utf8IsCjkBreakable(cp)`:** Han (CJK Unified Ideographs and extensions, compatibility ideographs), Hiragana,
  Katakana, Hangul syllables, CJK Symbols and Punctuation, and fullwidth/halfwidth forms. These are upstream's
  ranges (`Utf8.h:46-76` there).
- **`utf8IsHangul(cp)`:** Hangul syllables and jamo.
- **A Han/kana/CJK-punctuation class** for the newline rule (5.5), the squeeze (6.2) and the grid (6.3). It
  excludes Hangul.

### 5.2 Token boundaries

- Today a token boundary is either a word gap or "continues" (glued, unbreakable).
- A second per-token flag, `noSpaceBefore`, adds the zero-width breakable gap that CJK needs. The three kinds,
  as upstream's `TokenBoundary.h` defines them minus the dash state:

| Kind | Width | Line break allowed | Justifiable |
|---|---|---|---|
| Space | a space | yes | yes |
| CJK gap | zero | yes | yes |
| Glued | zero (kerning) | no | no |

- **Where the flag is threaded:**
  - **Per-word lists:** every place in `ParsedText` that keeps per-word lists in step. That is
    `addWord` and its heap gate, `preserveSource` snapshot and restore, the hyphenation-retry merge, the
    consumed-prefix erase, `releaseLayoutScratch`, `reset`, the bionic rebuild, `hyphenateWordAtIndex` inserts,
    and `extractLine`.
  - **Line breakers:** all four, plus `extractLine`. That covers the DP `computeLineBreaks` (including the float
    pre-pass and the gap cache), `computeSingleLineBreakNoHyphen` (page-bottom retry), and the two hyphenated
    greedy breakers.
  - **Cosmetic:** the cosmetic joins (`ensureFontReady`, `buildLinePreview`).
- **This lands first, with no behaviour change:** every existing golden stays byte-identical. A debug check
  asserts that the per-word lists keep equal lengths, because one missed site misaligns them silently.
- **Tests:** upstream's `TokenBoundaryTest` (7 tests) ports as-is.

### 5.3 CJK tokens and kinsoku (`ParsedText::addWord`)

- **Splitting:** a word with CJK-breakable codepoints is split into tokens at every allowed CJK boundary. The
  pieces after the first are `noSpaceBefore` when glued in the source; otherwise they keep a space.
- **Kinsoku** is enforced by never splitting:
  - before a no-line-start character;
  - after a no-line-end character.

  So 字。 and 「漢 stay one token. The sets are in A.1.
- **Visible offsets:** each token's offset is the base offset plus the visible bytes of its prefix (KOSync).
- **Memory:**
  - a token is 1-3 characters (3-9 bytes) and fits `std::string`'s inline buffer, so splitting allocates no
    character data;
  - the heap gate sizes for the tokens a word adds, not for one.
- **Bionic reading:** CJK tokens are not bolded (they are non-word spans). The rebuild carries the new flag.

### 5.4 Hyphenator and kerning

- **Hyphenator:** `Hyphenator::breakOffsets` draws no hyphen when the codepoint at or before the break is
  CJK-breakable (upstream `Hyphenator.cpp:259-270`).
- **Kerning:** `EpdFont::getKerning` returns 0 when either codepoint is CJK-breakable (upstream `EpdFont.cpp:117-120`).

### 5.5 Parser (`ChapterHtmlSlimParser`)

- **Chunk continuity:** after the forced `MAX_WORD_SIZE` flush, the next chunk continues the word (glued). That
  applies on both paths, including the ASCII fast path (upstream #2652).
- **Shorter CJK runs:** a CJK run is flushed every ~32 codepoints, with continuity kept.
  - Without this, a 200-byte flush adds up to ~66 tokens on top of the 96-token paragraph split.
  - The word lists would then double to 256 entries, a ~6 KB contiguous block on the C3.
- **Table budget:** the row budget is charged per token added, not per flush. CJK would otherwise undercount it
  about tenfold.
- **Newlines between CJK characters:** a segment break (newline plus surrounding whitespace) between two
  characters of the Han/kana/CJK-punctuation class is removed, not turned into a space (CSS Text 3). The plan
  first confirms where the parser still sees the newline; whitespace is collapsed in our parser, not by the XML
  tokenizer.

### 5.6 Korean

- **Words stay whole:** Hangul words are never split into syllable tokens, and justification uses word spaces
  only.
- **Line-end syllable breaks:** with hyphenation on, `hyphenateWordAtIndex` first tries upstream's Hangul
  line-end breaks (`hangulLineEndBreaks`). These are between syllables, and at Hangul↔digit, Latin or bracket
  boundaries; never before closing punctuation; no hyphen is drawn.
- **Over-wide words:** a word wider than the whole line reaches the same path even with hyphenation off.
- **Page-bottom retry:** a Hangul split counts as a hyphenated line end, so the page-bottom retry re-lays it
  unsplit, as it does for Latin.
- **Tests:** upstream's 6 `KoreanLineBreaking` and 3 `KoreanLayout` tests port with edits (our `ParsedText`
  constructor and callback, and our stub widths of 10 px per glyph and 5 px per space).

## 6. PR A: justification, squeeze and grid

### 6.1 Justification (`extractLine`)

- **CJK gaps stretch.** For Latin, our rules stay: no stretch before closing punctuation, and the `effWidth/8`
  cap.
- **The remainder is spread:** the leftover pixels after `spare / gaps` go 1 px each to evenly spaced gaps, so
  justified CJK lines end flush.
- **No guide dots** are drawn at a CJK gap. The check is at render time, on the boundary's codepoints; nothing is
  stored.

### 6.2 Punctuation squeeze

- **What it is:** the forced (positive) entries of crengine's simplified-Chinese table (`textlang.cpp#L813-L972`
  in koreader/crengine, 11 classes × 11, 1/8 em units; classes in A.2).
- **The table applies to every CJK book.** KOReader uses this one table for everything but Japanese and
  traditional Chinese.
- **Rules:**
  - punctuation at a line end becomes half-width;
  - an opening bracket at a line start becomes half-width;
  - in a run of punctuation all but one become half-width (」。, ）（);
  - a closing-then-opening pair is set 6/8 + 6/8;
  - an ideographic space U+3000 at a paragraph start keeps its full width.
- **Line-edge vs mid-line:**
  - **Line-edge entries** depend on the break, so `extractLine` applies them, and the freed space goes to
    justification (6.1).
  - **Mid-line entries** (runs) are applied to token widths before line breaking, so the breakers measure what
    is drawn.
- **Placement:** the full-width glyph is drawn inside its narrowed box by where its ink sits, using the glyph
  metrics we already have (`left`, `width` against the advance):
  - ink in the left half: left-aligned;
  - ink in the right half: right-aligned;
  - otherwise: centred.

  No per-character data is needed.
- **Applies** only in CJK context (both characters in the class).
- **Not ported:** the optional (negative) entries and "push-in" fitting are out of scope (section 3).

### 6.3 Whole-em grid for Chinese and Japanese

- **When:** for a book whose language (OPF `dc:language`, falling back to the root element's `lang`) is `zh*`
  or `ja*`.
- **Width:** a paragraph's text width is rounded down to a whole number of ems of its font size, and half the
  difference goes to each side.
- **Indent:** a first-line indent rounds down to whole ems, at least 1 em.
- **Effect:** pure CJK lines then fill exactly and characters line up in columns. Mixed lines still justify
  (6.1).
- **No setting:** it is on by default for those languages. A setting can follow if readers ask (YAGNI).
- **Floats** keep their own width, as in crengine.

## 7. PR B: Ruby

### 7.1 Parser

- **`<ruby>`:** flushes pending text, which then continues glued.
- **`<rt>`:** collects its text (trimmed, whitespace collapsed) as the ruby of the base tokens since the group
  start. Mono-ruby sequences such as 漢<rt>かん</rt>字<rt>じ</rt> get one group per base; an `<rt>` with no new
  base appends to the previous group.
- **`</ruby>`:** ends the group.
- **`<rp>`** (fallback parentheses) is skipped in layout only.
- **KOSync offsets:** `VisibleText`'s non-visible tags do **not** change, so `<rp>` and `<rt>` text keep counting
  toward the visible offsets, as crengine counts them.
- **No splits inside a group:** the 96-token paragraph split is suppressed inside a ruby group (upstream #3102).

### 7.2 Layout

- **Group marker:** group followers are glued to their leader. A layout-only marker (a per-token bit list in
  `ParsedText`) records group extent, because our `TextBlock` style byte has no free bit (`SMALL_CAPS` is 64, bit
  7 is the continues flag).
- **Width adjustment (JLReq style, upstream `calculateWordWidths`):**
  - ruby wider than its base overhangs each side by half the excess;
  - an adjacent ideograph is pushed away by the full overhang;
  - kana or punctuation may be overhung by up to half its width;
  - adjacent groups do not collide.
- **Line edges:** a group starting or ending a line keeps its overhang inside the margins, in all four breakers
  and in `extractLine`. Upstream does this in one breaker only.
- **Size:** ruby is half the base size, through our scaled-text path (`getTextWidthScaled` /
  `drawTextScaled` at 0.5 × the word's scale). Our superscript style does not shrink glyphs. Ruby follows inline
  font sizes.

### 7.3 Storage and drawing

- **Storage:** `TextBlock` turns its `hasSizes` byte into a flags byte (bit 1 = ruby present) and appends ruby
  groups `{firstWord, wordCount, text}` only when the line has ruby. Lines and books without ruby cost nothing.
- **Drawing:** ruby is centred over its group, one half-ascender above the base line.
- **Line height:** a line with ruby is taller by half an ascender. This applies in `addLineToPage`, in table rows
  and cells, in `TextBlock::wordBox` (dictionary) and in link areas.

## 8. Memory and performance

- **Tokens:** a CJK page holds several hundred one-to-three-character tokens. Each fits `std::string`'s inline
  buffer, and the ~32-codepoint flush (5.5) keeps the word lists at their 128-entry steady state.
- **Squeeze:** about 121 bytes of tables in flash, with no heap.
- **Ruby:** a `std::string` per group, only on ruby lines.
- **On the device, measure:**
  - the heap low-water mark in a CJK section build against a Latin one;
  - the scaled-glyph mask cache (80 entries) with ruby;
  - page time on a CJK page with hundreds of distinct glyphs (`.cpfont` fonts).

## 9. Validation

The maintainer does not read CJK, so pass or fail must not rest on reading the pages.

### 9.1 Host rule checker

A test target lays out books through the host pipeline (`epub_pipeline`) and asserts on every line:
- **Line edges:**
  - no line starts with a no-line-start character, and none ends with a no-line-end character (A.1);
  - no hyphen is drawn in CJK text;
  - no space separates two characters that were glued in the source;
  - no line has a newline-born space between CJK characters.
- **Justification:** a justified line's natural width plus its gaps equals the text width.
- **Squeeze:** squeezed punctuation has the width the table gives it.
- **Grid:** in zh/ja books, the text width is a whole number of ems.
- **Ruby:** every ruby group keeps its base glued, and the overhangs stay inside the margins.

It runs on:
- **a generated fixture EPUB** (`make_test_cjk.py`) with each case: kinsoku pairs, runs of punctuation,
  newlines in the source, Korean words, mono- and group-ruby, `<rp>`, ruby at line edges, and a table cell;
- **real public-domain books, checked in as small excerpts:**
  - a Japanese novel from Aozora Bunko (ruby-rich);
  - a Chinese classic (Project Gutenberg or Wikisource);
  - a Korean text.

The page dump (`dumpPage`) is extended to print the boundary kind and the ruby groups, so the goldens see them.
Upstream's tests port as listed in 5.2 and 5.6.

### 9.2 Device (X3)

- **Fonts:** Noto Sans CJK SC (Chinese/Japanese) and Noto Sans CJK KR (Korean), built locally as `.cpfont` with
  our converter. They are not shipped; `sd-fonts.yaml` keeps them commented out.
- **The user's part:** open each book and take screenshots of a few pages; Claude reviews them (line edges,
  punctuation widths, ruby placement, grid alignment).
- **Also checked:** the heap and timing figures in section 8, and that the KOSync position of a ruby book
  matches KOReader's.
- **Optionally,** a native reader looks at a few screenshots before release, for example the upstream
  contributors named in section 4.

## 10. Risks

1. **A missed per-word site** misaligns the lists silently. Mitigation: the no-change first step and the debug
   length check (5.2).
2. **A breaker that still measures a space** where layout now has a zero gap makes lines overflow or come out
   short. Mitigation: every gap site is listed (5.2), and the rule checker measures every line.
3. **The C3 heap with CJK token counts.** Mitigation: the ~32-codepoint flush; measured on the device (section 8).
4. **The squeeze's glyph placement** depends on each font's bitmap metrics; built-in, `.cpfont` and TTF fonts must
   all report `left` and `width` the same way.
5. **KOSync parity for `<rt>` and `<rp>`.** Mitigation: `VisibleText` is unchanged; a host test pins the offsets
   for the ruby fixture.
6. **The page-bottom retry** now also re-lays Hangul line-end splits: a behaviour change on Korean pages, checked
   in the Korean tests.

## Appendix A. Character sets

### A.1 Kinsoku

**No line start:**
- From upstream:
  - the explicit hyphens and dashes (`- 058A 2010 2012-2015 2043 207B 208B 2212 2E17 2E3A 2E3B FE58 FE63 FF0D _ …`);
  - `. , : ; ! ? ) ] }`, and » ’ ”;
  - 、。〉》」』】〕〗〙〛;
  - ！），．：；？］｝.
- Added from crengine (UAX #14 classes CL, EX, NS, IN):
  - 〞〟｠ and the small FE50 FE52 FE56 FE57;
  - halfwidth ｡｣､･;
  - 々 〻 ゝゞ ヽヾ ・ 〜 ゠ ‼ ‽ ⁇⁈⁉ ゛゜ ﾞﾟ 〼;
  - ‥ ․.

  A run of … or —— never splits.

**No line end:**
- From upstream: `( [ {`, « ‘ “, 〈《「『【〔〖〘〚, （［｛.
- Added from crengine (class OP): 〝 ｟ ｢ „ ‚.

**Still allowed at line start:** small kana (ぁぃぅぇぉっゃゅょゎ ァィゥェォッャュョヮヵヶ) and ー, and a break
after the ideographic space U+3000. KOReader's default does the same; strict JIS rules are out of scope.

### A.2 Squeeze classes (crengine `getCJKCharType`, `textlang.h#L32-L166`)

| Class | Characters |
|---|---|
| opening bracket | 「『（【〔〈《 and others |
| closing bracket | 」』）】〕〉》 and others |
| dividing | ！？ |
| middle dot | ・：； |
| full stop | 。． |
| comma | 、， |
| fullwidth space | U+3000 |
| ambiguous quote | ＂＇ |
| curly quotes | “”‘’, only when they measure at least 4/5 em in CJK context |

The plan copies the simplified-Chinese table's values from the source and keeps only its forced (positive)
entries.

## Appendix B. Where things are

- **Upstream** (`crosspoint-reader`, `upstream/develop` 83f9cc05):
  - `lib/Epub/Epub/ParsedText.cpp`: kinsoku 69-136, CJK breaks 138-232, `addWord` 550-814, breakers 1155-1359,
    `hyphenateWordAtIndex` 1363-1506, `extractLine` 1508-1932, ruby width 1057-1150, ruby edges 977-1046;
  - `lib/Epub/Epub/TokenBoundary.h`;
  - `lib/Epub/Epub/hyphenation/Hyphenator.cpp` 259-270;
  - `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`: ruby 1269-1299 and 1825-1867, flush 1743-1777;
  - `lib/Epub/Epub/blocks/TextBlock.{h,cpp}`: ruby storage and render;
  - tests: `test/token_boundary`, `test/korean_line_breaking`, `test/chapter_html_slim_parser` (KoreanLayout).
- **KOReader / crengine** (`ab1981c`):
  - `crengine/src/textlang.cpp`: line-break tailoring 696-810, squeeze tables 813-972;
  - `crengine/include/textlang.h` 32-166;
  - `crengine/src/lvtextfm.cpp`: segment-break comment 1463-1484, squeeze at break time 5149-5362, alignment
    3213-3262;
  - `crengine/src/lvfntman.cpp`: squeeze drawing 4961-4991;
  - `crengine/src/lvrend.cpp`: cjk-tailored width 9317-9335, indent 3678-3725.
- **Ours** (`master` 2be9c2f14):
  - `lib/Epub/Epub/ParsedText.cpp`: `addWord` 196-241, breakers 597-1129, `hyphenateWordAtIndex` 1133-1227,
    `extractLine` 1229-1379;
  - `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`: flush 3224-3245, ASCII fast path 3091-3095, `<br>`
    2637-2667;
  - `lib/Epub/Epub/blocks/TextBlock.{h,cpp}`: style byte, serialization 405-436.

  These line numbers are from the 2c-3 tree; the plan rechecks them.
