# A table cell keeps only its first picture

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A cell holding more than one `<img>` shows the first and drops the rest, silently, whether the table is a
grid or laid out as paragraphs. Probe (2026-11-19, the host dump tool, master 715896630): a grid cell
`GRIDA <img checker/> GRIDMID <img bar/> GRIDEND` dumps all three words and one `CELLIMG` (the checker), and the same
cell in a table a rowspan degrades dumps its words and one block `IMG` (the checker); the bar is in neither. Cells of
a nested table, and a cell's text after its inner table, are the exception: they stream the cell so far at a second
picture and go on holding the next.

**Why it matters.** It drops content without a word: an illustrated table (a plate with its legend, a row of icons)
loses every picture but the first in each cell.

**Where.** The in-cell branch of the image handler in `ChapterHtmlSlimParser::startElement` ("first image per cell
wins") sets the cell's `imageSrc` only while it is empty, unless the cell is `streamed`. A grid row sizes the cell for
that one picture (`layoutTableRow`, `cell.image = buildCellImage(...)`); a degraded cell places it after its text
(`emitCellAsParagraph`, or `emitRowImagesAsBlocks`).

**Next step.** In its own PR, place every picture of a cell. A degraded cell can stream the cell so far and go on, as
a nested table's cell does now; a grid cell needs a decision about how a second picture fits its row's height (stack
them, or degrade the row).
