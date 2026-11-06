# Text shaping: Indic scripts on the page

How a word of an Indic script is shaped at section build, stored in the section and drawn. This is the
**current-state reference**. Code is in `lib/TextShaping`, `lib/OtShaper` and `lib/Epub/Epub` unless named otherwise.

The shaped scripts are the ten Indic ones of U+0900-0DFF: Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil,
Telugu, Kannada, Malayalam and Sinhala (`lib/Utf8/IndicScripts.h`). A word is shaped once, when its section is built;
a page turn draws what the build stored and shapes nothing. Only a TrueType family (`.ttf` or `.otf`) that maps the
script shapes it. Every other font gets the vowel signs reordered, without conjuncts; what is not shaped yet is listed
at the end.

Related pages:
- the design, its deviations and the decisions behind them:
  [writing modes step 2](superpowers/specs/2026-11-02-writing-modes-step2-design.md), with its
  [measurements](superpowers/specs/2026-11-02-writing-modes-step2-measurements.md);
- where the shaping memory lands, and the word cache's free tail:
  [memory-allocation-strategy.md](memory-allocation-strategy.md) (§4, Class C; §6, invariants 6 and 10; §8.2);
- the bytes of a glyph form in the section file: [file-formats.md](file-formats.md) (a line's bidi region);
- what the reader sees: "Supported Languages" and "Current Limitations" in the [user guide](../USER_GUIDE.md).

## The layers

| Layer | Where | What it does |
|---|---|---|
| The parser | `ChapterHtmlSlimParser` (`characterData`, `consumeText`) | keeps a lone ZWJ or ZWNJ after an Indic letter; captures no drop cap from one |
| The shaping layer | `TextShaping`, `HebrewShaper`, `IndicShaper` | which glued tokens merge; a token's runs; a token turned into pieces over a face |
| The engine | `lib/OtShaper` (`ot::Face`, `ot::Plan`, `ot::shape`); `lib/Utf8/IndicReorder` | HarfBuzz 14.5's Indic and USE shapers with its GSUB and GPOS, as crosspoint-reader #3787 ported them; the vowel reordering for a font that cannot shape |
| The face and its plans | `ShapingFace` (`lib/EpdFont`), `TtfFont::shapingFace`, `GfxRenderer::shapingFace` | one `ot::Face` per style over the tables where the glyph store maps them; the plans built on it |
| Layout | `ParsedText`, `ShapedStore`, `WordShapeCache` | shapes each token once a paragraph, breaks the lines on the shaped widths, cuts tokens into pieces |
| Storage and draw | `TextBlock` (bit 2 of the bidi region), `lib/GfxRenderer/GlyphRun.h`, `GfxRenderer::drawGlyphRun` | the glyph form a shaped word is stored and drawn as |
| The build | `Section`, `InPlaceBuildGate`, `EpubReaderActivity` | the degraded bit and its rebuild; the end of the build's shaping memory; the X4's in-place gate |

## The parser

**Joiners** (deviation 8). The parser drops every Default_Ignorable code point, but a shaper needs ZWJ (U+200D) and
ZWNJ (U+200C): Hindi steers half forms with them, and a Malayalam chillu ends in ZWJ.
- `consumeText` keeps a lone joiner when the code point before it is in U+0900-0DFF (`keepsJoinerAfter`), a letter or
  a mark, whatever follows. It is held (`heldJoiner_`) until the next code point says it is alone, then put into the
  word ahead of that code point (`releaseHeldJoiner`). At a word's end (whitespace, a no-break space, a block's or a
  cell's end) `settleHeldJoiner` puts it last in the word, or onto the last token when an inline end sent that word
  out already.
- A second joiner drops both and the rest of the run (`droppingJoinerRun_`), and a joiner after any other code point
  is dropped as before. Runs of hundreds of joiners between words, a watermark, are still dropped whole.
- The look-behind (`joinerLookBehind_`) crosses SAX chunks and inline element edges. `characterData` holds a 3-byte
  sequence a chunk ended inside (`splitTail_`) and completes it from the next chunk, so a letter or a joiner is never
  read in halves; `deliverSplitTail` hands on a tail that never got its rest.
- A kept joiner counts in no visible offset inside a word: the offsets KOSync and the paragraph LUT read stay what
  they are without it.

**Drop caps.** `consumeText` abandons a drop-cap capture at its first Indic code point and gives the text back to the
word flow, to be shaped whole: a drop cap draws code points one by one, which would split a cluster.

## A paragraph, step by step

`ParsedText::layoutAndExtractLines` runs these for a paragraph, or for a pass of a long one.

1. **Whether the pass shapes** (`decideShaping`, after `decideBidi`): it does when the paragraph needs no bidi and a
   token holds a byte sequence of U+0900-0DFF (`IndicShaper::holdsIndic`). The book's language does not decide it;
   its text does. A pass that does not shape takes the path it took before shaping existed, and its lines keep their
   bytes.
2. **Glued groups** (deviation 7, `mergeGluedGroups`). Tokens with no space between them are one word that the
   parser split where a span started or ended. `TextShaping::mergesWithPrevious` joins two of them before anything is
   measured:
   - in a pass that shapes, in a group that holds Indic text, when they share a style (font, style, size and
     emphasis mark: a converter's span split), or when the glue falls inside a syllable
     (`indic::syllableBreakAllowed` false across it, as in `क<b>ि</b>`). The merged token is drawn in its first
     part's style, and the inner style is lost;
   - in any paragraph, a token of transparent marks only glued after the Hebrew divine name
     (`HebrewShaper::endsWithName`), so the name draws as he and geresh.

   A merged token keeps its first part's style, size, boundary and visible offset, and its parts' texts are joined.
   `TokenBoundary::Boundary::merged` counts its parts, so a line's token count (`lastLineTokenCount`) and a
   footnote's anchor (`sourceTokenCount`) stay the parser's. Ruby tokens, combine requests and no-break spaces are
   never merged, nor the bases of a ruby still open (`keepUnmergedFrom`). Tokens of different styles between
   syllables stay apart, and each is shaped with the other's syllable as context.
3. **The store** (`readyShapedStore`): an entry for every token in `ShapingState`'s `ShapedStore`, reserved from the
   paragraph's estimate (2 B of records a byte of Indic text).
4. **Each token** (`calculateWordWidths` calls `ParsedText::shapeToken`):
   - its entry, when it was made for the same text and `shapeKey` (font, style bits, size, font slot, the word's
     scale, the font's base scale, the drawn ppem, and whether there is a face and the line is vertical): a token kept
     for the next pass is not shaped again, and records shaped at one size are never reused at another;
   - else the build's word cache (below), for a token with no glued neighbour;
   - else `shapeInto`: an `IndicShaper::Request` with the face (`shapingFaceFor`, one renderer answer per font and
     style a pass), the drawn ppem (the face's 26.6 ppem times `fontBaseScale` times `wordScale`: records are in
     drawn pixels), the book's language systems (`TypographyProfile::shapingLanguage`, `ot::languageTagsFor` of the
     book's `dc:language`) and the glued neighbours kept apart as context; then `IndicShaper::shapeToken` in an
     `IndicShaper::CallMemory`.
5. **Warm and measure.** Each Glyphs piece's glyph IDs are looked up once (`GfxRenderer::prefetchGlyphRun`), inside the
   build's TrueType miss scope, so a page drawn later needs no miss memory for them. Each piece's width is then set
   (`measurePiece`): a Glyphs piece by its records' pen (`glyphRunWidth`), any other as its code points are measured.
   The token's width is their sum.
6. **Line breaking** with the breakers as they are, on those widths.
   - A forced split of a word wider than the line goes through the Hyphenator's fallback breaks, which never fall
     inside a syllable (`appendFallbackIndexes`). `hyphenateWordAtIndex` measures each candidate prefix, its hyphen
     included, by shaping it in the store's scratch entry (`shapedPrefixWidth`); the parts are then shaped again on
     their own.
   - Focus reading never splits a word that holds Indic text (`applyBionicReadingTransform`).
   - A glued gap next to a Glyphs piece takes no kerning (`kernBefore`, `glyphEdge`): the engine's kerning is keyed by
     code point, and the shaper kerned inside the run.
7. **Pieces on the line** (`viewShapedPieces`, from `extractLine`). A token with pieces becomes its pieces, glued, in
   order, each in the token's style and size, at the token's x plus the widths before it. A Glyphs piece goes to the
   line as its glyph form, a Reordered piece as its display form (without its soft hyphens), a Text piece as plain
   text; a Text piece of soft hyphens alone is left out. Annotations then name pieces. A token that is one run, the
   usual Indic word, stays one word. A line with no pieces keeps its bytes.

## The shaping layer

`lib/TextShaping` has no dependency on the TrueType engine: it sees a face through `ShapingFace`.

- **`TextShaping`**: `mergesWithPrevious`; `decodeAt`, a UTF-8 decode bounded by its view; `shape`, step 1's
  per-token units (the Hebrew module); and the build's heap block for shaping calls (`buildHeapBlock`,
  `returnBuildHeapBlock`, `releaseBuildMemory`).
- **`IndicShaper::scanRuns`** (upstream's `scanRun`): a token's runs, each one script's letters with the shared code
  points around them (dandas, Vedic signs, joiners, the dotted circle).
- **`IndicShaper::shapeToken`** writes a token's pieces into a `PieceSink` (the paragraph's `ShapedStore`) and answers
  an `Outcome`:
  - with no face, one Reordered piece over the whole token when `IndicReorder` changes it;
  - with a face, per run: shared code points alone are Text; a script whose probe letter the face's cmap does not map
    (`ScriptInfo::probe`, upstream's coverage test) is Reordered when `IndicReorder` changes it, else Text; any other
    run is Glyphs. Text between runs is Text;
  - `Plain` when nothing needs a form, `Formed` when the pieces are written, `Degraded` when a run could not be shaped.
- **A Glyphs run** (`shapeGlyphs`, `shapeRun`): its code points with the glued neighbours' syllables as context
  (`lastSyllable`, `firstSyllable`, at most `kContextCodepoints` each), shaped whole. A run past the buffer's storage
  is shaped by upstream's piecewise loop: as many syllables as the buffer holds, each piece with the syllable on
  either side as context, then one code point on either side, before, after, then none. The plan and the face are
  asked again before each `ot::shape`, because a plan is valid only until the next `plan()` on its face and a face's
  mapping can move.
- **The records** (`appendGlyphs`, upstream's rounding): the advance from 26.6 to 12.4 pixels, `(xAdvance + 2) >> 2`;
  the offsets to whole pixels, rounded half away from zero, y down; in visual order. A record outside its fields is
  refused, never clamped.
- **`IndicReorder`** (deviation 9) runs here, at build time, through a bounded overload of `indicReorderForDisplay`
  that writes into the call's block (`3 * len + 1` bytes always hold the form) and allocates nothing.

`IndicShaper.cpp` names what it ports from #3787's `lib/EpdFont/ComplexShaper.cpp`: `scanRun`, the piecewise loop of
`appendShapedRun` and the rounding of `appendGlyphs`. What it does not take: shaping in the renderer, the run cache
and the layout memo (deviation 2).

## The face and its plans

- **`GfxRenderer::shapingFace(fontId, style, &ppem)`** answers the `ShapingFace` of the TrueType file that draws that
  style (a synthetic style's source file) and its ppem in 26.6, or null for any other font. `TtfFont::shapingFace`
  makes it on first ask; when the heap refuses it, the answer is null with `*refused` set, which a token holding
  Indic text latches (see "What fails how").
- **`ShapingFace::current()`** is the `ot::Face` over the tables where the glyph store maps them, nothing copied
  (deviation 3), with the `CPac` lookup filters the install wrote (deviation 4). A flash-mode block moves when another
  style is mapped, and the face is then re-pointed (`Face::rebase`, deviation 12). Under RenderLock only.
- **`ShapingFace::plan(script, languageTags)`** builds an `ot::Plan` on first ask and keeps it in the face, by
  `unique_ptr`, never moved, so no plan outlives the face it points into. A face keeps `kMaxPlans` (2); a third key
  frees the one asked for least recently.
- **The plan's memos** (deviation 13): both leave the output as it is, by construction.
  - Each planned lookup's digest is copied into the plan when it is built, GSUB's then GPOS's (24 B a lookup, from
    `CPac`, else computed). The apply loop tests that copy before it reads the lookup's settings from the font
    (`Plan::mayApply`), and the would-substitute probes read it too. A plan that cannot allocate the copy fails to
    build, so no probe ever runs without its digest.
  - `consonantPositionFromFace`'s answers are remembered by the plan (`Plan::consonantPosition`): 64 direct-mapped
    slots keyed by the consonant's and the virama's glyph IDs, `mutable` in a const plan, which serves one build at a
    time under RenderLock.
  - `Plan::testBypassCaches` switches both off, for the tests that compare the output with and without them.

## The word cache

`WordShapeCache` keeps the build's shaped words, so a word the chapter repeats is shaped once. The parser owns it:
armed in `ChapterHtmlSlimParser::setup()` once the SAX state is in the arena, bound to each `ParsedText` beside its
line arena (`setWordCache`, table cells included), disarmed at the end of `finalize()`, before the section gives the
arena's blocks back.

- **Where:** the top 16 KB (`kWordCacheBytes`) of a lent build arena, which the arena still counts as free. No other
  arena user loses room to it, and any allocation may overwrite it ([memory strategy](memory-allocation-strategy.md)
  §4, Class C, "The free tail"; its place by moment in §8.2).
- **When it is valid:** `BuildArena::markPeak()` and `peakSinceMark()` tell the cache whether an allocation reached
  the tail since it last looked. It checks before every read and every write of a slot; when one has, it empties
  itself (`clears`) and arms again once the cursor is back below the tail by `kRoomBelow` (16 KB). It arms only with
  that room free below the tail too, so the owned 10 KB arena of a released build has no cache and shapes every word.
  The tail is sound because nothing writes an arena's memory except through `alloc()` (memory strategy §6, invariant
  10).
- **Layout:** 2-way sets of 64 B ways (`kWayBytes`), 128 sets. An entry holds the identity (the text's 64-bit hash
  and length, and `keyFor`: the token's `shapeKey` with its language tags folded in) and the token's pieces with their
  records or reordered form. A third entry in a set evicts the way used least recently. An entry of up to
  `kDoubleEntryBytes` (126 B) takes both ways of its set; a larger one, or a token of more than 255 bytes, is not
  stored (`oversize`).
- **What is cached:** only tokens with no glued neighbour, shaped `Formed`. Glued tokens, which are shaped with their
  neighbours as context, and the breaker's candidate prefixes take the uncached path. Widths are not cached: they are
  measured after a replay, as after a shaping.
- **The log:** each build prints `createSectionFile spine=<n> word cache: bytes= hits= misses= inserts= bypassed=
  oversize= clears=`, with `bytes=0` when it never armed.

The cache hit 78% of the lookups on the X3's timing book, whose chapter repeats its paragraphs, but 24% on Malayalam
prose (Indulekha) and 18% on Tamil (Ponniyin Selvan), laid out with the X3's arena.

## Glyph forms in the TextBlock

A shaped word is stored as a **glyph form** in its line's bidi region (decision 4), bit 2 of the region's `parts`
byte. Glyph forms share the display forms' offset array (bit 0's): a form led by 0xFF, which no UTF-8 form starts
with, then a `u16` glyph count, the compact records of `GlyphRun.h` (`u16` glyph; a `u16` advance in 12.4 pixels
whose top bit says an offset follows; `i8 dx`, `i8 dy`), then 0x00. The bytes are in [file-formats.md](file-formats.md).

- A word has one form at most: a glyph form or a display form. Its text stays its logical text, for the dictionary,
  footnote links, the QR text and KOSync.
- `TextBlock::hasGlyphForm(i)` and `glyphForm(i)` read it; `wordDrawText` answers the text for a glyph word.
- `TextBlock::bidiValid` refuses on load a glyph form that is not at least one glyph's whole records followed by its
  0x00, a 0xFF form without bit 2, and a glyph form on a vertical line.
- A line without glyph forms, every Latin, CJK and Hebrew line, keeps the bytes it had before them.

## The draw

- **`TextBlock::render`** draws a glyph word with `GfxRenderer::drawGlyphRun`, at the position and scale its text
  would take; underline, strike-through and guide dots take the run's width (`glyphRunWidth`). A glyph word gets no
  emphasis marks: they follow a text's advances, not its glyphs'.
- **`TextBlock::wordBox`** measures a glyph word by its run's pen, so the dictionary's highlight lands on it.
- **`drawGlyphRun`** looks each record's glyph up by ID, as the key 0x110000 + gid (`glyph_key`, deviation 5), in the
  word's font and style with no fallbacks (`findGlyphKey`), and draws it at the pen plus its offset. The pen moves by
  the record's advance; a glyph the font lacks draws nothing and the pen still moves. No kerning, ligatures or
  tracking; SUP and SUB halve nothing, because the records are in drawn pixels already. A font that draws no glyph
  keys (a `.cpfont` or built-in face under the ID) draws no run, logged once. While a page is scanned for the glyph
  prefetch, `drawGlyphRun` records the run's glyph IDs (`FontCacheManager::recordGlyphs`) instead of drawing.
- **The dictionary's word selection** (`DictionaryWordSelectActivity`) draws a shaped piece as its run (`DRAW_GLYPHS`,
  `glyphFormOf`) and never cuts it at a dash; the lookup reads the logical text.

**The code-point prefetch.** Before measuring, `layoutAndExtractLines` prefetches every code point of the paragraph
(`GfxRenderer::ensureFontReady`), shaped words' code points included, though a shaped word draws by glyph ID. So the
glyph store holds about one extra glyph per distinct code point of the shaped text, keyed by the code point beside the
glyph keys, once per glyph store.

## Lifetimes

Nothing of the shaper outlives the build that made it, so none of it is resident while a book is read, and a page draw
allocates nothing for a shaped word. Within the build, the faces and their plans and the paragraph store's chunks
(`reset()` keeps them from one paragraph to the next) live longest.

| What | Where | Lives |
|---|---|---|
| A call's block (`IndicShaper::CallMemory`: the `ot::Buffer` for `2 * kMaxRunCodepoints + 16` glyphs, the token's runs, its code points with their context, `IndicReorder`'s form; for a call without a face, `CallKind::Reorder`, the form alone, `3 * tokenBytes + 1` B) | a block of the lent arena (`ParsedText::lineArena_`), else the build's heap block | one `shapeToken` call; released before any of the token's glyphs is looked up, LIFO with the TrueType miss scope |
| The build's heap block (`TextShaping::buildHeapBlock`) | heap, made on first use, grown nothrow in 512 B steps to the longest token, the smaller one freed first | lent to one call at a time; freed by `TextShaping::releaseBuildMemory()` at the build's end |
| The faces and their plans | heap (`TtfFont::shapingFace`, `ShapingFace::plan`) | made on first ask; freed by `GfxRenderer::releaseShapingFaces()` at the build's end, or by the font set's next load (a reload or an install) |
| `ParsedText::ShapingState`: the store, the pass's face answers, the line's piece vectors | heap, `makeUniqueNoThrow` | a paragraph's passes, and the store's 1 KB chunks the parse's later paragraphs too (`reset()` empties them and keeps them); freed at a slice's yield (`releaseLayoutScratch`) unless a paragraph in progress shapes, and with the parser's text block |
| The word cache | the lent arena's free tail | one parse, emptied whenever an allocation reaches it |

The build's end is `Section::BuildState`'s destructor, which calls `BidiLine::releaseBuildMemory()`,
`TextShaping::releaseBuildMemory()` and `GfxRenderer::releaseShapingFaces()`. The dictionary's definition pages lay
out HTML without a section build, so `buildDictionaryHtmlPages` releases the same three on every exit. The X4's
in-place build lends no arena, so its calls would take the heap block: a book in a shaped-script language never builds
in place in a TrueType set (`InPlaceBuildGate::admitsResident`, `Request::shapedScript` from
`TypographyProfile::shapedScript`, logged as `shapedTtf=1`). The gate reads the book's language, not its text: the
primary subtags of `isIndicLanguage` (`TypographyProfile.cpp`), Hindi, Marathi, Nepali, Sanskrit, Konkani, Maithili,
Bhojpuri, Awadhi, Magahi and Rajasthani, Bengali, Assamese, Punjabi, Gujarati, Odia, Tamil, Telugu, Kannada,
Malayalam and Sinhala. Such a book in a bitmap or built-in font does build in place, and its calls only reorder: the
heap block they take is the longest token's form, in 512 B steps.

## What fails how

**Degraded.** A run that cannot be shaped is laid out as its code points, and the parse is latched:
- `IndicShaper::Outcome::Degraded`: no block for the call (no arena room and the heap block refused); the face not
  mapped now, or its plan refused or failed to build; the font's lookups failing the run (growing it past 2n + 16
  glyphs, nesting too deep, or running past the op budget); a run whose every glyph the font's GSUB deletes; a record
  outside its fields; the store refusing a piece;
- `ParsedText` without room for its `ShapingState`, its store's entries, or a forced split's entry.

The shaped layout's own containers grow behind a heap check too (`heapAllows` in `ParsedText.cpp`, the largest free
block against what the growth asks for, as `ShapedStore::heapAllows` checks its own), since a growth the heap refuses
aborts under `-fno-exceptions`. Each refusal latches, and takes the unshaped way for what it refused:
- a merge's text (`mergeGluedGroups`, up to 1 KB): the tokens stay apart, as past the cap;
- the copy a token's pieces are measured from (`ShapingState::measureScratch`): the token is laid out as its code
  points; a candidate's, in a forced split: the candidate is measured as code points;
