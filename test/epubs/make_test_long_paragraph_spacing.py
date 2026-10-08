#!/usr/bin/env python3
"""Build test_long_paragraph_spacing.epub — the corpus book that pins the top
spacing of a paragraph over 96 words.

The parser lays a block that passes 96 words out part-way, mid-paragraph, and
everything after that first chunk is a continuation whose top spacing must not
be applied again. The first chunk never got it either, so a long paragraph lost
its margin-top and padding-top -- and with them the blank line of a <br> scene
break and the margin-bottom of an empty container before it, which are both
carried as the next block's margin-top.

Each spine item is one case: a SHORT paragraph first, as the reference, then the
same setup before a LONG one. The gap above the long paragraph's first line must
match the gap above the short one.

  1. paragraph opening the chapter  margin-top above the first line of the page
  2. <br> scene break               one blank line before the paragraph
  3. margin-top                     the paragraph's own margin
  4. collapsed margins              margin-bottom 1em then margin-top 0.5em:
                                    the larger one, not the sum
  5. padding-top                    added after the margin
  6. empty spacer div               its margin-bottom opens the paragraph
  7. left float beside it           the float starts under the margin, and the
                                    paragraph after the long one wraps against
                                    where the float really is

Chapter 1 has only the long paragraph: the page does not exist yet when its
first chunk is laid out, and the spacing must survive the page being created.
Label paragraphs carry no spacing. At the corpus font 1em = 18px and a body line
is 24px, so an 18px gap shows as a 42px step between LINE y values.

Regenerate with:  python test/epubs/make_test_long_paragraph_spacing.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_long_paragraph_spacing.epub")

# Case 7's float: taller than the long paragraph beside it, so the paragraph after that one still
# wraps beside the image. The height puts one line of that paragraph (y=522) inside the 18px the
# margin moved the float down by: it wraps only if the float's extent moved with it.
FIG_W, FIG_H = 100, 490

CSS = """/* No book-level spacing: every gap below comes from the case's own CSS. */
body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 0 }
p.mt { margin-top: 1em }
p.mb { margin-bottom: 1em }
p.mt-half { margin-top: 0.5em }
p.pt { padding-top: 0.5em }
div.spacer { margin-bottom: 1em }
div.figleft { float: left; margin: 0; padding: 0 }
"""

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body}
</body>
</html>
"""

WORDS = (
    "the harbour lay quiet under a low grey sky while the boats rocked at their moorings and "
    "gulls wheeled over the fish market calling to one another across the wet stones of the quay"
).split()


def png_gray(width, height):
    """A small 8-bit greyscale PNG (diagonal gradient) -- enough for the header read and a dither."""
    rows = []
    for y in range(height):
        row = bytes(((x * 255) // (width - 1) + y * 2) & 0xFF for x in range(width))
        rows.append(bytes([0]) + row)  # filter type 0 per scanline
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    signature = bytes([0x89]) + b"PNG" + bytes([0x0D, 0x0A, 0x1A, 0x0A])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)  # 8-bit, greyscale, no interlace
    return signature + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def long_text(tag):
    """A paragraph of 104 words, past the 96 that trigger the part-way layout."""
    words = [tag] + [WORDS[i % len(WORDS)] for i in range(103)]
    return " ".join(words) + "."


def short_text(tag):
    return tag + " is a short paragraph."


def case(label, setup, cls, tag):
    """The setup, then a paragraph with class `cls`: short first, long second."""
    attr = f' class="{cls}"' if cls else ""
    return (
        f"  <p>{label} SHORT:</p>\n{setup}  <p{attr}>{short_text(tag + 'S')}</p>\n"
        f"  <p>{label} LONG:</p>\n{setup}  <p{attr}>{long_text(tag + 'L')}</p>\n"
    )


FLOAT_CASE = (
    "  <p>7. FLOAT LONG:</p>\n"
    f'  <div class="figleft"><img src="fig.png" alt="" width="{FIG_W}" height="{FIG_H}"/></div>\n'
    f'  <p class="mt">{long_text("Case7L")}</p>\n'
    f"  <p>{' '.join(['Case7After'] + [WORDS[i % len(WORDS)] for i in range(60)])}.</p>\n"
)

CHAPTERS = [
    ("Opening paragraph", f'  <p class="mt">{long_text("Case1L")}</p>\n'),
    ("BR scene break", case("2. BR", "  <p>Before the break.</p>\n  <br/>\n", "", "Case2")),
    ("Margin top", case("3. MARGIN", "", "mt", "Case3")),
    ("Collapsed margins", case("4. COLLAPSE", '  <p class="mb">Margin bottom above.</p>\n', "mt-half", "Case4")),
    ("Padding top", case("5. PADDING", "", "pt", "Case5")),
    ("Spacer div", case("6. SPACER", '  <div class="spacer"></div>\n', "", "Case6")),
    ("Float beside", FLOAT_CASE),
]


def main():
    manifest = [
        '<item id="css" href="style.css" media-type="text/css"/>',
        '<item id="fig" href="fig.png" media-type="image/png"/>',
    ]
    spine = []
    files = {"style.css": CSS}
    for i, (title, body) in enumerate(CHAPTERS, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=title, body=body.rstrip("\n"))
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Long Paragraph Spacing</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">long-paragraph-spacing</dc:identifier>\n'
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
        z.writestr("OEBPS/fig.png", png_gray(FIG_W, FIG_H))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
