# Text shaping: Indic scripts and Arabic on the page

How a word of an Indic script or of the Arabic script is shaped at section build, stored in the section and drawn. This
is the **current-state reference**. Code is in `lib/TextShaping`, `lib/OtShaper` and `lib/Epub/Epub` unless named
otherwise.

The shaped scripts are the ten Indic ones of U+0900-0DFF: Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil,
Telugu, Kannada, Malayalam and Sinhala (`lib/Utf8/IndicScripts.h`), and the Arabic script (Arabic, Persian, Urdu and
the other languages of `language_tag::isArabicScript`; its code points are in `lib/Utf8/ArabicScripts.h`). A word is
shaped once, when its section is built; a page turn draws what the build stored and shapes nothing. Only a TrueType
family (`.ttf` or `.otf`) that maps the script shapes it. Where the families come from, and which one a book takes, is
under "The families" below. Every other font gets the Indic vowel signs reordered, without conjuncts, and Arabic
letters drawn unjoined; what is not shaped yet is listed at the end.

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
| The shaping layer | `TextShaping`, `HebrewShaper`, `IndicShaper`, `lib/Utf8/ArabicScripts.h` | which glued tokens merge; a token's runs (Indic and Arabic); a token turned into pieces over a face |
| The engine | `lib/OtShaper` (`ot::Face`, `ot::Plan`, `ot::shape`); `lib/Utf8/IndicReorder` | HarfBuzz 14.5's Indic, USE and Arabic shapers with its GSUB and GPOS, as crosspoint-reader #3787 ported the first two; the vowel reordering for a font that cannot shape |
| The face and its plans | `ShapingFace` (`lib/EpdFont`), `TtfFont::shapingFace`, `GfxRenderer::shapingFace` | one `ot::Face` per style over the tables where the glyph store maps them; the plans built on it |
| Layout | `ParsedText`, `ShapedStore`, `WordShapeCache`, `BidiLine` | shapes each token once a paragraph, breaks the lines on the shaped widths, cuts tokens into pieces; in a bidi paragraph the line pass places the pieces |
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

1. **Whether the pass shapes** (`decideShaping`, after `decideBidi`): it does when a token holds a byte sequence of
   U+0900-0DFF (`IndicShaper::holdsIndic`), in a paragraph that needs bidi or not; or when the paragraph needs bidi, a
   token holds a byte sequence of the Arabic blocks (`arabic::holdsArabic`) and that token has a shaping face that
   maps beh (`IndicShaper::drawsArabic`, the coverage test `shapeToken` makes of each Arabic run; the first such token
   decides, and a face not mapped at that moment counts as one). The book's language does not decide it; its text
   does. An Arabic run starts at an AL letter, which makes the paragraph need bidi, so a Latin paragraph with
   Arabic-Indic digits or an Arabic comma shapes nowhere. An Arabic paragraph in a bitmap or built-in font, or in a
   TrueType face without Arabic (a Latin reader font with no Arabic family installed), makes no `ShapingState` and
   merges nothing: its lines are those of the line pass alone. A pass that does not shape takes the path it took
   before shaping existed, and its lines keep their bytes.
