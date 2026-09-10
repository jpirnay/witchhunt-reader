#!/usr/bin/env python3
"""Build test_link_supersub.epub — footnote references raised by CSS on the <a>
itself rather than by a <sup> wrapper.

test_inline_footnotes.epub and test_split_footnotes.epub both mark their callers
as plain "[n]" link text. The other half of the real-world population styles the
link instead:

    a.noteref { vertical-align: super; font-size: 0.7em }

The parser's internal-link branch builds its own style-stack entry and then skips
the generic inline styling path, so anything that branch does not apply itself is
lost. That branch applied underline and nothing else, and every reference in books
of this shape rendered full-size on the baseline (crosspoint-reader #3355).

The four cases this fixture pins, one per chapter, all as INTERNAL links so they
take that branch:
  1. vertical-align: super via a class rule            -> raised, 50% default size
  2. vertical-align: super plus an explicit font-size  -> publisher size wins
  3. vertical-align: sub                               -> lowered
  4. vertical-align: baseline on a link inside a <sup> -> explicitly cancelled

Case 4 is the one that separates our behaviour from upstream's: their helper
ignores `baseline`, ours treats it as a cancel, matching the inline path.

Regenerate with:  python test/epubs/make_test_link_supersub.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_link_supersub.epub")

CSS = """a.super { vertical-align: super; }
a.supersized { vertical-align: super; font-size: 0.8em; }
a.sub { vertical-align: sub; }
a.flat { vertical-align: baseline; }
"""

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body>
    <h1>{title}</h1>
    <p>{body}</p>
  </body>
</html>
"""

NOTE_DOC = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>Notes</title></head>
  <body>
    <p id="n1">The first note, reached from a class-styled superscript link.</p>
    <p id="n2">The second note, whose caller carries its own font-size.</p>
    <p id="n3">The third note, reached from a subscripted link.</p>
    <p id="n4">The fourth note, whose caller cancels the surrounding sup.</p>
  </body>
</html>
"""

CHAPTERS = [
    (
        "Raised By Class",
        'A rule on the link alone lifts this reference<a class="super" href="notes.xhtml#n1">1</a> '
        "clear of the baseline, and it should shrink the way a sup-wrapped marker does. The "
        "surrounding sentence must keep its own metrics so the raised glyph is the only thing "
        "that moves on the line.",
    ),
    (
        "Raised With A Size",
        'Here the publisher sets the size as well<a class="supersized" href="notes.xhtml#n2">2</a> '
        "so the 50% default has to give way to the stylesheet rather than override it. Body text "
        "either side of the marker anchors the comparison.",
    ),
    (
        "Lowered",
        'Subscripted callers are rarer but legal<a class="sub" href="notes.xhtml#n3">3</a> and take '
        "the same path through the parser, so they pin the other direction of the same branch.",
    ),
    (
        "Cancelled Inside A Sup",
        'A link that asks for the baseline inside a raised run<sup>lifted '
        '<a class="flat" href="notes.xhtml#n4">4</a> back down</sup> must drop back to the '
        "baseline instead of inheriting the sup, which is what an explicit cancel means.",
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

    files["notes.xhtml"] = NOTE_DOC
    manifest.append('<item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/>')
    spine.append('<itemref idref="notes"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Link Super And Sub</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">link-supersub</dc:identifier>\n'
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