- the string a forced split copies its candidates into (`ShapingState::candidate`, grown once to the word): the word
  is not split;
- a line's piece vectors, its x scratch and its form copies (`viewShapedPieces`, one check for all of them): the line
  keeps its bytes, its tokens drawn as their code points.

A face the heap refused (`TtfFont::shapingFace`'s own allocation, about 130 B) latches too, though the token is laid
out as without a face, in `IndicReorder`'s form: the renderer says so (`GfxRenderer::shapingFace`'s `refused`), the
pass's face answer keeps it, and `shapingFaceFor` latches. Only a token that holds Indic text asks for a face, so a
refusal for a style whose words are all Latin latches nothing.

The latch (`ParsedText::setShapingLatch`) sets `ChapterHtmlSlimParser::shapingDegraded()`, and the section writes
status bit 6, `kStatusShapingDegraded`, and logs `spine=<n> shaping degraded`. As for a demoted table row
(memory strategy §6, invariant 6):
- a cache read with the bit is rebuilt once per spine per session (`EpubReaderActivity::buildSection`,
  `degradedLayoutRebuildSpine_`); a rebuild that degrades again is read as it is;
- Background-B discards such a build for a foreground rebuild, and the current section's build falls back to the
  released rebuild;
- a degraded build's page count is not recorded.

