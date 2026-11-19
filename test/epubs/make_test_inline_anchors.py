#!/usr/bin/env python3
"""Build test_inline_anchors.epub: the corpus book for an INLINE ID's PAGE.

An id on an inline element (an <a id>, a <span id>) in the middle of a paragraph that crosses a page
break names the page of ITS OWN line, not the page the paragraph began on. One long paragraph of
numbered words carries both ids after the break, plus an <a id> after the paragraph's last word; a
block id on the paragraph after it stays on that block's first line.

Regenerate with:  python test/epubs/make_test_inline_anchors.py
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_inline_anchors.epub")

WORDS = ["w%03d" % i for i in range(1, 701)]
# The ids sit in front of these words (a word follows each, so its line is the one that counts).
AT = {"a": "w560", "s": "w640"}
parts = []
for w in WORDS:
    if w == AT["a"]:
        parts.append('<a id="inl-a"></a>' + w)
    elif w == AT["s"]:
        parts.append('<span id="inl-s"></span>' + w)
    else:
        parts.append(w)
LONG = " ".join(parts) + '<a id="inl-tail"></a>'

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>Inline ids</title></head>
  <body>
<p id="blk-first">BLOCKFIRST %s</p>
<p id="blk-next">NEXTBLOCK follows the long paragraph.</p>
  </body>
</html>
""" % LONG

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">inline-anchors-fixture</dc:identifier>
    <dc:title>Inline Anchors</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="c1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="c1"/></spine>
</package>
"""


def main():
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        z.writestr("OEBPS/content.opf", OPF)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
