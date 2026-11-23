# Per-line and per-cell objects are still on the heap

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A cell's line list is one allocation (9a, #841), and on an arena build a laid-out line's `TextBlock` and
`PageLine` take the page block (9b, #851; 9c, #856). What that left, and the decisions behind it, are in [Memory
Allocation Strategy §9.5](../../memory-allocation-strategy.md#95-class-d-still-on-the-heap). Still on the heap:

- each grid cell's own objects: about 160 B a cell, 11-15 KB for a page of grid rows (host);
- the per-page and per-row vectors: `TableRow::cells`, `TableCell::lines`, `PageTableFragment::rows` and
  `Page::elements` (`Page.h`). Moving them needs an allocator type in `Page.h` and its users; each is one allocation of
  known size;
- a line's `TextBlock` and `PageLine` in a heap build, where the lent region has no room beside the image walk's
  reserve, for a drop cap's line, and for every page loaded to be read.

The cells' word bytes already come from the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block
count rose from about 387 to 700-750 on the table pages while the bytes stayed in the arena (2026-09-26); after 9b the
table pages' median was 471 blocks at a page's end against 329 on the prose pages (X3, 2026-11-20).

**Where.** `ChapterHtmlSlimParser::layoutTableRow` (a grid cell's `TableCell`, its line list and its picture);
`TableCell`, `TableRow`, `PageTableFragment` and `Page` (`Page.h`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer
([Memory Allocation Strategy §8.4a](../../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving
the line bytes into the arena is done.

**Next step.** The grid cells' objects, which a row block could hold as it holds their bytes; then, if a measurement
asks for it, the vectors above. Still to run on the devices for 9b and 9c: the X3 table chapter for 9c, with the method
of §8.4a (its "before" arm is 9b's "after"), and an X4 image-heavy chapter, checking that no image header is degraded
that was not before.
