# List markers in a table cell go into the page's flow

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** Read from the code (master 50bc8f291), not probed on a host build. A `<ul>` or `<ol>` inside a `<td>` is
pushed on `listStack` (the list branch of `ChapterHtmlSlimParser::startElement` does not look at the cell), so each of
its `<li>` sets `pendingListMarker_`. `emitPendingListMarker` adds the marker to `currentTextBlock`, the page's block,
while the item's words go to the cell's text (`wordTarget`). The page's block is then no longer empty inside the
table, and the next `<li>`, `<p>` or `<br>` in the cell has `startNewTextBlock` lay the marker out (`makePages`): a line
of its own in the page's flow, which moves the page down under the rows packed for it.

**Why it matters.** Each marker is drawn on a line of its own outside the table. On an arena build the packed rows
then no longer fit where they were packed: `flushTableFragment` stops the build there (`stopOnLinesPastTheirPage`), and
a marker line the page cannot hold emits the page under the packed rows, which `emitPage` catches and stops the build
for. Either way the chapter is cut at the table. Both stops are what is left of the heap corruption the TOC page break
and `<hr>` caused in the same place (`test/epub_pipeline/GridTablePageFlowTest.cpp`).

**Where.** `ChapterHtmlSlimParser::emitPendingListMarker` (`currentTextBlock`, not `wordTarget()`); the list branch of
`startElement`; `startNewTextBlock`, which lays a non-empty page block out.

**Next step.** Add the marker to `wordTarget()`, the open cell's text, ahead of the item's first word, and add a list in
a cell to `GridTablePageFlowTest`: heap and arena builds alike and not truncated, at every page height it tries.
