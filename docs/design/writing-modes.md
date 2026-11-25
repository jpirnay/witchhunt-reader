# Writing modes: right-to-left and vertical text: design

Status: shipped, steps 0-4 (#560 to #673), and step 5's UI text (section 8). Design record; the code is the authority
on what it does now. What is left is in [../future_work/complex-scripts.md](../future_work/complex-scripts.md).

Hebrew and Arabic books, and vertical Chinese, Japanese and Korean. The design and the maintainer's decisions are in
`docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`:
- right-to-left bidi and Arabic shaping run once per paragraph at build time, and lines are stored in visual order;
- vertical text is a horizontal layout in a frame turned 90 degrees clockwise, not a second engine;
- the book decides the writing mode (CSS `writing-mode`, then OPF `page-progression-direction`), with a global setting
  and a per-book override;
- no right-to-left UI at first; Hebrew, Arabic and Persian interfaces came later (#711, section 8).

The turned frame was tried on the X3 (spec section 10, branch `spike/vertical-frame`): columns, ruby on the right,
sideways Latin and brackets all came out right, at the horizontal page's speed. Step 3 built it from that spike.

How a shaped word is built, stored and drawn is [text-shaping.md](../text-shaping.md). The CJK typography that vertical
text reuses is [cjk-typography.md](cjk-typography.md).

## 1. The steps

Each step was its own plan and PR (spec section 7). After step 0, steps 1-2 and 3-4 did not depend on each other.

| Step | What | Shipped |
|---|---|---|
| 0 | Parse every trigger (`writing-mode`, `text-orientation`, `text-combine-upright`, `direction`, `unicode-bidi`, `start`/`end`, `dir` with `<bdo>`/`<bdi>`, OPF ppd); CSS boxes along the writing mode, `start`/`end` along the direction; `line-height` parsed as CSS means it and kept through the cache; fixture books | #560 ([plan](../superpowers/plans/2026-10-26-writing-modes-step0.md)) |
| 1 | Bidi core and Hebrew; bidi controls from `unicode-bidi`, `<bdo>`, `<bdi>`; the horizontal right-to-left case of FlowContext and page progression, which step 3 built for vertical text (section 3) | PR 1 #599 (the library: bidi core, shaping layer, Hebrew divine-name rule); PR 2 #613 (layout and drawing: right-to-left lines, niqqud, lists and tables, bidi markup); PR 3, reading a Hebrew book, as #664, #665 and #666 (2026-11-09; section 4) ([design](../superpowers/specs/2026-11-02-writing-modes-step1-design.md), [PR 1 plan](../superpowers/plans/2026-11-02-writing-modes-step1-pr1.md), [PR 2 plan](../superpowers/plans/2026-11-02-writing-modes-step1-pr2.md)) |
| 2 | Arabic script, with one shaper for Arabic and the Indic scripts (section 7) | PR 1 #603, PR 2 #624; PR 3 #642 (Indic books: shaped text laid out and drawn); PR 3b #645 (the Font Manager's Indic families: 20 Noto families, chosen by the book's language); PR 4a #651 (the Arabic shaper in the engine: Arabic, Persian and Urdu shape right to left and match HarfBuzz); PR 4b #656 (Arabic books: Arabic, Persian and Urdu shape and join on the page, Indic words shape inside bidi paragraphs, and the Font Manager offers NotoNaskhArabic and NotoSansArabic); #657, #660, #672 and #667 followed, and Myanmar after it (#668, #671, #673). Speed followed (section 9) ([design](../superpowers/specs/2026-11-02-writing-modes-step2-design.md), [measurements](../superpowers/specs/2026-11-02-writing-modes-step2-measurements.md), [PR 1 plan](../superpowers/plans/2026-11-02-writing-modes-step2-pr1.md), [PR 2 plan](../superpowers/plans/2026-11-04-writing-modes-step2-pr2.md), [PR 3 plan](../superpowers/plans/2026-11-05-writing-modes-step2-pr3.md), [PR 3b plan](../superpowers/plans/2026-11-06-writing-modes-step2-pr3b.md), [PR 4a plan](../superpowers/plans/2026-11-07-writing-modes-step2-pr4a.md), [PR 4b plan](../superpowers/plans/2026-11-07-writing-modes-step2-pr4b.md), [reference](../text-shaping.md)) |
| 3 | Vertical frame (from the spike); the vertical decision and its settings; FlowContext and page progression | #577 ([plan](../superpowers/plans/2026-10-28-writing-modes-step3.md)) |
| 4 | Vertical typography: widows and orphans off and lone images centred (PR 1); 、。 and small kana placed from glyph ink, A3a along the column, Traditional Chinese marks (PR 2); tate-chu-yoko (PR 3); the dictionary on vertical pages (PR 4) | #594, #595, #597, #598 ([design](../superpowers/specs/2026-10-30-writing-modes-step4-design.md), [PR 1 plan](../superpowers/plans/2026-10-30-writing-modes-step4-pr1.md), [PR 2 plan](../superpowers/plans/2026-10-31-writing-modes-step4-pr2.md), [PR 3 plan](../superpowers/plans/2026-11-01-writing-modes-step4-pr3.md), [PR 4 plan](../superpowers/plans/2026-11-02-writing-modes-step4-pr4.md)) |
| 5 | Visual strings for titles and TOC; bidi in TXT and MD | titles and the TOC in visual order (#666, section 4); the last book's family behind the UI faces whatever its script (#670); Hebrew, Greek and Arabic in the UI faces (#681, #702); Indic titles reordered (#692), and Indic and Burmese titles shaped over the face behind the UI face (#699); right-to-left UI languages (#711) (section 8). TXT and MD have no paragraph bidi |

Line spacing ([cjk-typography.md](cjk-typography.md), 11.8) reads the `line-height` step 0 keeps.

## 2. Parsed in step 0, and who reads it

Step 0 parses, caches and tests every trigger; each waited for the step that first reads it.

| What | Kept in | Read by |
|---|---|---|
| `text-orientation` | `CssStyle::textOrientation` | nothing: parsed and cached only; every character takes its UAX #50 class (CSS `mixed`) |
| `text-combine-upright` | `CssStyle::textCombine` | step 4 PR 3: tate-chu-yoko, as a per-word flag |
| `direction`, `dir` (beyond `start`/`end`) | `CssStyle::direction` | step 1: a paragraph's base direction |
| `unicode-bidi`, `<bdo>`, `<bdi>` | `CssStyle::unicodeBidi` | step 1: embeddings and isolates |
| OPF `page-progression-direction` | `Epub::pageProgression()` | step 3: the vertical decision and page turns (`FlowContext`); step 1 reuses it |
| `line-height` | `CssStyle::lineHeightEm` | line spacing (cjk-typography.md, 11.8) |

## 3. Page progression

Step 3 built `FlowContext` (`lib/Epub/Epub/FlowContext.h`) and the page progression; step 1 added the horizontal
right-to-left case to them.
- **What it gives.** `FlowContext::of(request, vertical, ppd)` gives a section its `vertical` and
  `pageProgressionRtl` (`Section::flowContext()`). The reader's `pageProgressionRtl_` follows it: the tap zones, the page
  swipes, the progress bars and the percent picker mirror when it is set, and the Left and Right buttons too under
  Page Turn Buttons = Follow Book (`mirrorsLeftRight`).
- **A horizontal right-to-left book** (2026-11-08, #664). `FlowContext::requestFor` returns Off for a language that is
  never set vertically, so `pageProgressionRtlFor` takes the book's direction too (`FlowContext::bookIsRtl`: its
  language runs right to left, or its OPF says rtl and its language is never set vertically). Such a book turns right
  to left unless its OPF says ltr; a book under Off that is not right to left still turns left to right whatever its
  ppd (a CJK book read horizontally). The request stays out of the cache key: page progression is the reader's alone,
  read from `Section::flowContext()` at a section's load. X3: a Hebrew book shows `progression=rtl` and Left turns to
  the next page; Murphy: the left tap zone shows the next page.
- **A left-to-right ppd** is ignored for a section set vertically (spec 5.5): its pages turn right to left.

## 4. Reading a Hebrew book (step 1 PR 3)

The scope was the step 1 design (`docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md`, section "The
reader"); page progression is section 3 above.
- **Reading order in the consumers** (#664): the link-target walk and the QR page text read each line in its logical
  order (`TextBlock::readingOrder`, `appendReadingText`). Murphy: of two note markers on one Hebrew line, master gave
  only the first a target; both are tappable now. The dictionary's reading-order join was done in #656.
- **The reader's titles** (#666), not as the design said: "once, when set" does not survive the themes, which truncate
  and wrap a title to fit (45 calls), and a reversed string truncated at its end loses the title's start. So UI text
  stays in reading order through measuring and fitting, and `GfxRenderer::drawText` and `drawTextScaled` make a line
  with a right-to-left character visual as they draw it (`BidiUtils::visualUiLine`); a page's words, formed at section
  build, go through `drawFormedText` instead. Every UI string is covered at once: the status bar, Home, the lists, the
  TOC, centred text, alt text. X3: a Hebrew chapter title in the status bar read mirrored on master and reads right
  with the change. The UI faces hold the Hebrew letters since #681 (section 8).
- **Fonts** (#665): NotoSansHebrew and NotoSerifHebrew in the Font Manager, merged with Noto Sans/Serif Latin by
  `merge_noto_latin.py` (checked against a Hebrew corpus, `generate_corpus_hebrew.py`), and a Hebrew-script book picks
  an installed one by itself when the reader font has no alef (`SdCardFontSystem::selectLanguageFamily`). X3: under
  BookerlyTTF the Bialik chapter switches to NotoSerifHebrew and builds in 3,752 ms against master's 4,550 with the
  fallback face.
- **Upstream:** the bidi core's bracket mirroring before reordering (UAX #9 L4) went to crosspoint-reader as #4295,
  merged 2026-11-03.

## 5. Vertical text

Vertical text is a horizontal layout in the turned frame (step 3, #577); step 4 gave it its typography.
- **Upright or sideways.** `drawsUpright` (`lib/Epub/Epub/VerticalOrientation.cpp`) is the one place a UAX #50 class
  becomes a drawing: U and Tu characters (、。, small kana, ！？) upright, R and Tr (brackets, dashes, ……, ー) sideways.
- **Placed from glyph ink** (step 4 PR 2). `uprightPlacementOf` places an upright mark from its glyph ink (no font
  re-cut, design Decision 1): 、。，． go to the top-right corner of their cell, or to its centre in a Traditional
  Chinese book (line flags bit 3), and the small kana move 0.1 em up and right. A3a reads that placed ink along the
  column, so a 、 or 。 that A3b pushed in (a narrowed box at a column's end, or before a closing bracket) keeps its
  ink inside its box.
- **Tate-chu-yoko** (step 4 PR 3): `text-combine-upright` is read, as a per-word flag.
- **The page** (step 4 PR 1): widows and orphans are off and lone images are centred in a vertical section. The
  dictionary works on vertical pages (PR 4).
- **Line spacing:** the column pitch is the line pitch in the turned frame, so the same steps set it.
- **Limits** that shipped with it. None has a next step; each waits for a request.
  - `text-orientation` is parsed and cached (`CssStyle::textOrientation`, section 2) and never read: every character
    takes its UAX #50 class (CSS `mixed`).
  - `vertical-lr` books are set horizontally under Automatic; only `vertical-rl` has a turned frame (`FlowContext`).
  - Drop caps are off in a vertical section (`ChapterHtmlSlimParser::dropCapPositionAllows`): their ink metrics are
    horizontal.
  - No vertical forms, and the wave dashes stay rotated: their vertical form is a mirror image, and the renderer
    cannot mirror a glyph (step 4 design: not built).
  - No shaped run is set down a column: a vertical section reorders Indic words only
    ([text-shaping.md](../text-shaping.md#what-is-not-shaped-yet)).

## 6. Found in real books

- **A Chinese book declares `cn`** (#637, 2026-11-06). Not a language tag, so `TypographyProfile::forLanguage` gave it
  Latin rules. `language_tag::cjkRegionOf` reads `cn` alone as Simplified Chinese (`LanguageTag.h`).
- **Aozora Bunko conversions declare no writing mode.** They open horizontally under Automatic; the global setting's On
  is what sets them vertically (spec section 5.1).

**The acceptance book for steps 0, 3 and 4:** Hayakawa's 『七王国の玉座』 vol. 1 (`.idea/annas-arch-91dbd462043f.epub`,
local only, never in the repo): ppd `rtl`, `-webkit-writing-mode`, ~1,400 ruby groups, tate-chu-yoko, sesame dots, 12
images, ~380,000 CJK characters.

**The control for step 0:** the English original of the same novel (`.idea/A Game Of Thrones Book 1 -- Martin, George
R_ R_ -- 2011 -- chenjin5.epub`, local only): no `writing-mode` and 160 margin rules, so its page counts must not move.

## 7. Complex shaping: why one shaper (the analysis of 2026-10-29)

Step 2 built it (`lib/OtShaper`; #603, #624, #642, #645, #651, #656), with Myanmar after it (#668, #671, #673). Shaped
text is stored and drawn by glyph ID (`b0df1a4fa`); how is [text-shaping.md](../text-shaping.md). This is the analysis
that led there.

Hindi and the other Indic scripts need complex text shaping: what is drawn is decided by the font's GSUB and GPOS
tables, not by the characters one by one. CJK did not need it (one glyph per character). The spec baked Arabic's
joining forms into the fonts as Unicode presentation forms (section 5.6). That cannot work for Indic scripts: Unicode
has no presentation forms for conjuncts, which exist only as font-specific glyphs. So step 2 took one generic shaper
for both.

**What one generic shaping layer needs** (it serves Devanagari, Bengali, Gurmukhi, Gujarati, Odia, Tamil, Telugu,
Kannada, Malayalam, Sinhala, Arabic and, since #668 and #671, Myanmar; Khmer and Tibetan would need modules of their
own, not planned):
1. **Per-character tables:** the Unicode Indic syllabic and positional categories, to split text into syllables and
   place each mark. Data, like the bidi and UAX #50 tables.
2. **A shaper at section build time** (the placement section 5.3 chose for Arabic): reorder each syllable (pre-base
   vowel signs, reph); apply the font's GSUB features in the script's order (lookup types 1, 2, 4, 5, 6: single,
   multiple, ligature, contextual, chained); then GPOS mark attachment (mark-to-base, mark-to-mark) and kerning.
   Vendored HarfBuzz is too large for the C3 (+216 KB, upstream's closed #3721); upstream's port below is the route.
3. **Storage:** shaped runs as glyph IDs and positions, with the logical text kept beside them for the dictionary,
   KOSync and search; a section file version bump. The TrueType engine already draws by glyph index.
4. **Fonts:** Noto Sans and Serif per script in the Font Manager, with GSUB and GPOS kept (the CJK subsets drop them).
   The TrueType engine shapes in place from the mapped tables, at any size, a reader's own fonts included. Bitmap
   `.cpfont` families can be shaped too, but only by baking the GSUB closure's glyphs at private-use code points
   (upstream's way); later, if ever.
5. **Line breaking:** Hindi separates words with spaces, so today's breakers serve; a break never splits a syllable;
   the danda (। ॥) ends a sentence; Hindi hyphenation patterns exist (optional).
6. **UI:** book titles in the library need the same shaping (step 5's visual strings). A Hindi UI translation stays
   out of scope, as the right-to-left UI then did (it came in #711).

Thai and Lao need a further piece: no spaces between words, so line breaking needs a word dictionary (a CJK-like
problem with a large data cost).

**Upstream's shaper (crosspoint-reader #3787)**, looked at on 2026-10-29: "feat: add Indic language support without
HarfBuzz", open (head `0fbd1575`, 2026-10-23), run as an "Indic beta" on upstream's web flasher since 2026-10-14; users
report Kannada and Bengali on the X3 and Malayalam on the X4 Pro. Still open on 2026-11-09, at `03ea3b96`.
- **`lib/OtShaper`** (~5.4k lines): a hand port of HarfBuzz 14.5's Indic and Universal (USE) shapers and of its whole
  GSUB (types 1-8) and GPOS (1-9) engine, with syllable machines and categories generated from HarfBuzz. Scripts:
  Devanagari, Bengali, Gurmukhi, Gujarati, Odia, Tamil, Telugu, Kannada, Malayalam, Sinhala. The author reports
  33.7M of 33.7M test lines identical to HarfBuzz. Flash +58.4 KB on the C3 (+59,094 B measured on an X3).
- **Shaping at layout**, the shaped form stored beside the logical text (as section 5.3 plans); titles and UI strings
  shaped per draw with a run cache. Shaped forms are UTF-8 private-use tokens (glyph U+F0000+gid, advance, offset;
  4-12 bytes a glyph); mark offsets are whole pixels.
- **Fonts on the C3:** bitmap `.cpfont` with the GSUB closure rasterised at U+F0000+gid plus a layout font copied into
  the `spiffs` partition (`FlashBlobCache`); TrueType shaping only on PSRAM boards.
- **Open objection:** vectors `reserve()` after a heap check only, so a failed allocation aborts instead of falling
  back (against our nothrow rule). Our port resolves it with nothrow fixed storage (step 2's deviation 1,
  `OtFixedVector.h`).

**What was taken from it:** `OtShaper`, ported with nothrow fixed buffers and its tables built from our `FontBytes`
views (no table copy, no `spiffs`); the script tables, the syllable-safe breaks and the tests (HarfBuzz oracle, ~4k
strings a script, allocation-refusal and corrupt-font tests, the table generators). `FlashBlobCache` and the bitmap
path were skipped, and compact glyph records were chosen over its tokens (step 2's decision 4). The analysis proposed
keying glyph IDs in section files on the font's content, as their `3b047543` does; the step 2 design found them safe
already, since a TrueType family's font ID comes from its files' identity and `kRenderVersion` and the section's
property hash holds it. Attribution: crosspoint #3787 @ `0fbd1575` (MIT) and
HarfBuzz's MIT notice, which ship with the port. The engine already had every lookup type, so one shaper for Arabic (a
joining module plus right-to-left) and the Indic scripts was likely; section 5.6's "hundreds of KB" premise for a
shaper was out of date. Step 2 measured first (the measurements file) and ported it (#603). The allocation fix was
offered upstream in a comment on #3787 on 2026-11-04 (decision 7).

## 8. UI text (step 5)

UI text is drawn by bitmap UI faces, which cannot shape. Step 5 drew titles and the TOC in visual order (#666, section
4); what followed fills the faces and shapes what they cannot.
- **The last book's family behind the UI faces** (#670). Outside the reader, the screens that list books borrow the
  last book's TrueType family for the characters the UI faces lack. It was opened only when it had CJK, so Hebrew,
  Arabic and Indic titles drew blank; it is opened whatever it covers, and lends real characters only, so a Latin book
  font draws no U+FFFD or square of its own over a title the UI faces leave blank. It stays the last book's family for
  every row: only its set is in the glyph store, and opening another would rewrite it.
- **Hebrew and Greek in every UI face** (#681): the Hebrew letters and points from Noto Sans Hebrew behind Inter and
  Noto Sans (+13.8 KB), and modern Greek from Inter (the whole Greek block would cost 8.7 KB more, for Coptic and
  archaic letters). A Hebrew title shows whatever book was read last.
- **Arabic titles joined in the built-in faces** (#681, #702). `BidiUtils::arabicUiForms` turns Arabic UI text into
  its presentation forms, on minibidi's joining tables, before it is measured and drawn. #681 put the forms in a
  downloadable pack; #702 built 284 code points of Noto Sans Arabic UI into every UI face instead (+81,782 B on the
  `default` build), as CrossPoint builds Arabic into its menu fonts, and the pack went. Vowel marks are centred, not
  anchored by the font, and lam-alef is the only ligature.
- **Indic and Burmese titles** (#692, #699). `IndicReorder` puts Indic text in visual order where UI text is measured or
  drawn, once only, because the reorder is not idempotent; without shaping, conjuncts keep a visible virama. Where the
  TrueType face behind the UI face maps the script, the run is shaped over it with `IndicShaper::shapeToken` and drawn
  by glyph ID (`lib/GfxRenderer/UiShapedText`). When it is, and when not, is in
  [text-shaping.md](../text-shaping.md#what-is-not-shaped-yet), "Titles, chapter names and the TOC".
- **Right-to-left UI languages** (#711): Hebrew, Arabic and Persian interfaces, shipped as card packs. Their text reads
  right to left while the menus keep their left-to-right layout, as CrossPoint's do; the conventions are in
  `docs/i18n.md`, "Hebrew, Arabic and Persian".
- **The labels of the writing-mode settings** (Vertical Text, Automatic, Page Turn Buttons, Follow Book, Fixed) are in
  every UI language since #680.
- **TXT and Markdown** have no paragraph bidi. `drawText` reorders a TXT line whole, as one string, with no paragraph
  direction; a Markdown line is reordered span by span. TXT would get bidi from the parked TXT pipeline
  ([txt-pipeline.md](../future_work/txt-pipeline.md)); Markdown is open
  ([complex-scripts.md](../future_work/complex-scripts.md)).

## 9. Speed

The design's line is a cold chapter in at most 2x its Latin twin's time on the X3. Step 2 missed it: Arabic 3.17x,
Hindi 2.61x. The figures, and what each PR gained, are under "Time" in [text-shaping.md](../text-shaping.md#time); X3
builds are now at 1.88x (Hindi) and 2.00x (Arabic) (#874). Two findings decided the order of the work:
- **Most of the right-to-left excess was not the shaper.** Unshaped Arabic was already 2.22x its twin, on the bidi and
  TrueType path that Hebrew takes whole, so that path came first: the plain token (#661), the memory of letters that
  kern with nothing (#662), the glyph store's recent answers (#669), the space's width (#674).
- **The C3 is bound by fetching code.** Code and flash data share one 16 KB cache. Cutting the font data each word reads
  by 37-45% made X3 builds 3-6% faster; the engine's hottest 8.7 KB in IRAM made them 17-24% faster (#683, deviation
  21). A deviation from HarfBuzz has to earn its upkeep on every resync: #874 reverted the engine half of #872, whose two
  deviations were within the X3's noise.
