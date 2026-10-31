# Writing modes step 4: vertical typography (design)

Status: design, for the maintainer's review. Step 3 (#577) shipped vertical text with rough punctuation; this step
sets it properly. It builds on `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md` (5.4, 5.6,
section 7 row 4) and the list "What step 4 inherits from step 3" in `docs/future_work/writing-modes.md`, and replaces
the spec's font re-cut (5.6) with placement on the device.

## Intent

A vertical Japanese page should look like a printed one: 、 and 。 in the top-right corner of their cell, small kana
nudged up and right, two-digit chapter numbers upright as one block, punctuation that A3b narrows staying inside its
box. A reader who looks a word up on a vertical page gets the dictionary, as on a horizontal one.

Non-goals: vertical glyph forms from the fonts (no re-cut, no GSUB on the device); `text-orientation`; drop caps,
`vertical-lr`, Korean's KLREQ marks; any change to a horizontal page.

## Decisions (maintainer, 2026-10-30)

1. **Place marks from glyph ink, on the device; no font re-cut.** Measured against the `vert` alternates of Yu Gothic
   and MS Gothic (the CJK fonts we ship were cut without GSUB, and the Japanese and Korean cuts hold 2 vertical-form
   code points):
   - 、。，． — the vertical form is the horizontal ink box reflected through the cell's centre, within 1.4-6.2 % of
     an em (1-2 px at 14 pt);
   - small kana — a uniform shift of about 0.1 em right and 0.1 em up (8-14 % in both fonts);
   - brackets, ー, … — the rotation step 3 draws already matches (2-5 % of an em);
   - 〜 and ～ — the vertical form is the rotated glyph mirrored; step 3 draws it rotated only.
   It works with every font kind (built-in, SD `.cpfont`, TrueType, a reader's own `.ttf`) and nobody downloads a font
   again.
2. **Tate-chu-yoko follows the book's CSS only:** `text-combine-upright` (`all`, `digits N`) and its prefixed forms,
   which step 0 parses. No setting, no automatic digits. The acceptance book marks all 56 of its chapter numbers so.
3. **The dictionary on vertical pages is in step 4**, as its last PR.

## The pieces, one PR each, in this order

### PR 1: small fixes the step 3 PR named

- **Widows and orphans off in a vertical section.** `keepTogetherBreak` keeps 2 lines; with 9 columns a page
  (against 14 lines) it ended pages of the acceptance book after 5 of 9 columns. The rule was written for Latin
  text; on a page of 9 columns it costs more than it helps. A vertical section takes 1 for both (a heading's
  keep-with-next stays), whatever the reader's "Avoid Widows & Orphans" setting (#590, which came after this design)
  says: the setting governs horizontal pages. *My call.*
- **A lone image on a vertical page is centred along the block axis** (`ReaderPageLayout::imageOnlyPageYOffset`
  returns 0 for a vertical page: a small picture sits at the right edge). *My call.*
- Section version 105 (vertical pagination changes). Horizontal goldens must not move.

### PR 2: placed marks and A3a along the column

- **One decision per code point** beside `drawsUpright` in `VerticalOrientation`: `Placement::None`, `Reflect`
  (、。，．, U+3001 U+3002 U+FF0C U+FF0E), `SmallKana` (the hiragana and katakana small forms, U+31F0-31FF).
- **The ink query grows a vertical half:** `getGlyphInk` returns the ink's top and height too (`EpdGlyphRef` already
  holds `top` and `height`). Same cost as today: an interval lookup, or the glyph fetch the draw makes anyway.
- **Draw** (`drawTextVertical`): a Reflect mark's ink box is mirrored through the centre of its cell, across and along
  the column; a SmallKana one moves 0.1 em right and 0.1 em up. Traditional Chinese (`Punctuation::
  TraditionalChinese`) centres a Reflect mark's ink in its cell instead, as its fonts draw them. The advance stays one
  em, so measurement does not change.
- **The profile reaches the draw as a line flag.** The punctuation profile is book-wide (`Section.cpp`, from the
  OPF language), and `TextBlock`, `Page` and `drawTextVertical` know nothing of it. A vertical line built under
  `Punctuation::TraditionalChinese` gets a "centred marks" bit in its flags byte (bit 3, reserved and rejected on
  read today). No global state.
