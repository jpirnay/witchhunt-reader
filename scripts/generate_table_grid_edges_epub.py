#!/usr/bin/env python3
"""
Generate a test EPUB for the table grid path's edge cases.

Every case here is one the existing fixtures miss. test_tables.epub covers only
2x2 and 3x2 grids; test_table_streaming.epub covers nesting, rowspan, column
overflow and a grid-compatible colspan. Nothing exercised what happens when the
grid *almost* works and has to degrade, which is exactly where the streaming
rewrite changes the blast radius from the whole table to a single row.

Cases covered:

  1. ch1 — column count changes mid-table.
     A 3-column table whose first row has only 2 cells. Today maxCols wins and
     the short row is padded to 3. Under flush-and-restart it becomes a 2-column
     fragment followed by a 3-column fragment.

  2. ch2 — full-width single-cell row inside a multi-column table.
     <td colspan="3"> in the middle of a 3-column table. Already renders as a
     1-column fragment; here to lock that in across the rewrite.

  3. ch3 — a cell needing more than MAX_CELL_LINES (64) lines while staying
     under the 96-word cell bound.
     8 columns => 460/8 = 57px, inner 47px. Words are 9 characters, and the
     harness renderer's minimum advance is 6px/char, so every word is at least
     54px and takes its own line. 70 words => 70 lines > 64, and 70 <= 96 so the
     word bound does not divert the cell to the paragraph path first. This is
     the fixture the design note warned about: an earlier attempt tripped the
     word bound instead and never reached the grid path at all.

  4. ch4 — a row taller than the viewport with a cell that is still under both
     bounds. 40 words => 40 lines (<= 64) => 960px, over the 760px viewport.
     Distinct from case 3: the line check passes and the height check fires.

  5. ch5 — a table with more than MAX_TABLE_ROWS (48) rows.
     Today this never actually reaches the row limit: the 12KB buffer budget
     trips first, around row 35, and flattens the table to paragraphs either
     way. After the rewrite it becomes a grid spread over several fragments,
     which is what makes the per-fragment row cap load-bearing. Two rows carry
     ids so anchor-to-page mapping can be checked across the change.
"""

import os
import zipfile
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_PATH = OUTPUT_DIR / "test_table_grid_edges.epub"

CSS = """\
body  { margin: 0; padding: 0; }
p     { margin-top: 1pt; margin-bottom: 0; text-align: justify; }
h1    { text-align: center; margin-top: 0.5em; margin-bottom: 0.5em; }
table { border-collapse: collapse; }
"""


