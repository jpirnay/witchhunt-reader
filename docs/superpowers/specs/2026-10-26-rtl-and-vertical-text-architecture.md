# Right-to-left and vertical text: gap analysis and architecture

Status: decided by the maintainer on 2026-10-26 (section 9); the turned frame of section 5.2 was tried on the X3 the
same day (section 10).

The question: what is missing for Hebrew and Arabic books and for vertical Chinese and Japanese, and how to build it. The
starting idea was a "render format provider" per language (Latin, Hebrew, Arabic, Japanese, Traditional Chinese,
Simplified Chinese, Korean) that drives orientation, hyphenation, line distances and so on. Section 4 takes that idea
apart; section 5 is the proposal.

References: crosspoint-reader `upstream/develop` (Hebrew #1700, Arabic #2541 and follow-ups), freeink-sdk's FreeInkBook
(`freeink-sdk/libs/book/FreeInkBook/src/layout/ChapterLayout.cpp`), and matcha-reader
(github.com/eszter007/matcha-reader, MIT, a crosspoint fork with vertical Japanese and Chinese).

## 1. Two problems that share a little

| | Hebrew, Arabic, Persian, Urdu | Vertical Japanese and Chinese |
|---|---|---|
| Lines | horizontal | vertical columns |
| Characters in a line | right to left, with Latin words and numbers inside running left to right (bidi) | top to bottom; CJK upright, Latin turned sideways |
| Lines on a page | top to bottom | right to left |
| Pages | right to left | right to left |
| Glyph work | Arabic joining forms, lam-alef, marks over and under the base | vertical punctuation forms, small kana, turned brackets and dashes, tate-chu-yoko |
| Markup | `dir`, CSS `direction`, `text-align: start/end` | CSS `writing-mode`, `text-combine-upright` |
| Shared | OPF `page-progression-direction`; page turns, tap zones, progress bar; logical start and end | same |

Vertical text is not right-to-left text: its characters run top to bottom, and only the columns and the pages advance
right to left. The two share page progression and the start/end idea and nothing else, so they are two pieces of work on
shared foundations.

## 2. What we have

- **The per-language profile exists.** `TypographyProfile` (`lib/Epub/Epub/TypographyProfile.h`, PR A3) is chosen once
  per section build from `dc:language` and travels by value Section → ChapterHtmlSlimParser → every ParsedText. It
  already holds `Direction direction` and `verticalAllowed`; nothing reads either (`docs/future_work/test-suite.md:79`).
- **Character rules stay with the characters.** Kinsoku and the CJK breaks apply by codepoint in every book
  (`CjkLineBreak`); the profile adds only what needs the language. The new scripts should keep that split.
- **Layout is logical in all but name.** ParsedText computes each word's offset from the line's start
  (`ParsedText.cpp:1712`); the parser stacks lines down the page against `viewportHeight` (`addLineToPage`,
  `ChapterHtmlSlimParser.cpp:4508`) and shifts them by `leftInset()` (`:4573`). A laid-out line keeps one `xpos` per word
  and its own `(x, y)` (`TextBlock.h`, `Page.h`); render draws every word on its own at its x.
- **The renderer has turned frames.** Four orientations, each with a fast glyph path (`GfxRenderer.cpp:292-340`);
  `setDrawOrientation` draws in another frame without changing the held one (`GfxRenderer.h:383`); a turned-text path
  for the side-button labels (`TextRotation::Rotated90CW`, `GfxRenderer.cpp:290`); combining marks positioned over their
  base (`drawText`).
- **Fonts.** The TTF/CFF engine reads GSUB only for fi, fl, ff, ffi, ffl (`ttf/TtfOpenType.h:44`). The CJK families are
  cut without GSUB, vhea, vmtx and VORG (`cjk_subset.py:67`). The Japanese, Simplified Chinese and Korean families have
  none of the Unicode vertical forms (U+FE10-FE19, U+FE30-FE4F: JIS X 0208, GB2312 and KS X 1001 lack them); Traditional
  Chinese has 27 of U+FE30-FE4F through Big5 and none of U+FE10-FE19. No built-in face has a Hebrew or Arabic letter
  (the converter keeps only U+061C, `fontconvert.py:221`).
- **Reserved, not wired:** compiled content's base-direction bits and per-word `bidiLevel`
  (`content/CompiledContent.h:30-48`). Phase 3 has not started.

Missing outright:
- `dir`, CSS `direction`, `writing-mode`, `text-orientation` and `text-combine-upright` are not parsed;
  `text-align: start` and `end` become Left and Right at parse time (`CssParser.cpp:568-569`), so the direction they
  depend on is lost.
- OPF `page-progression-direction` is not parsed.
- Page turns are left to right only: buttons (`EpubReaderActivity.cpp:1041-1070`), tap zones (`ReaderUtils.h:166-172`),
  swipes, the progress bar.
- No bidi, no shaping, no vertical drawing.
- TXT and MD lay out their own lines (`LineReaderActivity`), so they need the work again.
- No Hebrew, Arabic or Persian UI language, and no UI glyphs for them.

## 3. What the references do

| | upstream crosspoint (RTL) | FreeInkBook (RTL) | matcha-reader (vertical) |
|---|---|---|---|
| Algorithm | mintty's minibidi (MIT): full UAX #9, presentation-form shaping with Perso-Arabic letters (`lib/MiniBidi`, ~900 lines of C and a 157-line class table) | its own UAX #9 subset: R, L, EN and neutrals, levels 0-2 | its own kinsoku and hand-made orientation classes (`Kinsoku.h`) |
| Where it runs | word order in `extractLine`; shaping and reversal inside the renderer, on every measure and draw of a string with Hebrew or Arabic lead bytes | the whole paragraph, in logical order, before line breaking; lines stored in visual order; the renderer knows nothing | a second engine: VerticalParsedText, VerticalSection, VerticalTextBlock, ~6,700 lines, its own section cache |
| Direction from | `dir`, CSS `direction`, the first strong characters; tap zones from the book language (#3709) | the first strong character | a CJK-language gate, a per-book switch, a global Automatic/On/Off (Automatic = OPF ppd `rtl`); CSS `writing-mode` is not read |
| Memory | a 3 KB workspace (2 × 128 `bidi_char`), allocated on first use, behind a mutex (#2554, #4116) | one level byte per text byte | a glyph grid, 20 B per glyph per page; heap trouble fixed repeatedly (#454, #530) |
| Limits | shaping sees one token at a time, so a word split by a style change (a bold letter mid-word) cannot join across the split; CPU on every draw; the positioning loop in `extractLine` exists twice | no number separators or terminators (ES, CS, ET), no explicit embeddings or isolates | no tables, no heading sizes, no justification; punctuation placed by hand offsets; 237 lines of its reader activity refer to the vertical mode |

**Lessons.**
1. Upstream proves the algorithm and the tables on these devices. Its placement (inside the renderer, per draw) is the
   part not to copy; FreeInkBook's placement (per paragraph, at build time, stored visual) is the one to take.
2. Matcha proves the typography: tentsuki, squeeze, small kana in the corner, Traditional Chinese centred marks, ruby on
   the right, sesame dots. It also shows what a second engine costs: every reader feature needs a vertical twin, and
   tables, heading sizes and justification never got one. Its rules are worth taking; its engine is not.

**koreader-tategumi** (m-tky/crengine and m-tky/koreader-base, active in October 2026), looked at on 2026-10-26 because
it is the vertical Japanese most often recommended for KOReader:
- **Layout:** each horizontal formatter function has a `*Vertical` sibling cloned from it, with an x/y coordinate swap
  (`lvtextfm_vert.cpp`, ~2,800 lines; ~4,500 lines of vertical code in all). Its own header asks that every upstream
  fix to a horizontal function be checked against its vertical copy: the cost the turned frame avoids.
- **Glyphs:** HarfBuzz applies the font's `vert` and `vrt2`; failing that, a 23-entry table maps to the Unicode vertical
  forms (from LuaTeX-ja); failing that, the glyph bitmap is rotated 90°. The font's `vmtx` is used where present.
  JLREQ slot widths come from LuaTeX-ja's `jfm-ujisv.lua`.
- **"Kerning must be Best"** is a crengine detail: only its Best mode runs HarfBuzz, so only Best applies `vert`. It
  has no counterpart here.
- **What it changes here:** nothing in the architecture. The same three-step order (the font's vertical form, then the
  Unicode vertical form, then rotation) is section 5.4's. Unicode itself gives the vertical-form mapping (33 code points
  in U+FE10-FE4F decompose `<vertical>` to their horizontal form), so no table needs to be taken from the fork, which
  is GPL code in an MIT project anyway. LuaTeX-ja's classes are a cross-check for A3a in vertical.

## 4. The proposal under review: a format provider per language

**What is right.**
- One place decides a language's rules, and layout asks it. That is what A3 built.
- Hyphenation, punctuation widths, justification and default line spacing belong there.

**What should change.**
1. **Orientation is not a language rule.** Japanese books are often horizontal (technical books, web novels,
   translations); Chinese books are vertical in Taiwan and mostly horizontal in the mainland. The book says which (CSS
   `writing-mode`, OPF ppd), the reader may override it, and the language only says whether vertical is legitimate
   (`verticalAllowed`). A provider that drives orientation would set a horizontal Japanese book vertically.
2. **Direction belongs to characters and paragraphs, not to the language.** A Hebrew paragraph holds Latin words and
   numbers that run left to right; an English book quotes Hebrew. A paragraph's base direction comes from `dir`, then
   CSS, then its first strong character. The language is the fallback for a paragraph of neutrals, and for page
   progression when the OPF says nothing.
3. **Shaping belongs to the script.** Arabic letters join in an English book too. Like kinsoku, it is a character rule
   and applies wherever the characters are.
4. **Data, not a class per language.** The differences are a handful of independent switches (breaking, justification,
   punctuation table, grid, hyphenator, default direction, vertical allowed, leading), each with two or three values. A
   language is a combination: Hebrew is Latin breaking with a right-to-left default; Persian is the same plus Arabic
   letters, which bring their shaping with them; Korean is the CJK repertoire with word breaking. A class per language
   would copy the shared logic once per language, put virtual calls in the measurement loop, and be harder to hand by
   value to a background build.
5. **The provider is the small part.** The work is a coordinate model that maps logical to physical positions, a
   measurement seam, a bidi and shaping stage, and a vertical draw path. None of those belongs to a language.

So: keep `TypographyProfile`, give its two unread fields a meaning and add a few more, and add the two things a language
profile cannot be: a **flow context** per book and section, and **script tables** per character.

## 5. Architecture

### 5.1 Who decides what

| What | Owner | Resolved | Carried |
|---|---|---|---|
| Bidi class, mirroring, Arabic joining, vertical orientation (UAX #50), line-break class | the character: tables in flash | per codepoint | nothing to carry |
| Punctuation widths, grid, justification, hyphenator, default direction, vertical allowed, leading default | `TypographyProfile` (the language) | once per section | by value, as now |
| Writing mode, page progression | `FlowContext` (the book and the reader's settings) | once per book and section | BuildParams, property hash, section header |
| A paragraph's base direction | the block: `dir`, else CSS `direction`, else first strong character, else the profile | per block | BlockStyle at parse time; baked into positions |

**FlowContext.**
- **Writing mode:** horizontal-tb or vertical-rl. Vertical when the profile allows it and the reader has not turned it
  off, and either the book asks for it (CSS `writing-mode`, with `-epub-` and `-webkit-`, on the section's `html` or
  `body`; failing that OPF ppd `rtl`, as matcha's Automatic) or the reader forces it on. A global setting
  (Automatic/On/Off, as matcha's Tategaki and Zhipai) and a per-book override.
- **Page progression:** OPF ppd; else right to left when vertical; else right to left when the profile's default
  direction is right to left (upstream's rule for Hebrew and Arabic books without ppd).
- **Per section,** because a vertical book's colophon may be horizontal. A mode change inside a section is not
  supported: the `html`/`body` value wins.

**The global setting is needed, not a nicety.** Of the Japanese books at hand (2026-10-26), a commercial one declares
`writing-mode: vertical-rl` and ppd `rtl`, but an Aozora Bunko conversion declares neither: under Automatic it opens
horizontally, and only the reader's On (or the per-book override) sets it vertically.

**Profile additions.** Tags for Hebrew and Yiddish (`he`, `yi`) and Arabic-script languages (`ar`, `fa`, `ur`, `ps`,
`ug`, `sd`, `ku-Arab`): default direction right to left, no hyphenator. `verticalAllowed` for Japanese, Chinese and
Korean (section 9). A leading default is possible, but cjk.md already says to judge a CJK default for line spacing from
screenshots first. One Chinese book at hand declares `cn`, which is not a language tag: today it gets Latin rules.
Reading `cn` as `zh` is a one-line fallback worth taking.

### 5.2 One engine: logical layout, physical mapping

Layout keeps working in inline and block terms, which today are x and y. The mapping to the panel happens in one place
per case:

- **Horizontal, left to right:** as today.
- **Horizontal, right to left:** the line is laid out from its start; a visualize pass (5.3) turns start offsets into
  physical x before the TextBlock is built. The composer takes the start inset from the right for a right-to-left block
  (`rightInset()` where it now takes `leftInset()`); list markers go on the right. CSS floats stay physical.
- **Vertical-rl:** the page is laid out as a horizontal page in a frame turned 90° clockwise: inline runs down, block
  runs leftward, so physical x = W − block and y = inline. That frame is a renderer orientation one step clockwise from
  the reader's. Composer, line breakers, page breaks, tables, rules and the ruby band all work unchanged in it:
  - The ruby band "above" the line lands right of the column, where JLREQ puts it; emphasis dots too.
  - Latin drawn normally in the turned frame comes out turned 90° clockwise, which is CSS's `text-orientation: mixed`,
    through the fast path that frame already has.
  - Upright characters are drawn in the reader's own frame, centred on their em box mapped to the panel: another
    existing fast path, no new rotation code.
  - Tables come out as vertical-rl tables (rows become columns), which is what CSS does.
  - Images are drawn in the reader's frame at the mapped rectangle; the parser swaps an image's width and height when
    it sizes it in the turned frame.
  - Taps map through the content frame (`tapToLogical` exists); selection overlays drawn in the frame map themselves.
  - Status bar and header stay in the reader's frame.
  - The frame switch lives inside `Page::render` and `renderTextOnly`, so the black-and-white pass, the grayscale
    planes, the prewarm scan and the pre-render all inherit it.

**Why not matcha's second engine.** It would duplicate tables, images, footnotes, links, anchors, KOSync offsets,
progress, the pre-render, the dictionary, background builds and the section cache, and its glyph grid costs 20 B per
glyph per page where we keep today's per-token storage.

### 5.3 Right to left: bidi and shaping at build time, stored visual

In ParsedText, once a paragraph's tokens are collected (`layoutAndExtractLines`) and before widths are measured:
1. **Levels.** The base direction comes from the block and is carried across the 32-token run flush and continuation
   passes (upstream needed #3875 for the same). Resolve UAX #9 levels per byte in arena scratch with minibidi's tables
   and `do_bidi`, not FreeInkBook's subset: `3.14`, `−5` and `$5` need ES, CS and ET.
2. **Split.** A token whose bytes have more than one level becomes several glued tokens (the continues bit), as CJK
   tokens already are. Every token then has one level.
3. **Shape** Arabic letters in logical order across glued tokens, so a bold letter mid-word still joins; skip
   transparent marks, honour ZWJ and ZWNJ, form lam-alef. The output is presentation forms (U+FB50-FDFF, U+FE70-FEFF)
   from minibidi's tables.
4. **Measure and break** the shaped text with the four breakers as they are. UAX #9 breaks lines first and reorders
   each line afterwards, which is this order.
5. **Visualize** in `extractLine`, after today's single positioning loop: reorder the line's tokens (L2), reverse each
   odd-level token's text by grapheme cluster so marks stay after their base, mirror brackets (L4), and place the tokens
   in visual order. `text-align: start` and `end` resolve against the base direction; the first-line indent sits on the
   start side. CJK edge trims and ruby boxes come from the logical pass and keep working.
6. **Store** visual order in TextBlock: no new fields, and render does not change.

**Rules.**
- A paragraph with a left-to-right base and no Hebrew or Arabic byte skips all of it, so Latin and CJK goldens stay
  byte-identical.
- No kerning inside right-to-left tokens: the pairs would be looked up in visual order. Latin ligatures are never
  applied to presentation forms (upstream #3294, with its test).
- Dictionary and clipping recover the logical word from the content: reverse it back and map presentation forms to
  their base letters by their compatibility decomposition.
- The same helpers give UI code a visual string for book titles, TOC entries and chapter names, drawn with the UI
  fallback of CJK step 6 widened to Hebrew and Arabic.

### 5.4 Vertical: the draw path

`GfxRenderer::drawTextVertical(fontId, x, y, text, style)`, called in the turned frame, treats each codepoint by its UAX
#50 class:
- **U** (CJK, kana, most symbols): upright, one em along the column, centred across it.
- **R** (Latin, digits, most Western punctuation): sideways through the turned frame's fast path; advance and kerning as
  today.
- **Tu, Tr** (CJK punctuation, brackets, dashes, small kana, ー): the vertical form when the font has one (5.6);
  otherwise upright for Tu and sideways for Tr, with matcha's offsets for 、。 and small kana.
- **Tate-chu-yoko** (`text-combine-upright`, optionally automatic for two digits): one em, the glyphs side by side,
  scaled to fit.

**Measurement.** `ParsedText::measureWordWidth` asks a vertical variant that sums the same per-codepoint advances. A
full-width CJK glyph's horizontal advance is already one em, so most widths do not move; the seam is for tate-chu-yoko
and for upright characters whose advance is not an em.

**A3a in vertical.** The JLREQ widths are the same along the column. `inkShift` needs the ink of the vertical form along
the column instead of `getGlyphInk`'s horizontal ink. Traditional Chinese keeps its centred marks, which the profile's
punctuation table already says.

### 5.5 Page progression

One `pageProgressionRtl` from FlowContext, read by the page-turn buttons (a setting: follow the book, or fixed), the tap
zones, swipes, the progress bar's fill side and the percent slider. Upstream flips the tap zones by book language, matcha
by its vertical decision; one rule covers both cases here.

### 5.6 Fonts: bake GSUB when the font is built

Vertical alternates (`vert`, `vrt2`) and Arabic joining forms (`isol`, `init`, `medi`, `fina`, `rlig`) are GSUB
substitutions. The device has no shaper and should not get one: the vertical ones are one-to-one substitutions with no
context, which a table lookup does, and HarfBuzz would cost hundreds of KB of flash and needs the full GSUB and GPOS
tables the CJK cuts leave out. They go into the cmap when the font is cut:
- **`cjk_subset.py`:** map each `vert` alternate to its Unicode vertical form, or to a private code point where Unicode
  has none (a few dozen glyphs per family). The manifest version goes up and the families are re-cut. `vert`, not
  `vrt2`: `vrt2` also holds pre-rotated glyphs for what is set sideways, and the turned frame already rotates those.
- **A font with the Unicode vertical forms but no baked map** (Traditional Chinese has 27 through Big5): the 33
  `<vertical>` decompositions of U+FE10-FE4F say which character each form stands for.
- **Hebrew and Arabic families** in the Font Manager (Noto Sans and Serif Hebrew, Noto Naskh or Sans Arabic), with
  presentation forms baked from GSUB where the cmap lacks them.
- **A reader's own `.ttf`** without presentation forms draws Arabic unjoined. Reading GSUB single substitutions on the
  device (the SDK's Gsub reader already loads the table for ligatures) is a later option.

### 5.7 Cache and file formats

- **BookMetadataCache:** a page-progression byte (cache version bump).
- **Section:** the writing mode in BuildParams, the property hash and the header; `SECTION_FILE_VERSION` bump.
  Right-to-left needs nothing new on disk: positions and text are already visual.
- **Nothing per word.**

## 6. Gap list

| Area | Right to left | Vertical | Where |
|---|---|---|---|
| `dir`, CSS `direction` | parse, inherit | | `CssParser`, `ChapterHtmlSlimParser` |
| CSS `writing-mode`, `text-combine-upright` | | parse (`html`/`body`; spans) | same |
| `text-align: start/end` | keep logical, resolve per block | | `CssParser.cpp:568-569`, `CssStyle.h` |
| OPF `page-progression-direction` | parse | parse | `ContentOpfParser`, `BookMetadataCache` |
| FlowContext and settings | page progression | plus the vertical decision | `Section::BuildParams`, settings |
| Profile | default direction read; new tags | `verticalAllowed` read | `TypographyProfile` |
| Script tables | bidi class, mirroring, joining | UAX #50 | new library (minibidi port) |
| ParsedText | levels, split, shape, visualize | measurement seam | `ParsedText.cpp` |
| Composer | start inset, list marker side | turned frame; image size swap | `addLineToPage`, image placement |
| Renderer | nothing | `drawTextVertical`, frame per glyph | `GfxRenderer` |
| Page render | nothing | frame switch in every pass | `Page.cpp`, `TextBlock.cpp` |
| Fonts | Hebrew and Arabic families, baked forms | baked vertical forms | font scripts, manifest |
| Reader | buttons, taps, swipes, progress bar | same, plus tap mapping and selection | `EpubReaderActivity`, `ReaderUtils` |
| Dictionary | logical word recovery | hit test in the frame | `DictionaryWordSelectActivity` |
| UI text | visual strings for titles and TOC; glyph fallback | | CJK step 6, widened |
| TXT and MD | bidi in their line layout | later (Aozora TXT) | `LineReaderActivity` |
| Tests | fixture book, rule checker | fixture book, rule checker | `test/epub_pipeline` |

## 7. Order of work

Each step is its own plan and PR, like the CJK roadmap.

| Step | What | What a reader sees |
|---|---|---|
| 0 | Parse `dir`, `direction`, `writing-mode`, `text-combine-upright`, `start`/`end`, OPF ppd; FlowContext; page progression in the reader; generated fixture books | right-to-left books turn the right way; nothing else changes |
| 1 | Bidi core and Hebrew: levels, split, visualize, mirrored insets, a Hebrew family | Hebrew books readable, niqqud included |
| 2 | Arabic script: logical shaping, ligature guard, an Arabic family with baked forms, Persian and Urdu letters | Arabic and Persian books readable |
| 3 | Vertical frame: turned frame, `drawTextVertical` (U and R), measurement seam, images, taps | vertical Japanese readable, punctuation rough |
| 4 | Vertical typography: baked vertical forms (fonts re-cut), small kana, tate-chu-yoko, A3a in vertical, Traditional Chinese marks | vertical text set properly |
| 5 | UI and other readers: visual strings and glyph fallback for titles and TOC; bidi in TXT and MD | right-to-left titles in the library |

After step 0, steps 1-2 and steps 3-4 do not depend on each other, and neither depends on CJK steps 6-9 in
`docs/future_work/cjk.md`.

**Validation.** Every step runs the host pipeline goldens (Latin and CJK books byte-identical) plus a rule checker for
its own rules: for bidi, visual order and mirroring on a generated fixture; for vertical, every U character upright,
every R run sideways, kinsoku at column ends. On the device: screenshots (`CMD:SCREENSHOT`), the build arena's high-water
mark and the free heap after a page turn (`CMD:STATE`).

## 8. Costs

- **Flash:** minibidi's class, mirror and shaping tables and code; UAX #50 as ranges. To be measured with the build
  inventory tool rather than guessed.
- **RAM:** a level byte per paragraph byte in the build arena, during layout only. Nothing resident per word. The
  vertical draw path allocates nothing. Upstream's resident 3 KB workspace is not needed.
- **Time:** shaping runs once per build instead of on every draw. The vertical draw switches frame once per upright
  glyph, an atomic store.

## 9. Decisions

By the maintainer, 2026-10-26:
1. **Right-to-left shaping at build time** (section 5.3), not in the renderer.
2. **Vertical text through the turned frame** (section 5.2), not a second engine.
3. **Follow the book, with overrides:** CSS `writing-mode`, then OPF ppd; a global setting and a per-book override.
4. **No right-to-left UI for now.** Hebrew and Arabic translations and mirrored screens are out of scope. Step 5's
   visual strings for book titles and TOC entries stay, since they belong to reading right-to-left books.
5. **Korean may be set vertically.** Modern Korean books are horizontal (vertical writing left mainstream publishing by
   the 1990s), so Automatic will almost never choose it; but older books, poetry and Hanja-heavy texts exist, KLREQ
   covers vertical Hangul, and in this design it costs nothing beyond `verticalAllowed`: Hangul is upright (UAX #50
   U) and Korean's word breaking works along a column as it does along a line. Korean uses Western punctuation, which
   UAX #50 turns sideways in a column; KLREQ prefers the CJK marks there, a refinement for later.

## 10. Spike: the turned frame on the X3

Branch `spike/vertical-frame` (8f28584a0, local, not for merge): every Chinese or Japanese book laid out vertically.
The build swaps the viewport's width and height; `Page::render` and `renderTextOnly` draw in the turned frame;
`TextBlock::render` draws upright characters through the reader's frame with a rough UAX #50 table and fixed offsets
for 、。 and small kana. 248 lines in 5 files; flash +1,956 B, static RAM +8 B. Section version 201, so no cache aliases a
master one.

**What it showed** (X3, portrait, NotoSansJP 14 pt, `test_cjk_ja_soseki` and `test_cjk_line_breaking`):
- Columns run top to bottom and advance right to left, with page breaks, first-line indents, kinsoku and justification
  unchanged from the horizontal layout.
- A centred heading is centred along its column; a Latin heading ("1. kinsoku") is set sideways.
- Ruby lands on the right of the column, upright, over the right base characters (61 groups in the Soseki excerpt):
  nothing in the ruby layout changed.
- 「」『』（）, ー, …… and —— turn into their vertical shapes by being drawn sideways; ！？ stay upright; the
  narrowed `。」` pairs of A3a keep their spacing.
- The status bar stays in the reader's frame.
- Every turn hit the pre-render; the panel update is unchanged (~487 ms FAST).

| Same book, same pages | pre-render | heap cost of the pre-render |
|---|---|---|
| horizontal (master 0625ac40a) | 63-70 ms | 5.8-6.1 KB |
| vertical (spike) | 62-83 ms | 6.4-10.4 KB |

**Left for step 3:**
- The pre-render costs ~4 KB more heap in vertical. Not investigated; the spike's per-character advance and ascender
  lookups during the prewarm scan are the first suspect.
- Images, the dictionary's word boxes, taps, and page progression were not touched by the spike.
- The corner offsets for 、。 and small kana are by eye; the baked vertical forms of 5.6 replace them where the font
  has them.
