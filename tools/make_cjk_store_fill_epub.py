#!/usr/bin/env python3
"""Build cjk_store_fill.epub: a Traditional Chinese book whose chapters hold different characters, to fill the
TrueType glyph store on the device and watch it emptied when the next chapter opens.

Every Big5 ideograph appears once, in Big5 order, about 1,700 a chapter over eight chapters, in paragraphs of 50.
At 12 pt the store holds about 9,500 glyphs, so Noto Sans TC fills it in the sixth chapter; at 26 pt (about 2,000
glyphs) in the second. No prose, only the character set.

    python tools/make_cjk_store_fill_epub.py [out.epub]

Not a test fixture: every book in test/epubs becomes a golden case, and this one is for the device only.
"""

import sys
import zipfile

CHAPTERS = 8
PER_PARAGRAPH = 50


def big5_ideographs():
    out = []
    for lead in range(0xA4, 0xFA):
        for trail in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                ch = bytes([lead, trail]).decode("big5")
            except UnicodeDecodeError:
                continue
            if 0x4E00 <= ord(ch) <= 0x9FFF:
                out.append(ch)
    return out


CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="zh-Hant" lang="zh-Hant">
<head>
  <title>{title}</title>
</head>
<body>
  <h1>{title}</h1>
{body}
</body>
</html>
"""


def main():
    chars = big5_ideographs()
    per_chapter = (len(chars) + CHAPTERS - 1) // CHAPTERS
    chapters = []
    for c in range(CHAPTERS):
        part = chars[c * per_chapter:(c + 1) * per_chapter]
        paragraphs = ["".join(part[i:i + PER_PARAGRAPH]) for i in range(0, len(part), PER_PARAGRAPH)]
        body = "\n".join(f"  <p>{p}</p>" for p in paragraphs)
        chapters.append(CHAPTER.format(title=f"第{c + 1}章", body=body))

    manifest = "\n".join(f'    <item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>'
                         for i in range(CHAPTERS))
    spine = "\n".join(f'    <itemref idref="c{i}"/>' for i in range(CHAPTERS))
    opf = f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">test-cjk-store-fill</dc:identifier>
    <dc:title>CJK glyph store fill</dc:title>
    <dc:language>zh-Hant</dc:language>
    <meta property="dcterms:modified">2026-10-20T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
{manifest}
  </manifest>
  <spine>
{spine}
  </spine>
</package>
"""
    nav_items = "\n".join(f'      <li><a href="c{i}.xhtml">第{i + 1}章</a></li>' for i in range(CHAPTERS))
    nav = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="zh-Hant">
<head><title>目錄</title></head>
<body>
  <nav epub:type="toc"><ol>
{nav_items}
  </ol></nav>
</body>
</html>
"""
    container = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
"""
    path = sys.argv[1] if len(sys.argv) > 1 else "cjk_store_fill.epub"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype", (2026, 10, 20, 0, 0, 0)), "application/epub+zip")
        files = [("META-INF/container.xml", container), ("OEBPS/content.opf", opf), ("OEBPS/nav.xhtml", nav)]
        files += [(f"OEBPS/c{i}.xhtml", chapters[i]) for i in range(CHAPTERS)]
        for name, text in files:
            info = zipfile.ZipInfo(name, (2026, 10, 20, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode("utf-8"))
    print(f"Wrote {path}: {len(chars)} characters, {per_chapter} a chapter")


if __name__ == "__main__":
    main()