**Not degraded.** A font that cannot shape is not a failure, and nothing is latched: no face (a `.cpfont` or built-in
font, a vertical line) or a face that does not map the script gives `IndicReorder`'s form. So a Hindi book in a bitmap
font is not rebuilt at every open.

**Fixed limits**, each with what happens past it:

| Limit | Value | Past it |
|---|---|---|
| `IndicShaper::kMaxRunCodepoints` | 32 code points a call (the buffer's `ot::maxRunLength`) | the run is shaped in pieces between syllables |
| `IndicShaper::kContextCodepoints` | 8 code points of a glued neighbour's syllable | the nearest 8, marked partial |
| A merged token (`kMaxMergedBytes`, `kMaxSyllableMergeBytes`, `Boundary::merged`) | 512 B; 1,024 B for glue inside a syllable; 255 parts | the tokens stay apart, shaped with each other as context |
| A token in a call | 65,535 B | degraded |
| `ShapedStore::kChunkBytes`, `kMaxChunks` | 1 KB chunks, 32 held | the piece is refused (logged once at ERR): degraded |
| A glyph record | glyph ID 65,535; advance -1,024 to 1,023.9375 px; offsets -128 to 127 px | degraded |
| `ShapingFace::kMaxPlans` | 2 plans a face | the plan asked for least recently is freed and built again when asked |
| `ShapingState::kFaceAnswers` | 8 fonts and styles a pass | the renderer is asked at each use |
| A `WordShapeCache` entry | 126 B; a token of 255 B | not cached: shaped each time |
| The engine (`lib/OtShaper`) | upstream's: `MAX_NESTING_LEVEL` 8, `MAX_CONTEXT_LENGTH` 64, 2n + 16 glyphs for a run of n, the op budget (`MAX_OPS_FACTOR`, `MAX_OPS_MIN`), `MAX_PLANNED_LOOKUPS` 4096 | the run fails: degraded |

## What is not shaped yet

- **Paragraphs that need bidi** (a right-to-left level, or a word with a right-to-left letter or control): their
  Indic words are drawn as their code points, not even reordered. Shaping in them needs the line pass's run-cut
  pieces, which the Arabic joining module brings (step 2, PR 4).
- **Titles, chapter names and the TOC**, on Home, in the Library, in the status bar and the chapter list: the UI
  faces are bitmap and cannot shape, so these draw unshaped in the face that draws them today. They wait for step 5's
  visual strings.
- **Bitmap fonts** (`.cpfont`, built-in) and a TrueType family that does not map the script: reordered only. The
  vowel signs are placed; conjuncts are not formed, and the virama stays visible.
- **Vertical sections**: no build sets a shaped run down a column (a load refuses one), so their Indic words are
  reordered, as in a bitmap font.
- **The Font Manager's Indic families**: until it offers them (step 2, PR 3b), a family's files go into
  `/.crosspoint/fonts/<Family>/` by hand.

## Time

Each shaped word costs a walk of the flash-mapped GSUB and GPOS, so an Indic chapter builds slower than a Latin one;
page turns cost nothing more. On the X3, a 2,483-word Hindi chapter builds in 4.9 s against 2.5 s for a Latin chapter
of the same length, with the word cache and the plan's memos (the
[measurements](superpowers/specs/2026-11-02-writing-modes-step2-measurements.md), "PR 3: shaped text on the page").

