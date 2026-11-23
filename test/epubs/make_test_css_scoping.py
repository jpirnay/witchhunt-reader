#!/usr/bin/env python3
"""Build test_css_scoping.epub -- the corpus book that pins which stylesheets a chapter resolves against.

A chapter resolves against the stylesheets its <link> elements name, and the sheets those import, as a browser
does (docs/design/stylesheet-scope.md). Before, every stylesheet of the manifest applied to every
chapter, so a sheet only some chapters link (matcha-reader #453's vertical_image.css, an omnibus edition whose
parts each ship a sheet with the same class names) styled the rest of the book too. A paragraph's text-align is its
own rules', so centre.css centres through `p`: under Justified, the default, a book's centre and right stand.

The manifest lists three sheets, in this order:

  css/main.css    @import url("base.css") and .part { text-align: right }
  css/base.css    .imported { font-style: italic }; no chapter links it, main.css imports it
  css/centre.css  p { text-align: center } and .part { text-align: center }

Each chapter has three paragraphs: a plain one, a .part and an .imported one.

  1. links main.css (a commented-out link to centre.css beside it): nothing is centred, .part is right, and
     .imported is italic through main.css's @import
  2. links main.css and centre.css: centred, .part centred (centre.css comes later), .imported italic
  3. links no sheet: every sheet applies, as before (centred, .part centred, .imported italic)
  4. links main.css and a sheet the book does not have: the chapter cannot be scoped, every sheet applies
  5. links centre.css alone: centred, .part centred, and .imported upright -- base.css comes only through main.css

Regenerate with:  python test/epubs/make_test_css_scoping.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_css_scoping.epub")

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

CHAPTERS = 5

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>CSS Scoping Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-css-scoping-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx"    href="toc.ncx"        media-type="application/x-dtbncx+xml"/>
    <item id="main"   href="css/main.css"   media-type="text/css"/>
    <item id="base"   href="css/base.css"   media-type="text/css"/>
    <item id="centre" href="css/centre.css" media-type="text/css"/>
{items}
  </manifest>
  <spine toc="ncx">
{itemrefs}
  </spine>
</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-css-scoping-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>CSS Scoping Test</text></docTitle>
  <navMap>
{points}
  </navMap>
</ncx>
"""

MAIN_CSS = """@charset "utf-8";
@import url("base.css");
.part { text-align: right }
"""

BASE_CSS = """.imported { font-style: italic }
"""

CENTRE_CSS = """p { text-align: center }
.part { text-align: center }
"""


def link(href):
    return '<link rel="stylesheet" type="text/css" href="%s"/>' % href


HEADS = {
    1: link("../css/main.css") + '<!-- <link rel="stylesheet" type="text/css" href="../css/centre.css"/> -->',
    2: link("../css/main.css") + link("../css/centre.css"),
    3: "",
    4: link("../css/main.css") + link("../css/missing.css"),
    5: link("../css/centre.css"),
}

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Chapter {n}</title>{head}</head>
<body>
<p>CH{n} BODY Plain running text.</p>
<p class="part">CH{n} PART The part's own class.</p>
<p class="imported">CH{n} IMPORTED Styled by base.css.</p>
</body>
</html>
"""


def main():
    items = "\n".join(
        '    <item id="ch%d" href="text/ch%d.xhtml" media-type="application/xhtml+xml"/>' % (n, n)
        for n in range(1, CHAPTERS + 1)
    )
    itemrefs = "\n".join('    <itemref idref="ch%d"/>' % n for n in range(1, CHAPTERS + 1))
    points = "\n".join(
        '    <navPoint id="np%d" playOrder="%d"><navLabel><text>Chapter %d</text></navLabel>'
        '<content src="text/ch%d.xhtml"/></navPoint>' % (n, n, n, n)
        for n in range(1, CHAPTERS + 1)
    )
    with zipfile.ZipFile(OUT, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF.format(items=items, itemrefs=itemrefs), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX.format(points=points), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/css/main.css", MAIN_CSS, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/css/base.css", BASE_CSS, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/css/centre.css", CENTRE_CSS, compress_type=zipfile.ZIP_DEFLATED)
        for n in range(1, CHAPTERS + 1):
            z.writestr(
                "OEBPS/text/ch%d.xhtml" % n,
                CHAPTER.format(n=n, head=HEADS[n]),
                compress_type=zipfile.ZIP_DEFLATED,
            )
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
