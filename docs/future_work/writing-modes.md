# Writing modes: right-to-left and vertical text

Status: open items as of 2026-10-29.

Hebrew and Arabic books, and vertical Chinese, Japanese and Korean. None of it is built yet; the design and the
maintainer's decisions are in `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`:
- right-to-left bidi and Arabic shaping run once per paragraph at build time, and lines are stored in visual order;
- vertical text is a horizontal layout in a frame turned 90 degrees clockwise, not a second engine;
- the book decides the writing mode (CSS `writing-mode`, then OPF `page-progression-direction`), with a global setting
  and a per-book override;
- no right-to-left UI for now.

The turned frame was tried on the X3 (spec section 10, branch `spike/vertical-frame`): columns, ruby on the right,
sideways Latin and brackets all came out right, at the horizontal page's speed.

## Roadmap

Each step is its own plan and PR (spec section 7). After step 0, steps 1-2 and 3-4 do not depend on each other.

| Step | What | State |
|---|---|---|
| 0 | Parse every trigger (`writing-mode`, `text-orientation`, `text-combine-upright`, `direction`, `unicode-bidi`, `start`/`end`, `dir` with `<bdo>`/`<bdi>`, OPF ppd); CSS boxes along the writing mode, `start`/`end` along the direction; `line-height` parsed as CSS means it and kept through the cache; fixture books | shipped (#560) ([plan](../superpowers/plans/2026-10-26-writing-modes-step0.md)) |
| 1 | Bidi core and Hebrew; bidi controls from `unicode-bidi`, `<bdo>`, `<bdi>`; FlowContext and page progression | open |
| 2 | Arabic script; before its plan, decide on one shaper for Arabic and the Indic scripts ([below](#complex-shaping-arabic-and-the-indic-scripts)) | open |
| 3 | Vertical frame (from the spike); the vertical decision and its settings | open |
| 4 | Vertical typography: baked vertical forms, small kana, tate-chu-yoko (the per-word flag), A3a in vertical | open |
| 5 | Visual strings for titles and TOC; bidi in TXT and MD | open |

**Next step.** Step 3: its plan is written (`docs/superpowers/plans/2026-10-28-writing-modes-step3.md`, on
`docs/writing-modes-step3`) and runs once the CJK work in flight has landed. Line spacing (`cjk.md`, step 10), which
reads the `line-height` step 0 keeps, has shipped.

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
   HarfBuzz is too large for the C3; a minimal shaper is the route, its flash cost to be measured.
3. **Storage:** shaped runs as glyph IDs and positions, with the logical text kept beside them for the dictionary,
   KOSync and search; a section file version bump. The TrueType engine already draws by glyph index.
4. **Fonts:** Noto Sans and Serif per script in the Font Manager, with GSUB and GPOS kept (the CJK subsets drop them).
   Bitmap `.cpfont` families cannot be shaped.
5. **Line breaking:** Hindi separates words with spaces, so today's breakers serve; a break never splits a syllable;
   the danda (। ॥) ends a sentence; Hindi hyphenation patterns exist (optional).
6. **UI:** book titles in the library need the same shaping (step 5's visual strings). A Hindi UI translation stays
   out of scope, as the right-to-left UI does.

Thai and Lao need a further piece: no spaces between words, so line breaking needs a word dictionary (a CJK-like
problem with a large data cost).

**Next step.** After CJK: a gap analysis and a spike, as the CJK font work had. Shape a Devanagari paragraph on the
host with the TrueType engine and a minimal shaper, compare it with HarfBuzz as the oracle, measure the flash cost,
then decide the step-2 question above.

## Parsed in step 0, read later

Step 0 parses, caches and tests every trigger; these wait for the step that first reads them.

| What | Kept in | First read by |
|---|---|---|
| `writing-mode` (the section's root mode) | `CssStyle::writingMode` | step 3: the vertical decision |
| `text-orientation` | `CssStyle::textOrientation` | steps 3 and 4: upright or sideways runs |
| `text-combine-upright` | `CssStyle::textCombine` | step 4: tate-chu-yoko, as a per-word flag |
| `direction`, `dir` (beyond `start`/`end`) | `CssStyle::direction` | step 1: a paragraph's base direction |
| `unicode-bidi`, `<bdo>`, `<bdi>` | `CssStyle::unicodeBidi` | step 1: embeddings and isolates |
| OPF `page-progression-direction` | `Epub::pageProgression()` | step 1: page turns (reused by step 3) |
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

## Left from the spike, for step 3

The reference run on the acceptance book is spec section 10.1: the same speed per page as horizontal, the same heap
peaks.
- **Images:** drawn in the turned frame they are rejected (a portrait image does not fit 792×528) and the placeholder
  turns sideways; their pixel caches were written at the turned size. Draw in the reader's frame, size with the
  reader's dimensions.
- **Decorations:** overlines and side lines land left of the column; in vertical they belong on the right.
- The dictionary's word boxes, taps and page progression were not touched.
