#!/usr/bin/env python3
"""Build test_body_size.epub -- a book whose CSS sets ALL its running text smaller than the
reader's size, the corpus book for Normalize Font Size's body size (lib/Epub/Epub/BodySize.h).

Modelled on "Die Buecherdiebin" (Markus Zusak): every paragraph carries a class whose rule is
`font-size: small`, the chapter titles are `large`, a subtitle `medium`, the small print
`x-small`. With the CSS Fonts 4 keywords small is 8/9 of medium, so without normalization the
whole book reads at 89 % of the reader's size, outside the +/-10 % band that snaps near-body sizes.

The body size is found in the largest spine item (chapter1.xhtml), not the title page before it,
whose few words are set much larger. With Normalize Font Size on, the sizes are taken relative to
it (89 %):

  * .tx  small    8/9  -> body, 100 %
  * h1   large    6/5  -> 135 %
  * .sub medium   1    -> 112 %
  * .note x-small 3/4  -> 84 %
  * .title xx-large 2  -> 225 % (title page)

With it off they stay as the CSS states them.

Regenerate with:  python test/epubs/make_test_body_size.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_body_size.epub")

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Body Size Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-body-size-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="titlepage" href="titlepage.xhtml" media-type="application/xhtml+xml"/>
    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="titlepage"/>
    <itemref idref="chapter1"/>
  </spine>
</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-body-size-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Body Size Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>Title</text></navLabel>
      <content src="titlepage.xhtml"/>
    </navPoint>
    <navPoint id="np2" playOrder="2">
      <navLabel><text>Chapter One</text></navLabel>
      <content src="chapter1.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

CSS = """/* The running text: every paragraph, as in the book this is modelled on. */
.tx    { font-size: small; text-align: justify; text-indent: 1em; margin: 0 }
/* Chapter title, a subtitle and the small print. */
h1.ct  { font-size: large; text-align: center }
.sub   { font-size: medium; text-align: center }
.note  { font-size: x-small }
/* The title page. */
.title { font-size: xx-large; text-align: center }
"""


def document(title, body):
    return """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head>
  <title>%s</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
%s
</body>
</html>
""" % (title, body)


TITLEPAGE = document("Title", """  <p class="title">Body Size Test</p>
  <p class="sub">A book set in small type</p>""")

PARAGRAPHS = [
    "It was a small book in every sense: small in its pages, small in its margins, and smallest of all in "
    "its type, which the publisher had set a step below the reader's own size on every paragraph.",
    "Nobody had asked for that. The reader had chosen fourteen points, and the book quietly made them "
    "twelve and a half, so that the lines ran long and the letters looked thin and far apart.",
    "Normalize Font Size used to forgive only the small differences, a few percent either way, which "
    "publishers scatter through their stylesheets without meaning anything by them.",
    "A whole book set in <em>small</em> type was another matter. Its body sat just outside the band, and "
    "every page of it was drawn from glyphs scaled down from the size the reader had picked.",
    "Measured once, when the book is first opened, the size of the running text becomes the reader's "
    "size, and everything else in the book keeps its place relative to it.",
    "The titles stay larger than the text, the subtitles a little larger, the small print smaller: the "
    "hierarchy the publisher drew is kept, only its scale is the reader's.",
]

CHAPTER = document("Chapter One", """  <h1 class="ct">Chapter One</h1>
  <p class="sub">In which the type is small</p>
%s
  <p class="note">Small print: set in x-small, three quarters of medium.</p>""" % "\n".join(
    '  <p class="tx">%s</p>' % p for p in PARAGRAPHS))


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        # mimetype must be first and STORED per OCF.
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        z.writestr("OEBPS/content.opf", OPF)
        z.writestr("OEBPS/toc.ncx", NCX)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/titlepage.xhtml", TITLEPAGE)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
