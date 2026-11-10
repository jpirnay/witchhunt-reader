#!/usr/bin/env python3
"""Build test_boxes.epub: the corpus book for CSS boxes (borders and background shades).

Each chapter draws its boxes as PageBox elements (BOX lines in the golden), placed before what they
frame; a box's border and padding are inside its frame, and a page break leaves it open.

  1. Shapes      a sidebar, a shaded note, a letter in a double frame, an epigraph with a bar on its
                 start side, a chapter summary between two rules, a code box, a callout bar wider
                 than 3 px (drawn 3), a dark heading bar (drawn as the light shade), a border the
                 paper shows as white (no frame), nested boxes, a rule and an empty rule box in a
                 box, every line style and width keyword, a fieldset outlined by default
  2. Contents    a floated picture, a block picture, lists, a table in a box, a box in a table
                 cell (not drawn), headings with rules, a box after a heading
  3. Long box    a box over two pages (open at the break), a box that starts at a page foot, a
                 box that page-breaks before itself
  4. Right to left  Hebrew paragraphs in boxes, one with a bar on its start (right) side
  5. Vertical    a vertical-rl chapter: with vertical text on, its boxes are drawn in the turned
                 frame, the border-right of an element on its block-start side

Regenerate with:  python test/epubs/make_test_boxes.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_boxes.epub")

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

CHAPTERS = [
    ("ch1", "Shapes"),
    ("ch2", "Contents"),
    ("ch3", "Long box"),
    ("ch4", "Right to left"),
    ("ch5", "Vertical"),
]

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Boxes Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-boxes-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="fig" href="fig.png" media-type="image/png"/>
%(items)s
  </manifest>
  <spine toc="ncx">
%(itemrefs)s
  </spine>
</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-boxes-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Boxes Test</text></docTitle>
  <navMap>
%(points)s
  </navMap>
</ncx>
"""

CSS = """body { margin: 0; padding: 0 }
p { margin: 0 0 0.4em; text-indent: 0 }
h2 { margin: 0.8em 0 0.4em }

/* Shapes */
div.sidebar { border: 1px solid #000; padding: 0.5em; margin: 1em 0 }
div.sidebar h3 { margin: 0 0 0.3em }
p.note { background-color: #eee; padding: 0.4em }
div.letter { border: 3px double #000; padding: 1em; margin: 1em 2em }
blockquote.epigraph { border-left: 3px solid #999; padding-left: 0.75em; margin: 1em 0 1em 2em; font-style: italic }
div.summary { border-top: 1px solid; border-bottom: 1px solid; padding: 0.3em 0; margin: 1em 0 }
pre.code { background: #f5f5f5; border: 1px dashed #666; padding: 0.4em; margin: 0.6em 0 }
p.callout { border-left: 6px solid #ccc; padding-left: 0.5em }
h3.bar { background: #333; color: white; padding: 0.2em 0.4em }
p.invisible { border: 1px solid white; padding: 0.3em }
div.outer { border: 2px solid; padding: 0.4em; margin: 0.6em 0 }
div.inner { border: 1px dotted; background-color: rgb(230, 230, 230); margin: 0.4em 1em; padding: 0.3em }
div.rule { border-top: 1px solid; margin: 0.6em 3em }
p.s1 { border: thin solid } p.s2 { border: medium double } p.s3 { border: thick dotted }
p.s4 { border: 0.2em dashed } p.s5 { border: 2pt groove } p.s6 { border-style: solid none; border-width: 1px }
p.s1, p.s2, p.s3, p.s4, p.s5, p.s6 { padding: 0.2em; margin: 0.3em 0 }

/* Contents */
div.figbox { border: 1px solid; padding: 0.4em; margin: 0.6em 0 }
span.left { float: left; width: 30% }
div.picbox { border: 2px solid; margin: 0.6em 3em; text-align: center }
ul.boxed { border: 1px solid; padding: 0.3em 0.3em 0.3em 2em; margin: 0.6em 0 }
div.tablebox { border: 1px dashed; padding: 0.4em; margin: 0.6em 0 }
td div.cellbox { border: 1px solid; background: #eee }
h2.ruled { border-bottom: 2px solid; padding-bottom: 0.2em }
h3.framed { border: 1px solid; padding: 0.2em; text-align: center }

/* Long box */
div.long { border: 1px solid; padding: 0.5em; background: #eee }
div.late { border: 2px solid; padding: 0.5em }
div.broken { border: 1px solid; page-break-before: always; padding: 0.4em }

/* Right to left */
div.rtlbox { border: 1px solid; padding: 0.5em; margin: 0.6em 1em }
p.rtlbar { border-right: 3px solid; padding-right: 0.5em }

/* Vertical */
html.vertical { writing-mode: vertical-rl; -epub-writing-mode: vertical-rl }
div.vbox { border-right: 3px solid; padding-right: 0.5em }
p.vshade { background: #ddd; padding: 0.3em }
"""

HEAD = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="%(lang)s"%(html_class)s>
<head>
  <title>%(title)s</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
"""

TAIL = """
</body>
</html>
"""

LOREM = ("The lighthouse keeper wrote every evening, a page or two, of what the sea had done that day and of "
         "the ships that passed.")

CH1 = """<h2>1. Shapes</h2>
<p>A paragraph before the sidebar, set as running text.</p>
<div class="sidebar">
  <h3>Sidebar</h3>
  <p>A sidebar frames its own heading and two paragraphs. %(lorem)s</p>
  <p>Its second paragraph stays inside the same frame.</p>
