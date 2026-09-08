#!/usr/bin/env python3
"""Build test_wrapped_paragraphs.epub — the corpus book for the PARAGRAPH ANCHOR.

Every other fixture in this corpus puts its <p> elements directly inside <body>.
Real books mostly do not: Calibre (and therefore a large share of commercial EPUBs)
wraps a chapter's content in a container div —

    <body class="calibre"><div class="chapter"><p>...</p>...</div></body>

— and the section cache's paragraph LUT only counts <p> at body-child level, so a
wrapped chapter records paragraph index 0 on every page. That is the shape the
reader's "return from a footnote", "keep my place across a relayout" and KOReader
XPath sync all anchor on, and none of the existing fixtures can see it.

Spine 0 is the wrapped chapter, spine 1 the same prose unwrapped as the control.
Both are long enough to span several pages at the test's viewport.

Regenerate with:  python test/epubs/make_test_wrapped_paragraphs.py
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_wrapped_paragraphs.epub")

# Enough distinct prose that the pages differ and paragraphs run over page breaks.
SENTENCES = [
    "The Company's clerks kept their ledgers in a hand so small that the columns "
    "looked like brickwork, and the sums in them were larger than the revenue of "
    "several European kingdoms put together.",
    "Nobody in the Founders' Hall that morning imagined that the subscription "
    "being raised would one day command an army, but the notaries wrote it all "
    "down anyway, because that was what notaries were for.",
    "A monsoon was late, a treaty was early, and between the two of them a great "
    "many people who had never heard of either found their circumstances "
    "rearranged without being consulted.",
    "The correspondence took eight months to reach London and eight months to come "
    "back, which meant that every instruction arrived describing a world that had "
    "already stopped existing.",
    "What the directors called policy the men on the ground called weather: "
    "something to be endured, occasionally exploited, and never in any meaningful "
    "sense obeyed.",
]

# 24 paragraphs is comfortably several pages at 480x800 with the harness font.
PARAGRAPHS = [SENTENCES[i % len(SENTENCES)] for i in range(24)]


def paragraphs_html(indent):
    out = []
    for i, text in enumerate(PARAGRAPHS, start=1):
        # A footnote caller in the middle of the chapter, so the anchor under test
        # is taken from a page well past the first.
        if i == 12:
            text += ' A disputed figure<a href="notes.xhtml#n1" epub:type="noteref">1</a> all the same.'
        out.append(f'{indent}<p id="p{i}">{text}</p>')
    return "\n".join(out)


WRAPPED = f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>Wrapped Chapter</title></head>
  <body class="calibre">
<div id="ch01" class="chapter">
<h1>Wrapped Chapter</h1>
{paragraphs_html("")}
</div>
  </body>
</html>
"""

UNWRAPPED = f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>Unwrapped Chapter</title></head>
  <body>
<h1>Unwrapped Chapter</h1>
{paragraphs_html("")}
  </body>
</html>
"""

NOTES = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>Notes</title></head>
  <body>
    <p id="n1">Estimates for the period vary by an order of magnitude and every one
    of them was produced by somebody with an interest in the answer.</p>
  </body>
</html>
"""

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
    <dc:identifier id="bookid">wrapped-paragraphs-fixture</dc:identifier>
    <dc:title>Wrapped Paragraphs</dc:title>
    <dc:language>en</dc:language>
    <dc:creator>Corpus</dc:creator>
  </metadata>
  <manifest>
    <item id="wrapped" href="wrapped.xhtml" media-type="application/xhtml+xml"/>
    <item id="unwrapped" href="unwrapped.xhtml" media-type="application/xhtml+xml"/>
    <item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  </manifest>
  <spine>
    <itemref idref="wrapped"/>
    <itemref idref="unwrapped"/>
    <itemref idref="notes"/>
  </spine>
</package>
"""

NAV = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>Contents</title></head>
  <body>
    <nav epub:type="toc">
      <ol>
        <li><a href="wrapped.xhtml">Wrapped Chapter</a></li>
        <li><a href="unwrapped.xhtml">Unwrapped Chapter</a></li>
      </ol>
    </nav>
  </body>
</html>
"""


def main():
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        z.writestr("OEBPS/content.opf", OPF)
        z.writestr("OEBPS/nav.xhtml", NAV)
        z.writestr("OEBPS/wrapped.xhtml", WRAPPED)
        z.writestr("OEBPS/unwrapped.xhtml", UNWRAPPED)
        z.writestr("OEBPS/notes.xhtml", NOTES)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
