#!/usr/bin/env python3
"""Build test_inline_anchors.epub: the corpus book for an INLINE ID's PAGE.

An id on an inline element (an <a id>, a <span id>, a <dfn id>) in the middle of a paragraph that crosses a page
break names the page of ITS OWN line, not the page the paragraph began on, nor the next block's. Each chapter holds
one case that AnchorMapTest reads (the spine indexes are its constants):

  0  a paragraph of three pages with ids on its middle page and an id after its last word
  1  a paragraph of under 96 words (one layout window) that crosses a page, a <span id> in its second half
  2  ids before a block's first word, each followed by a block with an id of its own
  3  ids in a table cell
  4  ids before words that hyphenate (the test builds it with hyphenation on and off)
  5  an id after a paragraph's last word, the next block forced onto a new page; ids on empty blocks at the end
  6  an <a id> in a div's text that wraps a block forced onto a new page
  7  ids before a <br>, in paragraphs whose varied margins put some <br> at a page's end

Regenerate with:  python test/epubs/make_test_inline_anchors.py
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_inline_anchors.epub")


def words(prefix, first, last, marks=None):
    """Numbered words prefix<first>..prefix<last>; marks maps a word to markup placed directly before it."""
    marks = marks or {}
    return " ".join(marks.get("%s%03d" % (prefix, i), "") + "%s%03d" % (prefix, i) for i in range(first, last + 1))


def page(body, title):
    return """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>%s</title></head>
  <body>
%s
  </body>
</html>
""" % (title, body)


# 0: the ids sit on the paragraph's middle page, so their page, the block's first and its last all differ. The <dfn>
# is one of the inline formatting tags a list of sixteen once left out.
LONG = words("w", 1, 1100, {"w560": '<a id="inl-a"></a>', "w600": '<dfn id="inl-dfn"></dfn>',
                            "w700": '<span id="inl-s"></span>'}) + '<a id="inl-tail"></a>'
CH0 = '<p id="blk-first">BLOCKFIRST %s</p>\n<p id="blk-next">NEXTBLOCK follows the long paragraph.</p>' % LONG

# 1: the short paragraph follows a filler that leaves it room for a few lines only.
CH1 = '<p>%s</p>\n<p>SHORTFIRST %s</p>' % (words("f", 1, 300), words("s", 1, 70, {"s055": '<span id="short-s"></span>'}))

# 2: each paragraph with an id before its first word crosses a page (where it says so), so that the block after it is
# a page later.
CH2 = """<p id="bf-p"><a id="bf1"></a><a id="bf2"></a>BFWORD %s</p>
<p id="bf-next">NEXTA</p>
<h2><a id="bf-h"></a>BFTITLE</h2>
<p id="bf-hp">HPWORD</p>
<p><a id="bf-n" href="#bf1">1.</a> NOTEWORD a note body</p>
<p id="bf-after">AFTERN</p>
<p><b id="bf-b">BOLDWORD</b> more</p>
<p id="bf-last">LASTN</p>
<p><a id="bf-solo"></a>SOLOWORD %s</p>
<p>SOLONEXT</p>
<p>BRFIRST<br/><a id="bf-br"></a>BRWORD %s<br/>BRNEXT</p>""" % (words("b", 1, 500), words("c", 1, 500),
                                                             words("d", 1, 500))

# 3: ids in a table cell, then a block with an id of its own.
CH3 = """<p>TABLEBEFORE</p>
<table><tr><td><a id="cell-1"></a>CELLA <a id="cell-2"></a>CELLB</td><td>CELLC</td></tr></table>
<p id="cell-after">TABLEAFTER</p>"""

# 4: ids before marker words of a paragraph whose long words break across lines, on every fourth marker: 8 in a layout
# window of 96 tokens, under the 16 the parser's queue holds, so that no id is recorded early for the cap's sake.
CH4 = "<p>%s</p>" % " ".join('%sM%03d internationalization characteristically' %
                             ('<span id="hy%03d"></span>' % i if i % 4 == 1 else "", i) for i in range(1, 160))

# 5: the id follows a space after the last word, so no word of the block reaches it: the block's end records it. The
# chapter ends with ids on elements that hold no text.
CH5 = ('<p>TAILFIRST %s <a id="tail-id"></a></p>\n'
       '<h2 style="page-break-before: always">TAILNEXT</h2>\n'
       '<p><a id="end-a"></a></p>\n<p><span id="end-s"></span></p>\n<p id="end-p"></p>') % words("t", 1, 400)

# 6: the <a id> opens after the div's last word and wraps the block that follows.
CH6 = ('<div>WRAPFIRST %s <a id="wrap-id"><p style="page-break-before: always">WRAPPED here</p></a></div>\n'
       '<p>WRAPNEXT</p>') % words("x", 1, 400)

# 7: each paragraph is a line, an id after its word (a space between), a <br> and a second line. Margins of 0 to 22 px
# vary where the pages end, so that some page ends between a paragraph's two lines.
CH7 = "\n".join('<p style="margin-top: %dpx">L%03d <a id="br-%03d"></a><br/>R%03d</p>' % ((i * 7) % 23, i, i, i)
                for i in range(1, 151))

CHAPTERS = [(CH0, "Long"), (CH1, "Short"), (CH2, "Before first word"), (CH3, "Table"), (CH4, "Hyphen"),
            (CH5, "Tail"), (CH6, "Wrap"), (CH7, "Break")]

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
%s
  </manifest>
  <spine>%s</spine>
</package>
"""


def main():
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        items = "\n".join('    <item id="c%d" href="chapter%d.xhtml" media-type="application/xhtml+xml"/>' % (i, i)
                          for i in range(len(CHAPTERS)))
        spine = "".join('<itemref idref="c%d"/>' % i for i in range(len(CHAPTERS)))
        z.writestr("OEBPS/content.opf", OPF % (items, spine))
        for i, (body, title) in enumerate(CHAPTERS):
            z.writestr("OEBPS/chapter%d.xhtml" % i, page(body, title))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
