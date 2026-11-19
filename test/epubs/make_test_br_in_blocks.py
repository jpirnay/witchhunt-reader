#!/usr/bin/env python3
"""Build test_br_in_blocks.epub — the corpus book that pins the font size of the
lines a <br> splits a heading or a sized paragraph into.

A <br> ends the current text block and starts a new one for the next line. That
block was built from scratch, so it dropped the size of the element it belongs
to: in

    <h1>Chapter 1<br/>Setting Sail</h1>

"Setting Sail" came out at body size (mult=1.000) under a 1.5x first line. The
line after a <br> is still the same element, so it keeps that element's size --
but only while the element is open: once it has closed, nothing it leaves behind
may lend its size to the next paragraph.

  1. heading split by <br>          both lines at heading size
  2. heading split twice            all three lines at heading size
  3. sized paragraph split by <br>  both lines at the paragraph's 1.3x
  4. long sized paragraph, <br>     the block is laid out part-way (over 96
     after it                       words) before the <br>; the line after it
                                    keeps the size all the same
  5. trailing <br> in a heading     the paragraph after the heading is body size
  6. <br> between heading and text  the same, with the usual blank-line gap
  7. empty heading                  the paragraph after it is body size

A <br> adds a blank line only when the line it ends is empty, as a browser lays
it out: after text on the same line it ends that line and adds nothing.

  8. trailing <br> in a paragraph   no gap before the next paragraph
  9. two <br> inside a paragraph    one blank line between the two lines
 10. two trailing <br>              one blank line before the next paragraph
 11. two <br> between paragraphs    two blank lines (a scene break)
 12. <br> after a paragraph, text   one blank line before the text

A line holds more than the text of the block's open element, and a <br> that
ends it adds nothing either:

 13. picture, <br>, caption         the caption right under the picture
 14. picture, trailing <br>         no gap before the next paragraph
 15. text, <br>, picture, <br>,     no gap above or below the picture
     caption
 16. text in a <section>, <br>      no gap, though no paragraph element is open
 17. text after a closed            no gap: the <br> ends the loose text's line
     paragraph, <br>                (which the parser still sets on the
                                    paragraph's last line)

Regenerate with:  python test/epubs/make_test_br_in_blocks.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_br_in_blocks.epub")


def png(width, height, ink):
    """An 8-bit greyscale PNG: ink(x, y) -> True paints black on white. The writer of make_test_inline_images.py."""
    rows = b"".join(
        b"\x00" + bytes(0 if ink(x, y) else 255 for x in range(width)) for y in range(height)
    )

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


# Wider than half the column and taller than a line: placed as a block of its own, not set in a line.
PICTURE = png(240, 120, lambda x, y: ((x // 20 + y // 20) % 2 == 0) or x < 3 or y < 3 or x >= 237 or y >= 117)

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0"
         xmlns="http://www.idpf.org/2007/opf"
         unique-identifier="BookId">

  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"
            xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>BR In Blocks Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-br-in-blocks-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>

  <manifest>
    <item id="ncx"      href="toc.ncx"        media-type="application/x-dtbncx+xml"/>
    <item id="css"      href="style.css"      media-type="text/css"/>
    <item id="picture"  href="picture.png"    media-type="image/png"/>
    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>

  <spine toc="ncx">
    <itemref idref="chapter1"/>
  </spine>

</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN"
  "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-br-in-blocks-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>BR In Blocks Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>BR In Blocks Test</text></navLabel>
      <content src="chapter1.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

CSS = """/* No book-level spacing: every size below comes from the case's own CSS. */
body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 0 }
h1, h2, h3 { margin: 0 }
p.big { font-size: 1.3em }
p.bigger { font-size: 1.2em }
"""

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN"
  "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head>
  <title>BR In Blocks Test</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>

  <p>1. HEADING:</p>
  <h1>Alpha top<br/>Alpha bottom</h1>

  <p>2. TWICE:</p>
  <h2>Bravo one<br/>Bravo two<br/>Bravo three</h2>

  <p>3. SIZED PARAGRAPH:</p>
  <p class="big">Charlie top<br/>Charlie bottom</p>

  <p>4. LONG SIZED PARAGRAPH:</p>
  <p class="bigger">LONGTEXT<br/>Delta after the break</p>

  <p>5. TRAILING BR:</p>
  <h2>Echo heading<br/></h2>
  <p>Echo body text</p>

  <p>6. BR BETWEEN:</p>
  <h2>Foxtrot heading</h2>
  <br/>
  <p>Foxtrot body text</p>

  <p>7. EMPTY HEADING:</p>
  <h3></h3>
  <p>Golf body text</p>

  <p>8. TRAILING BR IN A PARAGRAPH:</p>
  <p>Hotel top<br/></p>
  <p>Hotel next paragraph</p>

  <p>9. TWO BR INSIDE A PARAGRAPH:</p>
  <p>India top<br/><br/>India bottom</p>

  <p>10. TWO TRAILING BR:</p>
  <p>Juliett top<br/><br/></p>
  <p>Juliett next paragraph</p>

  <p>11. TWO BR BETWEEN PARAGRAPHS:</p>
  <p>Kilo top</p>
  <br/>
  <br/>
  <p>Kilo next paragraph</p>

  <p>12. BR AFTER A PARAGRAPH, THEN TEXT:</p>
  <p>Lima top</p>
  <br/>Lima loose text

  <p>13. PICTURE, BR, CAPTION:</p>
  <p><img src="picture.png" alt=""/><br/>Mike caption</p>

  <p>14. PICTURE, TRAILING BR:</p>
  <p><img src="picture.png" alt=""/><br/></p>
  <p>November next paragraph</p>

  <p>15. TEXT, BR, PICTURE, BR, CAPTION:</p>
  <p>Oscar top<br/><img src="picture.png" alt=""/><br/>Oscar caption</p>

  <p>16. TEXT IN A SECTION:</p>
  <section>Papa one<br/>Papa two</section>

  <p>17. TEXT AFTER A CLOSED PARAGRAPH:</p>
  <p>Quebec top</p>Quebec loose one<br/>Quebec loose two

</body>
</html>
""".replace("LONGTEXT", "word0 word1 word2 word3 word4 word5 word6 word7 word8 word9 word10 word11 word12 word13 word14 word15 word16 word17 word18 word19 word20 word21 word22 word23 word24 word25 word26 word27 word28 word29 word30 word31 word32 word33 word34 word35 word36 word37 word38 word39 word40 word41 word42 word43 word44 word45 word46 word47 word48 word49 word50 word51 word52 word53 word54 word55 word56 word57 word58 word59 word60 word61 word62 word63 word64 word65 word66 word67 word68 word69 word70 word71 word72 word73 word74 word75 word76 word77 word78 word79 word80 word81 word82 word83 word84 word85 word86 word87 word88 word89 word90 word91 word92 word93 word94 word95 word96 word97 word98 word99 word100 word101 word102 word103 word104 word105 word106 word107 word108 word109")


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    with zipfile.ZipFile(OUT, "w") as z:
        # mimetype must be first and stored uncompressed
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/style.css", CSS, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/picture.png", PICTURE, zipfile.ZIP_DEFLATED)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
