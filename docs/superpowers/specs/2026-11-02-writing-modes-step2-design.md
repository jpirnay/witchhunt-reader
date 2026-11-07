# Writing modes step 2: the joint shaper, Indic scripts then Arabic (design)

Status: approved by the maintainer on 2026-11-02, after a builder check; decisions 4-7 are the maintainer's, and the "My calls" list (the controller's calls marked in it included) was approved with the design. It builds on
`docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md` (decisions 1, 2 and 4, and "The shaping layer"),
on "Complex shaping: Arabic and the Indic scripts" in `docs/future_work/writing-modes.md`, and on the senior review of
2026-11-02 (section 4b, "Step 2, settle first"). It replaces the spec's 5.6 for Arabic
(`docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`), as step 1's decision 4 says.

## Intent

A Hindi, Bengali, Tamil or Malayalam book should read as printed:
- vowel signs sit where the script puts them (ि before its consonant, two-part vowels around it);
- conjuncts and half forms are the font's (क्ष, त्र, ক্ষ, ஸ்ரீ), and the reph rides above;
- marks sit where the font's GPOS puts them.

An Arabic, Persian or Urdu book should read joined:
- each letter takes its initial, medial, final or isolated form;
- lam-alef is one glyph, and the font's other required ligatures are applied;
- harakat sit where the font puts them.

Step 1's line order, mirroring and page progression stay as they are. Shaping runs once, at section build. Page turns
draw what the build stored and shape nothing.

The code stays as close to upstream crosspoint's shaper (#3787, `lib/OtShaper`) as this design allows, so upstream's
fixes keep porting. Every deviation is named below.

Non-goals:
- Shaping in a bitmap font (`.cpfont`, built-in). The TrueType engine shapes; bitmap fonts get upstream's vowel
  reordering for the Indic scripts and stay unjoined for Arabic (decision 5).
- Shaped UI strings: book titles on Home and in the Library, the status bar's chapter title, the TOC. They wait for
  step 5's visual strings (the controller's call, under "My calls").
- Khmer, Myanmar, Tibetan, Thai and Lao; Syriac, Thaana and N'Ko; Nastaliq Urdu.
- Kashida justification; Hindi hyphenation patterns.

## Decisions carried from step 1 (maintainer)

0. **Keep as close as possible to upstream code; every deviation named** (the house rule of step 1).
1. **One shaper for Indic and Arabic, right after step 1, Indic first.** Port crosspoint-reader #3787 ("feat: add
   Indic language support without HarfBuzz") at its current head, merged upstream or not. Then an Arabic joining
   module on the same engine, with HarfBuzz 14.5's Arabic shaper as the reference.
2. **Spec 5.6's baked Arabic presentation forms are dropped.** No Arabic stopgap in step 1: Arabic stays reordered and
   unjoined until the joining module lands.
