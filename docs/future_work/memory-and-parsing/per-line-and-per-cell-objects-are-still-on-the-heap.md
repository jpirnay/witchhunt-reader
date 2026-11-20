# Per-line and per-cell objects are still on the heap

Part of [Memory and parsing](../memory-and-parsing.md). Checked on 2026-11-20 against master 8d1541bf4 with 9b (#851).

**Open.** Each laid-out line still allocates a `PageLine` on the heap (24 B on the 64-bit host), and its `TextBlock` object (52 B on the C3, `TextBlock.cpp:23`) in a heap build or when the lent region has no room for it beside the image walk's reserve (9b, below); each grid cell its own objects (about 160 B a cell, 11–15 KB for a page of grid rows, host). Their word bytes already come from the page block or the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line, `TextBlock::make` at `ParsedText.cpp:4405`),
`ChapterHtmlSlimParser` (the `PageLine` per line in `addLineToPage`, `ChapterHtmlSlimParser.cpp:5957`; a grid cell's
lines in `layoutTableRow`, `:7255`, kept at `:7293`), `TableCell::lines` (`Page.h:146`), `Page::elements`
(`std::vector<std::unique_ptr<PageElement>>`, `Page.h:208`).

**Tried.** A heap pool for table line storage was reverted: its reservation pushed contig below the framebuffer ([Memory Allocation Strategy §8.4a](../../memory-allocation-strategy.md#84a-table-layout-and-the-ab-harness)). Moving the line bytes into the arena is done.

**Done (9a).** A grid cell's lines are collected in one parser member (`cellLineScratch_`, reserved to `MAX_CELL_LINES`
pointers at the table's first grid row, freed at `</table>`) and moved into `cell.lines` allocated at its exact size, so a
cell's line list is one allocation. A refusal of either sends the row out as paragraphs.

**Done (9b).** A line's `TextBlock` object is built in the line arena beside its bytes (`TextBlock::make`); a
class-scoped destroying `operator delete` frees only an object the heap made, so `std::unique_ptr<TextBlock>` keeps its
type. On a C3-sized region (below `WordShapeCache::kLargeArenaBytes`), whenever the build has an image manifest, the
object goes there only while the region keeps the walk's largest stage free after it (`kWalkArenaReserve`, 34 KB): on
the X4 a 32 KB walk stage leaves 64 B of the lent region free. While the page's objects hold room their line bytes have
not given back, line bytes that would cross the reserve go to the heap and give it back (`TextBlock::ArenaRoom`). A grid
row whose block is released (laid out again, or refused) takes its share of that count with it, and a heap that refuses
the given-back bytes leaves them in the arena, as before 9b. So on a page whose blocks only grow, or are released that
way, the walk keeps master's room. A bound on a page's text instead was not derivable: up to 16.8 KB with TrueType at
8 pt, and none for SD fonts.

The parser's teardown and `finalize()` destroy the open page's and a table's lines before the page block is rewound, and
the golden corpus is laid out in a lent region too (every lent case at 52,272 B, 15 books at 48,000 B), with a rewound
block filled on the host (`BUILD_ARENA_TRACE`). Allocations of a lent build, moby-dick: 78,775 to 51,452 at the X3's
52,272 B and to 78,720 at the X4's 48,000 B, where the line bytes give back most of what the objects take (host,
Windows); 78,917 to 51,594 at 52,272 B on CI's Linux (`alloc-counts`). Still to run on the devices (the triage's gates
for 9b): the X3 table chapter with `SCT_HEAP_TRACE=1` and `EHP_FORCE_BLOCKING_BUILD`, and an X4 image-heavy chapter
with no header newly degraded.

**Next step (9c).** Construct the `PageLine` in the page block under the same room test (`addLineToPage`), with a flag in
`PageElement` and a destroying `operator delete` on it that calls the virtual destructor and frees only a heap object, as
`TextBlock` does: no change to `Page::elements`' type (41 lines in `lib/Epub` and `src` use it). `Page::deserialize`
(`Page.cpp:705-721`) keeps the heap. Measure allocation counts and `HeapPeakRegression` on the host first.
