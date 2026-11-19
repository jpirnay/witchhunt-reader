#!/usr/bin/env python3
"""Build test_nested_tables.epub -- the corpus book that pins the text of a table inside a table.

A nested table can never be a grid: the outer table streams its cells as paragraphs from the
inner <table> on, and each inner cell becomes a paragraph of its own, laid out like a degraded
outer cell. The parser used to drop every word inside the inner table, silently.

One chapter per case, each word of an inner cell tagged with the case's marker, so a lost word
or a run-on paragraph shows in the golden:

  1. FIRST     an inner table in the first row's first cell (nothing buffered before it), with a
               <caption> and a <th>
  2. LEVEL     a table nested two deep: the innermost opens inside an inner cell, with the middle
               and the outer cell's text before and after it (depth goes up twice, down twice),
               then a plain table that is a grid only if the depth unwound
  3. IMG       an inner cell holding two pictures between runs of text; then an outer cell's text
               after its inner table, the next outer cell holding a picture alone
  4. RTL       an inner table with dir="rtl", one of its cells holding a dir="ltr" span
  5. TARGET    an inner cell whose id a link targets, between two inner cells, each cell before
               and itself longer than a page (AnchorMapTest: the id names the page of its text)
  6. SAME      two outer cells of one row, each holding an inner table: the second opens in a row
               the first has already degraded

Regenerate with:  python test/epubs/make_test_nested_tables.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_nested_tables.epub")

# A fixed timestamp, so two runs write the same bytes.
STAMP = (2026, 11, 18, 0, 0, 0)


def png(width, height, ink):
    """An 8-bit greyscale PNG: ink(x, y) -> True paints black on white. No external dependencies."""
    rows = b"".join(
        b"\x00" + bytes(0 if ink(x, y) else 255 for x in range(width)) for y in range(height)
    )

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def checker(w, h):
    return lambda x, y: ((x // 20 + y // 20) % 2 == 0) or x < 3 or y < 3 or x >= w - 3 or y >= h - 3


def bar(w, h):
    return lambda x, y: y < 3 or y >= h - 3 or (x // 8) % 2 == 0


IMAGES = {"checker.png": (160, 100, checker), "bar.png": (120, 24, bar)}

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

# Paragraphs as most books set them, so a style left over from the paragraph before a table shows
# if it leaks into an inner cell.
CSS = """p { text-indent: 0; margin: 0.5em 0 }
"""


def xhtml(title, body, lang="en"):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">
<head><title>{title}</title><link rel="stylesheet" type="text/css" href="../css/style.css"/></head>
<body>
{body}
</body>
</html>
"""


FIRST = xhtml(
    "First cell",
    """<h1>First cell</h1>
<p>CASE1 The inner table opens in the first cell of the outer table's first row.</p>
<table>
<tr>
<td><table>
<caption>FIRSTCAP Caption of the inner table.</caption>
<tr><th>FIRSTHEAD Inner header cell.</th><th>FIRSTHEAD2 Second header cell.</th></tr>
<tr><td>FIRSTA First inner cell.</td><td>FIRSTB Second inner cell.</td></tr>
</table>
FIRSTTAIL The outer cell goes on after the inner table.</td>
<td>FIRSTOUTERB The second outer cell.</td>
</tr>
<tr><td>FIRSTROW2A Next row, first cell.</td><td>FIRSTROW2B Next row, second cell.</td></tr>
</table>
<p>FIRSTAFTER Text after the table.</p>""",
)

LEVEL = xhtml(
    "Three deep",
    """<h1>Three deep</h1>
<p>CASE2 A table nested two deep: the innermost opens inside an inner cell.</p>
<table>
<tr>
<td>LEVEL1A Outer text before the middle table.
<table>
<tr>
<td>LEVEL2A Middle text before the innermost table.
<table>
<tr><td>LEVEL3A Innermost cell one.</td><td>LEVEL3B Innermost cell two.</td></tr>
</table>
LEVEL2TAIL Middle text after the innermost table.</td>
<td>LEVEL2B Second middle cell.</td>
</tr>
</table>
LEVEL1TAIL Outer text after the middle table.</td>
<td>LEVEL1B Second outer cell.</td>
</tr>
<tr><td>LEVELROW2A Next outer row.</td><td>LEVELROW2B Its second cell.</td></tr>
</table>
<p>LEVELAFTER Text after the table, back in normal block layout.</p>
<table>
<tr><td>LEVELGRIDA Grid.</td><td>LEVELGRIDB Grid.</td></tr>
</table>""",
)

