# Page writes could stage in the lent region

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-20 (8d1541bf4).

**Open.** The page writer gathers each page in a heap buffer held from the parse's start to the build's end
(`BuildState::pageWriter`, `Section.cpp:1982`, freed at `:2218`): 2 KB on the C3s, 4 KB on the PSRAM boards
(`PAGE_WRITE_BUFFER_BYTES`, `:1062-1073`). It took an X3's page write from 5.8 to 1.2 ms (the comment there).

**Why it matters.** It is in use at a lent C3 build's parse peak by design, and the lent region has no room for a
block that lives the whole build beside the extraction ring and the image walk.

**Next step** (design, not measured). On the C3, stage each page in a block of the lent region above the page block,
only inside `emitPage`, where no walk stage runs; flush the page's whole sectors and its tail, then release the block.
Keep the heap writer for the owned 10 KB arena, a region without room, and the S3s. Saves 2 KB at each lent C3
build's peak; costs one partial-sector write a page, so time it on an X3 first. A 1 KB C3 buffer is the cheaper
alternative (not measured). After 9b (#851), which places line objects above the page block in the same region.

From the attribution of master's heap-peak rise (2026-11-19, cb8a9a6ab). Its per-fixture peak figures are not
reliable to about 4 KB (the host heap tracker's snapshots lag), so measure each fixture before and after: #848
gave about a third of what that attribution predicted.
