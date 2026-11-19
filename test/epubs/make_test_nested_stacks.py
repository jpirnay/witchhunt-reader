#!/usr/bin/env python3
"""Build test_nested_stacks.epub -- the corpus book that nests every per-element stack 20 deep.

The chapter parser keeps seven stacks that grow with the document's nesting: the inline style stack, the list stack,
the container-width stack, the background-picture scopes, the held bottom spacing, the block insets and the anchors
awaiting a line. Each is a std::vector, and a refused growth aborts the firmware (-fno-exceptions). StackHeapTest walks
a refusal over every push this book makes (STACK_FAULT_INJECTION); the golden pins the layout when nothing is refused.

Twenty levels, each one a <div> that
  - has a width (container-width stack), a left margin (block inset) and a bottom margin and padding (held spacing),
  - carries an id and no text before its first paragraph (an anchor awaiting a line),
  - and holds a paragraph of styled spans: bold, italic, em, strong, underline, strike, sup, sub, small, a CSS span and
    a link (inline style stack, seven push sites).
Every fourth level also opens a list (list stack), and levels 3, 7 and 11 carry a background picture.

Regenerate with:  python test/epubs/make_test_nested_stacks.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_nested_stacks.epub")
LEVELS = 20


def png(width, height):
    """A grey diagonal-stripe PNG, 8-bit greyscale, no external dependencies."""
    rows = b"".join(b"\x00" + bytes(((x + y) // 8 % 2) * 160 + 40 for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


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
    <dc:title>Nested Stacks Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-nested-stacks-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="css" href="css/style.css" media-type="text/css"/>
    <item id="pic" href="images/pic.png" media-type="image/png"/>
    <item id="chapter1" href="text/chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="chapter1"/>
  </spine>
</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN"
  "http://www.daisy.org/z3986/2005/ncx/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-nested-stacks-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Nested Stacks Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>Nested Stacks Test</text></navLabel>
      <content src="text/chapter1.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

CSS = """body { margin: 0; padding: 0 }
div.lvl { width: 96%; margin: 0 0 0.3em 0.4em; padding: 0 0 0.2em 0 }
div.bg { background-image: url(../images/pic.png); background-repeat: no-repeat }
span.hl { font-weight: bold; font-style: italic }
"""


def paragraph(k):
    w = lambda tag: "L%02d%s" % (k, tag)
    return (
        "<p>%s <b>%s</b> <i>%s</i> <em>%s</em> <strong>%s</strong> <u>%s</u> <s>%s</s> "
        "<sup>%s</sup> <sub>%s</sub> <small>%s</small> <span class=\"hl\" id=\"s%d\">%s</span> "
        "<a href=\"#d%d\">%s</a> %s</p>"
        % (w("a"), w("b"), w("c"), w("d"), w("e"), w("f"), w("g"), w("h"), w("i"), w("j"), k, w("k"),
           k, w("l"), w("z"))
    )


def chapter():
    out = []
    for k in range(LEVELS):
        cls = "lvl bg" if k in (3, 7, 11) else "lvl"
        out.append('<div class="%s" id="d%d">' % (cls, k))
        out.append(paragraph(k))
        if k % 4 == 1:
            out.append("<%s><li>LI%02dA <b>LI%02dB</b></li><li>" % ("ol" if k % 8 == 1 else "ul", k, k))
    # The innermost text, then every level closes in turn.
    out.append("<p>INNERMOST text of the deepest level.</p>")
    for k in reversed(range(LEVELS)):
        if k % 4 == 1:
            out.append("</li></%s>" % ("ol" if k % 8 == 1 else "ul"))
        out.append("<p>AFTER%02d the level closed.</p></div>" % k)
    body = "\n".join(out)
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml">\n'
        '<head><title>Nested Stacks</title><link rel="stylesheet" type="text/css" href="../css/style.css"/></head>\n'
        "<body>\n<p>START of the nested stacks.</p>\n" + body + "\n</body>\n</html>\n"
    )


def main():
    with zipfile.ZipFile(OUT, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/css/style.css", CSS, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/images/pic.png", png(120, 80), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/text/chapter1.xhtml", chapter(), compress_type=zipfile.ZIP_DEFLATED)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