</div>
<p class="note">A shaded note: the background colour drawn as a light gray, the text black on it.</p>
<div class="letter">
  <p>Dear reader,</p>
  <p>a letter in a double frame, indented on both sides. %(lorem)s</p>
  <p>Yours, the keeper</p>
</div>
<blockquote class="epigraph"><p>An epigraph with a bar on its start side and no other edge.</p></blockquote>
<div class="summary"><p>A chapter summary between a rule above and a rule below.</p></div>
<pre class="code">int keeper(void) {
    return lamp_on();
}</pre>
<p class="callout">A callout bar declared six pixels wide, drawn three.</p>
<h3 class="bar">A dark heading bar, drawn as the light shade</h3>
<p class="invisible">A white border: no frame, the padding kept.</p>
<div class="outer">
  <p>An outer box.</p>
  <div class="inner"><p>An inner box, dotted and shaded, nested in it.</p></div>
  <p>The outer box again.</p>
  <hr/>
  <p>A rule above, centred in the box.</p>
</div>
<div class="rule"></div>
<p class="s1">thin solid</p>
<p class="s2">medium double</p>
<p class="s3">thick dotted</p>
<p class="s4">0.2em dashed</p>
<p class="s5">2pt groove, drawn solid</p>
<p class="s6">solid above and below only</p>
<fieldset><legend>Fieldset</legend><p>A fieldset is outlined by default.</p></fieldset>
"""

CH2 = """<h2>2. Contents</h2>
<div class="figbox"><span class="left"><img src="fig.png" alt=""/></span>
  <p>A floated picture inside a box: the text wraps beside it, inside the frame. %(lorem)s %(lorem)s</p>
</div>
<div class="picbox"><p>A block picture, centred in its box:</p><p><img src="fig.png" alt=""/></p></div>
<ul class="boxed"><li>A list in a box,</li><li>its markers inside the frame,</li><li>three items.</li></ul>
<div class="tablebox"><table><tr><th>Left</th><th>Right</th></tr><tr><td>a table</td><td>in a box</td></tr></table></div>
<table><tr><td><div class="cellbox">a box in a cell, not drawn</div></td><td>its neighbour</td></tr></table>
<h2 class="ruled">A heading with a rule below</h2>
<p>Text under the ruled heading.</p>
<h3 class="framed">A framed heading</h3>
<p>Text under the framed heading.</p>
"""


def ch3():
    paras = "\n".join("<p>Long box paragraph %d. %s</p>" % (i, LOREM) for i in range(1, 15))
    fill = "\n".join("<p>Filler %d. %s</p>" % (i, LOREM) for i in range(1, 7))
    return """<h2>3. Long box</h2>
<div class="long">
%s
</div>
%s
<div class="late"><p>A box that starts near the foot of a page. %s</p></div>
<div class="broken"><p>A box that breaks the page before itself.</p></div>
""" % (paras, fill, LOREM)


CH4 = """<h2>4. Right to left</h2>
<div class="rtlbox" dir="rtl">
  <p>השומר כתב כל ערב עמוד או שניים על מה שעשה הים באותו יום.</p>
  <p class="rtlbar">פסקה עם קו בצד ימין.</p>
</div>
<p>Back to left-to-right text after the box.</p>
"""

CH5 = """<div class="vbox"><p>灯台守は毎晩、一、二頁の日記を書いた。</p>
<p>海がその日にしたこと、通り過ぎた船のこと。</p></div>
<p class="vshade">影のある段落。</p>
<p>終わり。</p>
"""


def png_gray(width, height):
    """A small 8-bit greyscale PNG (diagonal gradient)."""
    rows = []
    for y in range(height):
        row = bytes(((x * 255) // (width - 1) + y * 2) & 0xFF for x in range(width))
        rows.append(b"\x00" + row)
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def chapter(cid, title, body):
    lang = {"ch4": "he", "ch5": "ja"}.get(cid, "en")
    html_class = ' class="vertical"' if cid == "ch5" else ""
    return HEAD % {"lang": lang, "title": title, "html_class": html_class} + body + TAIL


def main():
    bodies = {"ch1": CH1 % {"lorem": LOREM}, "ch2": CH2 % {"lorem": LOREM}, "ch3": ch3(), "ch4": CH4, "ch5": CH5}
    items = "\n".join('    <item id="%s" href="%s.xhtml" media-type="application/xhtml+xml"/>' % (c, c)
                      for c, _ in CHAPTERS)
    itemrefs = "\n".join('    <itemref idref="%s"/>' % c for c, _ in CHAPTERS)
    points = "\n".join(
        '    <navPoint id="np%d" playOrder="%d"><navLabel><text>%s</text></navLabel><content src="%s.xhtml"/>'
        "</navPoint>" % (i + 1, i + 1, t, c) for i, (c, t) in enumerate(CHAPTERS))
    if os.path.exists(OUT):
        os.remove(OUT)
    with zipfile.ZipFile(OUT, "w") as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF % {"items": items, "itemrefs": itemrefs}, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX % {"points": points}, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/style.css", CSS, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/fig.png", png_gray(80, 60), zipfile.ZIP_DEFLATED)
        for cid, title in CHAPTERS:
            z.writestr("OEBPS/%s.xhtml" % cid, chapter(cid, title, bodies[cid]), zipfile.ZIP_DEFLATED)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
