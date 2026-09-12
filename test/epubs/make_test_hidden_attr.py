#!/usr/bin/env python3
"""Build test_hidden_attr.epub -- elements carrying the HTML `hidden` attribute.

test_display_none.epub already covers the CSS route to invisibility. This one
covers the ATTRIBUTE route, which the parser ignored entirely: it extracted
class/style/id/aria-label/title/role and never looked at `hidden`, so anything a
publisher hid that way was laid out and rendered (crosspoint-reader #3390).

EPUBs use it for answer keys, teacher notes, alternate-language blocks and
metadata containers that are meant to ship but not display.

What each chapter pins:
  1. a plain hidden paragraph             -- the simple case
  2. a hidden div wrapping children       -- descendants go too, not just the element
  3. hidden="" and hidden="false"         -- a BOOLEAN attribute: the VALUE is not
                                             consulted, so even "false" means hidden
  4. hidden beside display:block          -- the attribute outranks the CSS cascade
  5. a visible control chapter            -- nothing here may disappear

Every case spells the attribute with a value. EPUB content is XHTML, i.e. XML,
where a valueless attribute is a parse error -- feeding one to the SAX parser
fails the entire spine rather than hiding one element, so no conforming book can
contain the valueless form and the fixture must not either.

Every chapter also carries visible text, so a golden that loses the visible half
is a regression rather than a pass.

Regenerate with:  python test/epubs/make_test_hidden_attr.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_hidden_attr.epub")

CSS = """.shown { display: block; }
.note { font-style: italic; }
"""

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body>
    <h1>{title}</h1>
{body}
  </body>
</html>
"""

CHAPTERS = [
    (
        "Plain Hidden Paragraph",
        '    <p>This opening paragraph is visible and must survive.</p>\n'
        '    <p hidden="hidden">HIDDENMARKERONE this answer key must never reach the page.</p>\n'
        '    <p>This closing paragraph is visible and must survive.</p>',
    ),
    (
        "Hidden Container",
        '    <p>Visible before the container.</p>\n'
        '    <div hidden="hidden">\n'
        '      <p>HIDDENMARKERTWO a child of a hidden div.</p>\n'
        '      <p class="note">HIDDENMARKERTHREE a second child, styled, still hidden.</p>\n'
        '    </div>\n'
        '    <p>Visible after the container.</p>',
    ),
    (
        "Boolean Attribute Forms",
        '    <p>Visible lead-in for the boolean forms.</p>\n'
        '    <p hidden="">HIDDENMARKERFOUR empty value still means hidden.</p>\n'
        '    <p hidden="false">HIDDENMARKERFIVE the literal string false still means hidden.</p>\n'
        '    <p hidden="hidden">HIDDENMARKERSIX the reflexive form.</p>\n'
        '    <p>Visible tail for the boolean forms.</p>',
    ),
    (
        "Attribute Outranks CSS",
        '    <p>The attribute has to win over a rule that asks to show the element.</p>\n'
        '    <p class="shown" hidden="hidden">HIDDENMARKERSEVEN display block in CSS, hidden by attribute.</p>\n'
        '    <p class="shown">This one has the same class and no attribute, so it stays.</p>',
    ),
    (
        "Nothing Hidden Here",
        '    <p>A control chapter: every word in it must appear in the golden.</p>\n'
        '    <p class="note">Including this italic note, which carries no attribute at all.</p>',
    ),
]


def main():
    manifest, spine, files = [], [], {}

    files["style.css"] = CSS
    manifest.append('<item id="css" href="style.css" media-type="text/css"/>')

    for i, (title, body) in enumerate(CHAPTERS, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=title, body=body)
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Hidden Attribute</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">hidden-attr</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n    " + "\n    ".join(manifest) + "\n  </manifest>\n"
        "  <spine>\n    " + "\n    ".join(spine) + "\n  </spine>\n"
        "</package>\n"
    )

    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        for name, content in files.items():
            z.writestr("OEBPS/" + name, content)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
