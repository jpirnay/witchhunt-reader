# A `<br>` inside a degraded table cell does not break the line

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A degraded (streamed) cell's text is one cell paragraph (`ChapterHtmlSlimParser::makeCellText`). The `<br>`
branch of `ChapterHtmlSlimParser::startElement` treats a line inside a table cell as having content, flushes the word
before the `<br>` into the cell's text, and starts a new block in the page's flow, not a line in the cell's text. So
the cell's words after a `<br>` continue on the same line. `test/epub_pipeline/goldens/test_br_in_blocks.golden.txt`
case 21 ("a run of `<br>` in a nested table's cell") lays "Uniform a / Uniform b / Uniform c" on one line at the
cell's inset: the `LINE y=386` record, y being the line's position on the page. Degraded outer cells behaved this way
on master before #832 and #834; nested inner cells now match them.

**Why it matters.** A cell whose lines the book breaks with `<br>` runs together on one line, against the
book's markup. Nothing is lost.

**Where.** `ChapterHtmlSlimParser::startElement`, the `<br>` branch (`lineHasContent`); `makeCellText`;
`emitCellAsParagraph`, which lays the cell's `ParsedText` out as one paragraph.

**Next step.** Have a `<br>` inside a cell end the cell paragraph's line: a forced line break in the cell's
`ParsedText`. Then check every table golden (`test_br_in_blocks`, `test_table_streaming`, `test_nested_tables`,
`test_tables`, `test_table_grid_edges`, `test_table_cell_overflow`).
