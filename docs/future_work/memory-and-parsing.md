# Memory and parsing

Status: checked against master on 2026-11-18 (5d903ca18): every item is still open, the font prewarm still partly
done. This pass moved the `file:line` references to the current code, counted the throwing allocations again, and corrected three descriptions: a nested-table
fixture already exists, a one-pass page's prewarm went back on the heap in 2.50 (not 2.41), and the libjpeg-script item's refills happen only without read
windows. Items as of 2026-10-07; those from "Throwing allocations" on added 2026-11-15. Collected from the heap-work
handover (2026-08), the memory audit (2026-09) and the earlier memory-allocation-strategy.md (removed or trimmed in the
2026-10-07 docs cleanup; in git history).

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
(`ChapterHtmlSlimParser.cpp:6055`), and only `makePages` (`:6013-6015`), `applyBlockTopSpacing` itself (`:6062`),
`emitPage` (`:1862`) and the box code (`:1944`, `:2014`) write `lastBlockMarginBottom`. A table writes nothing there: a
grid fragment is placed by `flushTableFragment`, and a degraded cell by `emitCellAsParagraph` (`:7051`), which hands
the cell's lines to `addLineToPage` (`:7081`) without `makePages`. So the paragraph laid out before the table still
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
("first image per cell wins", `ChapterHtmlSlimParser.cpp:3353`) and returns for the others unless the cell is
`streamed`. A grid row sizes the cell for that one picture (`layoutTableRow`, `cell.image = buildCellImage(...)`,
`:6977`); a degraded cell places it after its text (`emitCellAsParagraph`, `:7093`, or `emitRowImagesAsBlocks`,
`:6889`).

**Next step.** In its own PR, place every picture of a cell. A degraded cell can stream the cell so far and go on, as
a nested table's cell does now; a grid cell needs a decision about how a second picture fits its row's height (stack
them, or degrade the row).

## A `<br>` that ends a block adds a blank line

**Open.** `<p>Alpha<br/></p><p>Bravo</p>` lays out Bravo a whole line below Alpha (y=48 with the host stub, against 24
without the `<br>`), whether or not the `<br>` is styled `display: block`. A browser renders no line for a line break
that ends a block's content.

**Why it matters.** Calibre ends every entry of a converted contents page with `<br class="calibre1"/>` (and styles it
`display: block`): Hayakawa's 『七王国の玉座』, seen on the X3 on 2026-10-26, sets each contents entry as a line and a
blank line, about twice the pages it needs.

**Where.** The `<br>` branch of `ChapterHtmlSlimParser::startElement` (`ChapterHtmlSlimParser.cpp:4058-4090`) leaves an
empty block marked `fromBrElement`, and `startNewTextBlock` adds a line height to the next block's top margin for it
(`:2741-2748`); a block that gets text first loses the mark. `test_br_in_blocks.epub` and `test_br_section_break.epub`
pin what `<br>` does today, so a change has to keep their deliberate cases (a run of `<br>` as a scene break) apart from
a single trailing one. Case 5 of `test_br_in_blocks` ("TRAILING BR", `<h2>Echo heading<br/></h2>`) is this case: its
golden puts the paragraph 58 px below the heading line (y=140 to 198), against 34 px after case 2's heading without a
`<br>` (y=192 to 226). The extra 24 px is the gap case 6 gets on purpose from a `<br/>` of its own between a heading and
its paragraph.

**Next step.** Drop the empty line a single `<br>` leaves at the end of a block, check both goldens (case 5 of
`test_br_in_blocks` changes, case 6 must not), and compare a calibre book's page count before and after.

## No hint when a chapter was simplified

**Open.** When a fixed capacity changes what a chapter shows, `ChapterHtmlSlimParser::noteCapOverflow`
(`ChapterHtmlSlimParser.cpp:2126`) logs it once per limit and the section stores `kStatusSimplified`
(`Section.cpp:257`). There are seven such limits (`ChapterHtmlSlimParser.h:1426-1432`): footnote links per page,
anchors per chapter, page elements, printed-page labels, footnote href length, nesting depth and pages per section; the
footnote, anchor and label limits also trip when the heap has no block to grow their tables. The reader only logs
"cached simplified" on a cache hit (`EpubReaderActivity.cpp:5709-5711`); the reader sees less than the book contains
and is not told. Pages per section is the exception: it also stops the parse with the truncated status
(`ChapterHtmlSlimParser.cpp:1852-1858`), which shows the truncated-chapter hint.

