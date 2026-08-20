#!/usr/bin/env python3
"""Build test_split_footnotes.epub — the corpus book for notes SPLIT ACROSS MANY
DOCUMENTS, one note per spine file.

test_inline_footnotes.epub covers the other real-world shape: every note body in a
single rearnotes document, which a chapter resolves with one stream. The Discworld
EPUBs are the opposite and are what this fixture models — Feet of Clay puts its 14
notes in 14 separate spine files, Small Gods its 10 in 10 — so a single chapter's
callers point into several different documents and the resolver has to stream each
one. That is also the shape that exercises banking: the note documents are spine
entries nobody reads as chapters, so nothing else ever inflates them.

Chapter 1 references notes 1-3 (three documents), chapter 2 references note 4 and
note 1 again (a document already resolved, which must not be streamed twice).
Note bodies carry the two pieces of chrome real converters emit — the number as a
heading and a "back" link — so the preview text must come out as prose alone.

Regenerate with:  python test/epubs/make_test_split_footnotes.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_split_footnotes.epub")

NOTES = {
    1: "The Patrician had a broad and comprehensive view of the world, which is to say a short one.",
    2: "Or would have done, if anyone had thought to ask him first.",
    3: "This is not true of dwarfs, who measure height from the ground up like everyone else.",
    4: "Assassins are, however, extremely polite about it.",
}

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>{title}</title></head>
  <body>
    <h1>{title}</h1>
    <p>{body}</p>
  </body>
</html>
"""

NOTE_DOC = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>{n}</title></head>
  <body>
    <div class="note" id="note_{n}">
      <div class="title"><h2>{n}</h2></div>
      <p>{text}</p>
      <a class="note_anchor" href="chapter{chapter}.xhtml#note_{n}_back">note_{n}</a>
    </div>
  </body>
</html>
"""


def caller(n, chapter):
    return f'<a id="note_{n}_back" href="note{n}.xhtml#note_{n}">[{n}]</a>'


def main():
    chapter1 = CHAPTER.format(
        title="Chapter One",
        body="The city watch had a proud tradition" + caller(1, 1) + " of arriving shortly after the "
        "event, which the Guild of Thieves" + caller(2, 1) + " considered only sporting. Dwarfs were "
        "of course another matter" + caller(3, 1) + " entirely.",
    )
    chapter2 = CHAPTER.format(
        title="Chapter Two",
        body="Assassination was a licensed profession" + caller(4, 2) + " in Ankh-Morpork, and the "
        "Patrician" + caller(1, 2) + " approved of licences.",
    )

    manifest, spine, files = [], [], {}
    files["chapter1.xhtml"] = chapter1
    files["chapter2.xhtml"] = chapter2
    manifest.append('<item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>')
    manifest.append('<item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>')
    spine.append('<itemref idref="chapter1"/>')
    spine.append('<itemref idref="chapter2"/>')
    for n, text in NOTES.items():
        name = f"note{n}.xhtml"
        files[name] = NOTE_DOC.format(n=n, text=text, chapter=2 if n == 4 else 1)
        manifest.append(f'<item id="note{n}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="note{n}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Split Footnotes</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">split-footnotes</dc:identifier>\n'
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
