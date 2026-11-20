# Per-line and per-cell objects are still on the heap

Part of [Memory and parsing](../memory-and-parsing.md). Checked on 2026-11-20 against master b1a0fe019 with 9b (#851)
and 9c.

**Open.** Each grid cell still allocates its own objects on the heap (about 160 B a cell, 11–15 KB for a page of grid
rows, host), and the per-page and per-row vectors stay heap `std::vector`s: `TableRow::cells`, `TableCell::lines`
(`Page.h:198`), `PageTableFragment::rows` and `Page::elements` (`Page.h:260`). Moving those needs an allocator type in
`Page.h` and its users; after 9a each is one allocation of known size. A laid-out line's `TextBlock` object (52 B on the
C3, `TextBlock.cpp:23`) and its `PageLine` (12 B on the C3, `Page.cpp:20`) still go to the heap in a heap build, when the
lent region has no room for them beside the image walk's reserve (9b, 9c, below), for a drop cap's line, and for every
page loaded to be read. The cells' word bytes already come from the row block in the lent region.

**Why it matters.** Block count, not bytes, is what fragments the heap during a build. On an X3 table chapter the block count rose from about 387 to 700–750 on the table pages while the bytes stayed in the arena (2026-09-26).

**Where.** `ParsedText` (the `TextBlock` it creates per line, `TextBlock::make` at `ParsedText.cpp:4405`),
`ChapterHtmlSlimParser` (the `PageLine` per line in `addLineToPage`, `PageLine::make` at
`ChapterHtmlSlimParser.cpp:5966`; a grid cell's lines in `layoutTableRow`), `TableCell::lines` (`Page.h:198`),
`Page::elements` (`std::vector<std::unique_ptr<PageElement>>`, `Page.h:260`).

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
Windows); 78,917 to 51,594 at 52,272 B on CI's Linux (`alloc-counts`).

On the X3 (2026-11-20), master before and after 9b with `SCT_HEAP_TRACE=1` and `EHP_FORCE_BLOCKING_BUILD`, the table
chapter of *Through the Brazilian Wilderness* (appendix B, 68 pages, three tables) built from a cold cache: heap blocks
at a page's end, median 348 to 329 on the prose pages and 536 to 471 on the table pages (the tables' excess over prose
+188 to +142); the build's lowest largest free block 23,540 to 28,660 B; no row degraded and no arena refusal in
either arm, the arena's high water 44,972 and 44,968 B. Still to run: an X4 image-heavy chapter with no header newly
degraded.

**Done (9c).** A laid-out line's `PageLine` is built in the page block under the same rule as its `TextBlock` object
(`PageLine::make`, `Page.cpp:38`, called from `addLineToPage`): both place through `TextBlock::ArenaRoom::placeObject`
(`TextBlock.cpp:196`), so the `PageLine` counts in the page's `displaced` and later line bytes give its room back too.
`PageElement` has a destroying `operator delete` (`Page.h:66`) that runs the virtual destructor and frees only an
element the heap made; `std::unique_ptr<PageElement>` keeps its type. The flag is not in `PageElement`, which would
grow every element of a resident page from 8 to 12 B on the C3 (measured with the C3 and S3 compilers): the delete
asks `getTag()` and reads a spare bit of the line's `TextBlock` (`TextBlock::holderInArena`), so `PageElement` stays
8 B and `PageLine` 12 B (`static_assert`s at `Page.cpp:19-20`). On the host a `PageElement` destroyed after its block was
rewound aborts (`BUILD_ARENA_TRACE`). The drop cap's line stays on the heap with its `TextBlock`: the page block opens
with the page's first line, so on a new page it is not open when the cap is placed. `Page::deserialize` keeps the heap:
a mid-build draw loads its page from the section file (`Section::loadPageFromActiveBuild`), so no live page of a build
is ever drawn.

Allocations of one build on the host (`epub_pipeline_dump --bench`, Windows), master to 9c: moby-dick 54,070 to 30,902
lent 52,272 B and 81,338 to 75,341 lent 48,000 B; test_table_streaming 3,826 to 3,722 and 3,960 (unchanged);
test_nested_tables 5,414 to 5,090 and 5,610 to 5,456; test_br_in_blocks 1,659 to 1,489 and 1,749 to 1,671;
test_jpeg_images 5,635 to 5,511 and 5,635 to 5,515. Heap builds are unchanged (moby-dick 112,661). Every golden is
byte-identical, on the heap and lent at both sizes. Still to run on the devices: the X3 table chapter as for 9b (its
"after" arm is 9c's "before"), and an X4 image-heavy chapter with no header newly degraded.

**Next step.** The grid cells' objects, which a row block could hold as it holds their bytes; then, if a measurement
asks for it, the vectors above.