3. **The seam is step 1's:** `lib/TextShaping` (`TextShaping::shape` → `Unit{cp, cluster}`, a per-script dispatch,
   the Hebrew module) and the per-word display forms in the TextBlock's bidi region (step 1 PR 2). The rules it
   inherits are step 1's "The shaping layer": clusters as source byte offsets; a module sees the token's logical
   text and its glued neighbours as context (#3787's `RunContext`); glyph output in visual order, with bidi's token
   and piece order kept; display forms under the same region, a spare `parts` bit for glyph records; a choice
   between #3787's private-use tokens and compact glyph records (made: decision 4).

## Decisions (maintainer, 2026-11-02)

4. **Compact glyph records, not #3787's private-use tokens.** A glyph form takes bit 2 of the bidi region's `parts`
   byte ("What is stored", below). The reasons:
   - 4-6 bytes a glyph against 8-12, in the section file and in every resident page, where the X3 reads at 30-41 KB
     free, and every word of an Arabic book carries a form;
   - no in-band signalling, so no string has to be scrubbed of plane-15/16 code points before it is drawn;
   - the engine, the bulk of the port, is the same either way, and upstream's token loops sit in a renderer path we
     do not have, so tokens would not keep us closer to upstream;
   - tokens' one advantage, glyphs and code points mixed in one string, is what the run-cut pieces make unnecessary.
5. **TrueType only, with GDEF and `CPac` made at install.** The glyph store keeps every face's tables and lookup
   filters in flash, so a shaped page costs a few KB of build memory and no reading memory. It takes no partition (our
   `spiffs` is the glyph store, which `FlashBlobCache` would take over) and no converter fork. Accepted costs:
   - one reinstall of every TrueType family, CJK subsets included, through the `kRenderVersion` bump;
   - bitmap families stay unshaped: Indic vowel signs reordered (`IndicReorder`), Arabic unjoined.
6. **The Font Manager hosts all 22 families,** each merged with Noto's Latin and left out of "Download all", from
   one recipe:
   - Noto Sans and Noto Serif for each of the ten Indic scripts (20, PR 3): Devanagari, Bengali, Gurmukhi, Gujarati,
     Oriya, Tamil, Telugu, Kannada, Malayalam, Sinhala;
   - Noto Naskh Arabic and Noto Sans Arabic (2, PR 4).
   - The reason: #3787's beta has readers in Kannada, Bengali and Malayalam, so a Devanagari-only start would leave
     them out.
   - **Refined by the maintainer, 2026-11-02, after measure-first item 3**
     (`2026-11-02-writing-modes-step2-measurements.md`). The first recipe missed on two counts, and two rules were
     added:
     - **The merge drops `vhea` and `vmtx`.** Noto Serif Bengali and Noto Serif Tamil have both tables and Noto Serif
       has neither. fontTools' `vhea` merge raised on the missing table, so those two families did not merge. Our
       engine never sets Indic text vertically, so the tables carry nothing we draw.
     - **Latin letters A-Z and a-z always come from Noto Sans or Noto Serif.** Space, digits, punctuation and
       everything else the script font maps come from the script font. This keeps Latin looking the same in every
       family. #3787's "no Latin" claim held for letters in 42 of the 44 script files, but not for Noto Sans
       Sinhala, which maps all 52 letters (Noto Sans's now replace them). 34 of the 40 Indic files map 39 ASCII code
       points (space, the digits and the punctuation), and those stay the script font's.
   - **Refined by the maintainer, 2026-11-07, in PR 3b's review.** Noto's Latin subset also takes the IAST
     letters (24 precomposed letters and the 6 combining marks their decomposed forms use): U+1E0C/D, 1E24/5, 1E36-9,
     1E40-7, 1E5A-D, 1E62/3, 1E6C/D and U+0301, 0303, 0304, 0307, 0310, 0323. The reason: a Hindi or Sanskrit book
     often carries IAST transliteration, whose letters drew as boxes. The merge check shapes runs of those letters and
     marks too.
7. **Deviation 1 is offered upstream.** Once our port works, the maintainer posts a short comment on #3787 from the
   maintainer's account: what the nothrow fixed-capacity buffers change and why (the `reserve()` after a heap check
   aborts under `-fno-exceptions`), with a link to our code. The plan carries it as the last task of PR 1.

**My calls**, awaiting the maintainer's approval with the design (Claude's unless marked "controller's"; each is
explained where it appears):
- **(controller's) Measure first, as a gated phase.** The plan opens with measure-first items 1-3 before PR 1's code,
  and item 7 before PR 4. Each has a pass line, and a missed pass line reopens the design ("Measure first", below):
  - flash for PRs 1-3 at most 90 KB, and at least 250 KB of the app slot free after them;
  - on the X3, at least 10 KB of heap free at the floor of a cold build of a shaped-script fixture, with the shaping
    buffer and plans in place;
  - 0 differences from HarfBuzz on upstream's corpus under our compiler and flags; any difference stops the work
    until it is understood;
  - each merged family fits the font area, and builds in place or not, as this design says;
  - on the X3, a shaped chapter builds in at most twice the time of a Latin chapter of the same length; else #3787's
    run cache comes back.
- **(controller's) A style change inside a syllable** (`क<b>ि</b>`, a reph in another token): the group is merged into
  one token, shaped as one run and drawn in its first piece's style. The style change inside the syllable is lost;
  the glyphs stay right. Glued tokens of the same style (a converter's span splits) are merged before shaping too
  (deviation 7).
- **(controller's) Joiners:** a lone ZWJ or ZWNJ is kept when the code point before it belongs to a shaped script,
  whatever follows; runs are still dropped (deviation 8).
- **(controller's) One `ot::Face` per style,** rebuilt only when the mapping's address moves, and re-pointed without
  CPac validation when it does (deviations 3 and 12).
- **(controller's, 2026-11-02) `minibidi.c` and upstream's `test/minibidi_arabic` stay verbatim:** `do_shape` stays,
  uncalled, and the linker drops it (0 B). Decision 0: minibidi stays syncable with upstream, and the joining module
  is the only Arabic shaper the firmware links. Deleting it, as the review asked, would remove upstream's shaping
  code, `applyBidiVisualShaped` and their test, at the cost of a deviation in a file we otherwise keep close to
  upstream. Step 1's deviation 2 says why it stays uncalled.
- **(controller's, 2026-11-02) Shaped titles and TOC stay in step 5.** The UI faces are bitmap and cannot shape; a
  title would need the reader's TrueType face outside a build and a stored form per title, which is step 5's visual
  strings. Until then an Indic title draws unshaped and an Arabic one unjoined, in the face that draws it today.
- the runtime lookup filters replaced by upstream's own `CPac` table, written at install instead of by the converter;
- shaping memory per call from the build's lent arena, plans on the heap for one build;
- a shaped run never straddles a bidi level, and a token with a shaped run and other text becomes glued pieces, so a
  display form is all glyphs or all code points;
- every glyph of a run carries the run's start as its cluster (no per-glyph clusters);
- glyph IDs reach the TrueType engine as keys above U+10FFFF, not at U+F0000;
- upstream's `IndicReorder` taken, but run in the shaping layer at build time;
- the four-PR split.

## Where our tree differs from #3787's base, and what "as close as possible" means

#3787 has three layers. Only the first ports as it is:

| Layer | #3787 | Here |
|---|---|---|
| The engine | `lib/OtShaper` (~5.4k lines): HarfBuzz 14.5's Indic and USE shapers and its GSUB (1-8) and GPOS (1-9) engine, generated syllable machines and Unicode tables | **Taken file for file**, with the nothrow storage (deviation 1) and, in PR 4, the Arabic shaper (deviation 11) |
| The glue | `ComplexShaper`: shapes inside the renderer, at measure and at draw, with a run cache, a layout memo, a budgeted allocator, shared faces, a table loader that copies a TrueType file's layout tables to the heap | **Not taken.** Ours shapes once at section build, in the build's memory, from tables read in place (deviations 2-4). Its run scan, its piecewise shaping of long words and its output conversion are ported into our module |
| The integration | `ShapingTokens.h` in UTF-8, `EpdFontData::shapeHandler`, `GfxRenderer::resolveVisualText` / `LaidOutText` / `resolveForDisplay`, a `TextBlock` display region (v49), `FlashBlobCache` in `spiffs`, a `.cpfont` `CPSH` section with the GSUB closure baked at U+F0000+gid | **Not taken**, because what it plugs into is not ours (below). Its rules are kept: syllable-safe breaks, no focus split and no drop cap on a shaped word, the book's language for the language system |

Why the integration cannot port:
- **The renderer.** Upstream's renderer runs bidi and shaping on every string it draws or measures. Ours runs
  neither (step 1, deviation 1): the build stores display forms and the renderer draws them. Upstream's token loops
  sit inside a `drawText`/`getTextAdvanceX` we do not have in that form.
- **Layout and storage.** Our `ParsedText` (line arena, `WordRange` views, per-word font slots, the bidi line pass and
  its pieces) and our `TextBlock` (the bidi region with its `parts` byte, v108) diverged from upstream's. Upstream's
  display region has no counterpart to fill.
- **The fonts.** On the C3 upstream shapes bitmap `.cpfont` families: the GSUB closure rasterised at U+F0000+gid, the
  layout tables copied once into the `spiffs` partition by `FlashBlobCache`; TrueType shaping only on PSRAM boards,
  through the SDK's FreeType with the tables copied to the heap. Ours is the integer TrueType engine on every board.
  It draws by glyph index from tables mapped in place from the glyph store, and that store is what our `spiffs`
  partition holds (`docs/file-formats.md`, "The TTF glyph store"). `FlashBlobCache` would take the partition over.

So "as close as possible" here means: `lib/OtShaper`, `IndicScripts.h`, `IndicReorder`, the generators and the tests
as upstream has them, with named deviations; our own glue in our own files, which ports upstream's algorithms
(`scanRun`, the piecewise loop of `appendShapedRun`, `appendGlyphs`' rounding) and names where they moved.

## What we take from upstream

The baseline is crosspoint-reader PR #3787 at `dd45ba61` (2026-11-02). That head is a merge of develop `e8299b5f`
into the branch. `lib/OtShaper`, `ShapingTokens.h`, `ComplexShaper`, `FlashBlobCache`, `lib/Utf8/Indic*` and the
`ot_shaper`/`complex_shaper` tests are byte-identical to `0fbd1575`, the head the roadmap looked at (blob hashes
compared through the GitHub API). The PR is open, review required.

- **`lib/OtShaper`, all 25 files.** Attribution: #3787 (Safayet, MIT) and HarfBuzz 14.5's MIT ("Old MIT") notice,
  which ships with the port.
- **`lib/Utf8/IndicScripts.h`:** the ten scripts, `isShared`, `scriptOf`, `syllableBreakAllowed`.
- **`lib/Utf8/IndicReorder.{h,cpp}`:** vowel signs in visual order for a font that cannot shape.
- **The generators:** `scripts/gen_ot_shaper_machines.py`, `scripts/gen_ot_shaper_tables.{py,cc}`.
- **The tests:**
  - `test/ot_shaper` (`OtShaperTest`, `OtShapeCli`, `compare_harfbuzz.py`, `generate_corpus.py`);
  - the HarfBuzz reference fixtures `test/complex_shaper/ExpectedShaping.h` with `generate_fixtures.py`, and the
    `.layout` fonts and `OFL.txt` in `test/complex_shaper/data`. Our glue's test checks the same expected values;
    `ComplexShaperTest.cpp` itself is not ported;
  - `test/indic_text` (`IndicReorder`, the syllable breaks).
- **Rules, ported into our diverged files:**
  - the Hyphenator's fallback breaks only between syllables (`appendFallbackIndexes`);
  - no focus-reading split on a word with a shaped script (`ParsedText::addWord`);
  - no drop cap captured into a shaped script (`captureDropCapCodepoint`, f0757278);
  - the OpenType language system from `dc:language` (`languageTagsFor`, `setDocumentLanguage`'s role).

**Not taken:** `ComplexShaper`, `ShapingTokens.h`, `FlashBlobCache`, `Fnv1a.h` (we have `glyphstore::fnv1a`), the
`.cpfont` shaping section and its converter (`fontconvert_sdcard.py`, `shaping_blob.py`, the `sd-fonts.yaml`
presets), `TtfEpdFont`'s shaper, the renderer's token paths, `EpdFontData::shapeHandler`, the TextBlock display
region, the `ClipSelectionActivity` change, and the tests `flash_blob_cache` and `text_block_display`.

**The deviations, all of them:**
1. **Nothrow storage.** `Buffer`'s `info`, `pos`, `scratch` and `indices`, `Plan`'s and `PlanBuilder`'s vectors and
   `appendSubtableDigests`' output become one fixed-capacity view type with the same member names and the calls
   upstream makes of them (`push_back`, `erase`, `resize`, `assign`, `swap`, `size`, `capacity`, `data`), bound to
   memory allocated nothrow: the buffer's arrays to a block the caller hands in (`Buffer::setStorage`), a plan's lists
   and filters to blocks the plan allocates once their sizes are known, the planner's lists to one small block it
   allocates. `Buffer::prepare` binds instead of `reserve()`ing. Growth past the capacity fails the
   run, as `canGrow` already does; the caller lays the token out unshaped. This is the roadmap's open objection
   (`reserve()` after a heap check, so a failed allocation aborts under `-fno-exceptions`); it is offered upstream on
   #3787 once our port works (decision 7). The limits stay upstream's: `MAX_NESTING_LEVEL` 8, `MAX_CONTEXT_LENGTH` 64, a run's growth factor 2 + 16,
   the op budget (`MAX_OPS_FACTOR`, `MAX_OPS_MIN`), `MAX_PLANNED_LOOKUPS` 4096.
2. **Placement.** Shaping runs at section build only, in our module. No shaping in the renderer, no run cache, no
   layout memo, no per-draw shaping of UI strings. Step 1's deviation 1, extended to glyphs.
3. **Faces read in place.** An `ot::Face` is built over the glyph store's mapped style block (`FontBytes::span`): the
   whole file in flash mode, the block's own table directory in SD mode, through upstream's `tablesFromSfnt`. Nothing
   is copied and there is no table loader or budgeted allocator. There is one `Face` per style for a build (the
   controller's call), built on the style's first shaped token and kept on the heap until the build ends. Each call
   fetches the mapping first and renders no glyph; the face is rebuilt only when the mapping's address moved
   (deviation 12).
4. **Lookup filters made at install.** A TrueType font has no `CPac` table, so upstream's planner computes the lookup
   and subtable digests at run time, onto the heap: on the fixtures that is up to 17.5 KB for Noto Sans Devanagari
   and 27.1 KB for Noto Sans Bengali, per face. The install writes upstream's `CPac` (the format its converter puts
   in a `.cpfont`, `docs/file-formats.md` of #3787) into the style's block in flash, and `ot::Face` finds it there as
   it would in a `.cpfont`. Plans are still built at run time (no `CPpl`): they depend on the book's language and are
   small.
5. **Glyph output.** Compact glyph records in the bidi region instead of `ShapingTokens.h` (decision 4). Glyph IDs
   reach the TrueType engine as keys 0x110000 + gid, past Unicode in the store's 21-bit code point field, instead of
   U+F0000 + gid, so no real text ever reads as a glyph and nothing scrubs plane-15/16 text before a draw.
6. **Runs.** A shaped run is cut at bidi class changes as well as at script changes, so it never straddles a level.
   Arabic-Indic digits, punctuation and brackets stay code points that bidi orders and mirrors. Every glyph of a run
   carries the run's first byte as its cluster: the whole run is one cluster, which HarfBuzz's merge rules allow, and
   `GlyphInfo` gains no field (upstream's has no cluster either).
7. **Glued tokens** (the controller's call). Upstream uses `RunContext` only to shape a long word in pieces, and its
   words are never split by style. Ours are, wherever a span starts or ends inside a word. A group of glued tokens
   with a shaped run is handled before shaping, by where each glue falls:
   - **same style** (font, style, size and scale alike: a converter's span splits): the tokens are merged into one
     token, shaped as one run;
   - **another style, inside a syllable** (`syllableBreakAllowed` false at the glue: `क<b>ि</b>`, a reph in another
     token): merged as well, shaped as one run and drawn in the first token's style. The style change inside the
     syllable is lost; the glyphs stay right;
   - **another style, between syllables, or in Arabic** (a bold letter in a joined word): each token is shaped on its
     own, with its neighbours' runs as `RunContext` `before`/`after` context, so joining and word-start forms hold. A
     ligature that forms across the glue (lam-alef) is drawn once, in the first token's style.
   A merged token keeps the first token's link and visible offset; the parts' logical texts are joined.
8. **Joiners** (the controller's call). Upstream's parser keeps ZWJ and ZWNJ. Ours drops every Default_Ignorable code
   point (`ChapterHtmlSlimParser` at the `defaultIgnorableLen` skip; `utf8IsDefaultIgnorable`, `Utf8.h`), which a
   shaper cannot do without: Persian writes its half-space as ZWNJ, and Hindi, Bengali and Malayalam steer conjuncts
   and chillus with both.
   - The parser keeps a lone ZWJ or ZWNJ when the code point before it belongs to a shaped script, a letter or a mark
     alike (U+0900-0DFF; in PR 4 the Arabic blocks), whatever follows. So Hindi's half-form ZWJ after a virama and the
     Malayalam chillu's ZWJ at a word's end are kept.
   - A run of two or more joiners is still dropped whole, and so is a joiner after any other code point: the
     watermark defence (runs of hundreds between words) holds.
   - The look-behind crosses chunk boundaries: the parser keeps the last text code point across `characterData`
     calls and inline element edges (as `lastTextCodepoint_` does for CJK segment breaks). A kept joiner is held until the next
     code point arrives, in this call or a later one; a second joiner then drops both and the rest of the run.
   - A kept joiner counts nowhere in the visible offsets, as step 1's kept controls do.
9. **`IndicReorder` at build time.** Upstream runs it in the renderer for a font without shaping data. Here it is a
   shaping-layer module whose output is a UTF-8 display form (step 1's bit 0), for a face that cannot shape the
   token's script.
10. **GDEF in SD mode.** An SD-mode style block (`SdBlock`) copies cmap, head, hhea, hmtx, loca, maxp, OS/2, GPOS,
    GSUB, kern and post's header, not GDEF. The shaper needs GDEF's glyph classes, mark attachment classes and mark
    sets; without them it falls back to classes from Unicode and stops matching HarfBuzz on fonts that have GDEF. The
    block gains GDEF (and `CPac`). An engine change, not upstream's.
11. **The Arabic shaper (PR 4)**, a port of HarfBuzz 14.5's `hb-ot-shaper-arabic.cc` into the engine:
    - `ShaperKind::Arabic`, `Script::Arabic`; the joining state machine and its table, generated from HarfBuzz;
    - the features and stages of HarfBuzz's `collect_features`: the seven joining forms each in its own stage,
      `rlig`, `rclt` and `calt` with manual ZWJ, `mset`, with the common features and GPOS as for every script; marks
      zeroed late by GDEF;
    - the reordering of modifier combining marks (`reorder_marks`);
    - a buffer direction, the branches of `propagateAttachmentOffsets` for a backward run, and the final reverse of
      an RTL run, as `hb_ot_position` does it (upstream's engine is left to right only);
    - the Unicode tables extended past U+0900-0DFF by the generator: the Arabic blocks' categories, combining
      classes, decompositions and joining types; the language systems for `ar`, `fa`, `ur`, `ps`, `sd`, `ug` and
      `ku`.
    - Not ported: the fallback shaping with presentation forms for a font without the features, `stch`, Syriac,
      Mongolian, N'Ko and Phags-pa.
    - The feature list is to be read off HarfBuzz 14.5's source when the port is made; this list is from memory.
12. **A face re-pointed when its mapping moves** (the controller's call). `mapStyle` is not stable across calls in
    flash mode: flash-mode blocks map one at a time, and the pointer stays valid only until another flash-mode style
    is mapped (`FlashGlyphStore.h`, `mapStyle`). So a build that shapes regular and bold words in turn sees each
    style's block move. SD-mode blocks are mapped together at open and do not move.
    - `ot::Face` gains a re-point: given the old and the new base of the mapping, it moves every `Table` view and the
      `CPac` filter records by the difference, without the cmap search or `initFilters`' validation (the bytes are the
      same, already validated by the first `init`). A plan points at its face's filter records, not into the font, so
      it stays valid across the move.
    - The alternative, plans that copy their filters, is not taken: it puts the 16-27 KB of filters back on the heap.
    - Upstream never needs it: its `.cpfont` blobs and heap-copied tables do not move.
13. **The plan's memos** (PR 3, Task 12a: on the device `ot::shape` is dominated by reads of the flash-mapped GSUB).
    Both leave the output as it is, by construction:
    - Each planned lookup's digest is copied into the plan when it is built (24 B a lookup, GSUB's then GPOS's, from
      `CPac`, else computed), and the apply loop tests that copy before `Plan::settings` reads the font
      (`Plan::mayApply`). A plan that cannot allocate the copy fails, as a refused list block does. The
      would-substitute probes read it too, so they always have the digest HarfBuzz tests a format-3 context's first
      glyph against, and the port's "any first glyph" fallback for a plan without filters is gone. Subtable digests
      stay in `CPac` or the refusable filter block. All three masks are kept: over the four fixture books the best one
      alone (the low six bits) rejects 86% of what the three reject.
    - `consonantPositionFromFace`'s answers are remembered by the plan (`Plan::consonantPosition`): 64 direct-mapped
      slots keyed by the consonant's and the virama's glyph IDs, `mutable` in a const plan, which serves one build at
      a time under RenderLock. Upstream HarfBuzz, to 15.1.0 and main, walks every time.

## How shaping runs at build time, and what is stored

In `ParsedText`, for a paragraph that needs shaping only. A token needs shaping when it holds a letter of a shaped
script (U+0900-0DFF; in PR 4 the Arabic blocks) and its face can shape that script. Every other paragraph skips all
of it, so its layout and its section bytes are what they are today.

1. **The face.** The renderer answers, for a token's font, style and size: the shaping face of the TrueType file that
   draws that style (a synthetic style's source file), at the pixels per em it is drawn at (a native size's, or the
   body's with the word's scale applied to the output), or none. A face shapes a script when its cmap maps the
   script's probe letter, as upstream decides it (`ScriptInfo::probe`, `ComplexShaper::coverage`); a face without
   GSUB for the script still gets the shaper's reordering. `lib/TextShaping` sees this as a narrow interface it does
   not own; it does not include the TrueType engine. Behind it, one `ot::Face` per style lives for the build,
   re-pointed when its mapping moves (deviations 3 and 12).
2. **Glued groups.** Before anything is shaped, each group of glued tokens with a shaped run is resolved as deviation 7
   says: merged where the glue joins tokens of one style or falls inside a syllable, else kept apart with each other's
   runs as context. The shaping layer's context interface therefore takes the group, not one token: its tokens' texts
   in logical order, each token's style, and where each glue falls; a module returns, per token it keeps, its units
   or glyphs, and says which tokens it merged. The Hebrew module uses the same interface (PR 3). The parser's 96-token
   flush never separates the tokens of one group.
3. **Runs.** `scanRun` (upstream's): one script's letters plus the shared code points around them (dandas, joiners,
   dotted circle), cut further at bidi class changes (deviation 6). An Arabic run is its letters, marks, tatweel and
   joiners.
4. **Shape** each run once per paragraph:
   - a token kept apart from its glued neighbours with their runs as context (step 2);
   - in the book's language system;
   - into a buffer for at most a fixed number of code points per call (left to the builder: 32 or 48). A longer run
     is shaped in pieces split between syllables, each with the syllable on either side as context, then one code
     point, then none: upstream's loop, unchanged;
   - output, as upstream's `appendGlyphs` rounds it: the glyph, its advance in 12.4 pixels, its offset in whole
     pixels (y down), in visual order;
   - a run that cannot be shaped (no memory, the font fails the run) is laid out as code points, and the section is
     latched degraded: a new status bit 6 (`kStatusShapingDegraded`; bits 0-5 are taken) earns the spine one rebuild
     per session (invariant 6).
5. **Widths for breaking.** A token's width is its runs' advances plus its other code points measured as today. The
   records live in a per-paragraph store whose capacity survives the paragraphs (rule 1), reserved from an estimate
   and grown nothrow in chunks (invariant 9). A token measured again in a later pass reuses its records.
6. **Break lines** with the four breakers as they are. A fallback break never splits a syllable: upstream's Hyphenator
   rule, which every forced break of an overlong word goes through. Focus reading does not split a shaped word, and
   no drop cap is captured into one.
7. **Pieces.** A token with a shaped run and other text becomes glued pieces, cut at the run's edges, so a display
   form is all glyphs or all code points:
   - in a paragraph without bidi (Hindi), by a small step in `extractLine`; a token that is one run (the usual Indic
     word) stays one word;
   - in a bidi paragraph, by step 1's line pass, which cuts its pieces at run edges too. A run's slots are contiguous
     in visual order, because its level is uniform. A run's piece takes the run's glyphs as its form, already in
     visual order; bidi's character reversal is not applied to them. The token and piece order on the line is bidi's;
   - the line pass's fallback past `BIDI_MAX_LINE` cuts its per-word forms at run edges the same way.
8. **Gaps and justification** as step 1 and today. A glued gap next to a glyph piece takes no kerning: the engine's
   kerning is keyed by code point, and the shaper kerned inside the run.
9. **Store** the line. A word whose form is glyphs has a glyph record form in the bidi region (below). Its logical
   text stays its text, for the dictionary, footnote links, the QR text and KOSync.
10. **Warm the glyphs.** A shaped token's glyph IDs are looked up once during the build, inside the miss scope, as
   measuring does for code points today. So the store holds them, and a page drawn after the build needs no miss
   memory.

**The bidi region (step 1 PR 2, v108)** has a `u8 parts` byte: bit 0 display forms, bit 1 logical order, bits 2-7
reserved and rejected on load (`TextBlock::bidiValid`, commit `bcee36c96`). Step 2 takes bit 2: glyph record forms
(decision 4).
- A form is a count, then one record a glyph: `u16 glyph`, an advance in 12.4 pixels with one bit saying an offset
  follows, then `i8 dx`, `i8 dy` when it does. 4 bytes a glyph, 6 with an offset (#3787's tokens: 8 a glyph, 12 with
  an offset). The advance field's width (it must hold the widest glyph at 120 px a em) is the builder's.
- Records hold glyphs only. A code point never travels as a glyph and a glyph never as a code point: a piece is one
  or the other (step 7 above).
- A word has at most one form, UTF-8 or glyphs; load rejects a word with both.
- Every line without a shaped word keeps its bytes; every Latin, CJK and Hebrew line is unchanged.
- `SECTION_FILE_VERSION` goes up in PR 3 (Indic lines lay out differently) and again in PR 4 (Arabic). The TextBlock
  header does not change.

## Drawing

- **`TextBlock::render` and `wordBox`** draw and measure a glyph form through the renderer, in the word's font, style,
  scale and size, at its x. Nothing else in the draw path changes.
- **The renderer** gains a glyph-run draw and measure:
  - each record's glyph is looked up by ID in the family's TrueType font at that style and size, through the
    lookup without fallbacks (`EpdFont`'s no-fallback lookup, as `FallbackFaceProvider` uses), and drawn at the pen
    plus its offset;
  - the pen moves by the record's advance, snapped as the code point loop snaps it;
  - no kerning, ligatures or tracking inside a run (upstream's `kernUnlessShaped`, `trackingUnlessShaped`);
  - SUP and SUB halve advances and offsets, as upstream does;
  - in every rotation and gray pass the line draw uses (the turned vertical frame, the AA passes), and a glyph-ID
    variant of the prefetch.
- **The TrueType engine** reads a key from 0x110000 as a glyph index (`TtfFont::render`, next to the ligatures'
  `glyphFor`). The store keys it like any code point, under the style and face bits, so a synthetic style and the
  ring need nothing new.

## Fonts

- **TrueType only** shapes (decision 5). A reader's own TTF or OTF shapes, and so does a book's embedded font under
  Publisher Fonts, at any size.
- **The install** copies GDEF in SD mode and writes `CPac` into each style's block (deviations 4 and 10):
  - its size counted from the lookup lists when the files are probed (28 B per lookup, 24 B per subtable: about
    16 KB of flash for Noto Sans Devanagari, 25 KB for Noto Sans Bengali, 0.9 KB for Tamil and 3.0 KB for Sinhala);
  - written after the tables are copied, reading them mapped, each byte once;
  - `kRenderVersion` 4: the family identity changes, so every installed TrueType family installs again once, CJK
    subsets included (decision 5 accepts it), and its sections lay out again, as the font IDs follow the identity.
- **Font Manager families** (decision 6): 22 TrueType families with GSUB, GPOS and GDEF kept, made by
  `build-sd-fonts.py` with the pinned fontTools, `inDownloadAll: false`, the manifest regenerated with `--manifest`
  (the CJK re-cut trap):
  - A TrueType family has no fallback font, so each family is the script's font merged with Noto's Latin: a Sans
    with Noto Sans, a Serif and Naskh with Noto Serif. #3787's description says Noto's Indic TTFs carry no Latin.
    Measured (item 3), that holds for letters except in Noto Sans Sinhala, but most of the files map the ASCII
    space, digits and punctuation.
  - The recipe (decision 6, refined 2026-11-02):
    - the script font's cmap loses A-Z and a-z. Its glyphs stay, so its glyph IDs and its own shaping do not change;
      only Noto Sans Sinhala maps any;
    - Noto's Latin is a `fontTools.subset` of U+0020-007E, U+00A0-024F, U+2000-206F, U+20A0-20CF, U+2122 and U+FFFD,
      plus the IAST letters (24 precomposed letters and the 6 combining marks their decomposed forms use; refined by the
      maintainer, 2026-11-07: IAST transliteration), less every
      code point the script font still maps. So the Latin letters always come from Noto Sans or Serif,
      and everything else the script font maps comes from it;
    - `fontTools.merge`, the script font first (its glyph order, names and cmap win), with `vhea` and `vmtx`
      dropped;
    - measured consequence: Latin text in a merged family takes the script font's word space and punctuation. The
      space's width differs from Noto's in 14 of the 22 families (221-500 units against 260), and the punctuation's
      widths in most of them;
  - PR 3, 20 families: Noto Sans and Noto Serif Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu,
    Kannada, Malayalam and Sinhala;
  - PR 4, 2 families: Noto Naskh Arabic and Noto Sans Arabic;
  - regular and bold files; italic is the engine's slant (spec 7.4);
  - the merge is checked by the HarfBuzz oracle in two ways. The merged font must shape the script's corpus as the
    script's own font does. Each Latin line, restricted to its runs of A-Z and a-z, must shape as in Noto Sans or
    Serif itself.
- **Glyph IDs in section files** are safe against a replaced file already: a TrueType family's font IDs come from its
  identity (each file's name, size and modification time, and `kRenderVersion`: `ttf::familyIdentity`, then
  `SdCardFontManager::computeFontId`), and the section's property hash holds the font ID. That is the job #3787's
  `3b047543` does by hashing the table directory.
- **Bitmap fonts** (decision 5): an Indic word gets `IndicReorder`'s form (vowel signs in place, conjuncts with a
  visible virama); an Arabic word stays reordered and unjoined, as in step 1. Built-in fonts have neither script.

## Memory, flash and time

Every figure in this table is an **estimate**: none was measured on a build ("Measure first" says how to measure each).

| What | Size (estimate) | Lives | Where the estimate comes from |
|---|---|---|---|
| Glyph buffer per call | 62 B a slot (two `GlyphInfo` of 20 B, a `GlyphPosition` of 20 B, a `u16` index), 2n + 16 slots, plus the `Buffer` (its `matchStack` 1,152 B): about 6.3 KB for n = 32, 10.4 KB for n = 64 | one call: a block of the lent arena, else one heap block for the build | `OtBuffer.h`'s declarations, sized by hand for RV32; not compiled |
| A face per style | about 120 B (eight `Table` views and two filter records on RV32) | heap, the build | counted by hand from `OtFace.h` |
| A plan (face × script × language) | 8 B a planned lookup plus stages: about 1.5 KB for Devanagari (at most 185 lookups), 2.1 KB for Bengali (260) | heap, the build; freed at its end, like step 1's workspace | lookup counts read from the `.layout` fixtures by a small parser of the font files |
| Runtime lookup filters, avoided by `CPac` | up to 17.5 KB (Devanagari), 27.1 KB (Bengali), 1.0 KB (Tamil), 3.5 KB (Sinhala) per face | would be heap, the build | 36 B a lookup, 24 B a subtable, every lookup planned: an upper bound |
| `CPac` | about 16 KB (Devanagari), 25 KB (Bengali), 0.9 KB (Tamil), 3.0 KB (Sinhala) per style | flash, the style block | the same counts, 28 B a lookup, 24 B a subtable |
| Paragraph glyph store | about 8 B a glyph while laid out | heap, capacity kept across paragraphs | a guess |
| Resident page | 4-6 B a glyph of form (decision 4), against 8-12 B for #3787's tokens | page | the record format |
| The reinstall (decision 5) | every installed TrueType family copied again once, and its books' sections laid out again | one boot after the update | install times are not measured for the merged families |
| The 22 hosted families (decision 6) | 2 styles each; file sizes of the merged fonts unknown | the card, the font server | not built yet |

- **The arena.** During layout the lent region has about 37.3 KB free on the X3 and 33.1 KB on the X4, but 64 B on the
  X4 during a 32 KB image-header walk stage (memory strategy §8.2). Nothing of the shaper is held across a walk:
  a call's buffer is reserved, the run shaped, the records copied out and the block released before any glyph is
  looked up (LIFO with the TrueType miss scope, §9.7). Faces and plans are on the heap for that reason.
- **The heap floor.** Pass line (measure first): on the X3, a cold build of a shaped-script fixture keeps at least
  10 KB of heap free at its floor, with the shaping buffer, the faces and the plans in place. Missed, the design
  reopens.
- **The X4's in-place build** lends nothing, so its shaping takes the heap. A book in a shaped-script language with a
  TrueType reader font never builds in place, as a CJK book in a TrueType set does today (`InPlaceBuildGate`,
  `TypographyProfile`).
- **Flash.** The app slot is 6,553,600 B (`partitions.csv`, `app0` 0x640000). `default` was 6,150,595 B at step 1
  PR 1's base, 403,005 B under it; step 1 PR 2 adds some KB. Upstream measured +58.4 KB on the C3 (`30d7955c`) for
  its bitmap route, without the runtime planner (`OtPlanBuild.cpp`, linked only with TrueType sources; the S3 build,
  which links it, lost 11.9 KB in the same commit against the C3's 21.9 KB). Our route links the planner, the install
  writer and the glyph-run draw, and none of `ComplexShaper`, `FlashBlobCache`, the `.cpfont` reader or the token
  paths. **Estimate**, unmeasured: 55-75 KB for PRs 1-3 and 8-15 KB for Arabic, which leaves about 300 KB of the
  slot. Pass line (measure first, item 2): at most 90 KB for PRs 1-3, and at least 250 KB of the slot free after
  them; missed, the design reopens. PRs 1-4 record `Flash:` on `default` and `gh_release`.
- **Static RAM:** upstream +224 B. Record ours.
- **Stack: the loop task has 16 KB, not 8 KB.** Section builds run on the Arduino loop task. Its stack is set by
  `getArduinoLoopTaskStackSize()`, which `freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp:13` defines as
  `__attribute__((weak)) size_t getArduinoLoopTaskStackSize(void) { return 16 * 1024; }`. Nothing overrides it: a
  search of `src`, `lib`, `platformio.ini` and the sdkconfig files finds no other definition and no
  `ARDUINO_LOOP_STACK_SIZE`. The TTF step 0 bench (X3, 2026-10-13) saw the same 16,384-byte stack: 12,204 B free at
  the low-water mark while rendering. The "8 KB" is the comment at `src/main.cpp:1778` ("loop() runs on the Arduino
  loopTask (8 KB stack)"), which step 1's design (its Constraints) and the review's 4.5 repeated.
  - For step 1 PR 2's stack check (its Task 12): read `uxTaskGetStackHighWaterMark` against 16,384 B. That leaves
    `do_bidi`'s 1.3 KB and `computeVisualWordOrder`'s 512 B more room than the plan assumed; the measurement still
    decides. The comment and step 1's design should be corrected in that PR.
  - For step 2: the engine nests lookups up to 8 deep, each level a few hundred bytes (upstream's comment, not
    measured). PR 3 records the loop task's high-water on the Indic fixture on both boards, with
    `-DENABLE_BOOT_HEAP_DIAGNOSTICS`.
- **Time.** Every shaped word is shaped at build: an Indic chapter builds slower than a Latin one. Page turns shape
  nothing. PR 3 reports, on the X3: an Indic chapter's build time against a Latin chapter of the same length, the
  install time of a merged Devanagari family with its `CPac`, and the reading heap after a page turn on an Indic page.
  Pass line: a shaped chapter builds in at most twice the time of a Latin chapter of the same length. Missed, #3787's
  direct-mapped run cache comes back as a class C block of the build (not before).

## The settle-first list, answered

The review's section 4b, item by item:
1. **#3787's state.** Open, review required, head `dd45ba61` (2026-11-02, a develop merge). The shaper is unchanged
   since `0fbd1575`, and the throwing `reserve()` is still there. The last review comment (2026-11-02, bdeshi) is about
   the `.cpfont` `CPSH` field, which we do not take. Port at `dd45ba61` now (decision 1), with deviation 1, offered on
   #3787 once our port works (decision 7). If upstream merges a changed shaper, resync file by file.
2. **A glyph-record bit.** Step 1 PR 2 left bits 2-7 of `parts` reserved and rejected (`bcee36c96`). Step 2 takes
   bit 2 for compact records (decision 4). The section version goes up anyway (Indic lines lay out differently); the
   TextBlock header does not change.
3. **Flash headroom.** 403,005 B at step 1 PR 1's base, less PR 2's few KB; the estimate above leaves about 300 KB.
4. **An Arabic stopgap.** No (decision 2). Arabic stays unjoined until PR 4.
5. **Which fonts shape.** TrueType only, with GDEF and `CPac` made at install (decision 5).
6. **Syllable-safe breaks.** Part of PR 3: upstream's Hyphenator rule, and the focus-reading and drop-cap rules.
7. **`do_shape` afterwards.** Kept, uncalled, with `minibidi.c` and its upstream test verbatim (the controller's
   call, under "My calls").

## The pieces, one PR each, in this order

Each opens against master after the one before it has merged (#398, #584). Before PR 1's code, measure-first items
1-3 run and pass; before PR 4's, item 7 ("Measure first", below).

1. **The engine.**
   - `lib/OtShaper` with deviation 1; `IndicScripts.h`, `IndicReorder`; the generators; `OtShaperTest`, the HarfBuzz
     oracle tools, `IndicTextTest`, the fixtures with `OFL.txt`.
   - A host test that refuses every allocation size in turn and checks that the run fails cleanly.
   - Nothing calls it, so the diff can be read against upstream file by file. Flash from a scratch build.
   - The oracle of measure-first item 1 runs again on this PR's engine (0 differences), and the probe's figures go
     into the PR body.
   - Last task (decision 7): once the port works, the maintainer posts a short comment on #3787 from the maintainer's
     account describing deviation 1, with a link to our code. The plan drafts the comment.
2. **TrueType faces that shape** (decision 5).
   - SD-mode GDEF, the install's `CPac`, `kRenderVersion` 4; keys from 0x110000 as glyph IDs; one shaping face per
     style over the mapped block, re-pointed when the mapping moves (deviation 12).
   - The renderer's glyph-run draw and measure for compact records (decision 4), in every rotation and pass, and the
     glyph-ID prefetch.
   - Host tests over an in-memory Noto subset: the face with and without `CPac` shapes alike; a glyph run draws the
     glyphs the oracle names at its positions.
   - Reader-visible: every TrueType family installs again once (accepted), and its books lay out again.
3. **Indic books.**
   - The shaping layer's face and glued-group interface (step 2 of "How shaping runs"). The Hebrew module takes the
     group too, so the divine name glued to a mark-only token draws as ה׳ (the test `95d410915` pinned as a fallback
     flips).
   - The Indic module (our glue over the engine), the glued-group merges of deviation 7, `IndicReorder` for faces that
     cannot shape, the parser's joiners (deviation 8).
   - `ParsedText`: widths, run-cut pieces, the paragraph store, warmed glyphs. The TextBlock's bit 2; the Hyphenator,
     focus-reading and drop-cap rules; `kStatusShapingDegraded`; the section version; the in-place build gate.
   - The Font Manager's 20 Indic families (decision 6): the merge recipe in `sd-fonts.yaml` and `build-sd-fonts.py`,
     the oracle on each merged font, the manifest regenerated with `--manifest`.
   - A fixture book (Devanagari, Bengali, Tamil, Malayalam pages) with its goldens, and a rule checker after
     `VerticalRulesTest`: no glyph form on a line without a shaped letter, every shaped word's form sums to its
     measured width, no break inside a syllable.
   - `USER_GUIDE.md`, the roadmap, a reference page for shaping.
4. **Arabic joining.** Its code starts once measure-first item 7 has passed.
   - Deviation 11 in the engine, with the generator's Arabic tables; the Arabic module; the line pass's run-cut pieces
     and its fallback.
   - The Font Manager's 2 Arabic families (Noto Naskh Arabic, Noto Sans Arabic, decision 6); the section version.
   - The oracle on both merged fonts over an Arabic, Persian and Urdu corpus.
   - `minibidi.c` and `test/minibidi_arabic` untouched: `do_shape` stays uncalled (the controller's call). The docs
     say the joining module is the firmware's Arabic shaper.

## Constraints

- Every Latin, CJK and Hebrew golden stays byte-identical. Only step 2's fixtures change, and in PR 4 any Arabic line
  of step 1's fixtures (`test_rtl_markup` has none).
- The engine's output matches HarfBuzz 14.5 glyph for glyph on the oracle corpus (upstream's bar: 0 differences), for
  every font PR 3 and PR 4 ship, at two sizes, and with `CPac` from the install.
- Nothrow everywhere: a refused block or a failed run lays the token out unshaped and latches the status bit.
- No resident memory: the shaper's buffer, faces and plans live during a build only, and a page draw allocates nothing new
  for shaped words.
- Measured and reported on the X3 and the X4: flash on `default` and `gh_release`; static RAM; Background-B's lowest
  heap during an Indic build; the reading heap after a page turn on an Indic page; the loop task's stack high-water;
  build and install times.
- Attribution: crosspoint-reader #3787 (MIT) at `dd45ba61`, naming `0fbd1575` for the shaper's files; HarfBuzz's MIT
  notice in the ported files and the licences page; the Noto fonts' OFL.
- The device runs flash the web bundle as well (`web-<hash>.bin` to the card's root).

## Review focus

Things no planned test covers outright that would bite a reader first:
1. **A Hindi page after a cold build,** with the glyph store full (ring mode): every shaped glyph draws, and the
   page's heap stays where a Latin page's is.
2. **A paragraph shaped short of memory:** the unshaped words draw the width they were measured at, and the rebuild
   the latch earns shapes them.
3. **Joiners:** a Persian ZWNJ, a Hindi half form (virama + ZWJ) and a ZWNJ conjunct pair, a Malayalam chillu (ZWJ at
   a word's end), a joiner split from its letter by a chunk boundary and by an inline element's edge; and a
   watermarked book, whose runs are still dropped. KOSync's offsets on that book match a build without step 2.
4. **A style change inside a word** (deviation 7):
   - inside a syllable (`क<b>ि</b>`, `क<b>्ष</b>`, a reph in a token of its own): the group is merged and drawn as one
     word in the first token's style; the glyphs are right and the inner style is gone. Its link, its visible offset
     and the dictionary's lookup text are the first token's and the joined text;
   - a converter's span split of one style: merged, and the line is byte for byte what one token would give;
   - between syllables, and in Arabic (a bold letter in a joined word, a lam-alef across a `<span>`): each token keeps
     its style, the context gives the joined forms, and a ligature across the glue is drawn once, in the first
     token's style.
5. **Sizes:** a heading at a native size, a word at an inline size percent, a superscript: the stored positions match
   the drawn glyphs.
6. **Arabic lines:** a word in brackets, a word with Arabic-Indic digits, a vocalised line in landscape past 128
   slots (the fallback), and a line across the parser's 96-token flush.
7. **The X4's in-place gate** on a Hindi book in a TrueType font.

## Left out, on purpose

- **Bitmap `.cpfont` shaping:** the GSUB closure at private-use code points, `FlashBlobCache`, the `CPSH` section,
  the converter's compiled plans. Later, if ever, and then on top of `CPac` in our store.
- **Shaped UI strings** (titles, the status bar's chapter title, the TOC): step 5 (the controller's call).
- **Per-glyph clusters** (a selection inside a word); the run is the unit.
- **HarfBuzz's Arabic fallback** (presentation forms for a font without the features), Syriac, N'Ko, Mongolian; GPOS
  mark placement for Hebrew niqqud (step 1's anchors serve).
- **Khmer, Myanmar, Tibetan:** #3787's USE port covers Sinhala and the v3 Indic tags only.
- **Thai and Lao:** a word dictionary for line breaking.
- **Nastaliq Urdu** (Noto Nastaliq Urdu): its cursive chains reach far below the line. Naskh draws Urdu.
- **Kashida justification; Hindi hyphenation patterns.**
- **A run cache,** until PR 3's build times call for one.

## Left to the builder

- Names: the shaping face interface, the modules, the renderer's glyph-run calls, the TextBlock accessors.
- The glued-group interface's exact shape (what step 2 of "How shaping runs" asks of it is fixed), and how a merged
  token's parts are kept for the parser's offsets.
- The record's exact bytes, and whether glyph forms share bit 0's offset array (a form led by 0xFF, which no UTF-8
  form can start with) or have their own.
- The code points per shaping call (32 or 48), from the share of tokens past it in a Malayalam and a Tamil book.
- Where `CPac` sits in a flash-mode block (after the file) and in an SD-mode block (a table of its directory).
- The fixture's text and the oracle corpus's size (upstream: about 4k strings a script).

## Measure first (a gated phase)

The figures above are estimates. These measurements replace them. It is a gated phase (the controller's call, under
"My calls"):
- the plan opens with items 1-3, before any of PR 1's code; item 7 comes before PR 4's code; items 4-6 run in the PR
  named;
- each item with a **pass line** below passes, or the design reopens before the next PR starts. The one exception
  says its remedy (item 6's build time).

The pass lines:

| Item | Pass line | Missed |
|---|---|---|
| 1 | 0 differences from HarfBuzz on upstream's corpus under our compiler and flags | stop and look: no PR until the difference is understood |
| 2 | flash for PRs 1-3 at most 90 KB; at least 250 KB of the app slot free after them | the design reopens |
| 3 | each merged family fits the font area, and builds in place or not, as this design says (flash or SD mode; the in-place gate) | the design reopens |
| 6 | on the X3, at least 10 KB of heap free at the floor of a cold build of a shaped-script fixture, with the shaping buffer, faces and plans in place | the design reopens |
| 6 | on the X3, a shaped chapter builds in at most twice the time of a Latin chapter of the same length | #3787's run cache comes back |

1. **The host probe of upstream's OtShaper** (not run while drafting: compiling upstream's code was refused in that
   session). Build `lib/OtShaper` from `pr-3787` at `dd45ba61` (or, once PR 1 is in, from ours) with
   `g++ -std=c++20 -O2 -fno-exceptions`, plus a probe `main` that replaces the global `operator new`/`delete` with a
   counting pair (live bytes, peak, allocation count). Over the four `.layout` fixtures in
   `test/complex_shaper/data`, and over the real Noto TTFs of decision 6 (the fixtures are layout subsets):
   - with `CPac` and `CPpl` dropped from the `FaceTables` (the TrueType route): `Plan::build`'s result, the planned
     lookups per table, `Plan::memoryBytes()`, the heap peak and allocation count during the build, and the host time;
   - with `CPac` kept: the same, to show the filters leave the heap (the plan row of the table above);
   - `sizeof` of `GlyphInfo`, `GlyphPosition`, `Buffer`, `Face` and `Plan` on the host and, through the RISC-V
     toolchain, on the C3 (a `static_assert` in a scratch firmware build is enough): the buffer and face rows of the
     table;
   - `Face::init`'s cost with `CPac` present (its validation walks every lookup), against a re-point (deviation 12);
   - per word: `ot::shape`'s heap use and time on a corpus line per script, and the glyph count against the code
     points (the 2n + 16 bound);
   - the oracle: `ot_shape_cli` with `compare_harfbuzz.py` over `generate_corpus.py`'s strings, which must report 0
     differences on our compiler and flags (pass line).
2. **Flash.** A scratch build of `default` and `gh_release` that calls `ot::shape` from a dev-only path, against the
   same tree without the call: the 55-75 KB estimate, with an allowance for the glue, the install writer and the
   glyph-run draw against the 90 KB pass line.
3. **The fonts of decision 6.** The fontTools merge of Noto's Latin into each of the 22 fonts: that it keeps GSUB,
   GPOS and GDEF whole (the oracle against the script's own font), each merged style's size, whether each family fits
   the glyph store in flash mode or goes to SD mode (`ttf::fitsStore`) with its `CPac` counted, and whether Noto's
   Indic TTFs really carry no Latin (#3787's claim) (pass line). Since decision 6's refinement (2026-11-02) the merge
   drops `vhea` and `vmtx`, and the Latin check compares the Latin letters' runs with Noto Sans or Serif itself.
4. **On the X3 (PR 2):** `CPac`'s size and the install time of a merged Devanagari and a merged Bengali family; the
   time of one `Plan::build` and of one word's shaping with the tables read from mapped flash.
5. **In the pipeline dump (PR 3):** a resident Indic page's bytes with records, against the same page with tokens'
   byte counts; the allocation count per build on the Indic fixture against a Latin book's; the share of tokens past
   32 and 48 code points in a Malayalam and a Tamil book (the per-call cap).
6. **On both boards (PR 3):** the loop task's stack high-water against 16,384 B; Background-B's lowest heap during a
   cold Indic build (pass line on the X3); the reading heap after a page turn on an Indic page; an Indic chapter's
   build time against a Latin one of the same length (pass line on the X3).
7. **Before PR 4:** the Arabic shaper's feature list and stages read off HarfBuzz 14.5's `hb-ot-shaper-arabic.cc`
   (the list in deviation 11 is from memory), and the plan and `CPac` sizes of the two merged Arabic fonts.