**Why it matters.** The cap is deterministic, so a rebuild cannot help; a hint is the only way to tell the reader. It is
a product decision, not a memory one.

**Where.** `Section::isSimplified` (`Section.h:433`); the truncated-chapter hint is the model to reuse
(`truncatedSectionHintRendersRemaining`, armed at `EpubReaderActivity.cpp:6052-6056`, drawn at `:7025-7028`). Its
strings (`TRUNCATED_SECTION_HINT_LINE_1/2`, `:355-356`) are hard-coded English and would need `tr()` keys.

**Next step.** Decide whether the hint is wanted. If it is, show it the way the truncated hint is shown, with translated
strings.

## The font prewarm sets the session's heap minimum

**Partly done (#515, 2026-10-19).** Re-measured on the X3 (Build Through test book, `[MEM]` and per-phase lows of `renderContents`): the prewarm no longer sets the session's minimum. With the image decodes out of Background-B's way (#515, [the image lane](../background-rendering.md#image-lane--pixel-caches-ahead-of-the-reader)) the low point of a session sat at ~33 KB, inside B's build, and a normal render's phases stayed within 1-2 KB of their start:

| Page render, X3 | start | prewarm low | BW render low |
|---|---|---|---|
| TrueType body (BookerlyTTF), glyphs in the store | 50.5 KB | 48.6 KB | 50.3 KB |
| the same, first page after the glyph store was installed again, before | 50.3 KB | 38.7 KB | 39.9 KB |
| the same, after | 50.5 KB | 48.7 KB | 50.3 KB |
| built-in Bookerly | 53.0 KB | 45.0 KB | 49.4 KB |

What changed: a render lends the secondary buffer to the prewarm's TrueType misses as it lends it to an image decode (the glyph store has no glyph yet: a 10.5-14.5 KB scratch block, in SD mode a 4.2 KB page cache, each taken from the lent region and given back per miss), and whatever a miss still made on the heap is freed before the BW render instead of when RenderLock goes back (`renderContents`, `EpubReaderActivity.cpp:6649`; the lend at `:6931`, the release at `:6937`).

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
  lend either (`prewarmKeepsFrameOnScreen`, `EpubReaderActivity.cpp:6910-6911`): the buffer holds the frame on screen
  the one-pass needs, so that page's prewarm is back on the heap (the 43,434 B median for the ring; a TrueType miss's
  10.5-14.5 KB until `releaseTtfMissMemory()` frees it before the BW render). A page whose glyphs starved before
  (`glyphStarvedPage`) still lends, and the S3 boards lend a PSRAM arena and keep the frame either way.
- A pre-render with TrueType misses (15-26 KB on the X3, `PreRenderGate.h`) cannot lend for the same reason
  (`renderPreRenderPass`, `EpubReaderActivity.cpp:5018`). Its heap floor already asks for that cost (`:1620`).

**Next step.** None until a session's minimum is found in a render again. The per-phase lows came from a measurement-only patch (`heap_caps_monitor_local_minimum_free_size_start` restarted at each phase boundary of `renderContents`); a permanent version would cost the same monitor B's borrow uses.

## Preempted image decodes still lose work in some cases

**Open.** A lane decode that a page turn preempts parks if it is a JPEG: its caches keep their finished rows in `.part` files, a checkpoint (`RenderConfig::checkpointPath`) holds the decoder state, and the next warm resumes it. Three cases still lose work (X3, 2026-09-27): the first warm of an image extracts it from the EPUB first, and that extraction cannot be interrupted (1.6 s for a 514 KB JPEG); a resume that meets input at once parks again without progress (118–265 ms each); and PNG images and a progressive JPEG's 1/8 decode resized to its box restart from the beginning. After a preemption the lane waits only the normal `IMAGE_LANE_SETTLE_MS` (300 ms).

**Why it matters.** At a normal reading pace the lane lost large images to page turns again and again before parking existed: one page was started six times while the reader turned a page every 2 s.

**Where.** `EpubReaderActivity::stepImageWarmLocked` (`EpubReaderActivity.cpp:2396`) and `warmPageForImageLane`
(`:2515`; `imageWarmPreemptedMs_` is set at `:2561` and counts as activity for the settle at `:2357`;
`imageWarmMisses_` counts only failures without input, `EpubReaderActivity.h:411-422`); `IMAGE_LANE_SETTLE_MS`
(`EpubReaderActivity.cpp:305`); the park and resume code in `JpegToFramebufferConverter.cpp` (from `:1126`; what may
park, `:1834-1835`, leaves out a resized decode); `ImageBlock.cpp:866-867`, which gives only a JPEG a checkpoint path;
`Epub::extractItemToFile` (`Epub.cpp:2346`), which does not poll for input.

