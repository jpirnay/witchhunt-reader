# Per-line and per-cell objects are still on the heap

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** Each laid-out line still allocates a `TextBlock` object and a `PageLine` on the heap (88 B and 24 B on the 64-bit host), and each grid cell its own objects (about 160 B a cell, 11–15 KB for a page of grid rows, host). Their word bytes already come from the page block or the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line, `ParsedText.cpp:4405`), `ChapterHtmlSlimParser` (the
`PageLine` per line in `addLineToPage`, `ChapterHtmlSlimParser.cpp:5836`; a grid cell's lines in `layoutTableRow`,
`:7027`, kept at `:7034`), `TableCell::lines` (`Page.h:146`), `Page::elements`
(`std::vector<std::unique_ptr<PageElement>>`, `Page.h:208`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer ([Memory Allocation Strategy §8.4a](../../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving the line bytes into the arena is done.

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
