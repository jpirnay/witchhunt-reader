# Memory and parsing

Status: checked against master on 2026-11-19 (5d402e9c5), after the first wave of memory-and-parsing PRs (#829, #830,
#832-#838). Every item is still open, the font prewarm still partly done. This pass moved the `file:line` references to
the current code, dropped the simplified-chapter hint item (decided against), and added two: the page-elements limit and
a `<br>` in a degraded table cell. Items as of 2026-10-07; those from "Throwing allocations" on added 2026-11-15. Collected
from the heap-work handover (2026-08), the memory audit (2026-09) and the earlier memory-allocation-strategy.md (removed or
trimmed in the 2026-10-07 docs cleanup; in git history).

The rules and measurements these items refer to are in [Memory Allocation Strategy](../memory-allocation-strategy.md).

## The paragraph after a table loses its margin-top

**Open.** The paragraph after a table collapses its margin-top against the margin-bottom of the paragraph before the
table, as if the table were not there. In `test_table_streaming.epub` spine 3 (`p { margin: 1em 0 }`, 18 px), "Now a
table with more columns than the grid layout supports." sits at y=414, one line under the last cell of the degraded
rowspan table above it (y=390), and "Closing text after the tables." at y=395, the bottom of the grid fragment above it
(`TABLE y=324 h=71`; `test/epub_pipeline/goldens/test_table_streaming.golden.txt`). The paragraphs after the nested
tables of `test_nested_tables.epub` do the same (FIRSTAFTER at y=344 under the last cell at y=320).

**Why it matters.** A table runs into the text that follows it, against the book's CSS. Nothing is lost; it is spacing.

**Where.** `applyBlockTopSpacing` takes `collapse = min(lastBlockMarginBottom, marginTop)`
(`ChapterHtmlSlimParser.cpp:6136`), and only `makePages` (`:6094-6096`), `applyBlockTopSpacing` itself (`:6143`),
`emitPage` (`:1862`) and the box code (`:1944`, `:2014`) write `lastBlockMarginBottom`. A table writes nothing there: a
grid fragment is placed by `flushTableFragment`, and a degraded cell by `emitCellAsParagraph` (`:7131`), which hands
the cell's lines to `addLineToPage` (`:7164`) without `makePages`. So the paragraph laid out before the table still
holds `lastBlockMarginBottom` when the one after it opens.

**Next step.** Reset `lastBlockMarginBottom` once a table fragment or a degraded cell is placed, and regenerate the
goldens that hold a paragraph after a table (`test_table_streaming`, `test_nested_tables`, `test_tables`,
`test_table_grid_edges`, `test_table_cell_overflow`), checking that each moved line is the paragraph's own margin.

## A table cell keeps only its first picture

**Open.** A cell holding more than one `<img>` shows the first and drops the rest, silently, whether the table is a
grid or laid out as paragraphs. Probe (2026-11-19, the host dump tool, master 715896630 and this branch alike): a grid
cell `GRIDA <img checker/> GRIDMID <img bar/> GRIDEND` dumps all three words and one `CELLIMG` (the checker), and the
same cell in a table a rowspan degrades dumps its words and one block `IMG` (the checker); the bar is in neither.
Cells of a nested table, and a cell's text after its inner table, are the exception: they stream the cell so far at a
second picture and go on holding the next.

**Why it matters.** It drops content without a word: an illustrated table (a plate with its legend, a row of icons)
loses every picture but the first in each cell.

**Where.** The in-cell branch of the image handler in `ChapterHtmlSlimParser::startElement` keeps one `src` per cell
("first image per cell wins", `ChapterHtmlSlimParser.cpp:3384`) and returns for the others unless the cell is
`streamed`. A grid row sizes the cell for that one picture (`layoutTableRow`, `cell.image = buildCellImage(...)`,
`:7057`); a degraded cell places it after its text (`emitCellAsParagraph`, `:7174`, or `emitRowImagesAsBlocks`,
`:6976`).

**Next step.** In its own PR, place every picture of a cell. A degraded cell can stream the cell so far and go on, as
a nested table's cell does now; a grid cell needs a decision about how a second picture fits its row's height (stack
them, or degrade the row).

## The page-elements limit drops the tail of a page

**Open.** A page holds at most `Page::MAX_ELEMENTS` elements (1,024, `Page.h:228`). `Page::serialize` writes the first
1,024 (`Page.cpp:647`), and a page that holds more is cut: `Section::onPageComplete` logs "tail dropped" and marks the
section simplified (`Section.cpp:730-735`), so the rest of the page is lost. The page is not ended at the limit during
the build, and `addPageElement` (`ChapterHtmlSlimParser.cpp:2091`) has no such check. It takes 1,025 elements on one
page, which needs elements without height (a real page is about 28 lines plus images and rules), and no book is known to
reach it. (`kCapPageElements`, `ChapterHtmlSlimParser.h:1481`, is declared and nothing sets it; the section marks itself.)

**Why it matters.** Content after the 1,024th element of a page vanishes, with a log line only. The limit exists so that a
page that cannot be loaded (`Page.cpp:687`) is never built.

**Where.** `Page::MAX_ELEMENTS` (`Page.h:228`); the write path `Page::serialize` (`Page.cpp:647`) and
`Section::onPageComplete` (`Section.cpp:730-735`); the single add site `ChapterHtmlSlimParser::addPageElement`
(`ChapterHtmlSlimParser.cpp:2091`) and `emitPage` (`:1814`).

**Next step.** End the page at the limit: an `emitPage()` from `addPageElement` when the page holds `MAX_ELEMENTS`. Not a
hint (decided 2026-11-18: no hint for a chapter a fixed limit simplified; 2026-11-19: the truncated-chapter hint stays
untranslated English). Only with a fixture that reaches the limit, which no book does today.

## A `<br>` inside a degraded table cell does not break the line

**Open.** A degraded (streamed) cell's text is one cell paragraph (`makeCellText`, `ChapterHtmlSlimParser.cpp:6921`), and
the `<br>` branch in `ChapterHtmlSlimParser::startElement` (`:4143`) only flushes the word into it (`:4158-4160`), so the
cell's words after a `<br>` continue on the same line. `test/epub_pipeline/goldens/test_br_in_blocks.golden.txt` case 21
("a run of `<br>` in a nested table's cell", `:362-394`) lays "Uniform a / Uniform b / Uniform c" on one line at the
cell's inset (`LINE y=386`). Degraded outer cells behaved this way on master before the first wave; nested inner cells
now match them.

**Why it matters.** A cell whose lines the book breaks with `<br>` runs together on one line, against the
book's markup. Nothing is lost.

**Where.** `ChapterHtmlSlimParser::startElement`, the `<br>` branch (`ChapterHtmlSlimParser.cpp:4143-4188`);
`makeCellText` (`:6921`); `emitCellAsParagraph` (`:7131`), which lays the cell's `ParsedText` out as one paragraph.

**Next step.** Have a `<br>` inside a cell end the cell paragraph's line: a forced line break in the cell's
`ParsedText`. Then check every table golden (`test_br_in_blocks`, `test_table_streaming`, `test_nested_tables`,
`test_tables`, `test_table_grid_edges`, `test_table_cell_overflow`).

## The font prewarm sets the session's heap minimum

**Partly done (#515, 2026-10-19).** Re-measured on the X3 (Build Through test book, `[MEM]` and per-phase lows of `renderContents`): the prewarm no longer sets the session's minimum. With the image decodes out of Background-B's way (#515, [the image lane](../background-rendering.md#image-lane--pixel-caches-ahead-of-the-reader)) the low point of a session sat at ~33 KB, inside B's build, and a normal render's phases stayed within 1-2 KB of their start:

| Page render, X3 | start | prewarm low | BW render low |
|---|---|---|---|
| TrueType body (BookerlyTTF), glyphs in the store | 50.5 KB | 48.6 KB | 50.3 KB |
| the same, first page after the glyph store was installed again, before | 50.3 KB | 38.7 KB | 39.9 KB |
| the same, after | 50.5 KB | 48.7 KB | 50.3 KB |
| built-in Bookerly | 53.0 KB | 45.0 KB | 49.4 KB |

What changed: a render lends the secondary buffer to the prewarm's TrueType misses as it lends it to an image decode (the glyph store has no glyph yet: a 10.5-14.5 KB scratch block, in SD mode a 4.2 KB page cache, each taken from the lent region and given back per miss), and whatever a miss still made on the heap is freed before the BW render instead of when RenderLock goes back (`renderContents`, `EpubReaderActivity.cpp:6652`; the lend at `:6934`, the release at `:6940`).

**Still open.** Two transients remain, neither the session's minimum on these measurements:
- The built-in fonts' prewarm: 8 KB, of which the page slots (3.5 KB) stay for the BW render and the AA pass, and the
  rest is the group stream ring, already freed per group. **Done since (#519, 2026-10-19):** `renderContents` lends the
  buffer to every prewarm, not only with a TrueType set, and `FontDecompressor` takes a ring from its slot arena, else
  from the region lent to the glyph misses (`glyph_miss::Scope`; `ringArena`, `FontDecompressor.cpp:69`), giving it back
  before the next group; the slots stay on the heap. On the host the prewarm's heap peak loses its largest ring (3,036 B
  for a Noto Sans 14 sample; up to 4,096 B, `GROUP_RING_MAX_BYTES` in `lib/EpdFont/scripts/fontconvert.py:937`). On the
  X3 (#519's test) the prewarm's free low median went from 43,434 to 45,832 B. The pre-render still cannot lend the
  buffer (it holds the page on screen), so most turns, which show a pre-rendered page, are unchanged. Since 2.50
  (029416411) a C3 page that goes out composed one-pass (X3, X4, One-Pass Anti-Aliasing on, FAST, text only) does not
  lend either (`prewarmKeepsFrameOnScreen`, `EpubReaderActivity.cpp:6913-6914`): the buffer holds the frame on screen
  the one-pass needs, so that page's prewarm is back on the heap (the 43,434 B median for the ring; a TrueType miss's
  10.5-14.5 KB until `releaseTtfMissMemory()` frees it before the BW render). A page whose glyphs starved before
  (`glyphStarvedPage`) still lends, and the S3 boards lend a PSRAM arena and keep the frame either way.
- A pre-render with TrueType misses (15-26 KB on the X3, `PreRenderGate.h`) cannot lend for the same reason
  (`renderPreRenderPass`, `EpubReaderActivity.cpp:5021`). Its heap floor already asks for that cost (`:1623`).

**Next step.** None until a session's minimum is found in a render again. The per-phase lows came from a measurement-only patch (`heap_caps_monitor_local_minimum_free_size_start` restarted at each phase boundary of `renderContents`); a permanent version would cost the same monitor B's borrow uses.

## A page turn waits behind an image extraction

**Open.** A lane decode that a page turn preempts parks if it is a JPEG: its caches keep their finished rows in `.part` files, a checkpoint (`RenderConfig::checkpointPath`) holds the decoder state, and the next warm resumes it. A resume decodes one band (progressive) or `kResumeMinRows` (4) MCU rows (baseline) before it polls for input, so it cannot park again where it started. The first warm of an image extracts it from the EPUB first, and that extraction is not interrupted (1.6 s for a 514 KB JPEG on the X3, 2026-09-27). It loses no work: `ensureExtracted` (`ImageBlock.cpp`) passes `Epub::extractItemToFile` no stop callback, so it runs to its end and renames its `.part` file into place. What the reader pays is latency: the lane holds the `RenderLock`, so a page turn waits for what is left of the extraction. PNG images and a progressive JPEG's 1/8 decode resized to its box restart from the beginning after a preemption; a PNG decode's state is its inflate ring (at most 32 KB) and two scanline buffers (`PNG_ARENA_WORST_CASE_BYTES = 32 * 1024 + 2 * 4096`, `PngToFramebufferConverter.cpp:129`; a small image needs less, `:135-136`). Parking it would mean writing that state to the card, as the JPEG park writes its own; the PNG park does not exist, so that cost is an estimate from the JPEG's, and PNG parking is not planned. The resized decode is the next section's (a cover that fits only at 1/8).

**Why it matters.** At a normal reading pace the lane lost large images to page turns again and again before parking existed: one page was started six times while the reader turned a page every 2 s.

**Where.** `EpubReaderActivity::stepImageWarmLocked` and `warmPageForImageLane` (`EpubReaderActivity.cpp`; the lane's per-attempt `LOG_INF`); the park and resume code in `JpegToFramebufferConverter.cpp`; `ImageBlock.cpp`, which gives only a JPEG a checkpoint path; `Epub::extractItemToFile` (`Epub.cpp`), which does not poll for input.

**Next step (a device measurement).** On the X3, in a book of large JPEGs, read at one page every 2 s for 50 pages, and count the page turns that waited more than 500 ms behind an extraction, from the lane's per-attempt `LOG_INF` lines. If more than one turn in ten did, stop the extraction at the next inflate chunk and keep it for later: the `.part` file stays, and the state to keep is the ring (up to 32 KB) and `uzlib_uncomp` (about 1.3 KB). The sizes are estimates from the inflate ring's 32 KB window and `uzlib_uncomp`, not measured here. If the state has nowhere to live between slices on the C3 but the card (about 33 KB written per park by that estimate), the park may cost about what finishing costs, in which case the answer there is no; where `HalCapabilities::buildsInPsram()` is true the state could stay in PSRAM. Measure both before choosing.

## A cover that fits only at 1/8 restarts on every key press

**Open.** When a cover's progressive JPEG fits in memory only at 1/8, `JpegThumbSession` refuses it and the one-shot `convertResized` draws it. That conversion is not sliced: a key press aborts it (`CooperativeAbort`), and the next attempt starts again from byte 0. Before `ProgressiveJpegDc` was retired, this case was sliced; a cover that fits at no scale was one-shot before too.

**Why it matters.** Input stays responsive, but a large cover appears only once the reader pauses for one whole conversion. Small covers still finish between key presses.

**Where.** `JpegToBmpConverter.cpp`: `convertResized` (`:369`, its abort poll `progressiveFullShouldAbort` at `:284`),
`JpegThumbSession::begin` (`:1095`), which returns no session when `chooseProgressiveShift` (`:785`) leaves the cover to
the resize (`:1111`); `ProgressiveJpeg::decodeResized` (`ProgressiveJpeg.cpp:1525`).

**Next step.** Make the resized decode steppable in `ProgressiveJpeg`: its resampler state in the session, about 60 lines. Then give `JpegThumbSession` a third mode, with the per-output setup shared with the other two, about 120-150 lines with tests.

## A cut or corrupt progressive JPEG draws a placeholder

**Open.** A progressive JPEG whose stream is cut short or corrupt fails as soon as a scan's data does not decode, and the reader draws its framed placeholder (a cover is not converted). Work that changes this is written and tested but parked: the decoder stops a scan where its data is unusable and decodes every row from the scans that remain (a DC scan's blocks after the damage mid grey, as libjpeg leaves them), so a cut or corrupt progressive file draws what it holds, and a cut baseline file keeps its rows.

**Done (8a).** A baseline JPEG that fails to decode draws the same framed placeholder a progressive one does (it drew nothing). `ImageBlock::render` remembers, per reader session, the pictures whose decode failed for the file's own fault (`DecodeFailures`, 8 x 4 B hashes of the path in `EpubReaderActivity`, cleared on exit, nothing on the card) and draws their frame without decoding again; a render pass no longer decodes a cut file eight times (X4: about 14 s a visit). Only a decode error counts: a progressive `InvalidData` or `Unsupported`, a baseline format error, or a short read at the end of the file; a card error, a refusal for memory and a preemption by input do not. What stays parked is drawing what a damaged file holds, and caching it.

**Why the rest is parked.** Nobody has reported a damaged JPEG; the cut extracts it was written for came from the extractor itself (fixed in `Epub::extractItemToFile`). And caching damage makes a false detection permanent: a valid file that trips one of the new rules (the strict RSTn sequence, the padding overrun, a TJpgDec format error on an unusual baseline file) would be kept with grey or white areas for good, since `PixelCache::PXC_MAGIC` is not tied to the rules and cover BMPs are never invalidated.

**Where.** `lib/ProgressiveJpeg` (`BitReader`, `ProgressiveJpeg.cpp:667`; `decodeBand`, `:965`; `indexStep`, `:575`);
`JpegToFramebufferConverter::decodeToFramebuffer` (`JpegToFramebufferConverter.cpp:1521`); `JpegToBmpConverter.cpp`
(`SinkSet`, `:246`; `JpegThumbSession::continueSteps`, `:1154`); `PixelCache::PXC_MAGIC` (`PixelCache.h:93`).

**Tried.** The code and its tests (`test/epub_pipeline/JpegPartialDecodeTest.cpp`, the `prog_420_cut.jpg` fixture) are on the branch `archive/progressive-jpeg` (b623ca5a1). Its review found that once degraded pictures were cached, `Result::Degraded` behaved exactly like `Ok` everywhere, so the completeness tracking that told a file lacking only its EOI from a cut one bought nothing.

**Next step.** If it is ever taken, ship it decoder-only: fold `Degraded` into `Ok` with a `DecodeStats` flag for the logs, drop that completeness tracking, and leave the converters as they are -- they already cache `Ok`, and an I/O error keeps the placeholder. Bump the checkpoint magic with the `Cursor` layout it changes, and note in `PixelCache.h` that a change to the damage rules must bump `PXC_MAGIC`.

## Per-line and per-cell objects are still on the heap

**Open.** Each laid-out line still allocates a `TextBlock` object and a `PageLine` on the heap (88 B and 24 B on the 64-bit host), and each grid cell its own objects (about 160 B a cell, 11–15 KB for a page of grid rows, host). Their word bytes already come from the page block or the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line, `ParsedText.cpp:4405`), `ChapterHtmlSlimParser` (the
`PageLine` per line in `addLineToPage`, `ChapterHtmlSlimParser.cpp:5836`; a grid cell's lines in `layoutTableRow`,
`:7027`, kept at `:7034`), `TableCell::lines` (`Page.h:146`), `Page::elements`
(`std::vector<std::unique_ptr<PageElement>>`, `Page.h:208`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer ([Memory Allocation Strategy §8.4a](../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving the line bytes into the arena is done.

**Done (9a).** A grid cell's lines are collected in one parser member (`cellLineScratch_`, reserved to `MAX_CELL_LINES`
pointers at the table's first grid row, freed at `</table>`) and moved into `cell.lines` allocated at its exact size, so a
cell's line list is one allocation. A refusal of either sends the row out as paragraphs. Open: 9b (the `TextBlock` object in
the line arena) and 9c (the `PageLine` in the page block), which wait for a Linux `epub_pipeline_dump --bench` allocation
count.

**Next step (9b, 9c).** Placement-construct the objects in the page or row block, with an arena-aware deleter behind
`Page::elements`: 41 lines in `lib/Epub` and `src` use it, 15 of them add an element (the parser's nine adds, all
through `addPageElement`, and one `insert`, and the five of `Page::deserialize`, `Page.cpp:705-721`). Measure
allocation counts and `HeapPeakRegression` on the host first, then a table chapter on the X3 with `SCT_HEAP_TRACE=1`.
Budget for the image-header walk: on the X4 a 32 KB walk stage leaves 64 B of the lent region free, so objects of a
page that holds a late image could push the walk out of the arena.

## Throwing allocations left on the section-build path

**Open.** 2.52 sent every `Page` allocation through `ensurePage()` (`ChapterHtmlSlimParser.cpp:2073`), which stops the
build like the other heap refusals instead of aborting. These still allocate through the throwing `operator new`, so a
refusal there aborts and reboots (`-fno-exceptions`); counted in `ChapterHtmlSlimParser.cpp`:

- a table's packed rows, `t.packer.rows.push_back` (`:6320`, `:6330`; at most `MAX_TABLE_ROWS`, 48, per fragment);
- a row's cells: `pendingRow.cells` (`:3327`, `:6905`; the streamed nested cell's own `emplace_back`, `:6954`, asks
  `heapReserveFor` first), a laid-out row's `out.cells.reserve(columnCount)` (`:6995`; the pushes at `:7080` and `:7085` stay within it),
  a grid cell's lines are done (9a: reserved through `heapReserveFor`);
- `std::string` growth during the parse, for example the ruby text (`rubyText_`, `:4577-4581`, up to 255 bytes).

**Next step.** Ask `HeapFit` before each growth (`heapReserveFor`, as the elements reserve does) and stop the build on
a refusal (`stopForHeap`, `:2112`), or move the table to the build's arena or a `ChunkedVector`. Each site needs a host
fault-injection test, as `PageHeapTest.cpp` (`PAGE_FAULT_INJECTION`, `ELEMENT_FAULT_INJECTION`) does for pages and for
`Page::elements`, which `addPageElement` now covers, and `StackHeapTest.cpp` (`STACK_FAULT_INJECTION`) for the
per-element stacks, which `pushOrStop` now covers.

## CSS in a chapter's `<style>` blocks is never applied (matcha-reader #627)

**Open.** The parser skips `<head>` whole (`SKIP_TAGS`, `ChapterHtmlSlimParser.cpp:332`), `isNonVisibleTag()` counts a
`<style>` as invisible text (`lib/Epub/Epub/VisibleText.h:33`), and nothing parses its rules: only linked stylesheets
and `style=""` attributes reach the layout. A book whose only CSS is a `<style>` block in each chapter lays out
unstyled. matcha copies the block to a temp file (max 16 KB) and loads it like a linked sheet. First confirm with such
an EPUB. Size M, with a `SECTION_FILE_VERSION` bump.

## Every manifest stylesheet applies to every chapter (matcha-reader #453)

**Done (13a and 13b); what is left is below. Next in this area: the `<style>` blocks (the item above).** A chapter
resolves against the stylesheets its head links and the sheets those `@import`, as a browser does, not against every
sheet of the book: the rules of a sheet that no text chapter links (matcha's `vertical_image.css`) reached every
chapter, and an omnibus edition's part sheets with the same class names each other's. matcha-reader #453, Eszter
Schuffert (eszter007).

- **13a (`CSS_CACHE_VERSION` 36).** The compile records, at the end of `css_rules.cache`, each sheet's range of rule
  orders and its `@import` edges (`CssParser::sheetRanges`, `sheetImports`; layout in
  [file-formats.md](../file-formats.md#css_rulescache-css_bgbin-and-css_fontsbin)). A redeclaration no longer merges
  across a sheet boundary.
- **13b (`SECTION_FILE_VERSION` 128).** `Section::scopeStylesheets` reads the chapter's head from `html_<spine>.bin`
  before the parser's setup, through the scanner the heading carry already has (`HeadingCarry::Scanner`'s link hook,
  `HeadingCarry::scanHead`: up to `<body>`, at most 8 KB, a 256 B window and the links' values in a 256 B buffer, on
  the stack); a carried heading file's links come from the scan `Section::prepareCarry` makes of it.
  `CssParser::addLink` resolves each href, `CssParser::scopeToSheets` follows the import edges and keeps one bit a
  sheet, and `CssParser::Cascade::apply` skips a rule whose sheet is out of scope, for every store and the complex
  rules alike. The table is loaded with the index, on the heap (16 B to 1.5 KB; a build's lent region has no room for a
  block that lives the whole build), and written last: a cache that ends in no valid table is deleted and compiled
  again. 128 imports now (64 before), and an `@import` after `@namespace` no longer counts.
- **Unscoped, as before.** A chapter that links no sheet; a link or an import the table does not know (a path spelt
  another way, an import of a file the book does not have: nothing tells it from a sheet the reader failed to find);
  a head past 8 KB or not read whole; a `rel` or `href` not quoted or past 256 B; a link to a sheet past the 64th (its
  range is the fold, which has no path); a chapter that links every sheet. A build logs how many of the book's sheets
  it resolves against. `BookFonts` and `BodySize` resolve against every sheet by design.
- **Not seen, so the sheet drops out of a scoped chapter** (the only ways a linked sheet is lost): a stylesheet
  `<link>` in the body; an `@import` inside a `<style>` block in the head; a sheet named by an `<?xml-stylesheet?>`
  instruction beside `<link>`s. A prefixed `<h:link>` is seen (the scanner reads local names).
- **Open.** The background pictures (`css_bg.bin`) and the `@font-face` rules (`css_fonts.bin`) are not ranged and stay
  book-wide: a picture or a face from a sheet a chapter does not link still reaches it. Size S each, if a book shows it.

## Inline ids and footnote links drift with hyphenation

**Open.** An inline id waits with the index of the next word of its block, counted as the parser adds tokens
(`inlineAnchorWordIndex`, `ChapterHtmlSlimParser.cpp:5380`), and is recorded by the line whose running count of tokens
(`wordsExtractedInBlock`, `:5858`) reaches it. A word that hyphenation splits is one token to the parser and two in the
lines: the remainder becomes a token of its own (`ParsedText::hyphenateWordAtIndex`, `ParsedText.cpp:3157`, `:3190`),
which `extractLine` counts (`:3867-3870`). The lines' count runs a token ahead per split, so an id is recorded up to
that many tokens early, never late; within one layout window only, since `lineTokensSoFar` counts the splits already
made when the next id is queued. The bound is in tokens: under a page at 480x800, possibly more on a short screen
(Murphy, 240 px). Measured in `AnchorMapTest` (`test_inline_anchors.epub`, chapter 4: an id before every fourth marker
word of a paragraph of long words, 40 ids, 8 to a 96-token window, under the queue's 16): hyphenation off, 0 of 40 ids
off their word's page (`InlineIdsNameTheirWordsPageWithHyphenationOff`); on, 84 words split and 2 of 40 ids name the
page before their word's, none a later one or further (`InlineIdsHoldWithHyphenationOn`). (A first measure, 56 of 159,
put an id before every third token: past the 16 the queue holds per window, ids are recorded at the page reached so
far, and that cap, not hyphenation, made most of it.) A footnote link counts its word the same way (`:5140`) and
drifts alike (pre-existing).

Next step, a follow-up PR: `TokenBoundary::Boundary` gains `bool splitRest : 1` in the bitfield byte
`noSpaceBefore`/`keptSpaces` leave spare (`TokenBoundary.h:54-57`; no size change, lines do not serialize it), set on
the remainder's boundary in `hyphenateWordAtIndex` (`ParsedText.cpp:3190`), subtracted in `sourceTokenCount` (`:1172`)
and in `extractLine`'s `lineTokens` loop (`:3869`). A remainder never continues a group, so `mergeGluedGroups` keeps its
flag; a remainder split again keeps it and the new remainder gets one; the retry that joins the parts erases it. It
moves footnote links in hyphenated layouts too (the `footnotes=` lines of the `_hyph` goldens may change). Then
tighten `InlineIdsHoldWithHyphenationOn` to the word's page; until then it allows one page, the link Back stack's
tolerance (`EpubLinkBackStack.h:73-76`).

Also approximate, recorded only: an id between the images of an image-only paragraph whose images cross pages
(`<p><img/><a id/><img/><img/></p>`) names the last image's page, not the next image's: `placeImageOnlyParagraph`
records the waiting ids once all its images are placed (`:6805`). The one case now recorded late (master put it
later still, at the next block's lines); recording per placed image would be exact, worth it only if a book shows it.

## JPEG source reads of 512 bytes (matcha-reader #245)

**Open.** TJpgDec reads its input through `tjpgBmpInput` for covers
(`lib/JpegToBmpConverter/JpegToBmpConverter.cpp:57-66`) and `tjpgInput` in the reader
(`lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp:364-373`), one `HalFile` read of `JD_SZBUF` (512 B,
`lib/TJpgDec/src/tjpgdcnf.h:15`) per call. matcha measured about 2 ms fixed per read and cut a cold decode by 22 % with
an 8 KB read-ahead. The SDK's sector read-ahead (freeink-sdk cf63426ef, pinned in ec93b64fb) serves the SDMMC boards
only (`SdmmcBlockDevice`); the X3 and X4 read over SPI without it. Profile on the X3 and X4 first (#220 found the same
per-call cost on writes). matcha-reader #245, Eszter Schuffert (eszter007).
