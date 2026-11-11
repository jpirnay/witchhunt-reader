#!/usr/bin/env python3
"""Build test_inline_images.epub -- the corpus book that pins how small images are set
in their line of text (the parser's line images, ParsedText's inline objects).

An image at most two of its paragraph's lines tall, inside a run of text, is laid out
as a word: it takes its width, wraps, justifies, and sits on the baseline unless its
vertical-align says otherwise; a taller one lowers its line. Larger images, images the
book floats or makes a block, and the images of a paragraph that holds no text keep
their block placement.

Chapter 1, Latin:
   1. BASELINE   a 16 px key icon mid-sentence
   2. EM         an icon sized height: 1em
   3. TALL       a 44 px image that lowers its line
   4. VALIGN     middle, text-top, text-bottom, top, bottom, super, sub
   5. GLYPH      an image for a letter the font lacks, inside a word ("<img>gir")
   6. EMOJI      a run of glued emoji images that wraps between them
   7. LINK       an image alone inside a link to a note; a link with an image and text
   8. JUSTIFY    a justified paragraph with icons, long words for hyphenation
   9. FORMULA    a 280x28 formula image in a sentence
  10. MANY       thirty icons in one paragraph (laid out a split at a time)
  11. ORNAMENT   an image-only paragraph: a centred block, as before
  12. PICTURE    a 240x180 picture mid-paragraph: the block path, as before
  13. BLOCK      a small image the book makes display: block
  14. FLOAT      a small image floated left
  22. LENGTH     vertical-align as a length: em, ex, px and a percentage of the line height
Chapter 2, Hebrew (right to left): an icon between two words, ordered as a neutral.
Chapter 3, Japanese: an icon among CJK characters, one beside a ruby, one inside a ruby
(the block path, as before).
Chapter 4, Japanese set vertical-rl (in the _vertical golden; the reader's default sets it horizontal): an icon in a
column is an upright character (a gaiji), one em across at most and centred on the column; one beside a ruby; a
taller one shrunk to the em; a picture keeps the block path.

Regenerate with:  python test/epubs/make_test_inline_images.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import math
import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_inline_images.epub")


def png(width, height, ink):
    """An 8-bit greyscale PNG: ink(x, y) -> True paints black on white. No external dependencies."""
    rows = b"".join(
        b"\x00" + bytes(0 if ink(x, y) else 255 for x in range(width)) for y in range(height)
    )

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def disc(w, h):
    cx, cy, r = (w - 1) / 2, (h - 1) / 2, min(w, h) / 2 - 0.5
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(w, h):
    cx, cy, r = (w - 1) / 2, (h - 1) / 2, min(w, h) / 2 - 0.5
    return lambda x, y: (r - 2.5) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def key(w, h):
    # A ring with a bar: a key's bow and shank.
    bow = ring(h, h)
    return lambda x, y: (x < h and bow(x, y)) or (x >= h - 2 and abs(y - h // 2) <= 1) or (x >= w - 4 and y > h // 2)


def star(w, h):
    cx, cy = (w - 1) / 2, (h - 1) / 2

    def inside(x, y):
        a = math.atan2(y - cy, x - cx)
        r = math.hypot(x - cx, y - cy)
        return r <= (min(w, h) / 2 - 1) * (0.55 + 0.45 * math.cos(5 * a) ** 2)

    return inside


def smiley(w, h):
    face, cx, cy = ring(w, h), (w - 1) / 2, (h - 1) / 2
    return lambda x, y: face(x, y) or (
        abs(y - cy * 0.7) <= 1.5 and (abs(x - cx * 0.65) <= 1.5 or abs(x - cx * 1.35) <= 1.5)
    ) or (abs(math.hypot(x - cx, y - cy) - w * 0.25) <= 1 and y > cy)


def glyph_ae(w, h):
    # A rough Æ: two strokes of an A meeting an E's spine and bars.
    def inside(x, y):
        a_left = abs(x - (w * 0.45) * (1 - y / h)) <= 1.2
        spine = abs(x - w * 0.5) <= 1.2
        bars = x >= w * 0.5 and (y <= 1.5 or abs(y - h / 2) <= 1 or y >= h - 2.5)
        cross = abs(y - h * 0.6) <= 1 and x <= w * 0.5
        return a_left or spine or bars or cross

    return inside


def formula(w, h):
    # Strokes standing for "x = (-b ± √(b² - 4ac)) / 2a": a fraction bar, a root sign, letters as boxes.
    def inside(x, y):
        bar = abs(y - h // 2) <= 0 and 40 <= x <= w - 8
        root = 60 <= x <= 72 and abs((y - 4) - (x - 60) * 0.0) <= 0 or (72 <= x <= w - 20 and y == 3)
        letters = (y < h // 2 - 2 or y > h // 2 + 2) and (x // 10) % 3 == 1 and 2 < y < h - 2
        eq = 10 <= x <= 24 and y in (h // 2 - 3, h // 2 + 3)
        return bar or root or letters or eq

    return inside


def ornament(w, h):
    return lambda x, y: abs(y - h / 2 - 6 * math.sin(x / w * 4 * math.pi)) <= 1.5


def picture(w, h):
    return lambda x, y: ((x // 20 + y // 20) % 2 == 0) or x < 3 or y < 3 or x >= w - 3 or y >= h - 3


IMAGES = {
    "key.png": (22, 16, key),
    "star.png": (18, 18, star),
    "smiley.png": (20, 20, smiley),
    "ae.png": (16, 18, glyph_ae),
    "tall.png": (30, 44, ring),
    "dot.png": (12, 12, disc),
    "formula.png": (280, 28, formula),
    "ornament.png": (80, 20, ornament),
    "picture.png": (240, 180, picture),
}

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

CHAPTERS = ["chapter1", "chapter2", "chapter3", "chapter4"]


def opf():
    images = "\n".join(
        f'    <item id="img{i}" href="images/{name}" media-type="image/png"/>' for i, name in enumerate(IMAGES)
    )
    chapters = "\n".join(
        f'    <item id="{c}" href="text/{c}.xhtml" media-type="application/xhtml+xml"/>' for c in CHAPTERS
    )
    spine = "\n".join(f'    <itemref idref="{c}"/>' for c in CHAPTERS)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Inline Images Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-inline-images-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="css" href="css/style.css" media-type="text/css"/>
{images}
{chapters}
  </manifest>
  <spine toc="ncx">
{spine}
  </spine>
</package>
"""


