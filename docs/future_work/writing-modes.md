# Writing modes: right-to-left and vertical text

Status: open items as of 2026-10-30.

Hebrew and Arabic books, and vertical Chinese, Japanese and Korean. Vertical text is built (step 3); right to left
is not. The design and the maintainer's decisions are in `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`:
- right-to-left bidi and Arabic shaping run once per paragraph at build time, and lines are stored in visual order;
- vertical text is a horizontal layout in a frame turned 90 degrees clockwise, not a second engine;
- the book decides the writing mode (CSS `writing-mode`, then OPF `page-progression-direction`), with a global setting
  and a per-book override;
- no right-to-left UI for now.

The turned frame was tried on the X3 (spec section 10, branch `spike/vertical-frame`): columns, ruby on the right,
sideways Latin and brackets all came out right, at the horizontal page's speed. Step 3 built it from that spike.

## Roadmap

Each step is its own plan and PR (spec section 7). After step 0, steps 1-2 and 3-4 do not depend on each other.

| Step | What | State |
|---|---|---|
| 0 | Parse every trigger (`writing-mode`, `text-orientation`, `text-combine-upright`, `direction`, `unicode-bidi`, `start`/`end`, `dir` with `<bdo>`/`<bdi>`, OPF ppd); CSS boxes along the writing mode, `start`/`end` along the direction; `line-height` parsed as CSS means it and kept through the cache; fixture books | shipped (#560) ([plan](../superpowers/plans/2026-10-26-writing-modes-step0.md)) |
| 1 | Bidi core and Hebrew; bidi controls from `unicode-bidi`, `<bdo>`, `<bdi>`; the horizontal right-to-left case of FlowContext and page progression, which step 3 built for vertical text ([below](#what-step-1-takes-from-step-3)) | open |
| 2 | Arabic script; before its plan, decide on one shaper for Arabic and the Indic scripts ([below](#complex-shaping-arabic-and-the-indic-scripts)) | open |
| 3 | Vertical frame (from the spike); the vertical decision and its settings; FlowContext and page progression | shipped (#577) ([plan](../superpowers/plans/2026-10-28-writing-modes-step3.md)) |
| 4 | Vertical typography: baked vertical forms, small kana, tate-chu-yoko (the per-word flag), A3a in vertical | in progress (PR 1 #<this PR>) |
| 5 | Visual strings for titles and TOC; bidi in TXT and MD | open |

**Next step.** Step 3 has shipped. Not chosen yet: step 4 continues the vertical work, step 1 starts right to left
(asked of the maintainer on 2026-10-29; no answer yet). Line spacing (`cjk.md`, step 10), which reads the
`line-height` step 0 keeps, has shipped.

## What step 1 takes from step 3

Step 3 built `FlowContext` (`lib/Epub/Epub/FlowContext.h`) and the page progression; step 1 adds the horizontal
right-to-left case to them.
- **What exists.** `FlowContext::of(request, vertical, ppd)` gives a section its `vertical` and
  `pageProgressionRtl` (`Section::flowContext()`). The reader's `pageProgressionRtl_` follows it: the tap zones, the page
  swipes, the progress bars and the percent picker mirror when it is set, and the Left and Right buttons too under
  Page Turn Buttons = Follow Book (`mirrorsLeftRight`).
- **What stops a horizontal right-to-left book today.** `FlowContext::requestFor` returns Off for a language that
  is never set vertically (`TypographyProfile::verticalAllowed`: Chinese, Japanese and Korean), and
  `pageProgressionRtlFor` returns false for Off. A Hebrew or Arabic book with OPF
  `page-progression-direction="rtl"` therefore turns its pages left to right. Step 1 must give
  `pageProgressionRtlFor` the ppd and the base direction, not only the request. The request is also in the
  section's property hash and header (Off adds nothing), so a Latin book's cache key stays off the vertical
  setting; step 1 must not change that for a book that is not set vertically.
- **A left-to-right ppd** is ignored for a section set vertically (spec 5.5): its pages turn right to left.

## Complex shaping: Arabic and the Indic scripts

**Open; planned for after CJK, together with step 2** (maintainer, 2026-10-29: "store that plan for realisation
after CJK, when we need to do Arabic").

Hindi and the other Indic scripts need complex text shaping: what is drawn is decided by the font's GSUB and GPOS
tables, not by the characters one by one. CJK did not need it (one glyph per character). Nothing in the firmware
handles it today: no Devanagari support, the SDK's GSUB reader resolves only ligature lookups (types 4 and 7, for
fi/fl), and hyphenation patterns exist for ten European languages.

**What a Hindi book looks like today**, in a Devanagari TrueType font: the vowel sign ि drawn after its consonant
instead of before it, no conjuncts or half forms (क्ष, त्र), the reph (र्) not raised, marks placed by guesswork.

**Why it belongs with step 2.** The spec bakes Arabic's joining forms into the fonts as Unicode presentation forms
(section 5.6). That cannot work for Indic scripts: Unicode has no presentation forms for conjuncts, which exist only
as font-specific glyphs. A real shaper would serve Arabic properly as well, so the choice is made once, before step 2
is planned: one generic shaper for both, or baked Arabic forms now and a shaper later.

**What one generic shaping layer needs** (it would serve Devanagari, Bengali, Gurmukhi, Gujarati, Odia, Tamil,
Telugu, Kannada, Malayalam, Sinhala, Khmer, Myanmar and Tibetan, and Arabic):
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
   out of scope, as the right-to-left UI does.

Thai and Lao need a further piece: no spaces between words, so line breaking needs a word dictionary (a CJK-like
problem with a large data cost).

### Upstream's shaper (crosspoint-reader #3787)

Looked at on 2026-10-29: "feat: add Indic language support without HarfBuzz", open (head `0fbd1575`, 2026-10-23),
run as an "Indic beta" on upstream's web flasher since 2026-10-14; users report Kannada and Bengali on the X3 and
Malayalam on the X4 Pro.
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
  back (against our nothrow rule).

**What to do with it** (an analysis, not yet a decision): port `OtShaper` with nothrow fixed buffers and its tables
built from our `FontBytes` views (no table copy, no `spiffs`); take the script tables, the syllable-safe breaks and the
tests (HarfBuzz oracle, ~4k strings a script, allocation-refusal and corrupt-font tests, the table generators); skip
`FlashBlobCache` and the bitmap path for now; decide between their tokens and compact glyph records. Key glyph IDs in
section files on the font's content (their `3b047543`). Attribution: crosspoint #3787 @ `0fbd1575` (MIT) and
HarfBuzz's MIT notice, which must ship with the port. The engine already has every lookup type, so one shaper for
Arabic (a joining module plus right-to-left) and the Indic scripts is likely; section 5.6's "hundreds of KB" premise
for a shaper is out of date.

**Next step.** After CJK, and preferably once #3787 has merged with its allocation fix: port and measure. Run
`OtShaper` on the host over our `FontBytes` with the Noto Devanagari faces, diff against HarfBuzz with upstream's
corpus, link it into `default` for the real flash delta, measure its heap on the X3, then decide the step-2 question
above.

## Parsed in step 0, read later

Step 0 parses, caches and tests every trigger; these wait for the step that first reads them.

| What | Kept in | First read by |
|---|---|---|
| `text-orientation` | `CssStyle::textOrientation` | step 4: upright or sideways runs (step 3 gives every character its UAX #50 class) |
| `text-combine-upright` | `CssStyle::textCombine` | step 4: tate-chu-yoko, as a per-word flag |
| `direction`, `dir` (beyond `start`/`end`) | `CssStyle::direction` | step 1: a paragraph's base direction |
| `unicode-bidi`, `<bdo>`, `<bdi>` | `CssStyle::unicodeBidi` | step 1: embeddings and isolates |
| OPF `page-progression-direction` | `Epub::pageProgression()` | step 3: the vertical decision and page turns (`FlowContext`); step 1 reuses it |
| `line-height` | `CssStyle::lineHeightEm` | line spacing (`cjk.md`, step 10) |

## Found while looking at real books

- **A Chinese book declares `cn`.** Not a language tag, so `TypographyProfile::forLanguage` gives it Latin rules. Read
  `cn` as `zh` (`TypographyProfile.cpp`).
- **Aozora Bunko conversions declare no writing mode.** They open horizontally under Automatic; the global setting's On
  is what sets them vertically (spec section 5.1).

**The acceptance book for steps 0, 3 and 4:** Hayakawa's 『七王国の玉座』 vol. 1 (`.idea/annas-arch-91dbd462043f.epub`,
local only, never in the repo): ppd `rtl`, `-webkit-writing-mode`, ~1,400 ruby groups, tate-chu-yoko, sesame dots, 12
images, ~380,000 CJK characters.

**The control for step 0:** the English original of the same novel (`.idea/A Game Of Thrones Book 1 -- Martin, George
R_ R_ -- 2011 -- chenjin5.epub`, local only): no `writing-mode` and 160 margin rules, so its page counts must not move.

## What step 4 inherits from step 3

Step 3 ships vertical text with these limits; step 4 (vertical typography) picks them up.
- **Punctuation stays rough.** `drawsUpright` (`lib/Epub/Epub/VerticalOrientation.cpp`) is the one place a UAX #50
  class becomes a drawing: U and Tu characters (、。, small kana, ！？) upright, R and Tr (brackets, dashes, ……, ー)
  sideways. There are no vertical forms, and Tu marks are not placed in their cell. Step 4 places Tu from
  `getGlyphInk` or a re-cut font. With A3a in vertical, a 、 or 。 that A3b pushed in (a narrowed box at a
  column's end, or before a closing bracket) keeps its horizontal ink: drawn upright it can reach half an em past
  its box.
- **The dictionary is not available in vertical text.** `EpubReaderActivity::openDictionary` returns on a
  vertical section and the reader menu does not offer the Dictionary row, so the long-tap dictionary gesture does
  nothing on a vertical page. Word boxes and the selection's navigation need mapping through the frame.
- **`text-orientation` and `text-combine-upright`** are parsed and not read: every character takes its UAX #50
  class (CSS `mixed`), and tate-chu-yoko (the per-word flag) is step 4.
- **Drop caps are off in a vertical section** (`ChapterHtmlSlimParser::tryStartDropCapCapture`); their ink
  metrics are horizontal.
- **`vertical-lr` books are set horizontally** under Automatic; only `vertical-rl` has a turned frame.
- **The new settings' labels** (Vertical Text, Automatic, Page Turn Buttons, Follow Book, Fixed) are English in
  every UI language until a translation pass.