## Tests

- `test/text_shaping`: `IndicShaper` (runs, pieces, context, the piecewise loop, refusals), `TextShapingMerge`,
  `IndicRuns`; `FixtureShapingFace.h` is the shared test face.
- `test/shaped_store` (`ShapedStore`), `test/word_shape_cache` (`WordShapeCache`), `test/memory_arena` (the watermark).
- `test/word_size_layout/ShapedLayoutTest.cpp`: `ShapedLayout` (widths, merges, splits, refusals, keys) and
  `ShapedLine` (pieces on the line).
- `test/text_block_range/TextBlockGlyphTest.cpp` (`TextBlockGlyphForms`), `test/ttf_font/TtfShapingTest.cpp`
  (`ShapingFacePlans` among them), `test/ot_shaper` (the plan's memos against the walk).
- `test/epub_pipeline`: `Joiners`, `IndicRules`, `ShapingStatus`, and `ShapingRules` over four books
  (`test/epubs/test_shaped_{devanagari,bengali,tamil,malayalam}.epub`, from `make_test_shaped_indic.py`), with five
  goldens: one per book and `test_shaped_devanagari_unshaped`. The books' fonts are subsets of Noto Sans
  (`test/fixtures/fonts/README.md`).
- `test/hyphenation_eval` (`IndicFallback`), `test/in_place_build_gate`.