**Tried.** Parking (JPEG only). The other candidate, never built: once a page's decode has been preempted twice, retry it only after a longer 1.5 s pause.

**Next step.** From a device log with parking in place, count re-preemptions per page. If parks without progress dominate, require a longer settle after a resume that parks at once. For the extraction, parking the inflate (a 32 KB window) is the candidate.

## A cover that fits only at 1/8 restarts on every key press

**Open.** When a cover's progressive JPEG fits in memory only at 1/8, `JpegThumbSession` refuses it and the one-shot `convertResized` draws it. That conversion is not sliced: a key press aborts it (`CooperativeAbort`), and the next attempt starts again from byte 0. Before `ProgressiveJpegDc` was retired, this case was sliced; a cover that fits at no scale was one-shot before too.

**Why it matters.** Input stays responsive, but a large cover appears only once the reader pauses for one whole conversion. Small covers still finish between key presses.

**Where.** `JpegToBmpConverter.cpp`: `convertResized` (`:369`, its abort poll `progressiveFullShouldAbort` at `:284`),
`JpegThumbSession::begin` (`:1095`), which returns no session when `chooseProgressiveShift` (`:785`) leaves the cover to
the resize (`:1111`); `ProgressiveJpeg::decodeResized` (`ProgressiveJpeg.cpp:1520`).

**Next step.** Make the resized decode steppable in `ProgressiveJpeg`: its resampler state in the session, about 60 lines. Then give `JpegThumbSession` a third mode, with the per-output setup shared with the other two, about 120-150 lines with tests.

## A cut or corrupt progressive JPEG draws a placeholder

**Open.** A progressive JPEG whose stream is cut short or corrupt fails as soon as a scan's data does not decode, and the reader draws its framed placeholder (a cover is not converted). A baseline file cut short draws nothing and is decoded again on every pass. Work that changes this is written and tested but parked: the decoder stops a scan where its data is unusable and decodes every row from the scans that remain (a DC scan's blocks after the damage mid grey, as libjpeg leaves them), so a cut or corrupt progressive file draws what it holds; a cut baseline file keeps its rows; and a degraded picture is cached once instead of decoded on every visit (X4: 8 decodes, about 14 s, per visit of such a page).

**Why it is parked.** Nobody has reported a damaged JPEG; the cut extracts it was written for came from the extractor itself (fixed in `Epub::extractItemToFile`). And caching damage makes a false detection permanent: a valid file that trips one of the new rules (the strict RSTn sequence, the padding overrun, a TJpgDec format error on an unusual baseline file) would be kept with grey or white areas for good, since `PixelCache::PXC_MAGIC` is not tied to the rules and cover BMPs are never invalidated.

**Where.** `lib/ProgressiveJpeg` (`BitReader`, `ProgressiveJpeg.cpp:665`; `decodeBand`, `:963`; `indexStep`, `:575`);
`JpegToFramebufferConverter::decodeToFramebuffer` (`JpegToFramebufferConverter.cpp:1506`); `JpegToBmpConverter.cpp`
(`SinkSet`, `:246`; `JpegThumbSession::continueSteps`, `:1154`); `PixelCache::PXC_MAGIC` (`PixelCache.h:93`).

**Tried.** The code and its tests (`test/epub_pipeline/JpegPartialDecodeTest.cpp`, the `prog_420_cut.jpg` fixture) are on the branch `archive/progressive-jpeg` (b623ca5a1). Its review found that once degraded pictures were cached, `Result::Degraded` behaved exactly like `Ok` everywhere, so the completeness tracking that told a file lacking only its EOI from a cut one bought nothing.

**Next step.** If it is ever taken, ship it decoder-only: fold `Degraded` into `Ok` with a `DecodeStats` flag for the logs, drop that completeness tracking, and leave the converters as they are -- they already cache `Ok`, and an I/O error keeps the placeholder. Bump the checkpoint magic with the `Cursor` layout it changes, and note in `PixelCache.h` that a change to the damage rules must bump `PXC_MAGIC`.

## Per-line and per-cell objects are still on the heap

