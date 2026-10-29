# Memory and parsing

Status: open items as of 2026-10-07. Collected from the heap-work handover (2026-08), the memory audit (2026-09) and the earlier memory-allocation-strategy.md (removed or trimmed in the 2026-10-07 docs cleanup; in git history).

The rules and measurements these items refer to are in [Memory Allocation Strategy](../memory-allocation-strategy.md).

## Text inside a nested table is dropped

**Open.** `ChapterHtmlSlimParser::consumeText` returns early when `currentTable->depth > 1`, so every word inside a table nested in another table is discarded. The nested-table branch of `ChapterHtmlSlimParser::startElement` calls `degradeTable("nested table")`, and its comment says the inner cells fold into the outer table's stream, but the text never arrives there.

**Why it matters.** It breaks "never drop content", silently, on any book that nests tables. It does not depend on heap state, so it reproduces on the host.

**Where.** The early return in `consumeText`; the nested `<table>` branch in `startElement`; the `depth > 1` `</table>` branch in `endElement`; and every handler that tests `currentTable->depth == 1` (`tr`, `td`/`th`, `caption`, `table`). All of them need auditing, not just the one `return`: a `<tr>` or `<td>` inside the nested table must not open rows or cells in the outer one.

**Tried.** Kept out of the streaming table renderer (PR #126) on purpose, as its own change. Nothing since.

**Next step.** Add a fixture with a nested table to `test/epubs` (a `make_test_*.py` generator, like the others) whose golden shows the inner words, then route `depth > 1` text into the degraded outer table's paragraph stream and walk through each `depth` branch.

## Class and id selectors ignore case

**Open.** `CssParser` lowercases both sides of every class and id lookup: selector keys through `normalizedInto()` when a stylesheet is parsed, class attributes in `forEachNormalizedClassToken()`, and ids in `resolveStyle()` and `backgroundImageFor()`. A rule therefore reaches any element whose class or id differs only in case. XHTML class and id names are case-sensitive, and Kindle, ADE and Readest match them that way.

**Why it matters.** It styles text the book did not style. In *The Anarchy* the paragraph rule `.Dial` (display:block, 0.75em, a hanging indent) also lands on `<span class="dial">`, the speaker name in each dialogue line: the names print at about 56 % size and, since #393 made a `display:block` span open a block, on a line of their own. A survey of 109 books found three that collide (`Dial`/`dial`, `TOC`/`toc`) and none that matches a rule only through the lowercasing.

**Where.** `lib/Epub/Epub/css/CssParser.cpp` (the functions above). `CssGapsIdSelector.CaseNormalized` in `test/css_gaps` pins it for ids today.

**Tried.** PR #394 (branch `fix/css-class-case-sensitive`), stacked on #393 and not merged: only the tag part of a selector is lowercased, the name after `.` or `#` stays as written in the rule cache, the background-picture side file and the lookups; `CaseNormalized` and the `HERO` probe in `test/epub_css/CssParserCacheTest.cpp` become case-sensitive pins; `CSS_CACHE_VERSION` 21 → 22 and `SECTION_FILE_VERSION` 79 → 80.

**Next step.** Rebase the branch onto master. `CSS_CACHE_VERSION` is still 21 there, but `SECTION_FILE_VERSION` is 81, so the section bump becomes 81 → 82. Re-run the corpus comparison, then merge.

## A `<br>` that ends a block adds a blank line

**Open.** `<p>Alpha<br/></p><p>Bravo</p>` lays out Bravo a whole line below Alpha (y=48 with the host stub, against 24
without the `<br>`), whether or not the `<br>` is styled `display: block`. A browser renders no line for a line break
that ends a block's content.

**Why it matters.** Calibre ends every entry of a converted contents page with `<br class="calibre1"/>` (and styles it
`display: block`): Hayakawa's 『七王国の玉座』, seen on the X3 on 2026-10-26, sets each contents entry as a line and a
blank line, about twice the pages it needs.

**Where.** The `<br>` handling in `ChapterHtmlSlimParser::startElement` and the block flush after it;
`test_br_in_blocks.epub` and `test_br_section_break.epub` pin what `<br>` does today, so a change has to keep their
deliberate cases (a run of `<br>` as a scene break) apart from a single trailing one.

**Next step.** Drop the empty line a single `<br>` leaves at the end of a block, check both goldens, and compare a
calibre book's page count before and after.

## No hint when a chapter was simplified

**Open.** When a fixed capacity changes what a chapter shows (footnotes per page, anchors per chapter, page elements, nesting depth), `ChapterHtmlSlimParser::noteCapOverflow` logs it and the section stores `kStatusSimplified`. The reader only logs "cached simplified" on a cache hit (`EpubReaderActivity`, section load); the reader sees less than the book contains and is not told.

**Why it matters.** The cap is deterministic, so a rebuild cannot help; a hint is the only way to tell the reader. It is a product decision, not a memory one.

**Where.** `Section::isSimplified`; the truncated-chapter hint (`truncatedSectionHintRendersRemaining`, `TRUNCATED_SECTION_HINT_LINE_1/2`) is the model to reuse. Its strings are hard-coded English and would need `tr()` keys.

**Next step.** Decide whether the hint is wanted. If it is, show it the way the truncated hint is shown, with translated strings.

## The image lane waits for Background-B

**Done (2026-10-19, branch `perf/reader-phase-memory`).** The lane runs before B: B borrows the secondary buffer only once the lane has looked at its window and found nothing to decode, and a lane decode or a page render that meets a B build in the buffer has B give it back, at most `LaneBeforeBuild::kMaxYields` (4) times per target and not counted as preemptions. See `src/activities/reader/LaneBeforeBuild.h` and [the image lane](../background-rendering.md#image-lane--pixel-caches-ahead-of-the-reader).

**Measured (X3, Build Through test book, `make_test_font_families.py --build-through`).** Where the conflict happened was build-through, not a mid-chapter B build: with anti-aliasing off B took the buffer on a chapter's last pages 0.6 s after the page appeared, before the lane's settle, and the three picture pages decoded on the heap beside its build (11,780 and 15,672 B free at the JPEGs' lows, the PNG drawn as a low-memory box). With anti-aliasing on, the deferred AA pass delayed B past the lane's settle, so the lane already won. With the lane first, the pictures are decoded in the lent buffer before B starts, B builds through without meeting a decode, and it still finished the next chapter before the reader reached it (the turn onto the next chapter was a cache hit, 1.5 s to a settled screen, as before). A page render that decodes in the buffer B gave back reached 32-34 KB free at its low.

**Still open.** The target page of a chapter jump still decodes in the foreground: Background-C holds the buffer until its build ends, and C is not preempted.

## The font prewarm sets the session's heap minimum

**Partly done (2026-10-19, branch `perf/reader-phase-memory`).** Re-measured on the X3 (Build Through test book, `[MEM]` and per-phase lows of `renderContents`): the prewarm no longer sets the session's minimum. With the image decodes out of Background-B's way (see above) the low point of a session sat at ~33 KB, inside B's build, and a normal render's phases stayed within 1-2 KB of their start:

| Page render, X3 | start | prewarm low | BW render low |
|---|---|---|---|
| TrueType body (BookerlyTTF), glyphs in the store | 50.5 KB | 48.6 KB | 50.3 KB |
| the same, first page after the glyph store was installed again, before | 50.3 KB | 38.7 KB | 39.9 KB |
| the same, after | 50.5 KB | 48.7 KB | 50.3 KB |
| built-in Bookerly | 53.0 KB | 45.0 KB | 49.4 KB |

What changed: a render lends the secondary buffer to the prewarm's TrueType misses as it lends it to an image decode (the glyph store has no glyph yet: a 10.5-14.5 KB scratch block, in SD mode a 4.2 KB page cache, each taken from the lent region and given back per miss), and whatever a miss still made on the heap is freed before the BW render instead of when RenderLock goes back (`renderContents`).

**Still open.** Two transients remain, neither the session's minimum on these measurements:
- The built-in fonts' prewarm: 8 KB, of which the page slots (3.5 KB) stay for the BW render and the AA pass, and the rest is the group stream ring, already freed per group. **Done since (2026-10-19, branch `fix/lowmem-and-ram`):** `renderContents` lends the buffer to every prewarm, not only with a TrueType set, and `FontDecompressor` takes a ring from its slot arena, else from the region lent to the glyph misses (`glyph_miss::Scope`), giving it back before the next group; the slots stay on the heap. On the host the prewarm's heap peak loses its largest ring (3,036 B for a Noto Sans 14 sample; up to 4,096 B, `GROUP_RING_MAX_BYTES`); not yet measured on a device. The pre-render still cannot lend the buffer (it holds the page on screen), so most turns, which show a pre-rendered page, are unchanged.
- A pre-render with TrueType misses (15-26 KB on the X3, PreRenderGate) cannot lend for the same reason. Its heap floor already asks for that cost.

**Next step.** None until a session's minimum is found in a render again. The per-phase lows came from a measurement-only patch (`heap_caps_monitor_local_minimum_free_size_start` restarted at each phase boundary of `renderContents`); a permanent version would cost the same monitor B's borrow uses.

## Preempted image decodes still lose work in some cases

**Open.** A lane decode that a page turn preempts parks if it is a JPEG: its caches keep their finished rows in `.part` files, a checkpoint (`RenderConfig::checkpointPath`) holds the decoder state, and the next warm resumes it. Three cases still lose work (X3, 2026-09-27): the first warm of an image extracts it from the EPUB first, and that extraction cannot be interrupted (1.6 s for a 514 KB JPEG); a resume that meets input at once parks again without progress (118–265 ms each); and PNG images and a progressive JPEG's 1/8 decode resized to its box restart from the beginning. After a preemption the lane waits only the normal `IMAGE_LANE_SETTLE_MS` (300 ms).

**Why it matters.** At a normal reading pace the lane lost large images to page turns again and again before parking existed: one page was started six times while the reader turned a page every 2 s.

**Where.** `EpubReaderActivity::stepImageWarmLocked` and `warmPageForImageLane` (`imageWarmPreemptedMs_`; `imageWarmMisses_` counts only failures without input); the park and resume code in `JpegToFramebufferConverter.cpp`; `Epub::extractItemToFile`.

**Tried.** Parking (JPEG only). The other candidate, never built: once a page's decode has been preempted twice, retry it only after a longer 1.5 s pause.

**Next step.** From a device log with parking in place, count re-preemptions per page. If parks without progress dominate, require a longer settle after a resume that parks at once. For the extraction, parking the inflate (a 32 KB window) is the candidate.

## A cover that fits only at 1/8 restarts on every key press

**Open.** When a cover's progressive JPEG fits in memory only at 1/8, `JpegThumbSession` refuses it and the one-shot `convertResized` draws it. That conversion is not sliced: a key press aborts it (`CooperativeAbort`), and the next attempt starts again from byte 0. Before `ProgressiveJpegDc` was retired, this case was sliced; a cover that fits at no scale was one-shot before too.

**Why it matters.** Input stays responsive, but a large cover appears only once the reader pauses for one whole conversion. Small covers still finish between key presses.

**Where.** `JpegToBmpConverter.cpp` (`convertResized`, `JpegThumbSession`), `ProgressiveJpeg::decodeResized`.

**Next step.** Make the resized decode steppable in `ProgressiveJpeg`: its resampler state in the session, about 60 lines. Then give `JpegThumbSession` a third mode, with the per-output setup shared with the other two, about 120-150 lines with tests.

## A 1/8 decode of a file with libjpeg's scan script reads its DC refinement

**Open.** A 1/8 decode (the DC-only session, and with it `ProgressiveJpeg::decodeResized`, the low-heap fallback) indexes the file only until luma DC is final. A file whose first scan completes DC, as the real covers measured do, is read no further than that scan: about twice the bytes of the DC-only preview this replaced, as the index and the bands each read that scan once. A file with libjpeg's default script completes DC only in its DC refinement, 55-80% into the file, so its index skips the AC scans before it byte by byte, and through the shared 512-byte buffer its bands alternate between the two DC scans, refilling on every switch: 7-15x the preview's card bytes on the real files measured, up to 30x on small generated ones (host, one 464x670 box: a 530 KB file 1,098 reads / 539 KB against 100 / 51 KB).

**Why it matters.** It is the path the reader takes when only 1/8 fits, and covers when no finer scale does (one decode per output box). On the C3 that could be 1-3 s of SD time for such a file against about 0.15 s before; an estimate from host counts, not a device measurement.

**Where.** `indexStep` (the DC session's early stop), `ProgressiveJpeg::decodeResized`; `convertResized` in `JpegToBmpConverter.cpp`; the resized path of `JpegToFramebufferConverter::decodeToFramebuffer`.

**Next step.** Measure on the device first (Home with heavy covers and little heap: the `Progressive cover read N times` log and the time to the first cover). If it matters, give `decodeResized` a mode, through `DecodeOptions`, that reads the first luma DC scan alone and skips its refinements: the preview's exact I/O, at most one level off (mean 0.1-0.9 against the preview), while sliced 1/8 decodes and their goldens stay as they are.

## A cut or corrupt progressive JPEG draws a placeholder

**Open.** A progressive JPEG whose stream is cut short or corrupt fails as soon as a scan's data does not decode, and the reader draws its framed placeholder (a cover is not converted). A baseline file cut short draws nothing and is decoded again on every pass. Work that changes this is written and tested but parked: the decoder stops a scan where its data is unusable and decodes every row from the scans that remain (a DC scan's blocks after the damage mid grey, as libjpeg leaves them), so a cut or corrupt progressive file draws what it holds; a cut baseline file keeps its rows; and a degraded picture is cached once instead of decoded on every visit (X4: 8 decodes, about 14 s, per visit of such a page).

**Why it is parked.** Nobody has reported a damaged JPEG; the cut extracts it was written for came from the extractor itself (fixed in `Epub::extractItemToFile`). And caching damage makes a false detection permanent: a valid file that trips one of the new rules (the strict RSTn sequence, the padding overrun, a TJpgDec format error on an unusual baseline file) would be kept with grey or white areas for good, since `PixelCache::PXC_MAGIC` is not tied to the rules and cover BMPs are never invalidated.

**Where.** `lib/ProgressiveJpeg` (`BitReader`, `decodeBand`, `indexStep`); `JpegToFramebufferConverter::decodeToFramebuffer`; `JpegToBmpConverter.cpp` (`SinkSet`, `JpegThumbSession::continueSteps`).

**Tried.** The code and its tests (`test/epub_pipeline/JpegPartialDecodeTest.cpp`, the `prog_420_cut.jpg` fixture) are on the branch `archive/progressive-jpeg` (b623ca5a1). Its review found that once degraded pictures were cached, `Result::Degraded` behaved exactly like `Ok` everywhere, so the completeness tracking that told a file lacking only its EOI from a cut one bought nothing.

**Next step.** If it is ever taken, ship it decoder-only: fold `Degraded` into `Ok` with a `DecodeStats` flag for the logs, drop that completeness tracking, and leave the converters as they are -- they already cache `Ok`, and an I/O error keeps the placeholder. Bump the checkpoint magic with the `Cursor` layout it changes, and note in `PixelCache.h` that a change to the damage rules must bump `PXC_MAGIC`.

## Per-line and per-cell objects are still on the heap

**Open.** Each laid-out line still allocates a `TextBlock` object and a `PageLine` on the heap (88 B and 24 B on the 64-bit host), and each grid cell its own objects (about 160 B a cell, 11–15 KB for a page of grid rows, host). Their word bytes already come from the page block or the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line), `ChapterHtmlSlimParser` (the `PageLine` per line, `layoutTableRow`), `Page::elements` (`std::vector<std::unique_ptr<PageElement>>`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer ([Memory Allocation Strategy §8.4a](../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving the line bytes into the arena is done.

**Next step.** Placement-construct the objects in the page or row block, with an arena-aware deleter behind `Page::elements` (about 30 call sites). Measure allocation counts and `HeapPeakRegression` on the host first, then a table chapter on the X3 with `SCT_HEAP_TRACE=1`. Budget for the image-header walk: on the X4 a 32 KB walk stage leaves 64 B of the lent region free, so objects of a page that holds a late image could push the walk out of the arena.