IMG = xhtml(
    "Picture",
    """<h1>Picture</h1>
<p>CASE3 An inner table holding a picture.</p>
<table>
<tr>
<td>IMGOUTER The outer cell before the inner table.
<table>
<tr><td>IMGBEFORE Text before the picture. <img src="../images/checker.png" alt="checker"/> IMGAFTER Text after
the picture. <img src="../images/bar.png" alt="bar"/> IMGEND Text after the second picture.</td>
<td>IMGB Second inner cell.</td></tr>
</table>
</td>
<td>IMGOUTERB The second outer cell.</td>
</tr>
</table>
<p>IMGTAILCASE An outer cell's text after its inner table, the next outer cell a picture alone.</p>
<table>
<tr>
<td>TAILOUTER Before the inner table.
<table>
<tr><td>TAILINNER The inner cell.</td></tr>
</table>
TAILA The outer cell after its inner table.</td>
<td><img src="../images/bar.png" alt="bar"/></td>
</tr>
</table>
<p>IMGAFTERTABLE Text after the tables.</p>""",
)

RTL = xhtml(
    "Right to left",
    """<h1>Right to left</h1>
<p>CASE4 An inner table set right to left.</p>
<table>
<tr>
<td>RTLOUTER The outer cell before the inner table.
<table dir="rtl">
<tr><td>RTLA שלום עולם</td><td>RTLB ספר <span dir="ltr">RTLSPAN left to right</span> סוף</td></tr>
</table>
RTLTAIL The outer cell after it.</td>
</tr>
</table>
<p>RTLAFTER Text after the table.</p>""",
)

FILLER_WORDS = ("the reader turned another page while the rain kept falling on the old roof and "
                "somewhere below a door closed softly").split()


def filler(n, count):
    return " ".join(FILLER_WORDS[(n + i) % len(FILLER_WORDS)] for i in range(count))


# The cell before the target and the target are each longer than a page: the outer cell's text and the cell before
# start a page or more ahead of the target, the cell after it and the paragraph after the table a page or more later,
# so an id that took any of their pages names the wrong one.
TARGET = xhtml(
    "Link target",
    f"""<h1>Link target</h1>
<p>CASE5 <a href="#inner-target">The linked cell</a> sits in an inner table further on.</p>
<table>
<tr>
<td>TARGETOUTER The outer cell before the inner table.
<table>
<tr><td>TARGETLONG {filler(1, 380)}.</td>
<td id="inner-target">TARGETWORD {filler(2, 380)}.</td>
<td>TARGETB {filler(3, 40)}.</td></tr>
</table>
</td>
</tr>
</table>
<p>TARGETAFTER Text after the table.</p>""",
)

SAME = xhtml(
    "Same row",
    """<h1>Same row</h1>
<p>CASE6 Two outer cells of one row, each holding an inner table.</p>
<table>
<tr>
<td>SAMEA The first outer cell.
<table>
<tr><td>SAMEA1 First inner table, cell one.</td><td>SAMEA2 First inner table, cell two.</td></tr>
</table>
</td>
<td>SAMEB The second outer cell.
<table>
<tr><td>SAMEB1 Second inner table, cell one.</td><td>SAMEB2 Second inner table, cell two.</td></tr>
</table>
SAMEBTAIL The second outer cell after its inner table.</td>
</tr>
</table>
<p>SAMEAFTER Text after the table.</p>""",
)

CHAPTERS = [
    ("first", "First cell", FIRST),
    ("level", "Three deep", LEVEL),
    ("img", "Picture", IMG),
    ("rtl", "Right to left", RTL),
    ("target", "Link target", TARGET),
    ("same", "Same row", SAME),
]


def opf():
    images = "\n".join(
        f'    <item id="img{i}" href="images/{name}" media-type="image/png"/>' for i, name in enumerate(IMAGES)
    )
    chapters = "\n".join(
        f'    <item id="{c}" href="text/{c}.xhtml" media-type="application/xhtml+xml"/>' for c, _, _ in CHAPTERS
    )
    spine = "\n".join(f'    <itemref idref="{c}"/>' for c, _, _ in CHAPTERS)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Nested Tables Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-nested-tables-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="css" href="css/style.css" media-type="text/css"/>
{images}
{chapters}
  </manifest>
  <spine toc="ncx">
{spine}
  </spine>
</package>
"""


def ncx():
    points = "\n".join(
        f'    <navPoint id="np{i + 1}" playOrder="{i + 1}"><navLabel><text>{t}</text></navLabel>'
        f'<content src="text/{c}.xhtml"/></navPoint>'
        for i, (c, t, _) in enumerate(CHAPTERS)
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-nested-tables-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Nested Tables Test</text></docTitle>
  <navMap>
{points}
  </navMap>
</ncx>
"""


def write(z, name, data, stored=False):
    info = zipfile.ZipInfo(name, date_time=STAMP)
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    z.writestr(info, data)


def main():
    with zipfile.ZipFile(OUT, "w") as z:
        write(z, "mimetype", "application/epub+zip", stored=True)
        write(z, "META-INF/container.xml", CONTAINER)
        write(z, "OEBPS/content.opf", opf())
        write(z, "OEBPS/toc.ncx", ncx())
        write(z, "OEBPS/css/style.css", CSS)
        for name, (w, h, shape) in IMAGES.items():
            write(z, f"OEBPS/images/{name}", png(w, h, shape(w, h)))
        for c, _, text in CHAPTERS:
            write(z, f"OEBPS/text/{c}.xhtml", text)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