**Open.** Each laid-out line still allocates a `TextBlock` object and a `PageLine` on the heap (88 B and 24 B on the 64-bit host), and each grid cell its own objects (about 160 B a cell, 11–15 KB for a page of grid rows, host). Their word bytes already come from the page block or the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line, `ParsedText.cpp:4405`), `ChapterHtmlSlimParser` (the
`PageLine` per line in `addLineToPage`, `ChapterHtmlSlimParser.cpp:5689`; a grid cell's lines in `layoutTableRow`,
`:6812`, kept at `:6866`), `TableCell::lines` (`Page.h:146`), `Page::elements`
(`std::vector<std::unique_ptr<PageElement>>`, `Page.h:208`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer ([Memory Allocation Strategy §8.4a](../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving the line bytes into the arena is done.

**Next step.** Placement-construct the objects in the page or row block, with an arena-aware deleter behind
`Page::elements`: 47 lines in `lib/Epub` and `src` use it, 15 of them add an element (the parser's nine adds, all
through `addPageElement`, and one `insert`, and the five of `Page::deserialize`, `Page.cpp:705-721`). Measure
allocation counts and `HeapPeakRegression` on the host first, then a table chapter on the X3 with `SCT_HEAP_TRACE=1`.
Budget for the image-header walk: on the X4 a 32 KB walk stage leaves 64 B of the lent region free, so objects of a
page that holds a late image could push the walk out of the arena.

## Throwing allocations left on the section-build path

**Open.** 2.52 sent every `Page` allocation through `ensurePage()` (`ChapterHtmlSlimParser.cpp:2074`), which stops the
build like the other heap refusals instead of aborting. These still allocate through the throwing `operator new`, so a
refusal there aborts and reboots (`-fno-exceptions`); counted in `ChapterHtmlSlimParser.cpp`:

- the style stack, `std::vector<StyleStackEntry> inlineStyleStack` (`ChapterHtmlSlimParser.h:589`; pushes at
  `ChapterHtmlSlimParser.cpp:826`, `:3858`, `:4223`, `:4254`, `:4285`, `:4305`, `:4356`);
- the other per-element stacks: `listStack` (`:3887`), `containerWidthStack_` (`:3150`), `backgroundImageScopes_`
  (`:6281`), and, capped in count but grown the same way, `heldBottomSpacing_` (`:2650`, 8), `blockInsetStack_`
  (`:3953`, 8) and `anchorsAwaitingLine_` (`:5196`, 16);
- a table's packed rows, `t.packer.rows.push_back` (`:6173`, `:6183`; at most `MAX_TABLE_ROWS`, 48, per fragment);
- a row's cells: `pendingRow.cells` (`:3285`, `:6759`), a laid-out row's `out.cells.reserve(columnCount)` (`:6827`;
  the pushes at `:6912` and `:6917` stay within it), and a cell's lines (`:6866`, at most `MAX_CELL_LINES`, 64);
- `std::string` growth during the parse, for example the ruby text (`rubyText_`, `:4475-4479`, up to 255 bytes).

**Next step.** Ask `HeapFit` before each growth (`heapReserveFor`, as the elements reserve does) and stop the build on
a refusal (`stopForHeap`, `:2092`), or move the table to the build's arena or a `ChunkedVector`. Each site needs a host
fault-injection test, as `PageHeapTest.cpp` (`PAGE_FAULT_INJECTION`, `ELEMENT_FAULT_INJECTION`) does for pages and for
`Page::elements`, which `addPageElement` now covers.

## CSS in a chapter's `<style>` blocks is never applied (matcha-reader #627)

**Open.** The parser skips `<head>` whole (`SKIP_TAGS`, `ChapterHtmlSlimParser.cpp:332`), `isNonVisibleTag()` counts a
`<style>` as invisible text (`lib/Epub/Epub/VisibleText.h:33`), and nothing parses its rules: only linked stylesheets
and `style=""` attributes reach the layout. A book whose only CSS is a `<style>` block in each chapter lays out
unstyled. matcha copies the block to a temp file (max 16 KB) and loads it like a linked sheet. First confirm with such
an EPUB. Size M, with a `SECTION_FILE_VERSION` bump.

## Every manifest stylesheet applies to every chapter (matcha-reader #453)

**Open; the record it needs is done (13a), the scoping (13b) is next.** `Epub::parseCssFiles()`
(`lib/Epub/Epub.cpp:787`) compiles every stylesheet of the book (the manifest's, then any `.css` found only in the
archive, `discoverCssFilesFromZip`) into one rule set, and every chapter resolves against all of it: a
`body{text-align:center}` in a sheet that no text chapter links (matcha's case: `vertical_image.css`) centres every
chapter, horizontal books too. matcha-reader #453, Eszter Schuffert (eszter007).

- **Done (13a, `CSS_CACHE_VERSION` 36).** A rule's order runs on across the sheets in compile order, so each sheet owns
  one contiguous range of orders. The compile records, at the end of `css_rules.cache`, each sheet's `{path hash, first
  order, end order}` and its `@import` edges `{importer, imported path hash}` (`CssParser::sheetRanges`,
  `sheetImports`, `loadSheetTable`; layout in [file-formats.md](../file-formats.md#css_rulescache-css_bgbin-and-css_fontsbin)):
  8 B a sheet, 5 B an import. At most 64 ranges (a book with more keeps one range, for every chapter, from its 64th
  sheet on) and 64 imports (past that, an empty table: every sheet applies, as now). `CssParser::resolveStylesheetHref`
  resolves a reference the way the table's paths are resolved. Nothing reads the table yet, and the rule records and
  the index are as before but for the merge below.
- **Found on the way.** Nothing follows `@import` (`CssParser::loadFromStream` drops the at-rule at its `;`, and no
  other code in `lib/` or `src/` reads one): an imported sheet applies today only because every sheet is compiled, so
  13b must follow the edges. And a redeclaration merged across a sheet boundary (`existing.order + 1 >= order` in
  `processRuleBlockWithStyle`, in both stores): a sheet ending in `.x{...}` and the next one starting with `.x{...}` made
  one record under the second sheet's order that held the first sheet's declarations. It no longer merges there
  (`sheetFirstOrder_`); the two records resolve as the merged one did.
- **Next (13b), with a `SECTION_FILE_VERSION` bump.** Scan the chapter's head (`html_<spine>.bin`, up to `</head>`) for
  its `<link rel="stylesheet">` hrefs, resolve each with `resolveStylesheetHref` against the chapter's folder, find its
  ranges in the table (a sheet listed twice has two), follow the import edges (they can form a cycle; the folded
  range's too), and make `lookupRules` and the complex-rule scan skip every rule whose order is outside the chapter's
  ranges and the folded one. Fail open (every sheet) for a linked sheet the table does not know and for an empty table;
  a chapter with no `<link>` resolves against none. An import the table does not know names a sheet that was never
  compiled: nothing to add. The background pictures (`css_bg.bin`) and `@font-face` rules (`css_fonts.bin`) are not
  ranged and stay book-wide. `loadSheetTable` reads the table from the card (a seek from the file's end); where it is
  kept during a build is 13b's choice.

## An inline anchor lands a page off (CrossPoint #4328)

**Open.** A block's anchor waits for its first line, but an `id` on an inline element is placed approximately: an
`<a id>` inside a paragraph is recorded with the next block (`pendingAnchorId`, `ChapterHtmlSlimParser.cpp:2962`), a
`<span id>` with its block's first line (`recordSpanAnchor`, `:5205`). A link to it (a backlink to a noteref) can land a
page off; the link Back stack already allows for that (`EpubLinkBackStack.h:73-75`). Take the inline-anchor part only:
record such an id at the page of its own line. Needs a `SECTION_FILE_VERSION` bump. crosspoint-reader #4328, Mikalai
Kryvusha (@MikalaiKryvusha).

## JPEG source reads of 512 bytes (matcha-reader #245)

**Open.** TJpgDec reads its input through `tjpgBmpInput` for covers
(`lib/JpegToBmpConverter/JpegToBmpConverter.cpp:57-66`) and `tjpgInput` in the reader
(`lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp:364-373`), one `HalFile` read of `JD_SZBUF` (512 B,
`lib/TJpgDec/src/tjpgdcnf.h:15`) per call. matcha measured about 2 ms fixed per read and cut a cold decode by 22 % with
an 8 KB read-ahead. The SDK's sector read-ahead (freeink-sdk cf63426ef, pinned in ec93b64fb) serves the SDMMC boards
only (`SdmmcBlockDevice`); the X3 and X4 read over SPI without it. Profile on the X3 and X4 first (#220 found the same
per-call cost on writes). matcha-reader #245, Eszter Schuffert (eszter007).