- **A3a and A3b along the column.** `inkShift` (ParsedText) reads horizontal ink, which happens to give the right
  shift for a rotated bracket. For an upright placed mark in a vertical line it takes the placed ink's extent along the
  column. Then a narrowed 、 or 。 (at a column's end, before a closing bracket) keeps its ink inside its box: the
  overlap the step 3 PR showed (のか。」, である。）) goes away.
- **The wave dashes stay rotated** unless the renderer can mirror a glyph cheaply; it cannot today, and a mirrored
  blit is a renderer change of its own. *My call*: note it, do not build it here.
- Section version bump (the shifts change positions). Tests: the vertical rule checker gains "every placed mark's
  ink lies inside its box along the column"; `VerticalDrawTest` pins the offsets on the shim's glyphs; the fixture
  gains small kana and 、。 at column ends and before brackets. Traditional Chinese needs a book of its own (the
  profile is book-wide): a small zh-Hant vertical fixture from the same generator.

### PR 3: tate-chu-yoko

- **Carry the flag** from the style stack to the words, as `text-emphasis` is carried (`effectiveTextEmphasis_` →
  `addWord`); the element's text becomes its own token. `digits N` combines runs of 1 to N ASCII digits only; `all`
  combines the element's whole text.
- **Store** it per word in the line's annotation records (a new `Annotation` kind beside Ruby and Emphasis; the word
  style byte has no free bit). A combine record reserves no annotation band: today any record makes the line reserve
  the ruby band (`hasAnnotations()` → `annotationBandFor`, and the parser's line height), which would widen every
  column with a chapter number. Section version bump.
- **Measure** a combined word as one em along the column.
- **Draw** it upright in the reader's frame, its glyphs side by side, centred in the cell; scaled down to the em's width
  with the existing scaling path when wider (two half-width digits are about 1.1 em). More than 4 characters, or a
  scale below the font size ladder's floor: drawn sideways, as today. *My call* on the cap.
- Links on a combined run (all 56 in the acceptance book are links) hit its cell; `wordBox` returns the cell.

### PR 4: the dictionary on vertical pages

- **Word boxes** in the word screen (`DictionaryWordSelectActivity::extractWords`) are mapped from the content frame to
  the reader's with `turnedframe::contentOffsets` and `toReader`, as `publishPageLinkTargets` does. A row is a column.
- **Navigation:** Up and Down step through the words in reading order (down the column, then on to the next column);
  Left and Right move to the neighbouring column on that side, to the word nearest the same height. The first
  selection is the middle column's word nearest the page's vertical centre. Touch hit-tests the mapped boxes.
- **The highlight** redraws its words white on black with `drawTextVertical` inside the turned frame. Two small
  changes make that possible: `drawTextVertical` takes a colour (it draws black only), and the frame scope moves from
  `Page.cpp`'s anonymous namespace to a header the word screen can use.
- The CJK run, the lookup and the definition screen are unchanged; they read text, not geometry.
- `openDictionary`'s vertical guard and the menu row's are removed.

## Constraints

- ESP32-C3: no new resident RAM; the draw path still allocates nothing; flash growth measured per PR (`gh_release`
  against master).
- Every PR: host goldens of horizontal books byte-identical; the vertical rule checker extended for its rule; X4
  screenshots of the acceptance book and `test_vertical_text.epub`. An X3 run is an extra, not a gate.
- Section versions: each PR that changes vertical layout bumps once; renumber at merge if master moved.

## Left out, on purpose

- `text-orientation` (parsed, still not read): no book at hand uses it. Drawn as `mixed`.
- The unscaled ink-only pre-render for vertical lines (measured cost in #577: first pre-render 2-2.5×, floors +13-20
  KB with an SD `.cpfont`); a perf step of its own.
- か + U+3099 drawing a replacement box: on master in both modes, not vertical; a normalisation fix of its own.
- The five settings labels' translations: a translation pass of its own.

## Left to the builder

- The exact small-kana list and whether U+FE50-FE52 (small 、。 forms) join Reflect.
- Whether Reflect uses the cell from the font's em (一's advance and the ascender) or 口's ink centre; the measurement
  above used 口.
- The combined run's scale path (a smaller ladder size or `drawTextScaled`).
