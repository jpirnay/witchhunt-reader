#!/usr/bin/env python3
"""Build test_anchor_pagebreak.epub — the corpus book for ANCHOR PAGE ACCURACY.

An anchor's page is recorded when its element's text block STARTS, which is before that
block's first line has been placed. When the line does not fit on the page in progress,
the content moves to the next page and the anchor is left pointing at the previous one.

Reproducing that needs anchors whose content begins exactly at a page boundary, and the
only reliable way to get some is to have many of them: this fixture carries 60 anchored
paragraphs of varying length, so page breaks fall in different places relative to a
paragraph start and several land exactly on one.

Each paragraph opens with a unique MARKnn token so a test can find which page its text
actually rendered on and compare that with what the anchor map claims.

Regenerate with:  python test/epubs/make_test_anchor_pagebreak.py
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_anchor_pagebreak.epub")

SENTENCE = (
    "The subscription raised that morning would in time command an army, though "
    "the notaries recording it thought they were witnessing nothing more than a "
    "trading venture with unusually optimistic arithmetic. "
)

# Lengths chosen to drift relative to the page height so that paragraph starts fall at
# every offset within a page across the run, including exactly at a break.
PARAGRAPHS = [(i, SENTENCE * (1 + (i % 5))) for i in range(1, 61)]

BODY = "\n".join('<p id="a%d">MARK%d %s</p>' % (i, i, text) for i, text in PARAGRAPHS)

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>Anchored</title></head>
  <body>
%s
  </body>
</html>
""" % BODY

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
    <dc:identifier id="bookid">anchor-pagebreak-fixture</dc:identifier>
    <dc:title>Anchor Pagebreak</dc:title>
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