def xhtml(title, body):
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="styles/test.css"/>
</head>
<body>
{body}
</body>
</html>"""


def long_words(n, prefix):
    """n distinct 9-character words, each wider than an 8-column inner width.

    The harness renderer advances 6 + (codepoint % 7) px per character, so a
    9-character word is at least 54px against a 47px column: one word per line,
    deterministically, with no dependence on which letters are used.
    """
    return " ".join(f"{prefix}{i:04d}" for i in range(n))


def eight_col_row(fat_cell_text):
    """A row of 8 cells where the first is oversized and the rest are minimal."""
    cells = [f"<td>{fat_cell_text}</td>"] + [f"<td>c{i}</td>" for i in range(1, 8)]
    return "<tr>" + "".join(cells) + "</tr>"


# ---------------------------------------------------------------------------
# Chapter 1 — column count changes mid-table
# ---------------------------------------------------------------------------
ch1 = xhtml("Ch1: Mid-table column change", """
<h1>Ch 1: Column Count Changes Mid-Table</h1>
<p>PASS: the two-cell row should render as its own two-column band, and the
three-cell rows below it as a separate three-column band. It should NOT appear
as a three-column row with an empty third cell.</p>
<table>
<tbody>
<tr><td>Header left</td><td>Header right</td></tr>
<tr><td>Body one</td><td>Body two</td><td>Body three</td></tr>
<tr><td>Body four</td><td>Body five</td><td>Body six</td></tr>
</tbody>
</table>
<p>Text after the table, which must resume normal block layout.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 2 — full-width single-cell row inside a multi-column table
# ---------------------------------------------------------------------------
ch2 = xhtml("Ch2: Full-width span row", """
<h1>Ch 2: Full-Width Span Row</h1>
<p>PASS: the spanning row should stretch the whole table width, with the
three-column rows aligned above and below it.</p>
<table>
<tbody>
<tr><td>Alpha</td><td>Beta</td><td>Gamma</td></tr>
<tr><td colspan="3">A single cell spanning all three columns of this table.</td></tr>
<tr><td>Delta</td><td>Epsilon</td><td>Zeta</td></tr>
</tbody>
</table>
<p>Closing text.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 3 — cell over MAX_CELL_LINES but under the word bound
# ---------------------------------------------------------------------------
ch3 = xhtml("Ch3: Cell over the line cap", f"""
<h1>Ch 3: Cell Needing More Lines Than the Grid Carries</h1>
<p>PASS: no text may be lost. The oversized cell lays out to 70 lines against a
64-line cap, so it cannot be a grid cell; every word must still appear.</p>
<table>
<tbody>
{eight_col_row(long_words(70, "Wordy"))}
<tr><td>d0</td><td>d1</td><td>d2</td><td>d3</td><td>d4</td><td>d5</td><td>d6</td><td>d7</td></tr>
</tbody>
</table>
<p>Closing text.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 4 — row taller than the viewport, both bounds satisfied
# ---------------------------------------------------------------------------
ch4 = xhtml("Ch4: Row taller than the viewport", f"""
<h1>Ch 4: Row Taller Than the Viewport</h1>
<p>PASS: no text may be lost. The tall cell is 40 lines, under the 64-line cap,
but 960px against a 760px viewport, so the row can never be displayed as a
grid row on any device.</p>
<table>
<tbody>
{eight_col_row(long_words(40, "Talli"))}
<tr><td>e0</td><td>e1</td><td>e2</td><td>e3</td><td>e4</td><td>e5</td><td>e6</td><td>e7</td></tr>
</tbody>
</table>
<p>Closing text.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 5 — table longer than MAX_TABLE_ROWS
# ---------------------------------------------------------------------------
_long_rows = []
for i in range(60):
    row_id = ""
    if i == 30:
        row_id = ' id="row-thirty"'
    elif i == 55:
        row_id = ' id="row-fiftyfive"'
    _long_rows.append(f'<tr{row_id}><td>Left{i:02d}</td><td>Right{i:02d}</td></tr>')

ch5 = xhtml("Ch5: Table longer than the row cap", """
<h1>Ch 5: Sixty-Row Table</h1>
<p>PASS: no text may be lost, and the rows should stay aligned in two columns
across every page the table spans.</p>
<table>
<tbody>
""" + "\n".join(_long_rows) + """
</tbody>
</table>
<p>Closing text.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 6 — a row over the buffer budget, followed by a row that recovers
# ---------------------------------------------------------------------------
# 8 cells x 40 words is 8 x (128 + 40*48) = 16384 attributed bytes against a
# 12KB budget, so the budget trips partway through the row with a cell still
# open. The row after it is deliberately ordinary: the point of scoping the
# fallback to a row is that the NEXT row goes back into the grid.
_fat_cells = "".join(f"<td>{long_words(40, f'Bulk{i}')}</td>" for i in range(8))

ch6 = xhtml("Ch6: Row over the buffer budget", f"""
<h1>Ch 6: Row Over the Buffer Budget</h1>
<p>PASS: no text may be lost, and the second row must still render as a grid
row -- a row that blows the budget costs that row, not the table.</p>
<table>
<tbody>
<tr>{_fat_cells}</tr>
<tr><td>f0</td><td>f1</td><td>f2</td><td>f3</td><td>f4</td><td>f5</td><td>f6</td><td>f7</td></tr>
</tbody>
</table>
<p>Closing text.</p>
""")

CHAPTERS = [
    ("ch1", "chapter1.xhtml", "Chapter 1: Mid-table column change", ch1),
    ("ch2", "chapter2.xhtml", "Chapter 2: Full-width span row",     ch2),
    ("ch3", "chapter3.xhtml", "Chapter 3: Cell over the line cap",  ch3),
    ("ch4", "chapter4.xhtml", "Chapter 4: Row over the viewport",   ch4),
    ("ch5", "chapter5.xhtml", "Chapter 5: Sixty-row table",         ch5),
    ("ch6", "chapter6.xhtml", "Chapter 6: Row over the budget",     ch6),
]


def build_epub(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as epub:
        # mimetype must be first and uncompressed
        epub.writestr("mimetype", "application/epub+zip",
                      compress_type=zipfile.ZIP_STORED)

        epub.writestr("META-INF/container.xml", """\
<?xml version="1.0" encoding="UTF-8"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf"
              media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>""")

        epub.writestr("OEBPS/styles/test.css", CSS)

        manifest_items = []
        spine_items = []
        nav_items = []

        for (chid, chfile, chtitle, chcontent) in CHAPTERS:
            epub.writestr(f"OEBPS/{chfile}", chcontent)
            manifest_items.append(
                f'    <item id="{chid}" href="{chfile}" media-type="application/xhtml+xml"/>')
            spine_items.append(f'    <itemref idref="{chid}"/>')
            nav_items.append(f'      <li><a href="{chfile}">{chtitle}</a></li>')

        manifest_items.append(
            '    <item id="nav" href="nav.xhtml" '
            'media-type="application/xhtml+xml" properties="nav"/>')

        content_opf = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">test-epub-table-grid-edges</dc:identifier>
    <dc:title>Test: Table Grid Edges</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
{chr(10).join(manifest_items)}
  </manifest>
  <spine>
{chr(10).join(spine_items)}
  </spine>
</package>"""
        epub.writestr("OEBPS/content.opf", content_opf)

        nav_xhtml = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Table of Contents</title></head>
<body>
  <nav epub:type="toc">
    <ol>
{chr(10).join(nav_items)}
    </ol>
  </nav>
</body>
</html>"""
        epub.writestr("OEBPS/nav.xhtml", nav_xhtml)

    print(f"Wrote {path}")


if __name__ == "__main__":
    build_epub(OUTPUT_PATH)
