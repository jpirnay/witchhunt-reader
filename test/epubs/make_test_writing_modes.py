#!/usr/bin/env python3
"""Build test_writing_modes.epub, test_rtl_markup.epub and test_bidi_text.epub: the markup that says how text runs, as
real books write it, and right-to-left text as a Hebrew book holds it.

The first two stand in for real books whose markup cannot go in the repository. They are still set horizontally;
what they pin is what the parser reads from them (ChapterHtmlSlimParser, CssStyle::resolveLogical).

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
div; the dir="auto" paragraph decides from its first letter, Hebrew, so its start is right too.

test_bidi_text.epub, language he, no page progression: the book BidiRulesTest checks its rules on, with a default and a
_bookalign golden. The rules run at 460 px and at 1,600 px, where the 111-token paragraph's lines pass 128 slots (code
points other than marks) and take upstream's fallback, while the pointed Genesis lines, longer in code points, stay
on the line pass.
  - Spine 1, <html lang="he"> with no dir, so each paragraph's direction comes from its letters and the book's
    language (decision 6). In order:
    - a heading;
    - a Latin phrase in parentheses and a year inside Hebrew (mirrored brackets, an LTR run in an RTL line);
    - a Latin title of four words inside Hebrew;
    - numbers after a prefix and a maqaf (מ־25), a decimal and a percent;
    - Genesis 1:1-3 with its points (every mark stays with its letter; the line pass counts no mark as a slot);
    - the divine name, pointed and after the prefix ל (drawn ה׳, TextShaping's Hebrew module);
    - literal LRMs after C++ and C# and an RLM before 100 (kept for the pass, counting in no offset);
    - <bdi> around a Latin name and around a Hebrew one (isolates);
    - <bdo dir="rtl"> (an override reverses Latin) and a <span dir="ltr">;
    - two note markers on one line, read in order right to left (TwoMarkersOnALineAreReadInOrder);
    - an English paragraph in the Hebrew book: left to right, from the left edge;
    - "* * *", which has no letter and takes the book's direction;
    - an ordered and a bulleted list (markers on the right) and a 2 x 3 table (columns right to left);
    - text-align left and start (the _bookalign golden: start is right);
    - a hanging indent (.hang), the overhang on the right;
    - a 111-token paragraph that starts with a Latin word: past the 96-token flush, right to left by its Hebrew.
  - Spine 2, <html lang="en" dir="ltr">, English quoting Hebrew: the Hebrew pieces reverse inside left-to-right
    lines, a dir="rtl" paragraph runs right to left, and a <bdi> opens a paragraph.
  - Spine 3, the two notes the markers point at.

Regenerate with:  python -I test/epubs/make_test_writing_modes.py  (CPython 3.12; see write())
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

BIDI_CSS = """body { margin: 0 }
p { margin: 0; text-indent: 1em }
.left { text-align: left }
.start { text-align: start }
.hang { margin-right: 2em; text-indent: -1em }
"""

# The epub:type attributes need their namespace.
EPUB_NS = ' xmlns:epub="http://www.idpf.org/2007/ops"'

WIKIPEDIA_SENTENCE = "היא אנציקלופדיה חופשית שכל אחד יכול לערוך, והיא נכתבת בשיתוף פעולה."

BIDI_SPINE1 = (
    "<h1>פרק ראשון: בדיקה</h1>\n"
    "<p>הוא אמר (hello world) בשנת 1998, ואז הלך הביתה.</p>\n"
    "<p>בספר The Book of Ruth יש ארבעה פרקים בלבד.</p>\n"
    "<p>המחיר עלה מ־25 ל־3.50 שקלים, כלומר ב־14%.</p>\n"
    "<p>בְּרֵאשִׁית בָּרָא אֱלֹהִים אֵת הַשָּׁמַיִם וְאֵת הָאָרֶץ׃ וְהָאָרֶץ הָיְתָה תֹהוּ וָבֹהוּ וְחֹשֶׁךְ עַל־פְּנֵי תְהוֹם"
    " וְרוּחַ אֱלֹהִים מְרַחֶפֶת עַל־פְּנֵי הַמָּיִם׃ וַיֹּאמֶר אֱלֹהִים יְהִי אוֹר וַיְהִי־אוֹר׃</p>\n"
    "<p>וַיְדַבֵּר יְהוָה אֶל־מֹשֶׁה לֵּאמֹר׃ שִׁירוּ לַיהוָה שִׁיר חָדָשׁ׃</p>\n"
    "<p>השפה C++&#x200E; פופולרית, וגם C#&#x200E; היא שפה. &#x200F;100 אחוז.</p>\n"
    "<p>המשתמש <bdi>User_123</bdi> כתב 3 הודעות, והמשתמש <bdi>מנהל</bdi> כתב 5.</p>\n"
    '<p><bdo dir="rtl">reversed</bdo> ו<span dir="ltr">Latin 1-2-3</span>.</p>\n'
    '<p>הערה ראשונה<a epub:type="noteref" href="ch3.xhtml#n1">1</a> והערה שנייה'
    '<a epub:type="noteref" href="ch3.xhtml#n2">2</a> באותה שורה.</p>\n'
    "<p>This paragraph is English only, in a Hebrew book, 2024.</p>\n"
    "<p>* * *</p>\n"
    "<ol>\n<li>פריט ראשון</li>\n<li>פריט שני עם English</li>\n</ol>\n"
    "<ul>\n<li>נקודה</li>\n</ul>\n"
    "<table>\n<tr><td>א1</td><td>ב1</td><td>ג1</td></tr>\n<tr><td>א2</td><td>ב2</td><td>ג2</td></tr>\n</table>\n"
    '<p class="left">יישור לשמאל</p>\n'
    '<p class="start">יישור להתחלה</p>\n'
    '<p class="hang">פסקה עם הזחה תלויה שנמשכת יותר משורה אחת כדי לראות את השורה השנייה.</p>\n'
    "<p>Wikipedia " + " ".join([WIKIPEDIA_SENTENCE] * 10) + "</p>\n"
)

BIDI_SPINE2 = (
    "<p>He said שלום. and left.</p>\n"
    "<p>The word (שלום) means peace, she said in 1998.</p>\n"
    "<p>Hebrew: שלום עולם, and more.</p>\n"
    '<p dir="rtl">שלום עולם בתוך פסקה באנגלית.</p>\n'
    "<p><bdi>שלום</bdi>: 3 posts</p>\n"
)

BIDI_SPINE3 = (
    '<aside epub:type="footnote" id="n1"><p>ההערה הראשונה.</p></aside>\n'
    '<aside epub:type="footnote" id="n2"><p>ההערה השנייה.</p></aside>\n'
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
            # A fixed date keeps a regenerated book byte-identical, and a fixed system (Unix, as the first books were
            # written on) keeps it so on Windows. The deflated bytes are zlib's: zlib-ng (Python 3.14's) writes
            # others, so regenerate with CPython 3.12: with 3.14 the old books' bytes change too, not only the new
            # one's.
            info = zipfile.ZipInfo(path, date_time=(2026, 10, 26, 0, 0, 0))
            info.create_system = 3
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

    bidi = [
        chapter("Hebrew", BIDI_SPINE1, "he", html_attrs=EPUB_NS),
        chapter("English", BIDI_SPINE2, "en", html_attrs=' dir="ltr"'),
        chapter("Notes", BIDI_SPINE3, "he", html_attrs=EPUB_NS),
    ]
    files = [("mimetype", "application/epub+zip", stored), ("META-INF/container.xml", CONTAINER, deflated)]
    files.append(("OEBPS/content.opf", opf("Bidi Text", "bidi-text", "he", bidi), deflated))
    files.append(("OEBPS/style.css", BIDI_CSS, deflated))
    files += [(f"OEBPS/ch{i}.xhtml", c, deflated) for i, c in enumerate(bidi, 1)]
    write("test_bidi_text.epub", files)


if __name__ == "__main__":
    main()
