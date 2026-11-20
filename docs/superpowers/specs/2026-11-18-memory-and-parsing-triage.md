# Memory and parsing: triage and designs

Status: design for the maintainer, 2026-11-18. It decides each open item of
[future_work/memory-and-parsing.md](../../future_work/memory-and-parsing.md) and specifies the ones worth doing, one
small PR each. Nothing here is implemented. Line references are to master `715896630`.

**How each figure was obtained.** Every statement is one of four kinds, and says which:

- **read**: from source at master `715896630`.
- **measured, host**: run in this worktree on Windows with `epub_pipeline_dump_noheap` (layout only). The host heap
  tools (`epub_pipeline_dump --bench`, `epub_build_inventory`) were not run: their `malloc` interposer deadlocks under
  the MinGW runtime (`test/epub_pipeline/CMakeLists.txt:315-317`), so no allocation count in this document is new.
- **recorded**: a figure the repository's documents already hold, quoted with its board and date.
- **estimate**: given with its basis.

No device was used. No firmware was built. No flash figure is measured: every one is an estimate from line counts.

## The rules the designs follow

1. Memory comes from the build's arena or the lent region when one is present and has room (`BuildArena`).
2. Otherwise from the heap, asked first (`heapHasBlockFor`, `heapReserveFor`, `makeUniqueNoThrow`, `ChunkedVector`),
   with a defined refusal: the build stops through `ChapterHtmlSlimParser::stopForHeap`
   (`ChapterHtmlSlimParser.cpp:2092`) and keeps its pages, or a softer exit that keeps the content (a table row laid
   out as paragraphs).
3. The S3 boards take the same code path with a larger region: a build with nothing lent runs in a 256 KB PSRAM arena
   (`Section::setOwnedBuildRegionBytes`, behind `HalCapabilities::keepsSecondaryBuffer()`), and
   `HalCapabilities::buildsInPsram()` (`HalCapabilities.h:269-271`) is the check for a larger cap. No design below
   names a board.
4. No content is dropped. Where a design must choose, it stops the build with its pages kept, which shows the
   truncated-chapter hint, before it loses text silently.
5. One concern per PR. A PR that changes what a section file holds bumps `SECTION_FILE_VERSION` (`Section.cpp:42`,
   125 today); the number is taken from master when the PR merges, not when it is written.

## Codecs: what the tree has, and where compression pays

**Read.** No compressor runs on the device. The firmware has one general decoder, uzlib inflate
(`lib/uzlib/src/tinflate.c`, the only vendored source file; `uzlib_compress` is declared in `uzlib.h:146-158` and not
defined). Everything compressed is made on the host: font glyph groups (`scripts/deflate_groups.py`), language packs
(`scripts/gen_i18n.py`), the web bundle (`scripts/build_html.py`). There is no RLE, LZ4, heatshrink, zstd or brotli in
the tree; miniz was removed in `d14623ac6`.

