# The paragraph after a table loses its margin-top

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** The paragraph after a table collapses its margin-top against the margin-bottom of the paragraph before the
table, as if the table were not there. In `test_table_streaming.epub` spine 3 (`p { margin: 1em 0 }`, 18 px), "Now a
table with more columns than the grid layout supports." sits at y=414, one line under the last cell of the degraded
rowspan table above it (y=390), and "Closing text after the tables." at y=395, the bottom of the grid fragment above it
(`TABLE y=324 h=71`; `test/epub_pipeline/goldens/test_table_streaming.golden.txt`). The paragraphs after the nested
tables of `test_nested_tables.epub` do the same (FIRSTAFTER at y=344 under the last cell at y=320). All are the y
values of the golden's `LINE` and `TABLE` records, page positions in pixels.

**Why it matters.** A table runs into the text that follows it, against the book's CSS. Nothing is lost; it is spacing.

**Where.** `ChapterHtmlSlimParser::applyBlockTopSpacing` takes `collapse = min(lastBlockMarginBottom, marginTop)`, and
only `makePages`, `applyBlockTopSpacing` itself, `emitPage` and the box code (`openBox`, `closeBox`) write
`lastBlockMarginBottom`. A table writes nothing there: a grid fragment is placed by `flushTableFragment`, and a
degraded cell by `emitCellAsParagraph`, which hands the cell's lines to `addLineToPage` without `makePages`. So the
paragraph laid out before the table still holds `lastBlockMarginBottom` when the one after it opens.

**Next step.** Reset `lastBlockMarginBottom` once a table fragment or a degraded cell is placed, and regenerate the
goldens that hold a paragraph after a table (`test_table_streaming`, `test_nested_tables`, `test_tables`,
`test_table_grid_edges`, `test_table_cell_overflow`), checking that each moved line is the paragraph's own margin.
