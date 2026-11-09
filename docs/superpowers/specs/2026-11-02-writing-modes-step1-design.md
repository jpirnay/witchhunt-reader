# Writing modes step 1: right to left, bidi and Hebrew (design)

Status: approved by the maintainer on 2026-11-02 (decisions 0-8 are the maintainer's; a builder check ran first). It builds
on `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md` (5.1, 5.3, 5.5, 5.7, section 7 row 1)
and "What step 1 takes from step 3" in `docs/design/writing-modes.md`. Decisions 1-4 replace part of the spec's
5.3 and its 5.6 for Arabic.

Amended on 2026-11-02 after a review of the PR 2 plan, by the maintainer's decisions:
- deviation 12 (removed controls no longer split runs);
- transparent marks not counted toward `BIDI_MAX_LINE`;
- the reader's title strings in visual order (PR 3).
Deviation 11 names the line pass's entry points, and the memory and stack figures are corrected.

## Intent

A Hebrew book should read as a printed one:
- lines run right to left, with Latin words and numbers inside them left to right;
- punctuation sits where UAX #9 puts it, and brackets are mirrored;
- niqqud sits on its letter;
- paragraphs are indented and lists numbered on the right;
- pages turn right to left.

An English book that quotes Hebrew shows the quote correctly and stays an English page.

The code stays as close to upstream crosspoint's bidi as this design allows, so upstream's fixes keep porting. This
step also builds the place where step 2's joint shaper (Arabic and the Indic scripts) plugs in.

Non-goals:
- Arabic joining: Arabic letters are reordered in step 1 but drawn unjoined until step 2.
- A right-to-left UI (spec decision 4).
- Visual strings for the TOC, the library's other text and the rest of the UI; bidi in TXT and MD (step 5). The
  status bar's chapter title and the Home and Library book titles are the exception (PR 3, "The reader").
- Bidi inside a vertical section, which stays as steps 3 and 4 left it.

## Decisions (maintainer, 2026-11-02)

0. **Keep as close as possible to upstream's bidi.** Port upstream's code, and deviate only where this design needs
   it. Every deviation is named below.
1. **The visual form of a word is made once, at section build, and stored next to its logical text**: a display
   form, as #3787 stores shaped words. The renderer draws it and runs no bidi. The dictionary, footnote links, the QR
   text and KOSync keep reading the logical text. The spec's decision 1 (build time) holds. Its 5.3 plan to store
   only the visual text and recover the logical word is dropped.
2. **No Arabic joining in step 1.** minibidi's `do_shape` is not called. It waits for the joint shaper.
3. **Upstream's divine-name rule (#3834) is taken, as a shaper.** יהוה (with any marks) is drawn as ה'. The rule
   is the Hebrew module of the new shaping layer and works on the logical word. The stored text keeps יהוה.
4. **Step 2 is the joint shaper, right after step 1, Indic first.** Port #3787 at its head then, merged upstream
   or not. Then add an Arabic joining module on the same engine. The spec's 5.6 plan to bake Arabic presentation
   forms into the fonts is dropped.
5. **Punctuation follows UAX #9, not upstream's per-word result.**
   - Upstream gives a word one direction, from its first letter. So in an English sentence, `He said שלום.` draws
     the period left of the Hebrew word.
   - Here a line is resolved as a whole. A word whose characters land apart is split into glued pieces, so each mark
     goes where UAX #9 and browsers put it.
6. **A paragraph without `dir` or CSS `direction` takes the book language's direction, unless it has no letter of
   that direction.**
   - In a Hebrew book a paragraph is right to left unless it has no Hebrew or Arabic letter.
   - In any other book it is left to right unless it has no left-to-right letter.
   - So English quoting Hebrew stays English; a Hebrew-only paragraph in an English book turns right to left; a
     Hebrew paragraph that opens with an English term stays Hebrew.
   - This replaces upstream's probe, which calls a paragraph right to left when one of its first 3 words starts with
     a Hebrew letter.
7. **The divine name's mark is the Hebrew geresh U+05F3, not U+0027.** U+0027 has no direction, so on a whole-line
   pass it drifts in a left-to-right paragraph. The geresh is a Hebrew letter-class mark (R) and stays with the name.
8. **Isolates are ordered on their own.** PR 2's line pass orders each isolate's content by itself, at its own
   direction, and gives the outer pass one neutral (U+FFFC, class ON) in its place. That is UAX #9 BD13's effect.
   minibidi itself stays as it is. (Its isolate handling does; deviation 12 changes how it levels removed
   embedding controls.)

**My calls** (Claude's, for the maintainer to check; each is explained where it appears):
- deviations 4, 5 and 8 (the class table, CSS bidi controls as inserted characters, controls alone needing bidi);
- literal bidi controls kept only in paragraphs that need bidi;
- the logical-order region on reordered lines, and words split into pieces only in the line's stored copy;
- widths for line breaking from each word's own visual form, then exact widths for the pieces;
- `BIDI_MAX_LINE` kept at upstream's 128, with upstream's fallback past it. Since the review it is counted without
  transparent marks (the maintainer's decision);
- page progression from the book's direction, so a CJK book set to Vertical Text Off still turns left to right;
- the default list indent on the start side;
- table columns mirrored in right-to-left tables;
- Hebrew in two new Font Manager families, not a re-cut of the Noto Extended ones;
- the profile tags, Arabic ones included (an Arabic book's pages turn right to left from step 1);
- the shaping layer reporting clusters, and the rules it fixes for step 2;
- the three-PR split.

## What we take from upstream

The baseline is crosspoint-reader `develop` @ `e8299b5f` (2026-11-02).
- **`lib/MiniBidi`:**
  - `minibidi.c`, `minibidi.h`, `bidiclasses.t` and `bidi_pairs.t`: mintty's minibidi, MIT, with the notices kept.
  - `BidiUtils.{h,cpp}`: `startsWithRtl`, `detectParagraphLevel`, `isTransparentMark`, `applyBidiVisual` and
    `computeVisualWordOrder`. Layout calls `computeVisualWordOrder` only as the fallback past `BIDI_MAX_LINE`; the
    rest of the file stays as upstream has it, for step 5's UI strings and for future syncs.
  - Fixes included: #2554 (one shared workspace), #4116 (allocated on first use) and #3834 (the divine name).
  - Upstream's test `test/minibidi_arabic` comes along as a host test.
- **The line positioning** of #1700 and #2541 in `extractLine`:
  - the reordered loop, where gaps, kerning and justification are recomputed in visual order;
  - the `continues` flags re-derived for neighbours that swapped;
  - the default alignment flipped to Right when the book sets none (`textAlignDefined`), explicit `left` kept.
- **A word's own direction** is `detectParagraphLevel(word, paragraph)`, as upstream's `TextBlock` computes it. It
  gives the width a word is broken with.
- **Marks:** `isTransparentMark` (U+0591 and up with bidi class NSM) gets zero advance and is drawn over the
  preceding base glyph. The niqqud anchors `combiningMark::anchorFor` (dagesh inside the letter, shin and sin
  dots over the arms, holam) come from upstream's `EpdFontData.h`. The marks follow their base, as upstream's
  `applyBidiVisual` emits them.
- **Tap zones, page turns and the progress bar** follow the book (#3709), through the `FlowContext` step 3 built.
- **Upstream PRs for each part:** #1700, #2541, #2554, #3198 (inline direction inheritance), #3709, #3834, #4116
  and #4230 (focus reading). The PR body names them, at the commits above.

**The deviations, all of them:**
1. **Placement.** BidiUtils runs at layout and never in the renderer. Upstream's renderer hooks are not ported:
   `resolveVisualText`, `appendShapedRtlTokens`, and the `BidiBaseDir` parameters on `drawText` and
   `getTextWidth`. Neither are the per-draw glyph-cache prewarm of shaped forms, nor the resident workspace. The
   workspace is released when a section build ends. During a build with a lent arena the line pass borrows the
   workspace from it for each call (`bindWorkspace`); otherwise the workspace is allocated on first use.
2. **`applyBidiVisual` does not call `do_shape`** (decision 2), so the linker drops the shaping code and tables
   until step 2.
3. **The divine-name match moves** out of `applyBidiVisual`'s emission loop, where it matches in visual order, into
   the Hebrew module, which matches in logical order (decision 3). It produces the same visual result: ה' drawn
   as `'ה`.
4. **`bidiclasses.t` classes every script we draw as L, as whole blocks:**
   - the scripts are Greek, Georgian, Armenian, Ethiopic, CJK, kana, Hangul and the Indic scripts;
   - upstream leaves them out (they fall to ON) because it does not draw them;
   - for scripts written only left to right, a whole-block L gives the same levels as the UCD's per-character
     classes. It also keeps `isTransparentMark`, which tests NSM, to Hebrew and Arabic. With the UCD's classes,
     Indic vowel signs and U+3099/309A would move onto the overlay path in every book.
   - The Hebrew presentation forms (U+FB1D-FB4F) get their R and NSM classes.
   - Taken as the UCD has them, for the code points upstream lacks: the Hebrew block U+0590-05FF (U+05A2, 05BA,
     05C5 to 05C7 and 05EF), the ligatures U+FB00-FB17, the superscripts and subscripts U+2070-209F, and the
     letterlike symbols and number forms U+2100-218F.
5. **CSS bidi controls.** Step 0 parses `unicode-bidi`, inline `dir`, `<bdo>` and `<bdi>`; upstream has no
   equivalent. Each becomes the UAX #9 control characters CSS defines, inserted into the logical text at the
   element's edges: LRE/RLE, LRO/RLO, LRI/RLI/FSI and PDF/PDI.
6. **The paragraph direction rule** of decision 6 replaces upstream's probe (`RTL_PARAGRAPH_PROBE_WORDS`).
7. **The line pass** of decision 5 replaces `computeVisualWordOrder` plus per-word `applyBidiVisual` for placement
   (below).
8. **Controls alone make a paragraph need bidi:** a literal or inserted control, so `<bdo dir="rtl">` on Latin text
   works. The line pass runs the full `do_bidi` on a line with a control, with no strong-class shortcut.
9. **L4 runs before L2** in `do_bidi` (mirroring while the line is still in logical order). Upstream's order tests
   the logical levels against the reordered line and mirrors the wrong characters on an asymmetric line.
10. **`applyBidiVisual` treats U+FFFD as an ordinary code point** and stops only on NUL, so a token with a bad byte is
    not truncated. `startsWithRtl`, `detectParagraphLevel` and `computeVisualWordOrder` are as upstream has them.
11. **The line pass's entry points.** BidiUtils gains:
    - `orderLine`: one `do_bidi` over a caller's code points at a given level, giving the visual order and the drawn,
      mirrored code points, each run of marks after its base as `applyBidiVisual` emits them;
    - `hasRtlClass` and `strongLevel`, for the layout's tests;
    - `bindWorkspace` and `heapWorkspaceHeld`.
    Upstream's functions are unchanged.
12. **Removed controls keep their run** (maintainer, 2026-11-02, after the review). In `do_bidi`, after L1, each
    character X9 removed (RLE, LRE, RLO, LRO, PDF, BN) takes the level of the preceding retained character, or the
    paragraph level when there is none (UAX #9 §5.2).
    - Upstream gives a PDF the level after its pop, so it splits the run in L2: `RLE שלום PDF : 3 posts` draws
      `םולש3 : posts` instead of `3 :םולש posts`.
    - Every CSS `unicode-bidi: embed` or `bidi-override` span (deviation 5) would mis-order the neutrals after it.
    - It is to be offered upstream.
13. **A bracket pair resolves only its brackets** (UAX #9 N0). In `do_bidi`, N0 sets the two brackets of a pair to the
    pair's direction and leaves the neutrals inside to N1/N2; upstream sets every neutral between them to the pair's
    direction too.
    - Upstream draws `(hello world)` in a right-to-left paragraph as `(world hello)`: the space between two Latin
      words became R.
    - The rule's NSM sentence is added: characters that were NSM before W1 and directly follow a changed bracket take
      its new type.
    - It is to be offered upstream.

## How a paragraph is laid out

In `ParsedText`, for a paragraph that needs bidi only. A paragraph needs bidi when it has a right-to-left base, a
right-to-left letter (Hebrew, Arabic, or their presentation forms), or a bidi control. Every other paragraph skips
all of it, so its layout and its section bytes are what they are today.

1. **Base direction.**
   - `dir` or CSS `direction` when given (`directionDefined`).
   - Otherwise decision 6, decided on the tokens of the first layout pass, and kept across the parser's 96-token flush.
2. **Widths for breaking.**
   - The shaping layer runs on each token's logical text (below).
   - Then `applyBidiVisual` gives the token's own visual form at its own direction, with the embeddings and
     overrides open at the token's start prepended. That form is measured. This is the width upstream measures.
   - An isolate open at the token's start is never prepended, because minibidi has no isolating run sequences. It
     gives the token's level instead (LRI left to right, RLI right to left, FSI the token's first strong letter).
     Only the embeddings and overrides opened inside it are prepended.
3. **Break lines** with the four breakers as they are. Hyphenation runs on Latin tokens only. Hebrew has no hyphenator.
4. **The line pass** (decision 5):
   - `do_bidi` runs once over the line's characters at the paragraph's level. Tokens are joined with a space where they
     are not glued, and the controls open at the line's start are prepended.
   - Each character carries its token and byte offset.
   - Transparent marks ride on their base. Each `isTransparentMark` code point after a base of its own token is
     folded onto it and takes no slot, then comes back right after its base in logical order. W1 gives a mark its
     base's type and level anyway. A pointed line so needs about half the slots.
   - In the visual result, each maximal run of one token's characters becomes one piece: a glued word in the
     TextBlock.
   - A piece's text is its characters in logical order. Its display form is its characters in visual order: mirrored
     (L4), marks after their base as upstream emits them, controls dropped.
   - Pieces are measured exactly. Gaps, kerning and justification are recomputed in visual order, as upstream's
     reordered loop does.
   - A line over `BIDI_MAX_LINE` (128 slots: code points other than folded marks) falls back to upstream's result:
     `computeVisualWordOrder` with per-word display forms, over the words without their marks. A pure right-to-left
     line still comes out right. Two corners stay as upstream has them:
     - `computeVisualWordOrder` declines past 128 slots too, so a right-to-left fallback line reverses every word, an
       embedded Latin phrase's included;
     - it sees isolates as embeddings.
5. **Alignment and indent.**
   - `text-align: start` and `end` resolve against the paragraph's base direction at layout time, because decision 6
     may only settle it there (step 0 resolves them in the parser).
   - The first-line indent goes on the right of a right-to-left paragraph.
   - Only the copy going into the TextBlock is split and reordered. `ParsedText`'s own arrays stay in logical order,
     so the visible offsets KOSync uses do not move.
6. **Store** the line in visual order (upstream's order, which keeps the renderer's geometry as it is). Each word
   (piece) keeps its logical text and its display form when the two differ. A reordered line also stores each
   word's logical position, which the consumers that need reading order walk.

**Literal bidi controls in a book** (LRM, RLM, ALM, and the embeddings and isolates) are kept for paragraphs that
need bidi. Today the parser strips them. A paragraph that does not need bidi still drops them, so its text is
unchanged. Inserted and kept controls count nowhere in the visible offsets, and the word screen strips them from a
lookup.

## Storage and drawing

- **The TextBlock** gains two optional regions, present only on lines that need them:
  - the display forms: a `u16` offset per word (`0xFFFF` for none), then the strings;
  - the logical order: one `u8` per word.
  The flags byte is full, so the header grows. `SECTION_FILE_VERSION` goes to 108.
- **`TextBlock::render` and `wordBox`** draw and measure the display form when there is one. Nothing else in the
  draw path changes: words are drawn at their x, in the order stored.
- **The renderer** gets upstream's mark handling for `isTransparentMark` code points, with the niqqud anchors. It
  goes in the draw, measure and rotated paths that handle `utf8IsCombiningMark` today. A face without the marks
  skips them, as upstream does.

## The shaping layer (the joint shaper's seam)

A small library that turns a token's logical text into the code points (step 1) or glyphs (step 2) it is drawn
with, with one module per script:
- **Step 1:** the Hebrew module, which is the divine-name rule. It matches יהוה in logical order, skipping marks,
  and gives ה' (he, then the geresh U+05F3; upstream draws U+0027).
- **Step 2a:** Indic, the #3787 port. It gives shaped glyph runs.
- **Step 2b:** Arabic joining, on the same engine.

**The rules step 2 inherits:**
- Every output unit carries its cluster: the source byte offset it came from, as HarfBuzz reports it. The line pass
  splits pieces by cluster, so a piece's logical text is always a source range.
- A module sees the token's logical text and its glued neighbours as context (#3787's `RunContext`), so a word
  split by a style change still joins.
- A module that outputs glyphs outputs them in visual order. Bidi's character reversal is not applied to them, but
  the token and piece order on the line stays bidi's.
- A display form is UTF-8 in step 1. Step 2 decides between #3787's private-use glyph tokens and compact glyph
  records, under the same region.

## The reader

- **Page progression** (the gap named in `docs/design/writing-modes.md`, then in `docs/future_work`).
  - `FlowContext::pageProgressionRtlFor` takes the book's direction as its own input: the profile's default
    direction, or `dir`/CSS `direction` on the section's `html` or `body`.
  - A horizontal section turns right to left when the book's direction is right to left, unless the OPF says
    `ltr`.
  - A book whose direction is left to right turns left to right when set horizontally, whatever its ppd. A CJK book
    under Vertical Text Off keeps doing so, and `FlowContextTest`'s `pageProgressionRtlFor(Off, false, Rtl)` stays
    false.
  - A book that is not set vertically keeps its section cache key: the request stays Off.
- **The profile** learns `he`, `iw` and `yi`, and `ar`, `fa` and `ur`: default direction right to left, no hyphenator.
- **Lists:** the default list indent (`LIST_DEFAULT_PADDING_EM`, today always on the left) and the hanging-indent
  clamp go on the start side. That side comes from the direction known in the parser: `dir` or CSS `direction`,
  else the book's direction.
- **Consumers that need reading order** walk the logical order:
  - the QR page text;
  - the footnote-link overlay (two markers on one right-to-left line);
  - the dictionary word screen, when it joins a word's glued pieces into the lookup text. Its cursor still moves by
    position: Left goes left.
- **Bionic reading** on Hebrew words: the bold logical prefix lands on the right. It needs a check, not new code:
  our bionic split is two glued tokens, which the line pass orders. #4230's fix belongs to upstream's in-token split.
- **Tables** in a right-to-left table run their columns right to left, as upstream does.
- **Drop caps:** a right-to-left paragraph takes none. A book's `float: left` cap would cut the first, rightmost
  letter and float it on the left (PR 2).
- **The reader's title strings** (maintainer, 2026-11-02, after the review). The status bar's chapter title and the
  book titles on Home and in the Library are drawn in logical order today, so a Hebrew title shows mirrored on every
  page.
  - Each gets its visual form once, when it is set: one `applyBidiVisual` pass at the title's own direction, kept
    with the string.
  - Nothing is reordered per draw, so deviation 1 holds.
  - The rest of the UI stays out of scope (step 5).
- **Hebrew faces:**
  - Two new Font Manager families, Noto Sans Hebrew and Noto Serif Hebrew: Latin from Noto Sans/Serif, and Hebrew
    (U+0590-05FF, U+FB1D-FB4F) from Noto Sans/Serif Hebrew. The converter takes a second source per style for those
    ranges.
  - New names, not a re-cut of Noto Extended: the flash font cache is keyed on family and size, so a re-cut family
    would show garbled text to anyone who has it cached (the open stale-entry bug, which stays its own fix).
  - The built-in faces get no Hebrew. The title strings above are drawn in the face that draws them today: in visual
    order wherever that face has the letters, and with missing glyphs where it does not, until step 5's faces.

## The pieces, one PR each, in this order

1. **The library.**
   - `lib/MiniBidi` and its tests, with deviations 2 to 4; the shaping layer with its Hebrew module.
   - Nothing calls it yet, so the diff can be read against upstream file by file.
2. **Layout and drawing.**
   - The paragraph pipeline and line pass, the CSS controls, the kept literal controls, and the two TextBlock regions
     with v108.
   - The renderer's marks, the profile tags, list indents, and the table column order.
   - Deviation 12, marks that take no slot, and no drop cap in right-to-left paragraphs.
   - A fixture book and a rule checker.
   - Reader-visible: Hebrew paragraphs read right, but pages still turn left to right.
3. **Reading a Hebrew book.**
   - Page progression, the logical-order consumers, the bionic check and the Hebrew faces.
   - The status bar's chapter title and the Home and Library book titles in visual order, made once when each is set.
   - The docs: `USER_GUIDE.md`, `docs/design/writing-modes.md`, and a reference page for bidi.

## Constraints

- Every Latin and CJK golden stays byte-identical. Only the right-to-left fixtures change (`test_rtl_markup`, and the
  new one).
- Nothrow everywhere: a failed workspace allocation lays out the paragraph without bidi and logs it, as upstream's
  `false` returns do.
- No resident memory:
  - the 1.5 KB workspace and the line pass's scratch (about 6 KB: 384 code points' units, 128 slots of order) live
    during a section build only;
  - on a build with a lent arena both are borrowed from it per call;
  - piece texts and display forms are views, so a bidi line allocates no per-line strings.
- Stack: `do_bidi` takes about 1.3 KB (`types`, `levels` and `skip` 384 B, its `DirStatusStack` about 391 B,
  `openers[63]` 504 B), and `computeVisualWordOrder` 512 B more.
  - Both run on the 16 KB Arduino loop task, under the parser's deepest frames.
  - PR 2 records the loop task's stack high-water on a Hebrew book on both boards.
  - If it is thin, the first three arrays and the `DirStatusStack` move into the lent workspace.
- Measured and reported on the X3 and the X4:
  - flash on `default` and `gh_release`;
  - build heap;
  - the free heap after a page turn.
- Attribution:
  - crosspoint-reader (MIT), naming the commits and PRs above;
  - mintty and minibidi (MIT; Ahmad Khalifa, Thomas Wolff), with the notices in the ported files;
  - the font sources' OFL.
- The device runs flash the web bundle as well (`web-<hash>.bin` to the card's root).

## Review focus

Things no planned test covers outright that would bite a reader first:
1. **A mixed line:** a Hebrew sentence with an English phrase, a year, parentheses and a trailing period, both ways
   round. The fixture pins each mark's side to what a browser shows.
2. **A long paragraph** whose direction decision 6 settles, split by the 96-token flush. Every pass must keep it.
3. **Two footnote markers on one right-to-left line,** and the QR text of a mixed page.
4. **A long line** (landscape, small font): a pointed line stays on the line pass because marks take no slot; a line
   over 128 slots takes the fallback.
5. **KOSync** on a Hebrew book with literal LRM or RLM. Its offsets must match those of a build without this step.

## Left out, on purpose

- **Paragraph-level resolution.** Levels are resolved per line, so an isolate or embedding that crosses a line break
  loses its opener's neutral context on the next line. The line pass prepends the open controls, which covers what
  readers see. An isolate that stays on one line is handled (decision 8).
- **A direction settled on the first pass only.** Decision 6 reads the tokens of a paragraph's first layout pass, its
  first 96. A Hebrew book's paragraph that opens with a long quote without Hebrew stays left to right for good, and
  the converse in an English book. Rare.
- **The divine name glued to a mark-only token** (a cantillation mark in a span of its own) falls back to upstream's
  word order until step 2: the mark pulls the name's geresh away from its he. Step 2's shaping context fixes it.
- **GPOS mark placement for niqqud.** The anchors serve. The shaper can take it later.
- Ruby in right-to-left text; Hebrew hyphenation; Syriac, Thaana and N'Ko.

## Left to the builder

- Names: the shaping layer's library and entry point, and the TextBlock accessors.
- The exact header layout of the two regions.
- The fixture's text and the rule checker's shape. Follow `VerticalRulesTest`: rules over every page, with
  minimum counts.
- The acceptance book: a public-domain Hebrew EPUB from Project Ben-Yehuda (benyehuda.org), local only, with a
  vocalised text for the niqqud.