| Candidate | Working set | Speed on the C3 | Flash | Usable |
|---|---|---|---|---|
| uzlib inflate (have) | ring up to 32 KB (`InflateReader.cpp:8`), state about 1.3 KB (`docs/i18n.md:443`) | about 2.5 µs a byte plus 0.4 ms a stream start (recorded, host-scaled, `docs/future_work/font-flash.md:53`) | 0 | decode only |
| ROM miniz `tdefl` (C3 and S3 ROM, `esp32c3.rom.ld:68-84`) | `tdefl_compressor` about 168 KB (estimate from the ROM header's array sizes) | not measured | 0 | no: the state does not fit the C3's heap |
| uzlib deflate (`genlz77.c`, `defl_static.c`, not vendored) | a hash table of 2^N pointers, 4-16 KB | 5-10 µs a byte (estimate, hash-chain LZ77 on a 160 MHz RV32) | 2-3 KB (estimate) | possible, no user |
| byte RLE (PackBits) | none | under 0.2 µs a byte, about 8 ms for a 52 KB frame (estimate, 20-30 cycles a byte) | about 200 B (estimate) | possible |
| sparse record plus dedup (have: `compressStyle`, `CssParser.cpp:3133`) | none | microseconds a record | 0 | in use for the CSS ruleset: at most 83 B against a 132 B `CssStyle` |

**Where it pays among these fifteen items: nowhere as a byte codec.** The reasons, item by item:

- The per-line and per-cell objects live for one page. They are a block-count problem (§9), and a codec does not
  change a count.
- The throwing allocations (§10) are small tables: a stack of 64 entries is under 2 KB.
- A parked inflate or PNG decode would write its 32 KB ring to the card (§5). Card time is the cost there, and the ring
  is already-inflated data that deflates badly at the speed above.
- The one place a byte codec could buy memory is the font prewarm of a one-pass page (§4): the secondary buffer holds
  the frame on screen, so the prewarm cannot borrow it. A text frame run-length encodes to roughly a fifth (estimate),
  but the encoded frame needs a heap block whose size depends on the page, an image page does not compress, and no
  measurement shows that prewarm setting a session's minimum. It is not designed here.

The technique that does pay is the one the CSS ruleset already uses, a canonical sparse record with dedup. §12 reuses
it for a chapter's `<style>` rules, at no flash cost.

## 1. Text inside a nested table is dropped

**Verdict: do now.** Size M. No dependency. PR 1 of the table track.

**The premise holds, and the fix is smaller than the doc's audit list suggests.** Read: when the inner `<table>` opens,
`degradeTable("nested table")` (`:3161`) runs `degradeRow` (`:6680`), which writes the buffered outer cells out as
paragraphs, clears `pendingRow.cells` and sets `currentTableCell = nullptr`. From then on a word is no cell's: with the
early return gone it goes to `currentTextBlock` like any paragraph text. The `depth == 1` handlers do not fire inside
the inner table, so an inner `<tr>` or `<td>` opens no row or cell in the outer one.

**Measured, host** (a throwaway edit, reverted): removing the early return in `consumeText` (`:4435`) and flushing the
part-word buffer at the inner `</table>` (`:4891`, which discards it today) puts all fourteen inner words of
`test_table_streaming.epub` on the page, in document order, between the outer cells before and after. Two defects
remain in that output, and they are the real work:

- the four inner cells run together as one paragraph ("Inner cell one. Inner cell two. Inner cell three, ...");
- they sit at x=0, where the outer table's degraded cells sit at x=18.

**Design.**

- `consumeText` (`:4435`): delete the `depth > 1` return.
- `startElement`: add a branch for `currentTable->depth > 1` and `td`, `th` or `caption`, before the generic element
  handling. It flushes the part-word buffer and starts a new text block with the block style a degraded outer cell
  gets, so an inner cell is a paragraph with the same inset. Take that style from the code `emitCellAsParagraph` uses;
  do not write a second copy. An inner `<tr>` needs no branch.
- `endElement` (`:4890-4896`): flush instead of discarding (`flushPartWordBuffer()`, returning on a refusal), and end
  the inner table's last paragraph so the outer cell's following text starts a new one.
- `openBidiEdge` (`:1600`): drop the `depth > 1` test. The text is laid out now, so it has edges to bracket.
- An `<img>` inside the inner table: check what the image branch does at `depth > 1` and make it emit the image as a
  block (`placeImageBlockAsBlock`), as a degraded cell's image is.
- The comment at `:3158-3159` already describes this behaviour; it becomes true.

**Memory.** No new allocation. The inner words go through the paragraph path, whose bytes come from the page block in
the lent region or from the heap as today. The C3 and the S3 path are the same.

**Flash.** About +150 B (estimate).

**Cache.** Bump `SECTION_FILE_VERSION`: a cached section of such a chapter holds the page without the words.

**Tests.**

- `test/epub_pipeline/goldens/test_table_streaming.golden.txt` gains the inner words. The rest of its diff must be
  y-shifts of what follows on that page and nothing else.
- Add a generator, `test/epubs/make_test_nested_tables.py` (the fixture has none), with: a table nested two deep, an
  inner table with an image, an inner table with `dir="rtl"`, an inner table whose first cell has an `id` that a link
  targets, and an inner table in the first row's first cell (nothing buffered before it).
- `AnchorMapTest`: the inner cell's id resolves to the page of its text.

**Risks.** A layout-table book (a page laid out as one outer table with content tables inside) now gains pages; that
is the content that was missing. A nesting three deep increments `depth` twice and must unwind twice; the generator
covers it.

## 2. A `<br>` that ends a block adds a blank line

**Verdict: do now.** Size S to M. No dependency. PR 1 of the block track.

**The doc's next step treats one case; the rule behind it is simpler and fixes two.** In a browser a `<br>` adds a
blank line only when the line it ends is empty. A `<br>` after text on the same line ends that line and adds nothing,
whatever follows it. The parser decides late instead: a `<br>` leaves an empty block marked `fromBrElement`
(`:4089`), and `startNewTextBlock` adds a line to the next block's top margin when it finds that mark (`:2741-2748`).
So a run of N `<br>` after text gives N blank lines when a block follows and N-1 when text follows:

| Markup | Browser | Today | With the rule |
|---|---|---|---|
| `<p>A<br/></p><p>B</p>` (this item) | 0 | 1 | 0 |
| `<h2>A<br/></h2><p>B</p>` (case 5 of `test_br_in_blocks`) | 0 | 1 | 0 |
| `<h2>A</h2><br/><p>B</p>` (case 6) | 1 | 1 | 1 |
| `<p>A<br/><br/>B</p>` | 1 | 1 | 1 |
| `<p>A<br/><br/></p><p>B</p>` | 1 | 2 | 1 |
| `<p>A</p><br/><br/><p>B</p>` (a scene break) | 2 | 2 | 2 |
| `<p>A</p><br/>B` | 1 | 0 | 1 |

**Design.** Decide at the `<br>`, not at the next block.

- In the `<br>` branch (`:4058-4090`), the `<br>` ends a line with content when `currentTextBlock` is not empty and its
  element is still open (`currentBlockOwnerDepth_ >= 0`). Then the new empty block carries no gap.
- Otherwise (the current block is empty, or the block before it has closed) the `<br>` stands on an empty line: add one
  line height to the new block's `marginTop` there, with the line-height expression `:2746` uses.
- `startNewTextBlock` no longer adds a line for `brGapPending` (`:2741-2748`). `fromBrElement` stays for its other
  readers (`:2753`, `:5156`, `BlockStyle.h:169-171`); check each still means "an empty block a `<br>` left".
- The margin a `<br>` adds must merge as it does today (`getCombinedBlockStyle` at `:2750`), so a run still adds up.

**Memory.** None. **Flash.** About 0 (estimate). **Cache.** Bump `SECTION_FILE_VERSION`.

**Tests.** In `test_br_in_blocks` only case 5 changes: the paragraph moves from 58 px below the heading line to 34 px.
`test_br_section_break` does not change. Extend `make_test_br_in_blocks.py` with the rows of the table above that it
lacks. Run every golden: a `<br>` is common, and a diff outside those two fixtures is a finding to explain in the PR.
Count the pages of one Calibre-converted contents page before and after (the doc's Hayakawa case; any Calibre book
with `<br class="calibre1"/>` entries serves).

**Risks.** A book that ends each paragraph with a `<br>` to space them loses that space; a browser shows the same. The
last row of the table is a layout change in a case nobody reported: if its goldens move more than expected, keep
today's behaviour for a `<br>` after a closed block followed by text and say so in the PR.

## 3. No hint when a chapter was simplified

**Verdict: owner decision. Recommendation: no on-page hint.** Two small PRs are worth doing whatever is decided.
**Decided 2026-11-18 by the owner: A, no hint.** 3a is plan Task 8; 3b stays an open item.

**Why not a hint.** Read, the seven limits (`ChapterHtmlSlimParser.h:1426-1432`):

| Limit | What the reader loses | Would a hint help |
|---|---|---|
| footnote links per page (64) | the 65th link on a page is text, not a link | no |
| anchors per chapter (1,024, plus 512 span ids) | a link to a late id lands on the chapter's fallback page | no |
| printed-page labels (65,535) | nothing visible | no |
| footnote href length | one link is text | no |
| nesting depth (64) | styling of the 65th level | no |
| page elements (1,024) | **the tail of one page** (`Section.cpp:725-729`) | it would, but see below |
| pages per section (10,000) | the rest of the chapter; already shows the truncated hint | has one |

Five of the seven lose a link or a style, the reader cannot act on any of them, and a hint drawn over two page renders
(`TRUNCATED_SECTION_HINT_RENDER_COUNT`) costs reading attention every time such a chapter opens. The log line is
enough for those.

**The options.**

- A. No hint (recommended).
- B. The truncated hint's mechanism with a second pair of strings, armed at `EpubReaderActivity.cpp:5709-5711`. Size S.
  About +300 B of flash plus two strings in 23 languages (estimate).
- C. A line in the book's information screen instead of on the page. Size S. Nobody would find it.

**Worth doing either way.**

- **3a. Translate the truncated hint.** `TRUNCATED_SECTION_HINT_LINE_1/2` (`EpubReaderActivity.cpp:355-356`) are
  hard-coded English on a device with 23 languages. Move them to `tr()` keys. Size S. Never run `gen_i18n.py` by hand.
- **3b. The page-elements limit drops content.** A page past `Page::MAX_ELEMENTS` loses its tail when it is written.
  It takes 1,025 elements on one page, which needs elements without height; no book is known to reach it. If it is
  ever reached, the fix is to end the page at the limit (`emitPage()` from the helper §10a adds), not a hint. Do it
  only with a fixture that reaches the limit.

## 4. The font prewarm sets the session's heap minimum

**Verdict: not worth it now.** The doc's own measurement (X3, 2026-10-19) shows the prewarm within 1-2 KB of a
render's start and the session minimum inside Background-B's build. The two transients left (a one-pass page's
prewarm on the heap, a pre-render's TrueType misses) are both there because the secondary buffer holds a frame the
display needs. The S3 boards lend a PSRAM arena and are not affected.

**What reopens it.** A device log in which a session's lowest `[MEM]` free or largest-block reading falls inside
`renderContents` or `renderPreRenderPass`, on the X3, in a book set in a TrueType face. Then the design is the
measurement patch the doc names, made permanent behind the dev build, before any change to the prewarm.

## 5. Preempted image decodes still lose work

**Verdict: one part now, one part corrected in the doc, two parts not worth it.**

**The doc is wrong about the extraction.** Read: `ensureExtracted` (`ImageBlock.cpp:75-104`) calls
`Epub::extractItemToFile` with no stop callback (`Epub.cpp:2315`), so the extraction runs to its end and renames its
`.part` file into place (`Epub.cpp:2336-2343`). The work is kept. What the reader pays is latency: the lane holds the
`RenderLock`, so a page turn waits for what is left of the 1.6 s. PR 5b rewrites that paragraph.

**5a. A resume makes progress before it can park again. Do now.** Size S.

- Read: a baseline resume always decodes one MCU row before it polls (`JpegToFramebufferConverter.cpp:2017-2019`). A
  progressive resume polls before its first band (`ProgressiveJpeg.cpp:1258`), so a key press in the window between
  the lane's input check (`EpubReaderActivity.cpp:2356-2360`) and the first band parks it where it started, after
  118-265 ms of loading the page, borrowing the buffer and reading the checkpoint (X3, 2026-09-27, recorded).
- Design: after a resume the decode ignores the abort poll until it has finished one band (progressive) or
  `kResumeMinRows` MCU rows (baseline; start with 4). The state is one counter in the decode call's locals.
- This replaces the doc's "longer settle after a resume that parks at once": a longer settle delays every image, and
  the minimum progress bounds the loss without a timer.
- Memory: none. Flash: about +150 B (estimate). No cache change.
- Cost to the reader: a page turn during a resume waits one band longer. One band of a 1/2-scale progressive decode
  is tens of milliseconds on the C3 (estimate); the PR states the measured worst band from `ImageParkTest`'s host
  clock and the device figure stays open until a log shows it.
- Tests: `test/epub_pipeline/ImageParkTest.cpp` gains a case that raises input before the first poll of a resume and
  asserts the parked row advanced. Open a `host_clock::Ticking` scope, or the test compares the blocking path with
  itself.

**5b. Doc correction.** Size S, docs only: the extraction paragraph as above.

**5c. Interrupting the extraction: do after a device measurement.** Measure on the X3 with the existing `LOG_INF`
lines (the lane's per-attempt line at `EpubReaderActivity.cpp:2594`): in a book of large JPEGs, read at one page every
2 s for 50 pages, count the page turns that waited more than 500 ms behind an extraction. If more than one turn in
ten does, the design is to stop at the next inflate chunk and keep going later: the `.part` file stays, and the state
to keep is the ring (up to 32 KB) and `uzlib_uncomp` (about 1.3 KB). On the C3 that state has nowhere to live between
slices but the card (33 KB written per park), which costs about what finishing costs. So on the C3 the answer is
probably no; on the boards where `HalCapabilities::buildsInPsram()` is true the state can stay in PSRAM. Size M to L.

**5d. PNG and the resized 1/8 decode: not worth it.** A PNG's state is the same 32 KB ring plus two scanlines, with
the same card cost. The resized decode is §6.

## 6. A cover that fits only at 1/8 restarts on every key press

**Verdict: do after a device measurement.** Size S plus M if built. It shares its decoder change with §7.

**Why measure first.** Read: `chooseProgressiveShift` (`JpegToBmpConverter.cpp:797-805`) leaves a cover to the resize
only when the lent region lacks the workspace or free heap is under 16 KB, or, with no region, when free heap is under
the workspace plus 16 KB. Home lends the secondary buffer (`HomeActivity.cpp:244`), and a 1/4 workspace for a 1,600 px
wide 4:2:0 file is about 19 KB (estimate from `ProgressiveJpeg.cpp:363-371`), so in a 48-52 KB region the 1/8-only
case needs a cover about 8,000 px wide or a starved heap. On the S3 boards the region is 256 KB and the case does not
arise.

**The measurement.** X3, dev build, a library of 30 books with progressive covers of 1,400 px and wider: open Home and
the file browser's cover view, and count the `Progressive cover read N times` lines (`JpegToBmpConverter.cpp:359`),
which only this path prints. Build it if any cover takes that path on a normal open with the buffer lent. If it shows
only with the buffer released, fix the lend, not the decoder.

**The design, if built.**

- **6a. `ProgressiveJpeg::ResizedDecoder`.** The steppable `Decoder` (`ProgressiveJpeg.h:129-170`) already runs a
  DC-only session. A resized decode adds a `RowScaler` (`ProgressiveJpeg.cpp:1421-1506`, about 36 B) and its three row
  buffers (3 × output width bytes). The rows come from the lent region when the caller passes one, else
  `makeUniqueNoThrow<uint8_t[]>`; a refusal fails `begin()` and the caller draws the placeholder as today.
  `decodeResized` (`:1520`) becomes a loop over it. Size S.
- **6b. A third mode of `JpegThumbSession`** (`JpegToBmpConverter.cpp:1095`) that feeds every output box from one
  decode, where `convertResized` (`:369`) runs one decode per box (`:999-1008`). That halves the card reads of §7 as
  well. Size M.
- Do not take the ten-line alternative (ignore the abort after half the decode): it trades the restart for input lag
  on the Home screen.

## 7. A 1/8 decode of a file with libjpeg's scan script reads its DC refinement

**Verdict: do now.** Size S. The doc says measure first; the change is smaller than the measurement.

**Read.** The DC session's index stops only when the last luma DC scan recorded has `al == 0`
(`ProgressiveJpeg.cpp:610-616`). With libjpeg's default script that is the DC refinement, 55-80 % into the file. The
first DC scan stores `predictor << al` (`:784`) and the refinement ORs one bit in (`:790`).

**Design.** A flag in `DecodeOptions`, `firstDcScanOnly`. With it `indexStep` stops at the first luma DC scan
(`st.scanCount > 0` at `:615`) and the bands decode that scan alone. Only `decodeResized` (`:1520`) sets it; the
sliced 1/8 decodes and the reader's 1/8 path do not, so their goldens stand.

**What it costs in the picture.** One missing DC bit is at most one quantised DC step: `q_dc / 8` grey levels at 1/8
scale, 0.25 to 2 levels for the usual `q_dc` of 2 to 16, before a dither to one or two bits. The doc's host comparison
found a mean difference of 0.1-0.9 levels.

**Memory.** None. **Flash.** Under 100 B (estimate). **Cache.** None: a cover BMP already written stays.

**Tests.** `JpegDownscaleTest`: a fixture with libjpeg's default script (`cjpeg -progressive`; add it to the
generator that makes the progressive fixtures) decoded both ways. Assert the card bytes read fall to the first DC
scan's extent and the largest pixel difference is at most `q_dc / 8` rounded up.

**Risk.** A file whose first DC scan has `al` of 2 or more loses two bits. Guard it: take the short path only when the
first DC scan's `al` is 0 or 1.

## 8. A cut or corrupt progressive JPEG draws a placeholder

**Verdict: not worth it; leave it parked.** One reduced fix is worth doing.

**Why.** Nobody has reported a damaged JPEG. The branch (`origin/archive/progressive-jpeg`, `b623ca5a1`) is 1,727
commits behind master, and a trial merge conflicts in `ProgressiveJpeg.cpp/.h`, both converters, `Epub.cpp` and six
test files: taking it is a re-port (size M), with the permanence risk the doc states.

**8a. A failed baseline decode draws the placeholder, and a page does not decode it eight times. Do now.** Size S.

- Read: a cut baseline file returns false with nothing drawn (`JpegToFramebufferConverter.cpp:2068-2072`); a
  progressive one returns `framedGap` (`:2062-2067`). An empty space where a picture belongs is content missing
  without a sign. Return the same placeholder for both.
- Read: nothing remembers a failure outside the lane's session list (8 slots, 2 tries, `EpubReaderActivity.h:416-422`),
  so every pass of a foreground render decodes the file again (X4: 8 decodes, about 14 s a visit, recorded).
- Design: the reader keeps a fixed array of the hashes of images that failed with a decode error (not an I/O error,
  not a preemption) in this session: 8 entries of 4 B in `EpubReaderActivity`, no allocation, cleared on `onExit`. A
  render that finds the hash draws the placeholder without decoding. Nothing is written to the card, so a false
  detection lasts until the book is closed.
- Flash: about +200 B (estimate). No cache change.
- Tests: `ImageParkTest` or `JpegDownscaleTest` with a baseline file cut at half its length: one decode attempt per
  session, placeholder drawn.

## 9. Per-line and per-cell objects are still on the heap

**Verdict: do now, in three PRs, the second and third gated on a host count.** Sizes S, M, S.

**The doc's next step is heavier than it needs to be.** It proposes an arena-aware deleter behind `Page::elements`,
which changes the element type at 47 lines. C++20 destroying `operator delete` does the same with no change to any
`std::unique_ptr` type: the class's own `operator delete` reads a flag from the object, runs the destructor, and frees
only an object the heap made. The build is `-std=gnu++2a` (`platformio.ini:83`) and GCC has had it since version 9.

**9a. A cell's line list is allocated once. Do first.** Size S.

- Read: `layoutTableRow` pushes each line into `cell.lines` without a reserve (`:6866`), so a four-line cell
  allocates three times and frees twice (1, 2, 4 entries), all inside one row's layout.
- Design: collect a cell's lines in one parser member, `std::vector<std::unique_ptr<TextBlock>> cellLineScratch_`,
  reserved to `MAX_CELL_LINES` (64 pointers, 256 B on the C3) through `heapReserveFor` at the table's first grid row
  and freed at `</table>`. At the cell's end allocate `cell.lines` at its exact size (`heapReserveFor(cell.lines, n)`)
  and move the pointers. A refusal of either makes `layoutTableRow` return false, and the row goes out as paragraphs,
  which is its existing exit (`:6838-6840`).
- Memory: 256 B for the table's life (C3), heap, asked first. The S3 path is the same.
- Flash: about +100 B (estimate).
- This also settles the cell-lines site of §10d.

**9b. The `TextBlock` object goes where its bytes go.** Size M.

- Read: `ParsedText` builds each line with `makeUniqueNoThrow<TextBlock>(*line, *xs, blockStyle, lineArena_)`
  (`ParsedText.cpp:4405`). The line's bytes already come from `lineArena_` (the page block for prose, the row block
  for a cell); the object does not. One site serves both prose and cells.
- Design: when `lineArena_` is set and has room for `sizeof(TextBlock)` beside the reserve below, construct the object
  there (`new (lineArena_->alloc(sizeof(TextBlock), alignof(TextBlock))) TextBlock(...)`) with `objectInArena_ = true`;
  else `new (std::nothrow)` as today. `TextBlock` gains
  `static void operator delete(TextBlock*, std::destroying_delete_t)`, which runs the destructor and calls
  `::operator delete` only when the flag is clear.
- Lifetime: the object dies with its `PageLine` or its `TableCell`, before `releasePageBlock()` rewinds the block
  (`:1851`, and the destructor at `:5172`). That order already holds for the line's bytes; the object adds no new
  constraint.
- **The walk reserve.** An object placed in the page block takes room from whatever is allocated above it later on
  that page. The image-header walk's 32 KB stage needs 33,312 B and leaves 64 B free on the X4 at a 1-2 KB page block
  (recorded, 2026-09-26); pushed out of the arena it goes to the heap within a budget or the header is degraded
  (`EpubImageManifest.cpp:378-381`). So: while the chapter's image manifest still has headers to resolve, an object
  goes to the arena only if `capacity - used` stays at or above 34 KB after it; otherwise to the heap as today. With
  no header pending there is no reserve. Expose "headers pending" from `EpubImageManifest` as one `bool`.
- Worst-case size: `sizeof(TextBlock)` is 88 B on the 64-bit host and about 56 B on the C3 (estimate: its pointers
  halve). A 60-line page adds 3.4 KB to the page block on the C3; a usual 30-line page 1.7 KB.
- The word-cache tail (`WordShapeCache`): the long Hindi chapter's parse peak sits 13.6 KB (X3) and 9.3 KB (X4) under
  the tail (recorded, 2026-11-06). 3.4 KB more keeps both under it.
- C3 path: as above. S3 path: the 256 KB region always has room, so every object goes to the arena.
- Flash: about +200 B (estimate).

**9c. The `PageLine` goes in the page block.** Size S, after 9b and §10a.

- `addLineToPage` (`:5689`) constructs the `PageLine` in the page block under the same room test, with a flag in
  `PageElement` and a destroying `operator delete` on `PageElement` that calls the virtual destructor and frees only a
  heap object. `PageElement` is the first and only base, so the pointer is the allocation's address.
- `PageElement` grows from 8 to 12 B on the C3 (estimate: a `bool` after two `int16_t` and the vtable pointer). A page
  loaded for rendering pays 4 B a line on the heap, about 120 B a page.
- The five `Page::deserialize` sites (`Page.cpp:705-721`) stay on the heap: a loaded page outlives any arena scope
  except in a mid-build draw, and that is a later step if a measurement asks for it.

**Not done: the vectors.** `TableRow::cells`, `TableCell::lines`, `PageTableFragment::rows` and `Page::elements` stay
`std::vector`s on the heap. Moving them needs an allocator type in `Page.h` and its 47 users; after 9a each is one
allocation of known size.

**The gates.**

- 9a merges on the goldens being byte-identical.
- 9b and 9c merge on: goldens byte-identical; `HeapPeakRegression` not above its baseline; and the allocation count
  of one build from `epub_pipeline_dump --bench` on Linux (CI or WSL; the interposer does not run under MinGW) quoted
  in the PR for `moby-dick` and `test_table_streaming`, before and after. The recorded count to beat is 67,342
  allocations for `moby-dick` (host, 2026-09-25).
- Before the release that carries them: the X3 table chapter with `-DSCT_HEAP_TRACE=1 -DEHP_FORCE_BLOCKING_BUILD`,
  the A/B method of [Memory Allocation Strategy §8.4a](../../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness).
  The recorded figure to beat is a block count rising from about 387 to 700-750 on the table pages (X3, 2026-09-26).
  And an X4 build of an image-heavy chapter, checking that no image header is degraded that was not before.

**Risks.** An object that outlives its block is a use after rewind; the debug build should fill a released block with
a pattern on the host (`BUILD_ARENA_TRACE`) so a golden run catches it. A second `delete` path that bypasses the class
(`::delete p`) would free arena memory to the heap; grep for it in review.

## 10. Throwing allocations left on the section-build path

**Verdict: do now, five PRs.** Sizes S, S, M, M, M. This is the item that ends a reboot loop, so it starts first.

Every PR adds a fault-injection test in the style of `PAGE_FAULT_INJECTION` (`ChapterHtmlSlimParser.cpp:2077-2083`,
`test/epub_pipeline/CMakeLists.txt:249-253`): a counter that makes the Nth request of that kind refuse, and a test
that walks N over a fixture and asserts the build ends with its pages kept, the section marked truncated, and no
abort.

**10a. `Page::elements` grows through one helper.** Size S.

- `bool ChapterHtmlSlimParser::addPageElement(std::unique_ptr<PageElement> element)`: `heapReserveFor(currentPage->elements, 1)`,
  then `push_back`; on a refusal `stopForHeap("a page's element table", ...)` and false. The nine `push_back` sites
  (`:2349`, `:2607`, `:4169`, `:5695`, `:6370`, `:6576`, `:6634`, `:6669`, `:6948`) call it; the `insert` at `:2041`
  already asks.
- Worst case: a doubling from 32 to 64 pointers is 256 B on the C3. Heap, asked first. The vector cannot live in the
  arena without an allocator type (§9).
- A refused element's caller returns as it does when `ensurePage()` refuses: the line is not placed and the parse has
  stopped, so the page before it is the section's last.
- Flash: about +150 B (estimate), less the nine inlined growth paths.
- Test: `PageHeapTest.cpp` gains `ELEMENT_FAULT_INJECTION` over a page of more than 32 elements.

**10b. The parser object and the TOC anchors.** Size S. `Section.cpp` only.

- `st.visitor = std::make_unique<ChapterHtmlSlimParser>(...)` (`Section.cpp:1511`) becomes `makeUniqueNoThrow`. On
  null the build does not start: log the size and take the exit the other setup refusals take in that function. The
  PR states `sizeof(ChapterHtmlSlimParser)` from the host and the C3 map file.
- `tocAnchors` (`Section.cpp:1496-1506`) is a `std::vector<std::string>` whose length the book decides: a book that is
  one XHTML file puts every TOC entry in one spine. Replace it with one packed pool, `id\0id\0...`, as the page-list
  anchors already are (`externalPageBreakAnchorPool`, `ChapterHtmlSlimParser.h:812-816`). Two passes over
  `getTocItem`: sum the lengths, then fill.
- Where the pool comes from: the build arena at setup when it has room (it is build-lifetime and allocated before the
  page block opens, so it fits the arena's order), else `makeUniqueNoThrow<char[]>`. A refusal of both: the build does
  not start. A TOC anchor drives a page break and navigation, so it is not dropped.
- Worst case: the sum of the spine's TOC anchor lengths plus one byte each. 500 entries of 20 B are 10.5 KB; a usual
  chapter has one to five entries, under 200 B. If the pool would take more than a quarter of the arena, take it from
  the heap instead so the parse lane keeps its room.
- The parser's three `std::find` uses (`:2769`, `:2795`, `:2959`) become one
  `isTocAnchor(std::string_view)` that walks the pool.
- Flash: about +200 B (estimate).

**10c. The per-element stacks.** Size M.

- `inlineStyleStack` (seven push sites), `listStack`, `containerWidthStack_`, `backgroundImageScopes_`,
  `heldBottomSpacing_`, `blockInsetStack_`, `anchorsAwaitingLine_`.
- One helper, `template <typename V, typename T> bool pushOrStop(V& v, T&& x, const char* what)`: `heapReserveFor(v, 1)`,
  else `stopForHeap(what, ...)` and false. Every push site uses it and returns on false as the sites around it do
  after `flushPartWordBuffer()` refuses.
- At `setup()` reserve each stack to 16 entries through `heapReserveFor`, before the page block opens, so a usual
  chapter never grows one mid-parse (a growth step leaves a hole). `StyleStackEntry` is about 28 B: 448 B for the
  style stack, under 1 KB for all seven (estimate). A refusal at setup is not fatal; the first push asks again.
- Depth bounds them: `SaxParser` flattens nesting past 64 (`kCapSaxDepth`), so the worst case is a small multiple of
  64 entries, about 2 KB for the style stack.
- Heap, not arena: each is a `std::vector` that grows, and the arena only rewinds newest-first.
- Flash: about +300 B (estimate).
- Test: `STACK_FAULT_INJECTION` over a fixture nested 20 deep with styled spans and lists.

**10d. Table rows and cells.** Size M. After §1 and §9a, which touch the same lines.

- `pendingRow.cells` (`:3285`, `:6759`): `heapReserveFor(row.cells, 1)` before `emplace_back`. On a refusal degrade the
  row (`degradeRow("no heap for the row")`), which writes its cells out as paragraphs and keeps their text, then let
  the cell being opened stream as a degraded row's does.
- `out.cells.reserve(columnCount)` (`:6827`): `heapReserveFor(out.cells, columnCount)`; false returns false from
  `layoutTableRow`, the paragraph exit. At most 8 × `sizeof(TableCell)`.
- `t.packer.rows.push_back` (`:6173`, `:6183`): `heapReserveFor(t.packer.rows, 1)`. On a refusal flush the fragment
  (`flushTableFragment`), which empties `rows`, and ask once more; then `stopForHeap`. At most 48 rows.
- Flash: about +250 B (estimate).
- Test: extend `TABLE_CELL_FAULT_INJECTION` (`:6776-6782`) to these three kinds; the degraded rows' words must all be
  in the dump.

**10e. Strings.** Size M. Last, because it starts with a census.

- List every `std::string` member of `ChapterHtmlSlimParser` and every local built from an attribute or from text.
  Classify each: bounded by a constant (reserve it once through `heapReserveFor` at first use: `rubyText_`, 255 B),
  or sized by the document (an `id`, an `href`, a `class`, a footnote preview).
- A document-sized string gets a length limit before the copy. An `id` or `href` over 255 B is not recorded
  (`noteCapOverflow` with the anchor or href flag); no text is lost. A class attribute is passed on as a view where the
  callee allows it.
- The PR's description holds the census table; a string it leaves throwing is listed with its bound.

**An optional net: owner decision, recommendation not now.** A `std::new_handler` that frees a ballast block held for
the build and latches a flag the parser turns into `stopForHeap` would catch every site the five PRs miss. It costs
4 KB of heap for the length of every build on the C3, which lowers the free-heap readings the CSS resolver and the
table gates decide on, and libstdc++'s retry loop through the handler is unverified on the device. Take it only if a
crash log shows an abort from a site the census did not list. The S3 boards would not hold a ballast
(`HalCapabilities::buildsInPsram()`).

## 11. `shrink_to_fit()` sweep

**Verdict: do now.** Size S. One PR; it touches one line of the parser.

- Thirteen of the fourteen vector calls become `std::vector<T>().swap(v)`: `SerialTransferDevice.cpp:215`,
  `FileBrowserActivity.cpp:167`, `FileBrowserModel.h:219`, `FileBrowserModel.cpp:242,244,246,545,818,823,905`,
  `BookRowResolver.cpp:163`, `EpubReaderActivity.cpp:1024`. Read each first: where the vector is filled again at once,
  delete the call instead, since freeing and regrowing is the churn the rule exists to stop.
- The fourteenth, `t.pendingRow.cells.shrink_to_fit()` (`ChapterHtmlSlimParser.cpp:6714`), is deleted, not swapped:
  the next `<tr>` fills the same vector, and its capacity is at most nine cells.
- The two strings on kept text: `ChapterHtmlSlimParser.cpp:2299` and `XtcParser.cpp:72-73` become
  `if (heapHasBlockFor(s.size() + 1)) std::string(s).swap(s);`. The shrink allocates the small block before it frees
  the large one, so it asks first.
- The string calls after `clear()` stay as they are; they free.
- Memory: this frees memory the code believed it had freed. No new allocation except the two string copies.
- Flash: about 0 (estimate).
- Test: none new on the host for the UI files. The PR states, per site, what the vector held and its usual capacity.
  For `FileBrowserModel` quote the device's `STATE` free heap before and after leaving a folder of 500 entries when a
  device is free; it is not a merge gate.

## 12. CSS in a chapter's `<style>` blocks is never applied

**Verdict: do after a host confirmation, and after §13.** Size L.

**Confirm first, on the host.** Write `test/epubs/make_test_style_block.py`: a book with no stylesheet in the
manifest whose chapters each carry a `<style>` block (alignment, italic by class, `display: none`, a margin). Its
golden today shows unstyled text and the `display: none` text visible. That golden is the confirmation; no device is
needed. The work is justified once one real book of that kind is named in the PR (hand-made and Sigil books do this;
Calibre moves the blocks into a sheet).

**The design: a chapter overlay through the existing compile, not a second rule store.**

- §13b adds a head scan between a build's extraction and its parse. The scan also collects the text of `<style>`
  elements, up to a cap, into the build arena (it runs before the parser's setup, when the arena holds only the CSS
  ruleset and the feed chunk).
- The cap is 16 KB on the C3 (matcha's figure) and 64 KB where `HalCapabilities::buildsInPsram()` is true. A block
  past the cap is compiled up to the last whole rule inside it, and the section is marked `kStatusCssDegraded`.
- The text is compiled by a second `CssParser` whose cache file is the spine's own (`style_<spine>.bin` beside the
  section file; the constructor takes a file name where it takes only a folder today). `beginCacheCompile`,
  `appendCompiledFromStream` and `endCacheCompile` are used as they are. The compile's heap need is the gate
  `Epub::parseCssFiles` already applies (24 KB plus the text's size, `Epub.cpp:795`, `:838-839`); refused, the chapter builds
  without the block and is marked `kStatusCssDegraded`, the status that already rebuilds a chapter when the heap
  allows.
- The overlay loads resident into the build arena after the book's ruleset (`setIndexArena`), as the sparse records
  the book's rules use: the compression that pays here (see the codec section).
- The cascade: `CssParser::resolve` (`CssParser.h:513`) feeds one `Cascade` from both parsers. A rule's rank is
  `specificity << 16 | order` (`CssParser.cpp:126-127`); the overlay's orders are offset by 0x8000 so a `<style>` rule
  beats a linked one of equal specificity, as it does in a document whose `<style>` follows its `<link>`s. The book's
  orders stay under 0x8000: `MAX_RULES` is 1,500.
- A `<style>` in the body is not collected. The PR says so in the doc.
- Cache: bump `SECTION_FILE_VERSION`. The overlay file is deleted with the book's CSS cache.
- Flash: about +1.5 KB (estimate).
- Tests: the fixture's golden; a specificity case (a linked `p.x` against a `<style>` `p`); a block over the cap; a
  compile refused by `CSS` fault injection.

**Risk.** The resolver is the densest code in `lib/Epub`. Other readers of the book's CSS (`BookFonts.cpp`, the body
size measurement, footnote previews) do not see the overlay; say which do and which do not in the PR.

## 13. Every manifest stylesheet applies to every chapter

**Verdict: do now, two PRs.** Sizes M and M. It is a wrong rendering of a correct book, and omnibus editions whose
parts each ship a sheet with the same class names hit it as well as matcha's case.

**The doc overstates the cost.** It expects a rule-cache format change and a 4 KB head scan per build. Read: a rule's
`order` comes from one counter that runs across the sheets in manifest order (`nextRuleOrder_`, `CssParser.cpp:1779`,
`:1918`), so **each sheet already owns one contiguous range of orders**. Scoping a chapter to its sheets is a filter on
`order`; the rule records do not change.

**13a. Record each sheet's order range.** Size M. No change in behaviour.

- `setStylesheetPath` (`CssParser.cpp:3972`) starts a sheet. Record `{path hash, first order}` per sheet in the cache
  header (after the complex-rule table) and bump `CSS_CACHE_VERSION` (35 today). At most 64 sheets; a book with more
  folds the rest into one "always applies" range.
- A redeclaration merges into the staged record when `existing.order + 1 >= order` (`:1870`, `:1910`). That must not
  merge across a sheet boundary: advance `nextRuleOrder_` by 2 at each sheet's start.
- `@import`: the compiler does not read it (no occurrence in `CssParser.cpp`). Today an imported sheet applies only
  because every manifest sheet applies. Record the imports at the top of each sheet as edges `{importer, imported path
  hash}` in the same header, so 13b can follow them. Without this, 13b unstyles every book that imports.
- Memory: the table is 8 B a sheet, read into the parser with the index (heap, `heapReserveFor`; or the arena when the
  index is resident). 64 sheets are 512 B.
- Flash: about +400 B (estimate).
- Tests: a CSS unit test that compiles two sheets with the same selector and reads two rules back with their ranges;
  an import edge round trip.

**13b. A chapter resolves only against the sheets it links.** Size M.

- The head scan: after the extraction and before the parser's setup, read the start of the chapter's inflated XHTML
  (already on the card as `html_<spine>.bin`) up to `</head>` or 8 KB, and collect the `href` of each
  `<link rel="stylesheet">`. One 512 B stack buffer and a small tag matcher; no SAX state. Resolve each href against
  the chapter's path, hash it, look it up in 13a's table, follow the import edges, and build the chapter's set of
  order ranges: at most 64 `{first, last}` pairs, 256 B, in the parser object.
- The filter: `lookupRules` and the complex-rule scan skip a rule whose order is outside the set. The resident index
  entry already holds `order` (`ResidentEntry`, `CssParser.h:477-481`); the disk record does too.
- **Fail open.** If a chapter links a sheet the table does not know (an odd relative path, a case mismatch, a sheet
  that was skipped at compile for size or heap), the chapter resolves against every sheet, as today, and logs it. A
  chapter with no `<link>` at all resolves against none; that is what the document says.
- The other callers of `resolveStyle` (`BookFonts.cpp`, two calls) stay unscoped.
- Memory: 256 B in the parser object, no allocation. The C3 and the S3 path are the same.
- Cache: bump `SECTION_FILE_VERSION`.
- Flash: about +600 B (estimate).
- Tests: `make_test_css_scoping.py`: two chapters that link different sheets with conflicting `body` and class rules;
  one sheet no chapter links (`body { text-align: center }`, matcha's case); a chapter that links through `@import`; a
  chapter with a broken href (fails open). Goldens for each.

**Risk.** A book that works today only because an unlinked sheet applies loses that styling; a browser shows it the
same. 13b logs a chapter that resolves against no sheet, so such a report can be read from its log.

## 14. An inline anchor lands a page off

**Verdict: do now.** Size S to M. After §10a (same function). The mechanism exists for footnote links.

**Read.** A footnote link waits in `pendingFootnotes` with the index of the word it follows (`:4998-5005`), and
`addLineToPage` hands it to the page of the line that reaches that index (`:5660-5669`). An inline id has no such
index: `queueAnchorForNextLine` (`:5187`) records it at the next line placed, which for an id in the middle of a long
paragraph is the paragraph's first line, possibly a page earlier.

**Design.**

- `anchorsAwaitingLine_` holds `{std::string id, int wordIndex}`. An id on a block element keeps index 0: the block's
  first line. An id on an inline element opened while its block has words (`<a id>`, `<span id>` through
  `recordSpanAnchor` at `:5205`, and the other inline formatting tags) gets
  `wordsExtractedInBlock + currentTextBlock->sourceTokenCount() + 1`, the index of the next word, computed as `:4998`
  computes a footnote's.
- `flushAnchorsAwaitingLine` (`:5179`) runs after `wordsExtractedInBlock` has been advanced for the line (move the
  call below `:5660`) and records the entries whose index the line has reached; the rest wait.
- At the block's end, and at the chapter's, every entry still waiting is recorded at the current page, so an id after
  the block's last word is not lost.
- `pendingAnchorId` (`:2962`) stays for ids that precede their block.
- Memory: 4 B more per queued entry, at most `MAX_ANCHORS_AWAITING_LINE` (16) entries. The push already goes through
  §10c's helper once that lands; until then it is the capped `push_back` it is today.
- Cache: bump `SECTION_FILE_VERSION`; the anchor map's page numbers change.
- Flash: about +150 B (estimate).
- The link Back stack's one-page tolerance (`EpubLinkBackStack.h:73-75`) stays: an id can still sit at a line that a
  later hyphenation retry moves.

**Tests.** `AnchorPageAccuracyTest`: a paragraph long enough to cross a page with an `<a id>` and a `<span id>` in its
second half; both resolve to the second page. `AnchorMapTest` for the block-end case. The goldens do not print
anchors, so a green golden run says nothing here.

## 15. JPEG source reads of 512 bytes

**Verdict: do after a device measurement.** Size S if built.

**Why measure.** The one figure the repository holds is 0.47 ms per 512 B refill on the X4
(`docs/future_work/cjk.md:244`, recorded), a quarter of matcha's 2 ms. At that rate a 500 KB baseline JPEG spends
about 0.46 s in reads (976 refills, estimate), and matcha's 22 % is not to be expected. There is no X3 figure.

**The measurement.** X3 and X4, dev build, one measurement-only patch: accumulate `micros()` inside `tjpgInput`
(`JpegToFramebufferConverter.cpp:364-373`) and log it with the decode's total for five cold baseline decodes of a
300-600 KB JPEG. Build 15a if reads are 15 % or more of a cold decode on either board.

**15a, if built: raise `JD_SZBUF`, no wrapper.** Read: TJpgDec takes its input buffer from its own pool
(`alloc_pool(jd, JD_SZBUF)`, `tjpgd.c:965`) and refills it whole (`:291`), aligned to `JD_SZBUF` after the SOS
(`:1064-1067`), so SdFat sees aligned multi-sector reads.

- `JD_SZBUF` 2048 (`lib/TJpgDec/src/tjpgdcnf.h:15`) and `TJPG_WORK_POOL_SIZE` from 12 KB to 14 KB in both converters
  (`JpegToBmpConverter.cpp:42`, `JpegToFramebufferConverter.cpp:379`). The pool comes from the `image_scratch` arena
  first, else the heap (`JpegWorkPool`), as today; the heap floors follow the constant.
- The baseline checkpoint stores a file position (`:2030`) and is not affected.
- A read-ahead wrapper as matcha built it is a second buffer and a second copy; the pool already is the buffer.
- `ProgressiveJpeg`'s read windows (`ProgressiveJpeg.cpp:44-45`) serve random access and are not reusable here.
- S3 boards: the SDK's sector read-ahead already serves them (`HalCapabilities::sdUsesSpi()` is false there). The
  constant is compile-time; the 2 KB more comes from PSRAM.
- Flash: 0. RAM: +2 KB per decode, from the arena when one is installed.
- Test: the JPEG goldens are byte-identical; `JpegDownscaleTest` asserts a file with a DHT segment over 512 B, refused
  today (`JDR_MEM2`, `tjpgd.c:986`), now decodes.

## Order of work

**Tracks.** A track is serial; tracks run in parallel. The line ranges are the parts of
`ChapterHtmlSlimParser.cpp` a PR touches.

| Track | PRs in order | Parser regions |
|---|---|---|
| T, tables | §1 nested tables → §9a cell lines → §10d table rows and cells | `:3157-3300`, `:4435`, `:4890-5095`, `:6165-6185`, `:6759-6920` |
| L, lines and pages | §10a `addPageElement` → §9b `TextBlock` (`ParsedText.cpp`, `TextBlock.h`) → §9c `PageLine` → §14 inline anchors | the nine push sites, `:5655-5700`, `:5179-5230`, `:2946-2967` |
| B, blocks | §2 trailing `<br>` → §10c stacks → §10e strings | `:2741-2755`, `:4058-4090`, the stack push sites |
| C, CSS | §13a sheet ranges → §13b scoping → §12 `<style>` (after its fixture) | `CssParser.*`, `Section.cpp`, `Epub.cpp`; the parser's `resolveStyle` calls only |
| J, JPEG | §7 first DC scan; §5a resume progress; §8a failed baseline; then §6 and §15 after their measurements | none |
| M, small | §10b parser object and TOC pool (`Section.cpp`); §11 `shrink_to_fit`; §3a hint strings; §5b doc | `:6714` only (§11) |

**What can run in parallel.** One PR from each of T, L, B, C, J and M at a time. T, L and B all edit
`ChapterHtmlSlimParser.cpp`, in the disjoint regions above; the second to merge rebases, and the conflicts are in
context lines only. Within a track the order is fixed. Two cross-track points:

- §11 deletes `:6714`, inside track T's `degradeRow`. Merge §11 before §10d, or leave that line to §10d.
- §10b changes how the parser reads `tocAnchors` (`:2769`, `:2795`, `:2959`), next to §14's anchor code. §10b goes
  before §14.

**Section versions.** Five PRs bump `SECTION_FILE_VERSION`: §1, §2, §13b, §14 and §12. §9 and §10 bump nothing. Each bump
rebuilds every chapter of every book once. Merge them one at a time, each taking master's number plus one at merge,
and land them inside one release so a reader pays the rebuild once per release, not five times.

**The first three to start**, one per parser track, all independent:

1. §10a, `addPageElement` (track L): the smallest change that turns a reboot loop into a truncated chapter.
2. §1, nested tables (track T): content that is dropped today, with a fixture and a measured starting point.
3. §2, trailing `<br>` (track B): visible on every Calibre contents page.

Tracks C, J and M have no parser overlap and start whenever someone is free: §13a, §7 and §10b first.

## Not settled

- **Sizes on the C3.** `sizeof(TextBlock)`, `sizeof(PageLine)`, `sizeof(StyleStackEntry)` and
  `sizeof(ChapterHtmlSlimParser)` are estimates from the host's. Each PR that depends on one reads it from a firmware
  build.
- **Allocation counts.** None was taken: the host heap tools do not run on this Windows host. §9's gates name the
  Linux run that supplies them.
- **§2's rule is designed from the code and the browser's behaviour, not prototyped.** Its goldens decide whether the
  last row of its table ships.
- **§13a's `@import` finding** rests on a search of `CssParser.cpp` that found no handling. If the tokenizer follows
  imports somewhere else, the edges are unnecessary; check before building them.
- **§13b's merge rule.** Whether a redeclaration merges across sheets today was read at `:1870` and `:1910` only.
- **§5a's band time, §6's trigger rate and §15's read share** need the device runs their sections name.
- **§3**: the hint is the owner's decision.
- **§10's `new_handler` net** is unverified on the device and not recommended.

## Amendments during implementation

Where the code or a measurement disagreed with a design above, the implementation followed the evidence. Each
amendment names its PR.

- **§1, nested tables (#832).** Laying inner cells out through `currentTextBlock` gave them the reader's paragraph
  spacing, indent, alignment and hyphenation, so with Extra Paragraph Spacing on (the default) they sat at x=0 with
  gaps beside degraded outer cells at x=18. Inner cells take the degraded cell's own path instead (`makeCellText`,
  `openStreamedCell`, `streamClosedCell`), and an outer cell's text after its inner table keeps every picture.
- **§2, `<br>` (#834).** The table's "Today" column was wrong for two rows: `getCombinedBlockStyle` keeps the larger
  `marginTop`, so a run of `<br>` gave one line whatever its length. `currentBlockOwnerDepth_` is not set inside
  `section`, `article`, `aside`, `main`, at body level or for loose text, and an empty text block follows a block
  picture. The rule as built: the blank line goes in `paddingTop` (paddings add); a line is empty when no word or
  block picture was put on it since its block began or since the last block-level element closed; a `<br>` inside
  a table cell leaves the outer flow alone; and the gap a drained block carries is cleared at an `<hr>`.
  `test_br_section_break` chapter 3 now gives the 2 and 3 blank lines its own text asks for.
- **§10b, the TOC-anchor pool.** "The arena when the pool is at most a quarter of it" crowded the inflate ring out of
  the X4's 48 KB lent region for a one-file book (measured, host `--arena=48000`: 200 TOC entries pushed a 33,792 B
  ring onto the heap; 1 entry, none). §8.2 of the memory allocation strategy admits a build-long block into the
  arena only beside every peak it coexists with, and the X4's walk margin is 64 B. On the C3 boards the pool is one
  heap block; the build region takes it only when `HalCapabilities::buildsInPsram()` is true.
- **§13a, stylesheet ranges (#835).** Nothing follows `@import`, so the edges are recorded. A redeclaration did merge
  across sheets on master (into the later sheet's order); a merge guard keeps one record per sheet instead of the
  "+2 order gap", and the table sits at the end of the cache file so the rule bytes stay master's.
- **§5, §9.** §5b rides in the §5a PR. §9b and §9c wait for a Linux `epub_pipeline_dump --bench` allocation count
  (their gate): this PC has WSL without a distribution.
