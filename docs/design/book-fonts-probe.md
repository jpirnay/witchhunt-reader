# Book fonts: a probe across the whole spine, ranked by text, with fewer faces

Status: shipped 2026-11-01; section 8 is what was built and measured. Builds on the TTF spec's Stage B
(`docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, 11.1) and touches `BookFonts`, and, for the space after a
word in an accent family, `ParsedText::spaceSource`: no glyph store, engine or section format change.

## 1. Why

`BookFonts::prepare` decides once per book which embedded families the reader may use (Publisher Fonts): the body
family and up to `kMaxAccents` (3) accent families, then copies their faces to `<book cache>/fonts/`. On a real book it
picks the wrong ones.

*Misery* (Stephen King, Scribner EPUB 2, `.idea/Misery, a novel - Stephen King.epub`, not in the repo) embeds 8
families in 15 CFF faces (640 KB). Text per family over the whole book (Python tally, `class` rules resolved):

| Family | Used for | Text bytes | Blocks | Faces (R/B/I/BI) |
|---|---|---|---|---|
| Garamond3LTStd | body | 563,438 (91.7 %); bold 121, bold italic 6, italic 57,863 | 2,977 | 4 |
| AmericanTypewriterStd | the typed manuscript | 43,580 (7.1 %) | 207 | R |
| JamesHandscript | ~2,400 hand-filled n/e/t inside typed words (`Du<span class="fontj">n</span>thorpe`) | 3,464 | 0 (inline only) | R |
| AmericanScribe | a handwritten note, 164 KB into part03a | 1,472 | 5 | R |
| ITCGaramondStd-BkNarrow | part epigraphs, 97 % italic | 1,446 | 15 | R, I |
| GoudyReg | ebook-registration ad pages | 518 | 12 | R, B |
| TradeBio | about the author | 103 | 3 | B |
| ITCGaramondStd-BkCond | chapter-title words | 43 | 10 | R, I |
| KubisHandscript | "THE END" | 7 | 1 | R |

The real `detectFamilies` + `prepare`, run on the host (a temporary gtest, reverted), chose body Garamond3LTStd and
accents GoudyReg and ITCGaramondStd-BkCond, the third slot empty, all eight style blocks spent (4 + 2 + 2). It missed
AmericanTypewriterStd and JamesHandscript, the two faces the novel is about. Two causes:

1. **The probe reads the front matter only.** It starts at the text reference (`getSpineIndexForTextReference`; here
   the OPF guide's `type="text"` is `cover.html`, spine 0) and reads `kProbeChapters` (6) items, stopping at
   `kProbeEnoughText` (16 KB) of text (`BookFonts.cpp` `detectFamilies`): cover, ad, praise, title, dedication, fm01.
2. **Accents rank by block count** (`BodyFontProbe::accents`). A family used only inline never ranks, however much text
   it carries; a family on an ad page with 12 short blocks outranks one with 1,400 bytes of epigraphs.

And a third waste: every face a family declares is copied and installed, so a bold used for 121 bytes takes a style
block (one of the store's 8) that an accent family needed.

## 2. Goals, non-goals, anti-goals

**Goals**
- The accents are the embedded families with the most text in the book, inline uses included.
- Families with negligible text get no slot, even when one is free: each loaded family costs 2-3 KB of heap for the
  whole session (X4, spec 11.1.3).
- A style with negligible text is not copied; the engine draws it synthetically (`TtfFont::chooseSynthetic`).
- *Misery* ends with body Garamond3LTStd (regular, italic) and accents AmericanTypewriterStd, JamesHandscript,
  ITCGaramondStd-BkNarrow: 6 style blocks instead of 8, the hand-filled letters drawn in JamesHandscript.

**Non-goals**
- Raising `kMaxAccents`, `TtfFontSet::kMaxFamilies` or the store's `kMaxStyles` (the expensive lever).
- Dormant families (tables on the heap only while a page uses them).
- Finding families used only deep inside a long chapter (AmericanScribe stays unfound: the probe reads a chapter's
  head, and inflate cannot skip).
- Discovering families later, during section builds.
- Subsetting fonts.

**Anti-goals** (a failure even if it works)
- A first open more than **1 s** slower than today on the X4 for an ordinary novel (the user's limit), or unbounded
  for a book with thousands of spine items (King's Avatar: 1,732; `FirstOpenIndexingTest`'s lesson: nothing in
  proportion to the spine). If the device measurement goes over, the sample shrinks (`kProbeBudgetBytes`, then
  `kProbeMaxItems`), even if *Misery* then loses a minor accent such as ITCGaramondStd-BkNarrow: speed wins over the
  last accent (the user's call).
- More heap at reading time than today, or a heap peak during `prepare` above today's by more than a few KB.
- A changed `fonts.bin` layout, store format or section format.
- A body family that differs from today's on ordinary books. Checked by the existing body tests, new host tests of
  the weighting, *Misery* and the device books; the 921-book library comparison (TTF spec 11.1, Stage A) is not
  re-run (the user's call).

## 3. Constraints

- ESP32-C3, no PSRAM. The probe object is on the heap (`makeUniqueNoThrow`), about 1.3 KB today (as built ~2.3 KB,
  with its CSS memo); the parser state and the inflate ring live in the lent framebuffer (`BuildArena`) at first open.
- `prepare` runs once per book and `kVersion`, behind the first open's popup (`ReaderActivity.cpp`, `bookFontsPending`).
- `fonts.bin` keeps its layout: body family, 3 accent identities, names (docs/file-formats.md).
- The resolver and CSS: `css_.resolveStyle(tag, class, id)` per element as today.

## 4. Design

### 4.1 Which bytes the probe reads

- **Every spine item from 0**, not from the text reference: front matter is weighted by its size (4.2), so it no longer
  needs skipping, and the guide's text reference is not trustworthy (*Misery* points it at the cover).
- **At most `kProbeMaxItems` (64) items.** With more, an evenly spaced subset: item `floor(k * count / kProbeMaxItems)`
  for k = 0..63.
- **A per-item cap** of `clamp(kProbeBudgetBytes / sampledItems, 8 KB, 64 KB)` of inflated markup, with
  `kProbeBudgetBytes` 512 KB (as built: 160 KB and 2 KB, and the read itself stops at the cap; section 8). The cap is
  enforced as today, through the SAX parser's byte offset (`stopAtLimit`).
  *Misery*: 20 items, 25.6 KB a chapter, 141 KB read in all (four chapters capped, the rest whole).
- `kProbeChapters`, `kProbeChapterBytes` and `kProbeEnoughText` go.
- **An item that cannot be read is skipped**, logged, and the probe goes on: with 64 items from all over the spine,
  one missing entry or corrupt stream must not cost a book its fonts (today any failed read fails the examination,
  and after `kMaxAttempts` the book keeps the reader's font for good). The examination still fails, to be retried at
  the next open, when no item could be read, when the probe or the parser state cannot be allocated, and on the
  resolver's low-heap skips, as today. `AChapterThatCannotBeReadIsRetriedTwiceMore` (its one chapter missing) keeps
  its meaning: nothing was read.

### 4.2 Weighting: estimates for the whole book

Each text count from item i is multiplied by `itemBytes / readBytes` (the item's inflated size, from
`SpineEntry::zipUncompressedSize` with no ZIP lookup, over the cap or the item's size when it is read whole; factor 1
when the size is unknown), and, when the spine is subsampled, by `count / sampledItems`. The factor is known before the
item is read, so it applies as text arrives: no per-item table. Counts are estimates of the whole book's visible text
bytes (`VisibleText::visibleBytes`, as the probe counts today). Fixed point (for example 8 fractional bits) in 64-bit
accumulators, or 32-bit with saturation: the builder's call.

The body tally (`usage_`, text by paragraph family) is weighted the same way, so the body family is still the family
most of the text is set in, now over the whole spine.

### 4.3 What the probe counts

- **Body:** unchanged rule (`family()`: the paragraph family with the most text), weighted.
- **Family and style tally (new):** per (family identity, style), the weighted visible bytes of text set in that
  family and style, the text's own family (an inline span counts for its own family, unlike the body tally). Style as
  the chapter parser reads it (`ChapterHtmlSlimParser`): inherited; `b`, `strong` and `h1`-`h6` bold; `i`, `em` italic;
  a CSS `font-weight` or `font-style` the element defines overrides (`CssStyle::defined`). EpdFontFamily numbering
  (bit 0 bold, bit 1 italic).
- **Block minimum:** a block (p, div, li, dd, blockquote, pre, h1-h6, as the probe counts blocks today) whose text is in
  one family and shorter than `kBlockMinBytes` (64) adds the shortfall, weighted, to that family in the block's own
  style. A heading counts like a short paragraph, so a heading face used at every chapter start is not outranked by a
  few lines of inline text.
- The block count table (`blocks_`) goes; the family and style table replaces it. Room: the families of the `@font-face`
  records and none, as `kMaxFamilies` today (36), 4 styles each.

### 4.4 Accents

- A family's score is the sum of its four styles.
- Candidates: as today, every family but the body's, the reader's (0) and those without a drawable face
  (`pickFaces`); generics are not candidates.
- **Negligible:** a candidate whose score is below `kMinFamilyBytes` (256) **and** below `kMinFamilyShare` (0.1 %) of
  the book's estimated text gets no slot. Both, so a short book keeps its small families.
- The rest in score order, highest first, up to `kMaxAccents`; a family whose faces are all refused gives its place to
  the next, as today. The order is the rank `fonts.bin` stores, and `TtfFontSet` drops families from the end.

*Misery*, estimated from the head sample: AmericanTypewriterStd, JamesHandscript, ITCGaramondStd-BkNarrow, then
GoudyReg (above the floor, out-ranked); TradeBio negligible; ITCGaramondStd-BkCond above the floor through its block
minimum (10 blocks) but out-ranked; AmericanScribe and KubisHandscript unseen (past the cap in part03a). Simulated in
Python with these rules (scratch script, not in the repo): estimated scores Garamond3LTStd 460k, AmericanTypewriterStd
84.5k, JamesHandscript 8.1k, ITCGaramondStd-BkNarrow 1.8k, GoudyReg 650, ITCGaramondStd-BkCond 640, TradeBio 192.

### 4.5 Which faces are copied

For each chosen family (body included), from `pickFaces`' face per style:

1. The **primary** style: of the styles with a face, the one with the highest score; the regular on a tie, and the
   regular when no style with a face has text in the sample.
2. Another style with a face is copied when its score is at least `kMinStyleBytes` (256) **or** at least
   `kMinStyleShare` (1 %) of the family's score.
3. **Bold italic** is copied only when bold and italic are both copied: a bold-italic file alone stops
   `chooseSynthetic` from emboldening the regular (bold reads as present), and likewise for italic.
4. When the regular is not copied, the copied face nearest to it becomes the regular, as `pickFaces` does for a
   family without a normal face today (italic, then bold, then bold italic; moved, not shared).

The styles not copied are drawn by the engine's synthetic styles, or fall back to the regular as today.

Style scores are the same weighted text as 4.3, block minimum included, so a style used for headings keeps its face.

*Misery*: Garamond3LTStd regular and italic (bold 121 B and bold italic 6 B synthetic); ITCGaramondStd-BkNarrow
regular (362 B with its blocks' minimum) and italic (1,471 B); the one-face families as they are.

### 4.6 Version and logs

- `kVersion` 7: every book is examined again at its next open (its faces copied again once, behind the popup). Say so
  in the `kVersion` comment.
- One `LOG_INF` line for the probe: items read of the spine, bytes read, milliseconds. One per chosen family: name,
  estimated score, styles copied. One per negligible family left out. These are the device test's evidence.

## 5. Testing

**Host (`test/epub_pipeline/BookFontsTest.cpp`):**
- A family used only in a chapter after the sixth spine item is found (today's 6-item limit).
- A text reference pointing at the cover does not hide the chapters.
- An inline-only family (spans inside words in a body paragraph) ranks by its text.
- Ranking by text: a family with many short blocks of little text loses to one with more text in fewer blocks; the
  block minimum lets a family of headings across chapters outrank a few inline words.
- Weighting: a small front-matter item read whole does not outweigh a large chapter read in part.
- A long spine (more than 64 items) reads 64 items and stays under the budget (count the bytes, as `FirstOpenIndexingTest`
  counts card calls).
- Negligible: a family below both floors gets no slot with a slot free; the same family in a short book keeps its slot.
- Faces: a bold used below the floors is not copied and the family's synthetic bold draws it; bold italic dropped with
  bold; an italic-dominant family keeps its italic alone, as its regular.
- Existing tests whose fixtures declare faces without text in them (for example
  `AnEmbeddedTrueTypeBodyIsCopiedWithItsBoldFace`) get the text that justifies the face, rather than a weakened check.
- `FontFamilyLayoutTest`: a word with a mid-word span in an accent family (`Du<span>n</span>thorpe`) lays out as glued
  pieces, the span's piece in the accent family's face.
- The corpus book `test_font_families` and its goldens: if the rank order of its accents changes, the goldens change
  deliberately and the PR says why.
- Full host suite on the final tree.

**Device (X4, COM5; the X3 only if asked):** with `tools/devctl.py`. *Misery* on the card in Accents mode (Bookerly):
the log lines of 4.6 (body Garamond3LTStd R+I; accents AmericanTypewriterStd, JamesHandscript,
ITCGaramondStd-BkNarrow R+I), the probe's time at first open, screenshots of a part02a page (typed text, hand-filled
letters in JamesHandscript) and of a part title page; heap in the book against master. `families_test.epub` in each
mode as in spec 11.1.3. An ordinary novel with embedded fonts (20-60 chapters), first open against master: the probe's
logged time no more than 1 s above today's (pass/fail; over it, shrink the sample per section 2 and measure again).
A book with a large spine for the probe's time.

## 6. Left to the builder

- Accumulator width and fixed point (4.2); the exact sampling arithmetic.
- How the probe tracks style per frame and the block's own style for the shortfall.
- Constant names, and where the new constants live (`BookFonts.h` beside today's, documented as today's are).
- Refactoring `pickFaces` and the copy loop in `examine` so pruning reads plainly.

## 7. Calls made in this design (the author's, not asked)

- The probe samples heads of every item from spine 0 with a 512 KB budget and 64-item cap, as a starting point for
  the 1 s limit the user set (160 KB as built, section 8).
- Ranking by weighted text with a 64-byte block minimum (not pure text, not block counts).
- Floors: family 256 B and 0.1 % (both must fail); style 256 B or 1 % (either keeps it).
- Negligible families take no slot even when one is free (heap over fidelity, as asked).
- Bold italic only beside both bold and italic.
- An unreadable item is skipped; only nothing read, OOM or low-heap CSS skips fail the examination (4.1).

## 8. As built (2026-11-01)

Branch `feat/book-fonts-spine-probe`. Sections 4.1-4.6 are built as written, with these changes found on the devices:

- **Budget 160 KB, 2 KB minimum per item** (was 512 KB and 8 KB). The probe's time was not its bytes: on an X3 the
  first build read 270 KB in ~2.4 s and a 99 KB read took ~3.0 s. Four real books (The Anarchy, Misery, King's
  Avatar, Small Gods) found the same body and accents at every budget from 96 KB to 512 KB on the host.
- **CSS resolved as a section build resolves it.** The cost was the CSS rule lookups on the card. The probe now
  memoises the three properties it needs per tag, class and id (a 64-slot direct-mapped table; The Anarchy made 2,749
  resolves for 78 distinct element kinds), runs the resolver lean (no hot rule cache, 24 KB floor; the hot cache
  grew on the heap and pushed an X3 under the 40 KB floor, which failed every examination there), and loads the book's
  rules into the lent region before the first read (BuildArena is LIFO: in a block above the parser state's and below
  every read's, given back first). Only while each read still fits beside them: a head is read shorter to fit (down to
  2 KB), past that, or when the rules do not load, they stay on the card (the second review's finding: a read that
  left the region took up to 32 KB of an X3's ~48 KB heap).
- **A book without `@font-face` rules is probed lightly**: 8 items spread over the spine, 32 KB at most. No accent
  can be copied; only the body's generic family is wanted. The Anarchy, King's Avatar and Small Gods keep the same
  body on the host.
- **The read stops at the cap itself** (`EntryReader::open(stat, cap)`): the parser reports no progress inside a long
  text run or a `data:` attribute, so a head could be inflated whole (the final review's finding).
- **The space after a word in an accent family is the block's** (`ParsedText::spaceSource`). Misery's hand-filled
  letters end many words ("Ia‹n› Carmichael"); the space took JamesHandscript's wide space at 0.9 em and read as a
  double gap. Not visible before: those letters were never in their own face.

Measured on an X3 (Publisher Fonts Accents, Bookerly 14 pt, the first-open window between "Lent secondary framebuffer
for first-open indexing" and "Returned ..."), master dd986fffc against the branch:

| Book | master | branch | probe |
|------|--------|--------|-------|
| The Anarchy (40 items, no `@font-face`) | 1,824 / 1,823 ms | 1,631 / 1,620 ms (-0.2 s) | 235 ms, 8 items, 19,088 B |
| Misery (20 items, 8 families) | 3,918 ms | 3,476 ms (-0.44 s) | 513 ms, 20 items, 64,761 B |

Before the light probe for books without faces, The Anarchy read all 40 heads (98,845 B) in 1,048 ms: 2,473 /
2,461 ms (+0.64 s). The X4 was measured before the speed-ups only (master 1,047 / 1,031 ms, the first branch build
2,774 / 2,797 ms; its probe ~140 KB/s); it was not available afterwards. The remaining probe cost is ~25 ms a spine
item (opening the entry, setting up the inflate), not bytes.

Misery on the X3: `Probe: 20 of 20 spine items, 64761 bytes, 513 ms`, `Left out TradeBio: 192 bytes of text (est.)`,
body Garamond3LTStd (styles 0x7), accents AmericanTypewriterStd (0x1), JamesHandscript (0x1),
ITCGaramondStd-BkNarrow (0x5: regular and italic); master chose GoudyReg and ITCGaramondStd-BkCond. The typed
manuscript (part02a) draws in AmericanTypewriterStd with the hand-filled n's in JamesHandscript inside their words;
~48 KB free in the book.
