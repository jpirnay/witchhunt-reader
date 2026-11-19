#!/usr/bin/env python3
"""Build test_toc_one_file.epub: a book that is ONE XHTML file with 150 table-of-contents entries.

Every entry points at a heading of the same file, so the spine's TOC anchors number 150 and the
section build's anchor pool holds all of them (~3.5 KB). A TOC anchor starts a fresh page, so the
golden shows 150 chapter starts. The size of that pool is the book's to decide, which is what the
tests of the build's setup (SectionSetupHeapTest) lean on.

Regenerate with:  python test/epubs/make_test_toc_one_file.py
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_toc_one_file.epub")

ENTRIES = 150
SENTENCE = (
    "The notaries recording the subscription thought they were witnessing nothing more than "
    "a trading venture with unusually optimistic arithmetic. "
)


def anchor(i):
    return "section-heading-%04d" % i


BODY = "\n".join(
    '<h2 id="%s">Section %d</h2>\n<p>S%dA %s</p>\n<p>S%dB %s</p>'
    % (anchor(i), i, i, SENTENCE * (1 + i % 3), i, SENTENCE)
    for i in range(1, ENTRIES + 1)
)

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>One file</title></head>
  <body>
%s
  </body>
</html>
""" % BODY

NAV = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>Contents</title></head>
  <body>
    <nav epub:type="toc" id="toc">
      <ol>
%s
      </ol>
    </nav>
  </body>
</html>
""" % "\n".join(
    '        <li><a href="chapter1.xhtml#%s">Section %d</a></li>' % (anchor(i), i)
    for i in range(1, ENTRIES + 1)
)

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
    <dc:identifier id="bookid">toc-one-file-fixture</dc:identifier>
    <dc:title>TOC One File</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
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
        z.writestr("OEBPS/nav.xhtml", NAV)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
