#!/usr/bin/env python3
"""Build test_heading_split.epub and test_heading_near_miss.epub -- a chapter's heading cut off into a spine file of
its own, and the files that come close to that and are not.

A converter (calibre's split, among others) can leave a chapter's heading in a spine file of its own, with the text in
the next one. Every spine file starts a page, so the heading stood alone on one. The reader lays such a file out at the
top of the next one's first page (HeadingCarry).

test_heading_split.epub (spine items in order):
  0     an ordinary chapter: number with its printed page, rule, date line, text, all in one file
  1-2   the same chapter as calibre cuts it: number and split marker in _split_000, the rest in _split_001
  3-4   a heading file the TOC alone ties to the text (no split names), a printed page from the nav page-list, and
        a text that starts with a forced page break
  5-7   two heading files in a row by split names (a part title, then a number): only the number is carried
  8-9   a heading file before a picture file
  10-11 an empty file with a TOC entry before its text
  12-13 a right-to-left heading file and its text
  14    a heading alone as the last spine item, kept

test_heading_near_miss.epub (spine items in order; its cache files are pinned byte for byte by CacheFileIdentityTest):
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
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SPLIT = os.path.join(HERE, "test_heading_split.epub")
NEAR_MISS = os.path.join(HERE, "test_heading_near_miss.epub")

CSS = """h1 { text-align: center; page-break-after: avoid; }
p { margin: 0; text-indent: 1em; }
"""

SPLIT_CSS = """h1 { text-align: center; page-break-after: avoid; font-size: 1.5em; margin: 0; }
.chapter { border-bottom: 2px solid black; margin: 0 43%; }
.date { text-align: center; font-size: 1.5em; margin: 7% 0 10%; text-indent: 0; }
p { margin: 0; text-indent: 1em; }
.fresh { page-break-before: always; }
"""

DOC = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"{html_attrs}>
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body{body_attrs}>
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


def png_gray(width, height):
    """A tiny 8-bit greyscale PNG (diagonal gradient)."""
    rows = []
    for y in range(height):
        row = bytes(((x * 255) // (width - 1) + y * 2) & 0xFF for x in range(width))
        rows.append(b"\x00" + row)
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def chapter_number(number, page):
    return (f'    <div class="c1">\n      <h1 class="num" id="n{number}"><span epub:type="pagebreak" id="page_{page}" '
            f'role="doc-pagebreak" title="{page}"></span>{number}</h1>\n'
            '      <div class="pb" id="calibre_pb_0"></div>\n    </div>')


def chapter_text(word):
    paragraphs = "\n".join(
        f"      <p>{word} paragraph {i} goes on long enough to take a few lines of the page it is laid out on.</p>"
        for i in range(1, 7))
    return ('    <div class="c1">\n      <div class="chapter"></div>\n      <p class="date">This Summer</p>\n'
            + paragraphs + "\n    </div>")


HEBREW = "\u05e9\u05d1\u05e2"  # "seven"


def split_spine():
    """(file name, title, body, TOC label or None, TOC fragment or None, html attrs, body attrs) per spine item."""
    rtl_text = "\n".join(f"    <p>{HEBREW} {i} {HEBREW} {HEBREW} {HEBREW} {HEBREW} {HEBREW}</p>" for i in range(1, 4))
    whole = chapter_number(1, 10).replace("\n    </div>", "") + "\n" + chapter_text("SAME").replace(
        '    <div class="c1">\n', "")
    return [
        ("c00_Chapter_1.xhtml", "1", whole, "1", None, "", ""),
        ("c01_Chapter_2_split_000.xhtml", "2", chapter_number(2, 12), "2", None, "", ""),
        ("c01_Chapter_2_split_001.xhtml", "2", chapter_text("SAME"), None, None, "", ""),
        ("c03_title.xhtml", "Three", '    <h1 id="three"><span id="pg30"></span>Three</h1>', "Three", None, "", ""),
        ("c04_text.xhtml", "Three", '    <div class="fresh">\n' + chapter_text("THREE") + "\n    </div>", None, None,
         "", ""),
        ("c05_split_000.xhtml", "Part Two", "    <h1>Part Two</h1>", "Part Two", None, "", ""),
        ("c05_split_001.xhtml", "4", "    <h1>4</h1>", None, None, "", ""),
        ("c05_split_002.xhtml", "4", chapter_text("FOUR"), None, None, "", ""),
        ("c08_head.xhtml", "5", "    <h1>5</h1>", "5", None, "", ""),
        ("c09_picture.xhtml", "5", '    <div><img src="img/pic.png" alt=""/></div>', None, None, "", ""),
        ("c10_empty.xhtml", "Six", '    <div class="pb" id="calibre_pb_1"></div>', "Six", None, "", ""),
        ("c11_text.xhtml", "Six", chapter_text("SIX"), None, None, "", ""),
        ("c12_rtl.xhtml", "Seven", f"    <h1>{HEBREW}</h1>", "Seven", None, ' dir="rtl" lang="he"', ' dir="rtl"'),
        ("c13_rtl_text.xhtml", "Seven", rtl_text, None, None, ' dir="rtl" lang="he"', ' dir="rtl"'),
        ("c14_last.xhtml", "End", "    <h1>End</h1>", "End", None, "", ""),
    ]


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


def nav_doc(entries, pages=()):
    items = "\n".join(f'        <li><a href="{href}">{label}</a></li>' for href, label in entries)
    page_list = ""
    if pages:
        page_items = "\n".join(f'        <li><a href="{href}">{label}</a></li>' for href, label in pages)
        page_list = ('    <nav epub:type="page-list" id="pages">\n      <ol>\n' + page_items +
                     "\n      </ol>\n    </nav>\n")
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">\n'
        "  <head><title>Contents</title></head>\n"
        "  <body>\n"
        '    <nav epub:type="toc" id="toc">\n'
        "      <ol>\n" + items + "\n      </ol>\n"
        "    </nav>\n" + page_list +
        "  </body>\n"
        "</html>\n"
    )


def write_book(out, title, identifier, spine_items, css, pages=(), extra=None):
    manifest = ['<item id="css" href="style.css" media-type="text/css"/>',
                '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>']
    spine, files, toc = [], {"style.css": css}, []
    for name, (data, media_type) in (extra or {}).items():
        files[name] = data
        manifest.append(f'<item id="x{len(manifest)}" href="{name}" media-type="{media_type}"/>')
    for i, item in enumerate(spine_items):
        name, doc_title, body, label, fragment = item[:5]
        html_attrs, body_attrs = item[5:7] if len(item) > 5 else ("", "")
        files[name] = DOC.format(title=doc_title, body=body, html_attrs=html_attrs, body_attrs=body_attrs)
        manifest.append(f'<item id="s{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="s{i}"/>')
        if label:
            toc.append((name + ("#" + fragment if fragment else ""), label))
    files["nav.xhtml"] = nav_doc(toc, pages)
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
    write_book(SPLIT, "Heading Split", "heading-split", split_spine(), SPLIT_CSS,
               pages=[("c03_title.xhtml#pg30", "30")], extra={"img/pic.png": (png_gray(120, 60), "image/png")})
    write_book(NEAR_MISS, "Heading Near Misses", "heading-near-miss", near_miss_spine(), CSS)


if __name__ == "__main__":
    main()