def ncx():
    titles = ["Latin", "Hebrew", "Japanese", "Vertical"]
    points = "\n".join(
        f'    <navPoint id="np{i + 1}" playOrder="{i + 1}"><navLabel><text>{t}</text></navLabel>'
        f'<content src="text/{c}.xhtml"/></navPoint>'
        for i, (c, t) in enumerate(zip(CHAPTERS, titles))
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-inline-images-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Inline Images Test</text></docTitle>
  <navMap>
{points}
  </navMap>
</ncx>
"""


CSS = """p { margin: 0 0 0.5em 0; text-indent: 0 }
p.just { text-align: justify }
p.orn { text-align: center }
img.em { height: 1em }
img.block { display: block; margin: 0 auto }
img.float { float: left }
.va-middle { vertical-align: middle }
.va-text-top { vertical-align: text-top }
.va-text-bottom { vertical-align: text-bottom }
.va-top { vertical-align: top }
.va-bottom { vertical-align: bottom }
.va-super { vertical-align: super }
.va-sub { vertical-align: sub }
.va-em { vertical-align: -0.3em }
.va-ex { vertical-align: 0.5ex }
.va-px { vertical-align: 4px }
.va-pct { vertical-align: -20% }
body.v { writing-mode: vertical-rl }
"""


def xhtml(title, body, lang="en", html_attrs="", body_attrs=""):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="{lang}" lang="{lang}"{html_attrs}>
<head><title>{title}</title><link rel="stylesheet" type="text/css" href="../css/style.css"/></head>
<body{body_attrs}>
{body}
</body>
</html>
"""


def img(name, cls="", alt=""):
    c = f' class="{cls}"' if cls else ""
    return f'<img src="../images/{name}" alt="{alt}"{c}/>'


ICONS = " ".join(img("dot.png") + f" w{i}" for i in range(30))

CHAPTER1 = xhtml(
    "Latin",
    f"""<p>CASE1 BASELINE Press the {img("key.png", alt="key")} key to open the door, then turn left.</p>
<p>CASE2 EM An icon one em tall {img("star.png", "em", "star")} sits on the line like a letter does.</p>
<p>CASE3 TALL This line holds a taller image {img("tall.png", alt="ring")} that lowers its text, and the
line before it keeps its place above; the next line follows below the taller line.</p>
<p>CASE4 VALIGN mid {img("dot.png", "va-middle")} ttop {img("dot.png", "va-text-top")}
tbot {img("dot.png", "va-text-bottom")} top {img("dot.png", "va-top")} bot {img("dot.png", "va-bottom")}
sup {img("dot.png", "va-super")} sub {img("dot.png", "va-sub")} end.</p>
<p>CASE5 GLYPH The sea god {img("ae.png", alt="AE")}gir and the hall of {img("ae.png", alt="AE")}sir stand whole.</p>
<p>CASE6 EMOJI {"".join(img("smiley.png", alt=":)") for _ in range(14))} the run wraps between its images.</p>
<p>CASE7 LINK See the note <a href="#note1">{img("star.png", alt="note")}</a> here, and
<a href="#note1">{img("key.png")} the keyed note</a> there.</p>
<p class="just">CASE8 JUSTIFY Notwithstanding the extraordinarily {img("star.png")} uncharacteristic circumstances,
the representatives {img("key.png")} acknowledged the incomprehensibilities {img("dot.png")} of
the counterrevolutionary proposals, establishing {img("smiley.png")} unquestionably the
interdisciplinary collaboration.</p>
<p>CASE9 FORMULA The roots are {img("formula.png", "va-middle", "quadratic formula")} for every quadratic.</p>
<p>CASE10 MANY {ICONS} end.</p>
<p class="orn">{img("ornament.png", alt="ornament")}</p>
<p>CASE11 ORNAMENT above: a paragraph of an image alone stays a centred block.</p>
<p>CASE12 PICTURE before {img("picture.png", alt="picture")} after: a picture keeps its block.</p>
<p>CASE13 BLOCK before {img("star.png", "block")} after: the book made it a block.</p>
<p>CASE14 FLOAT {img("star.png", "float")} a floated image keeps the float path beside this text.</p>
<p>CASE22 LENGTH em {img("dot.png", "va-em")} ex {img("dot.png", "va-ex")} px {img("dot.png", "va-px")}
pct {img("formula.png", "va-pct")} end.</p>
<p id="note1">NOTE1 The note the linked images lead to.</p>""",
)

CHAPTER2 = xhtml(
    "Hebrew",
    f"""<p dir="rtl">CASE15 RTL שלום {img("key.png")} עולם וגם
{img("star.png")} המשך.</p>""",
    lang="he",
    html_attrs=' dir="rtl"',
)

CHAPTER3 = xhtml(
    "Japanese",
    f"""<p>CASE16 CJK 吾輩は猫{img("dot.png")}である。名前はまだ無い。どこで生れたか{img("star.png")}とんと見当がつかぬ。</p>
<p>CASE17 RUBY {img("dot.png")}<ruby>漢字<rt>かんじ</rt></ruby>の前に画像。</p>
<p>CASE18 INRUBY 前<ruby>漢{img("dot.png")}<rt>かん</rt></ruby>後。</p>""",
    lang="ja",
)

CHAPTER4 = xhtml(
    "Vertical",
    f"""<p>CASE19 VERTICAL 縦書きの文{img("dot.png")}の中の画像。</p>
<p>CASE20 GAIJI <ruby>漢<rt>かん</rt></ruby>{img("ae.png", "gaiji", "gaiji")}字の後、{img("tall.png", "gaiji")}も一字。</p>
<p>CASE21 PICTURE 前{img("picture.png")}後。</p>""",
    lang="ja",
    body_attrs=' class="v"',
)


def main():
    with zipfile.ZipFile(OUT, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", opf(), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", ncx(), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/css/style.css", CSS, compress_type=zipfile.ZIP_DEFLATED)
        for name, (w, h, shape) in IMAGES.items():
            z.writestr(f"OEBPS/images/{name}", png(w, h, shape(w, h)), compress_type=zipfile.ZIP_DEFLATED)
        for name, text in zip(CHAPTERS, [CHAPTER1, CHAPTER2, CHAPTER3, CHAPTER4]):
            z.writestr(f"OEBPS/text/{name}.xhtml", text, compress_type=zipfile.ZIP_DEFLATED)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