2. **Glued groups** (deviation 7, `mergeGluedGroups`). Tokens with no space between them are one word that the
   parser split where a span started or ended. `TextShaping::mergesWithPrevious` joins two of them before anything is
   measured:
   - in a pass that shapes, in a group that holds Indic text or Arabic text of a paragraph that needs bidi
     (`shapedText`), when they share a style (font, style, size and
     emphasis mark: a converter's span split), or when the glue falls inside a syllable
     (`indic::syllableBreakAllowed` false across it, as in `क<b>ि</b>`). The merged token is drawn in its first
     part's style, and the inner style is lost;
   - in a pass that shapes, a token of marks only glued after a token that ends with an Arabic letter
     (`arabic::endsWithLetter`: the last code point that is no mark is a run letter, not AL punctuation), whatever the
     styles: the mark is placed on its base and takes its style;
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
     `IndicShaper::CallMemory`. A paragraph that needs bidi shapes all its tokens first, before the line pass borrows
     its workspace (`storedWidth` then reads the widths the store kept), so a shaping call's block and the workspace are
     never in the lent arena at once.
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
   - **A paragraph that needs bidi** takes its pieces through the line pass instead (see "Shaped text in a bidi
     paragraph" below): the pass cuts a shaped token at its pieces' edges, so each piece draws one segment's characters
     and is placed by the bidi order.

## The shaping layer

`lib/TextShaping` has no dependency on the TrueType engine: it sees a face through `ShapingFace`.

- **`TextShaping`**: `mergesWithPrevious`; `decodeAt`, a UTF-8 decode bounded by its view; `shape`, step 1's
  per-token units (the Hebrew module); and the build's heap block for shaping calls (`buildHeapBlock`,
  `returnBuildHeapBlock`, `releaseBuildMemory`).
- **`IndicShaper::scanRuns`** (upstream's `scanRun` for Indic text): a token's runs, each one Indic script's letters
  with the shared code points around them (dandas, Vedic signs, joiners, the dotted circle), or an Arabic run (see
  "The run scan" below).
- **`IndicShaper::shapeToken`** writes a token's pieces into a `PieceSink` (the paragraph's `ShapedStore`) and answers
  an `Outcome`:
  - with no face, a Reordered piece where `IndicReorder` changes the text between the Arabic runs (over the whole token
    when it holds no Arabic run); an Arabic run is left as Text, unjoined;
  - with a face, per run: shared code points alone are Text; a script whose probe letter the face's cmap does not map
    (`ScriptInfo::probe`, upstream's coverage test) is Reordered when `IndicReorder` changes it, else Text; an Arabic
    run in a face that does not map beh (U+0628, `arabic::kProbe`) is Text, never reordered; any other run is Glyphs.
    Text between runs is Text;
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

## The families

The Font Manager offers 22 TrueType families: Noto Sans and Noto Serif of each of the ten Indic scripts
(NotoSansDevanagari ... NotoSerifSinhala; Oriya's are NotoSansOriya and NotoSerifOriya), and NotoNaskhArabic and
NotoSansArabic for the Arabic script (Noto's Arabic serif is Naskh; there is no NotoSerifArabic). Each is a Regular and
a Bold file in
`assets/sd-fonts/<Family>/` and listed in `lib/EpdFont/scripts/sd-fonts.yaml` (`format: ttf`, `inDownloadAll: false`,
so "Download All" leaves all 22 out). `lib/EpdFont/scripts/merge_noto_latin.py` builds each file from Noto's script
font and Noto Sans or Noto Serif, so the Latin in a book is drawn in the same face (NotoNaskhArabic takes Noto Serif's,
NotoSansArabic Noto Sans's);
`build-sd-fonts.py` checks the merge with HarfBuzz and fails on any difference. `test/ot_shaper/oracle_shipped_fonts.py`
shapes all 40 Indic files through the font's `CPac` and HarfBuzz and diffs them (measurements, "PR 3b");
`oracle_arabic.py` does the same for the four Arabic files, 6,338 corpus lines at two sizes in ten languages
(measurements, "PR 4b: the shipped Arabic families against HarfBuzz").

**The reader's automatic choice.** `language_tag::indicScriptOf` (`lib/Epub/Epub/LanguageTag.h`) maps a book's language
tag to its script, and the tag's script subtag may veto it ("sa-Latn" is not Indic). `language_tag::isArabicScript`
does the same for the Arabic script: ar, fa, prs, ur, ps, sd, ug, ckb, ks, pnb, azb, bal, skr, glk, mzn, lrc, bqi and
ota, and any language whose first script subtag is Arab ("ku-Arab"); bare "ku" is Latin in most books, and any other
first script subtag vetoes ("ur-Deva", "sd-Latn"). When the reader font has no letter of that script (for Arabic: no beh, `arabic::kProbe`),
`SdCardFontSystem::selectLanguageFamily` takes an installed `NotoSans<Script>` or `NotoSerif<Script>`; for an Arabic
book, `NotoSansArabic` or `NotoNaskhArabic`. Sans first when the reader font's name contains "Sans", else Serif (Naskh)
first, the other as the fallback, as for a CJK book. A reader font that has the script is kept. While the book's language is not yet known,
and any Noto CJK, Indic or Arabic family is installed, the book's font waits for it
(`SdCardFontSystem::waitsForReaderLanguage`), so a book opened from a cold cache goes straight to the family rather than
installing the reader font first. With no family of the script installed the reader font stays, and the script draws as
replacement glyphs. The same predicate sets an Arabic-script book right to left (`TypographyProfile`) and makes it a
shaped-script book for the X4's in-place gate.

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

The cache hit 78% of the lookups on the X3's timing book, whose chapter repeats its paragraphs, but 51% on Hindi prose
(Premchand's Gaban, on the X3), 24% on Malayalam prose (Indulekha) and 18% on Tamil (Ponniyin Selvan), the last two
laid out on the host with the X3's arena. A chapter's distinct words bound it: Gaban's 2,587 words hold 852, so no
cache could hit more than 67%.

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
Malayalam and Sinhala, and every language of `language_tag::isArabicScript`. Such a book in a bitmap or built-in font does build in place, and its calls only reorder: the
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
| `IndicShaper::kContextCodepoints` | 8 code points of a glued Indic neighbour's syllable | the nearest 8, marked partial |
| `IndicShaper::kArabicContextCodepoints` | 16 code points of a glued Arabic neighbour, whole clusters nearest the token, up to the neighbour's run edge | the clusters that fit; one that does not, and all farther, are left out |
| An Arabic token split by a style span, with its context | the buffer's 32 code points: context, run and context together | the piecewise loop reaches only 2 clusters: a ligature across the cut can be lost or doubled (see "Not shaped yet") |
| A merged token (`kMaxMergedBytes`, `kMaxSyllableMergeBytes`, `Boundary::merged`) | 512 B; 1,024 B for glue inside a syllable; 255 parts | the tokens stay apart, shaped with each other as context |
| A token in a call | 65,535 B | degraded |
| `ShapedStore::kChunkBytes`, `kMaxChunks` | 1 KB chunks, 32 held | the piece is refused (logged once at ERR): degraded |
| A glyph record | glyph ID 65,535; advance -1,024 to 1,023.9375 px; offsets -128 to 127 px (`i8`, whole pixels) | degraded |
| `ShapingFace::kMaxPlans` | 2 plans a face | the plan asked for least recently is freed and built again when asked |
| `ShapingState::kFaceAnswers` | 8 fonts and styles a pass | the renderer is asked at each use |
| A `WordShapeCache` entry | 126 B; a token of 255 B | not cached: shaped each time |
| The engine (`lib/OtShaper`) | upstream's: `MAX_NESTING_LEVEL` 8, `MAX_CONTEXT_LENGTH` 64, 2n + 16 glyphs for a run of n, the op budget (`MAX_OPS_FACTOR`, `MAX_OPS_MIN`), `MAX_PLANNED_LOOKUPS` 4096 | the run fails: degraded |

## The engine's Arabic shaper

`lib/OtShaper` shapes Arabic, Persian and Urdu (HarfBuzz 14.5's Arabic shaper, deviation 11 of the design), and since
PR 4b the section build uses it: an Arabic-script word in a TrueType family that maps beh is shaped like an Indic one
(the steps above), and the Font Manager's two Arabic families are what such a book is drawn in. The engine is also
reachable from the host tests, the oracle and the dev command `CMD:TTF:SHAPE` ([serial-test-api.md](serial-test-api.md)).

What it does, with the font's own lookups:

- **The plan**: `Script::Arabic` (after the Indic scripts, so a CPpl's values keep their meaning) builds HarfBuzz's
  feature list and stages: the joining forms (`isol`, `fina`, `fin2`, `fin3`, `medi`, `med2`, `init`), `rlig`,
  `calt`, `rclt` (a horizontal feature), `liga` and the rest, with the language systems for `ar`, `fa`, `prs`, `ur`,
  `ps`, `sd`, `ug`, `ku`, `ckb`, `kmr`, `sdh` and `ks`.
- **Joining**: the state machine over the run and its context sets each letter's form mask; transparent marks are
  skipped; ZWJ and ZWNJ join and cut as in HarfBuzz.
- **Marks**: the modifier combining marks are reordered (UTR #53), in the modified classes, before the lookups run.
- **Right to left**: the run is shaped as HarfBuzz's `hb_ot_shape` does a right-to-left buffer: every character takes the
  `rtlm` mask and none is swapped (no character the tables cover has a mirror), cursive attachment and attachment offsets propagate backwards, and the glyphs and positions are reversed
  at the end, so a run comes out in visual order.

It matches HarfBuzz on all 1,808 recorded fixture words and on the oracle's 100 runs over 6,333 lines (five fonts, two
sizes, ten languages), and the Indic oracle is unchanged.

**Not ported**: `stch` (neither Noto Arabic family has it), the fallback shaping with presentation forms for a font
without the features, the mark placement and legacy `kern` that HarfBuzz's `fallback_position` gives a font without
GPOS, and Syriac. The pauses of the unported features stay, so the stages are HarfBuzz's. Mongolian, N'Ko and Phags-pa are shaped by USE in
HarfBuzz 14.5, not by this shaper.

## The run scan

`IndicShaper::scanRuns` finds an Arabic run with `lib/Utf8/ArabicScripts.h`.
- **Members** (`arabic::isRunMember`): the code points of U+0600-06FF, 0750-077F and 0870-08FF whose bidi class is AL or
  NSM, less U+061C (ALM, a bidi control). A run starts at a member that is no mark and ends at the first code point that
  is no member; a ZWJ or ZWNJ after a member stays inside the run (and an Indic code point, shared ones included, ends
  an Arabic run, as an Arabic letter ends an Indic one).
- **AL punctuation and symbols are members** (the Arabic semicolon, question mark and full stop, the percent-like signs:
  U+0608, 060B, 060D, 061B, 061D-061F, 066D, 06D4, 06FD, 06FE, 0888): they are strong right to left, so a run cannot
  straddle a level, and joining gives them no joining. A joiner follows only a letter or a mark (`takesJoiner`).
- **Not members**: the digits (AN, EN), the number signs (U+0600-0605, U+06DD, U+08E2), the Arabic comma, the neutral
  symbols and the presentation forms (U+FB50-FDFF, FE70-FEFF), which bidi orders or which are formed already. They are
  drawn as plain text in the book's font. Brackets and every other mirrored code point are outside by construction.
- **Cheap tests**: `arabic::holdsArabic` is a byte pre-test over the UTF-8 (digit-only text passes it and its scan
  finds no run); `holdsShaped` is Indic or Arabic. `maxRuns` is `bytes / 2 + 1`, since an Arabic run takes 2 bytes.
- **Context**: an Arabic run is shaped with its glued neighbours' whole clusters nearest the token (a letter with the
  marks and joiners after it), as many as fit in `kArabicContextCodepoints` (16), up to the neighbour's run edge, and its
  own joiners just before it. A glued neighbour kept apart is the case: a word cut by a `<span>` shapes on each side as
  its neighbour's letters say, and a ligature across the cut (the Allah ligature, lam-alef) is drawn once, in the first
  token's style; the second token's run is then a Glyphs piece with no record, which the line leaves off.
- **A forced split of an Arabic word** falls only between base clusters (`appendFallbackIndexes`): never before a mark,
  never before a joiner that follows an Arabic letter or mark, never after a ZWJ. The letters at the cut take final and
  initial forms.

## Shaped text in a bidi paragraph

Arabic paragraphs, and Indic words inside a right-to-left paragraph, go through the line pass (`BidiLine`), which orders
a line's tokens by the bidi levels. PR 4b teaches it to carry shaped pieces.
- **Widths first.** The paragraph shapes all its tokens, then the line pass borrows its workspace and breaks on the
  stored widths (step 4 above). The breaker kerns a gap by the tokens' logical edges (a Glyphs piece takes no kerning
  at a glued gap), while placement uses the visual neighbours.
- **Cuts.** `ParsedText::cutShapedTokens` gives each token of several pieces its cut points (`BidiLine::Token::cuts`,
  the bytes where each piece after the first begins) in `bidiState_->tokenCuts`. `BidiLine::layoutLine` cuts the token
  there, so a piece draws one segment's characters. A cut list that breaks its contract (`cutsAreValid`: strictly
  ascending, inside the text, on code point boundaries) is ignored, and so is one the heap has no room for (logged,
  latched): the token's pieces are then drawn as the line pass's code points.
- **What a piece is drawn as** (`formBidiPieces`): its store piece when that piece is Glyphs or Reordered, at the
  store's width; otherwise the line pass's display form. A segment that lands in several pieces on the line (at two
  levels, around an isolate, or a mark across a cut drawn inside its base's segment: `Piece::segmentPieces` above 1) is
  drawn as code points, because its glyph run would be drawn on each. An empty run is left off only where the ligature
  that took its letter is drawn from glyphs (on this line, or an earlier one after a fallback break); where that
  neighbour is drawn as code points, the letter is drawn as its own.
- **Past the line pass's limits** (`BidiLine::kMaxCodepoints`, or no workspace) the pass runs its fallback, which gives
  a piece for each run of one segment's characters in the token's own form: a segment another one interrupts gives a
  piece on each side, counted by `Piece::segmentPieces` as on a line that fits. The line is laid out without the pass
  when the workspace is missing, its tokens' bidi controls dropped (`withoutControls`).
- **A glyph form on a bidi line** is stored in the same region as a display form (bit 2); the section file version is
  115 for it.
- **The dictionary** reads a right-to-left line's words in reading order: a word's glued pieces are joined by their
  logical positions, a hyphen-joined word's head before its tail.

## What is not shaped yet

- **Presentation forms and ayah signs**: the code points of U+FB50-FDFF and FE70-FEFF are no run members (they are formed
  already) and draw as plain text; the end-of-ayah sign (U+06DD) and the other number signs are no run members, so they are drawn as the
  font's glyph for them, not enclosing the digits that follow; a Quran or tajweed book will show it.
- **Fonts without GPOS**: the engine runs the font's own lookups only. The fallback mark placement and legacy `kern`
  HarfBuzz gives a font without GPOS is not ported, so such a font draws its marks where its advances put them.
- **A long split word can still lose or double a ligature.** The buffer takes 32 code points of context, run and
  context together, so a word past 32 code points split by a style span goes through the piecewise loop, which reaches
  only 2 clusters. Across the cut, a font with long ligatures can then lose or double one. The shipped Naskh and Sans
  Arabic fonts have one consuming ligature, Allah (U+FDF2), and no word holding it nears 32 code points.
- **A soft hyphen inside an Arabic word cuts its joining.** U+00AD is no run member, so a run ends at it: the letter
  before it takes its final form and the letter after it its initial form (HarfBuzz joins across it, as joining type T).
  No letter is lost; the soft hyphen is a Text piece and is not drawn. Arabic text is not hyphenated, so only a book
  that carries its own soft hyphens shows it.
- **A bidi paragraph whose only Arabic code points are digits or punctuation** (a Hebrew paragraph with Arabic-Indic
  digits, U+0660-0669, in a TrueType font) turns shaping on: each such token takes a shaping call that keeps it plain. The output is the
  same; the cost is the call.
- **A consumed letter is missing from the dictionary lookup text across a style glue** (a letter its glued neighbour's
  ligature took), on the device only.
- **A no-face Reordered Indic segment split by levels draws unreordered** (the guard, not latched).
- **A text block of more than 96 tokens whose lines hold more than 96 tokens** (landscape, a span on every word of a
  400-token paragraph) holds about 16 KB more heap shaped than unshaped, all ShapedStore chunks on the heap, while the
  lent arena peaks at 34 KB of 52 KB. The test book's landscape chapter 2 then degrades on the X3, gracefully (the
  chapter is cached with the status bit set, the heap's low point 2,936 B); real prose passes in both orientations.
  Keeping the store's chunks in the lent arena's free room would remove it.
- **Titles, chapter names and the TOC**, on Home, in the Library, in the status bar and the chapter list: the UI
  faces are bitmap and cannot shape, so these draw unshaped in the face that draws them today. They wait for step 5's
  visual strings.
- **Bitmap fonts** (`.cpfont`, built-in) and a TrueType family that does not map the script: Indic text is reordered
  only (the vowel signs are placed; conjuncts are not formed, and the virama stays visible), and Arabic letters are
  drawn unjoined, each in its isolated form, as before shaping.
- **Vertical sections**: no build sets a shaped run down a column (a load refuses one), so their Indic words are
  reordered, as in a bitmap font.
- **A book whose script has no installed family**: it opens in the reader's font and the script's letters draw
  as that font's replacement glyph (a box, for a font without them) until a family of the script is installed. Arabic
  text inside a book of another language does not pick a family; it is shaped when the font in use maps beh.

## Time

Each shaped word costs a walk of the flash-mapped GSUB and GPOS, so an Indic chapter builds slower than a Latin one;
page turns cost nothing more. On the X3, with the word cache and the plan's memos, a 2,593-word chapter of Hindi prose
(Gaban) builds in 6.9 s against 2.6 s for a Latin chapter of the same length (2.6x), and the repeated-text timing book
in 4.9 s against 2.5 s (1.95x) (the
[measurements](superpowers/specs/2026-11-02-writing-modes-step2-measurements.md), "PR 3: shaped text on the page").
A word the cache misses still costs about 2.5 ms, most of it the conjunct ligature walks; speeding the engine up is
left to a later change.

Arabic misses the design's 2x line. On the X3, Jurji Zaydan's England chapter (2,529 words) builds in a median 7,880 ms
against 5,586 ms on master, which drew it unshaped, and its Latin twin in 2,489 ms: 3.17x the twin, where master was
already 2.22x. Shaping adds about 2.3 s, or 0.9 ms a word, and the word cache hits 707 of 2,527 lookups (28%).
Master's bidi-only Arabic build is already past 2x, so shaping alone cannot reach it; as for Hindi, the engine's speed
is left to a later change ([measurements](superpowers/specs/2026-11-02-writing-modes-step2-measurements.md), "PR 4b:
Arabic on the page").

## Tests

- `test/text_shaping`: `IndicShaper` (runs, pieces, context, the piecewise loop, refusals), `TextShapingMerge`,
  `IndicRuns`; `FixtureShapingFace.h` is the shared test face.
- `test/shaped_store` (`ShapedStore`), `test/word_shape_cache` (`WordShapeCache`), `test/memory_arena` (the watermark).
- `test/word_size_layout/ShapedLayoutTest.cpp`: `ShapedLayout` (widths, merges, splits, refusals, keys) and
  `ShapedLine` (pieces on the line).
- `test/text_block_range/TextBlockGlyphTest.cpp` (`TextBlockGlyphForms`), `test/ttf_font/TtfShapingTest.cpp`
  (`ShapingFacePlans` among them), `test/ot_shaper` (the plan's memos against the walk).
- `test/arabic_text` (`ArabicScripts.h`), `test/bidi_line` (cut tokens in the line pass), `test/epub_pipeline`
  `BidiRules` (the Arabic, Persian and Urdu books and `test_shaped_bidi_indic`, at 460 and 1,600 px, with their glyph
  and width rules) and `LongBlockSplit`; `test/ttf_font` `ShippedFamilies` (the Arabic families through their `CPac`).
- `test/epub_pipeline`: `Joiners`, `IndicRules`, `ShapingStatus`, and `ShapingRules` over four books
  (`test/epubs/test_shaped_{devanagari,bengali,tamil,malayalam}.epub`, from `make_test_shaped_indic.py`), with five
  goldens: one per book and `test_shaped_devanagari_unshaped`. The books' fonts are subsets of Noto Sans
  (`test/fixtures/fonts/README.md`).
- `test/hyphenation_eval` (`IndicFallback`), `test/in_place_build_gate`.
