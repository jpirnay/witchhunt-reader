#!/usr/bin/env python3
"""Build test_heading_near_miss.epub -- spine files that look like a chapter's heading cut off by a converter, and are not.

A converter (calibre's split, among others) can leave a chapter's heading in a spine file of its own, with the text in
the next one. Every spine file starts a page, so the heading stood alone on one. This book holds the files that come
close to that shape but must keep their own page, and its cache files are pinned byte for byte
(CacheFileIdentityTest): a layout rule for the real shape must not reach them.

What each pair pins (spine items in order):
  0     an ordinary chapter (heading and text in one file)
  1-2   a heading alone whose next file has a TOC entry of its own: two chapters
  3-4   calibre split names, but the first file holds a paragraph besides its heading
  5-6   a heading alone with a link in it
  7-8   a heading alone and a rule
  9-10  a heading alone longer than a heading line (over 200 bytes of text)
  11-12 a heading alone padded past 4 KB
  13-14 a TOC entry that points into the heading file with a fragment
  15-16 a heading alone with no TOC entry and no split name before a file that has one
  17    a heading alone as the last spine item

Regenerate with:  python test/epubs/make_test_heading_split.py
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
NEAR_MISS = os.path.join(HERE, "test_heading_near_miss.epub")

CSS = """h1 { text-align: center; page-break-after: avoid; }
p { margin: 0; text-indent: 1em; }
"""

DOC = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body>
{body}
  </body>
</html>
"""

TEXT = (
    "    <p>{word} opens the chapter on the first line of its text, after the heading that names it.</p>\n"
    "    <p>{word} goes on with a second paragraph, long enough to take more than one line of the page.</p>\n"
    "    <p>{word} closes it.</p>"
)


def text(word):
    return TEXT.format(word=word)


def heading(label, inner=None):
    return f'    <h1 id="h-{label.lower()}">{inner if inner is not None else label}</h1>'


def near_miss_spine():
    """(file name, title, body, TOC label or None, TOC fragment or None) per spine item."""
    long_heading = " ".join(["Longheading"] * 20)  # 239 bytes of heading text
    padding = "\n".join("    <!-- " + "padding " * 12 + "-->" for _ in range(50))  # past 4 KB of XHTML
    return [
        ("c00_whole.xhtml", "Whole", heading("Whole") + "\n" + text("WHOLE"), "Whole", None),
        ("c01_head.xhtml", "Own Entry", heading("Owned"), "Owned", None),
        ("c02_text.xhtml", "Own Entry Text", text("OWNEDTEXT"), "Owned text", None),
        ("c03_split_000.xhtml", "Split", heading("Split") + "\n    <p>SPLITNOTE besides the heading.</p>", "Split",
         None),
        ("c03_split_001.xhtml", "Split", text("SPLITTEXT"), None, None),
        ("c05_link.xhtml", "Link", heading("Linked", '<a href="c00_whole.xhtml">Linked</a>'), "Linked", None),
        ("c06_text.xhtml", "Link Text", text("LINKTEXT"), None, None),
        ("c07_rule.xhtml", "Rule", heading("Ruled") + "\n    <hr/>", "Ruled", None),
        ("c08_text.xhtml", "Rule Text", text("RULETEXT"), None, None),
        ("c09_long.xhtml", "Long", heading("Long", long_heading), "Long", None),
        ("c10_text.xhtml", "Long Text", text("LONGTEXT"), None, None),
        ("c11_big.xhtml", "Big", heading("Big") + "\n" + padding, "Big", None),
        ("c12_text.xhtml", "Big Text", text("BIGTEXT"), None, None),
        ("c13_frag.xhtml", "Fragment", heading("Fragment"), "Fragment", "h-fragment"),
        ("c14_text.xhtml", "Fragment Text", text("FRAGTEXT"), None, None),
        ("c15_bare.xhtml", "Bare", heading("Bare"), None, None),
        ("c16_text.xhtml", "Bare Text", text("BARETEXT"), "Bare text", None),
        ("c17_last.xhtml", "Last", heading("Last"), "Last", None),
    ]


def nav_doc(entries):
    items = "\n".join(f'        <li><a href="{href}">{label}</a></li>' for href, label in entries)
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">\n'
        "  <head><title>Contents</title></head>\n"
        "  <body>\n"
        '    <nav epub:type="toc" id="toc">\n'
        "      <ol>\n" + items + "\n      </ol>\n"
        "    </nav>\n"
        "  </body>\n"
        "</html>\n"
    )


def write_book(out, title, identifier, spine_items):
    manifest = ['<item id="css" href="style.css" media-type="text/css"/>',
                '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>']
    spine, files, toc = [], {"style.css": CSS}, []
    for i, (name, doc_title, body, label, fragment) in enumerate(spine_items):
        files[name] = DOC.format(title=doc_title, body=body)
        manifest.append(f'<item id="s{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="s{i}"/>')
        if label:
            toc.append((name + ("#" + fragment if fragment else ""), label))
    files["nav.xhtml"] = nav_doc(toc)
    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        f'    <dc:identifier id="bookid">{identifier}</dc:identifier>\n'
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

    # Fixed timestamps: the archive is committed, so a rebuild must be byte-identical.
    def add(z, name, data, compress=zipfile.ZIP_DEFLATED):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = compress
        z.writestr(info, data)

    with zipfile.ZipFile(out, "w") as z:
        add(z, "mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        add(z, "META-INF/container.xml", container)
        add(z, "OEBPS/content.opf", opf)
        for name, content in files.items():
            add(z, "OEBPS/" + name, content)
    print("wrote", out)


def main():
    write_book(NEAR_MISS, "Heading Near Misses", "heading-near-miss", near_miss_spine())


if __name__ == "__main__":
    main()
