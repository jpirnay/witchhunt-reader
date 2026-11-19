# Throwing allocations left on the section-build path

Part of [Memory and parsing](../memory-and-parsing.md).

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
