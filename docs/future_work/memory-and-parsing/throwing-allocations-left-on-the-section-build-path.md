# Throwing allocations left on the section-build path

Part of [Memory and parsing](../memory-and-parsing.md).

**Open (triage spec §10e).** The parser's pages, page elements, per-element stacks, table rows and cells, and the
parser object and TOC anchors themselves, are allocated nothrow or asked for first, and a refusal stops the build with
its pages kept ([Memory Allocation Strategy §9.6](../../memory-allocation-strategy.md#96-pre-size-and-chunk-the-per-page-build-tables)).
What still allocates through the throwing `operator new` in `ChapterHtmlSlimParser.cpp` is `std::string` growth during
the parse, so a refusal there aborts and reboots (`-fno-exceptions`). For example: the ruby text (`rubyText_`,
appended in `consumeText`, at most `TextBlock::kMaxAnnotationText`, 255 bytes), a footnote preview's text, the
lower-cased copy of a `style` attribute, and ids and picture paths copied from attributes.

**Next step.** First the census of spec §10e: list every `std::string` member of `ChapterHtmlSlimParser` and every
local built from an attribute or from text, and classify each as bounded by a constant (reserve it once through
`heapReserveFor` at first use, as `rubyText_` could be) or sized by the document (an `id`, an `href`, a `class`, a
footnote preview). A document-sized string gets a length limit before the copy: an `id` or `href` over 255 B is not
recorded (`noteCapOverflow`); no text is lost. The PR's description holds the census table; a string it leaves throwing
is listed with its bound. A refusal stops the build (`stopForHeap`). Each site needs a host fault-injection test, as
`PageHeapTest.cpp` (`PAGE_FAULT_INJECTION`, `ELEMENT_FAULT_INJECTION`) does for pages and for `Page::elements`
(`addPageElement`), `StackHeapTest.cpp` (`STACK_FAULT_INJECTION`) for the per-element stacks (`pushOrStop`), and
`TableCellTextHeapTest.cpp` (`TABLE_CELL_FAULT_INJECTION`, the `TableRowGrowth` cases) for a table's rows and cells.
