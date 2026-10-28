#!/usr/bin/env python3
"""Build test_writing_modes.epub and test_rtl_markup.epub: the markup that says how text runs, as real books write it.

Both stand in for real books whose markup cannot go in the repository. They are still set horizontally and left to
right; what they pin is what the parser reads from them (ChapterHtmlSlimParser, CssStyle::resolveLogical).

test_writing_modes.epub, language ja, spine page-progression-direction="rtl" (the golden's BOOK line ends ppd=rtl):
  - Spine 1, as a calibre conversion writes it: the mode only as -webkit-writing-mode, through a class on <body>.
    Its contents entries are `.toc { margin: 1em 0 }`, which in a vertical book means "start each column 1em down,
    columns touching": read along the writing mode they sit on consecutive lines (24 px apart in the host stub)
    inset by one em (x=18). The heading's `padding-top: 2em` is an inset too. Tate-chu-yoko arrives as a class with
    -webkit-text-combine and text-combine-upright, an upright Latin run as -epub-text-orientation, and a
    `writing-mode: horizontal-tb` div holds one paragraph read as written (its margins above and below).
  - Spine 2, as the EBPAJ template writes it: the mode on html.vrtl, with -ms-writing-mode beside it.
  - Spine 3, a control: the same contents list with no writing mode, 42 px apart (a line and the collapsed 18 px
    margin) and not inset.

test_rtl_markup.epub, language he, no page progression: <html dir="rtl">, text-align start and end, a div with
direction: ltr, <bdo> and <bdi>, a <span dir="ltr">, a dir="auto" paragraph and a unicode-bidi: plaintext rule. Its
_bookalign golden (the book's own alignment) shows start right and end left under rtl, and start left inside the ltr
div and on the dir="auto" paragraph (auto reads as ltr until the right-to-left step decides it from the text).

Regenerate with:  python test/epubs/make_test_writing_modes.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CONTAINER = (
    '<?xml version="1.0"?>\n'
    '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
    '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
    'media-type="application/oebps-package+xml"/></rootfiles>\n'
    "</container>\n"
)

WRITING_MODES_CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 0 }
.class { -webkit-writing-mode: vertical-rl; display: block }
html.vrtl { writing-mode: vertical-rl; -ms-writing-mode: tb-rl }
.toc { margin: 1em 0 }
.tcy { -webkit-text-combine: horizontal; text-combine-upright: all }
.hd { padding-top: 2em; margin: 0.83em 0; font-size: 1em; font-weight: normal }
p.body { text-indent: 1em }
.up { -epub-text-orientation: upright }
"""

# No links: a link into the same chapter would be laid out with its target's text as an inline footnote preview.
CONTENTS = (
    '<p class="toc"><span class="tcy">１</span> 第一章</p>\n'
    '<p class="toc"><span class="tcy">１２</span> 第十二章</p>\n'
    '<p class="toc"><span class="tcy">３４</span> 第三十四章</p>\n'
)

SPINE1 = (
    CONTENTS
    + '<h2 class="hd" id="c1">第一章</h2>\n'
    + '<p class="body">吾輩は猫である。名前はまだ無い。<span class="up">ABC</span>どこで生れたかとんと見当がつかぬ。</p>\n'
    + '<p class="body" id="c2">何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。</p>\n'
    + '<div style="writing-mode: horizontal-tb"><p class="toc" id="c3">横書きの段落。</p></div>\n'
)

RTL_CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 0 }
.s { text-align: start }
.e { text-align: end }
.l { direction: ltr }
.pt { unicode-bidi: plaintext }
"""

RTL_BODY = (
    '<p class="s">שלום עולם, התחלה.</p>\n'
    '<p class="e">סוף הפסקה.</p>\n'
    '<div class="l"><p class="s">Left to right inside, start.</p></div>\n'
    '<p class="s" dir="auto">טקסט אוטומטי</p>\n'
    '<p class="s">מילה <bdo dir="ltr">bdo text</bdo> ו<bdi>bdi text</bdi> ו<span dir="ltr">Latin 123</span></p>\n'
    '<p class="s pt">פסקה רגילה.</p>\n'
)


def chapter(title, body, lang, html_attrs="", body_attrs=""):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}"{html_attrs}>\n'
        f'<head>\n  <title>{title}</title>\n  <link rel="stylesheet" type="text/css" href="style.css"/>\n</head>\n'
        f"<body{body_attrs}>\n{body}</body>\n</html>\n"
    )


def opf(title, identifier, lang, chapters, spine_attrs=""):
    items = "".join(
        f'    <item id="ch{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>\n'
        for i in range(1, len(chapters) + 1)
    )
    refs = "".join(f'    <itemref idref="ch{i}"/>\n' for i in range(1, len(chapters) + 1))
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        f"    <dc:language>{lang}</dc:language>\n"
        f'    <dc:identifier id="bookid">{identifier}</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        f"{items}"
        "  </manifest>\n"
        f"  <spine{spine_attrs}>\n{refs}  </spine>\n"
        "</package>\n"
    )


def write(name, files):
    out = os.path.join(HERE, name)
    with zipfile.ZipFile(out, "w") as z:
        for path, data, method in files:
            # A fixed date keeps a regenerated book byte-identical.
            info = zipfile.ZipInfo(path, date_time=(2026, 10, 26, 0, 0, 0))
            info.compress_type = method
            z.writestr(info, data)
    print("wrote", out)


def main():
    stored, deflated = zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED
    vertical = [
        chapter("Calibre", SPINE1, "ja", body_attrs=' class="class"'),
        chapter("EBPAJ", CONTENTS, "ja", html_attrs=' class="vrtl"'),
        chapter("Control", CONTENTS, "ja"),
    ]
    files = [("mimetype", "application/epub+zip", stored), ("META-INF/container.xml", CONTAINER, deflated)]
    files.append(("OEBPS/content.opf", opf("Writing Modes", "writing-modes", "ja", vertical,
                                            ' page-progression-direction="rtl"'), deflated))
    files.append(("OEBPS/style.css", WRITING_MODES_CSS, deflated))
    files += [(f"OEBPS/ch{i}.xhtml", c, deflated) for i, c in enumerate(vertical, 1)]
    write("test_writing_modes.epub", files)

    rtl = [chapter("RTL markup", RTL_BODY, "he", html_attrs=' dir="rtl"')]
    files = [("mimetype", "application/epub+zip", stored), ("META-INF/container.xml", CONTAINER, deflated)]
    files.append(("OEBPS/content.opf", opf("RTL Markup", "rtl-markup", "he", rtl), deflated))
    files.append(("OEBPS/style.css", RTL_CSS, deflated))
    files += [(f"OEBPS/ch{i}.xhtml", c, deflated) for i, c in enumerate(rtl, 1)]
    write("test_rtl_markup.epub", files)


if __name__ == "__main__":
    main()
