# A `<br>` inside a degraded table cell does not break the line

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A degraded (streamed) cell's text is one cell paragraph (`makeCellText`, `ChapterHtmlSlimParser.cpp:6921`), and
the `<br>` branch in `ChapterHtmlSlimParser::startElement` (`:4143`) only flushes the word into it (`:4158-4160`), so the
cell's words after a `<br>` continue on the same line. `test/epub_pipeline/goldens/test_br_in_blocks.golden.txt` case 21
("a run of `<br>` in a nested table's cell", `:362-394`) lays "Uniform a / Uniform b / Uniform c" on one line at the
cell's inset (`LINE y=386`). Degraded outer cells behaved this way on master before the first wave; nested inner cells
now match them.

**Why it matters.** A cell whose lines the book breaks with `<br>` runs together on one line, against the
book's markup. Nothing is lost.

**Where.** `ChapterHtmlSlimParser::startElement`, the `<br>` branch (`ChapterHtmlSlimParser.cpp:4143-4188`);
`makeCellText` (`:6921`); `emitCellAsParagraph` (`:7131`), which lays the cell's `ParsedText` out as one paragraph.

**Next step.** Have a `<br>` inside a cell end the cell paragraph's line: a forced line break in the cell's
`ParsedText`. Then check every table golden (`test_br_in_blocks`, `test_table_streaming`, `test_nested_tables`,
`test_tables`, `test_table_grid_edges`, `test_table_cell_overflow`).
