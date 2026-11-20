# Throwing allocations left on the section-build path

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-20 (8d1541bf4).

**Open.** 2.52 sent every `Page` allocation through `ensurePage()` (`ChapterHtmlSlimParser.cpp:2073`), which stops the
build like the other heap refusals instead of aborting. These still allocate through the throwing `operator new`, so a
refusal there aborts and reboots (`-fno-exceptions`); counted in `ChapterHtmlSlimParser.cpp`:

- `std::string` growth during the parse, for example the ruby text (`rubyText_`, `:4630-4634`, up to 255 bytes).

**Next step.** Ask `HeapFit` before each growth (`heapReserveFor`, as the elements reserve does) and stop the build on
a refusal (`stopForHeap`, `:2112`). Each site needs a host fault-injection test, as `PageHeapTest.cpp`
(`PAGE_FAULT_INJECTION`, `ELEMENT_FAULT_INJECTION`) does for pages and for `Page::elements` (`addPageElement`),
`StackHeapTest.cpp` (`STACK_FAULT_INJECTION`) for the per-element stacks (`pushOrStop`), and `TableCellTextHeapTest.cpp`
(`TABLE_CELL_FAULT_INJECTION`, the `TableRowGrowth` cases) for a table's rows and cells.
