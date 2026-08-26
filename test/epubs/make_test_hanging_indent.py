#!/usr/bin/env python3
"""Build test_hanging_indent.epub — the corpus book that pins where a line starts
horizontally when CSS states an inset and a negative text-indent.

Verse is the shape that broke (issue #198): the positive inset and the negative
indent that hangs out of it are stated on DIFFERENT elements —

    div.stanza { margin-left: 2em }      p.line { text-indent: -1em }

so a block that only carries its own CSS keeps the -1em and loses the 2em, and
every line but the first of each stanza started left of the panel and lost its
first glyph. The cases below cover each way the two halves can be split:

  1. wrapper + hanging children   the reported bug: the wrapper's inset must
                                  reach EVERY child, not just the first
  2. both on the paragraph        the control shape, which always worked
  3. inset past the 4em cap       the cap clamps the inset, so the indent has
                                  to be clamped with it or the difference lands
                                  off-panel
  4. negative margin-left         no parent box to pull back into, so it must
                                  not move text off the panel either
  5. one <p>, <br>-separated      every line keeps the paragraph's inset; only
                                  the first line is indented (they are lines of
                                  ONE paragraph, so the rest get no indent)
  6. nested wrappers              insets accumulate down the tree
  7. sibling after a wrapper      the wrapper's inset must NOT leak past </div>

At the corpus font 1em = 18px, so the goldens read: case 1 and 2 lines all sit
at LINE x=36 with W x=-18 (net 18); case 3 at LINE x=72 with W x=-72 (net 0, the
cap and the clamp cancelling exactly as the CSS does); case 4 at LINE x=0; case
5 opens at 36/-18 and continues at 36/0; case 6 at LINE x=63; case 7 back at
LINE x=0. No word may ever land at a negative x once the line offset is added.

Regenerate with:  python test/epubs/make_test_hanging_indent.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_hanging_indent.epub")

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0"
         xmlns="http://www.idpf.org/2007/opf"
         unique-identifier="BookId">

  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"
            xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Hanging Indent Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-hanging-indent-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>

  <manifest>
    <item id="ncx"      href="toc.ncx"        media-type="application/x-dtbncx+xml"/>
    <item id="css"      href="style.css"      media-type="text/css"/>
    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>

  <spine toc="ncx">
    <itemref idref="chapter1"/>
  </spine>

</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN"
  "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-hanging-indent-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Hanging Indent Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>Hanging Indent Test</text></navLabel>
      <content src="chapter1.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

CSS = """/* No book-level insets: every x below comes from the case's own CSS. */
body { margin: 0; padding: 0 }
p { margin: 0 }

/* 1. The reported shape: the inset is on the wrapper, the hang on each line. */
div.stanza { margin-left: 2em }
p.line { text-indent: -1em }

/* 2. The same two halves stated together on the paragraph itself. */
p.hang { margin-left: 2em; text-indent: -1em }

/* 3. Inset past the 4em cap, with an indent that matches the UNCAPPED inset. */
p.overcap { margin-left: 6em; text-indent: -6em }

/* 4. A negative margin, which has no parent box here to pull back into. */
p.pullleft { margin-left: -2em }

/* 5. One paragraph whose lines are split by <br>. */
p.brverse { margin-left: 2em; text-indent: -1em }

/* 6. A second wrapper inside the first: the insets add up. */
blockquote.quote { margin-left: 1.5em }
"""

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN"
  "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head>
  <title>Hanging Indent Test</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>

  <h1>Hanging Indent Test</h1>

  <!-- 1. Wrapper holds the inset, each line hangs out of it (issue #198) ─── -->
  <p>1. WRAPPER: every line of both stanzas starts at the same x. Before the
  fix only the first line of each stanza did.</p>

  <div class="stanza">
    <p class="line">Listen, listen</p>
    <p class="line">Remember the wane</p>
    <p class="line">Of suns fury and waving grain</p>
    <p class="line">We fell and fell</p>
  </div>

  <div class="stanza">
    <p class="line">My son, my son</p>
    <p class="line">Remember the burn</p>
  </div>

  <!-- 2. Control: both halves on the paragraph ────────────────────────────── -->
  <p>2. CONTROL: the same indent stated on the paragraph itself, which never
  lost its inset. It must still land where case 1 does.</p>

  <p class="hang">Listen, listen</p>
  <p class="hang">Remember the wane</p>

  <!-- 3. Inset past the cap ───────────────────────────────────────────────── -->
  <p>3. OVER CAP: a six em inset is capped at four, so a six em hang would put
  two em of every line off the panel.</p>

  <p class="overcap">Listen, listen</p>
  <p class="overcap">Remember the wane</p>

  <!-- 4. Negative margin ──────────────────────────────────────────────────── -->
  <p>4. NEGATIVE MARGIN: nothing to pull back into, so this line starts at the
  text edge like any other.</p>

  <p class="pullleft">Listen, listen</p>

  <!-- 5. One paragraph, lines split by <br> ───────────────────────────────── -->
  <p>5. BR VERSE: one paragraph, so only its first line is indented and the
  rest keep the paragraph inset.</p>

  <p class="brverse">Listen, listen<br/>Remember the wane<br/>We fell and fell</p>

  <!-- 6. Nested wrappers ──────────────────────────────────────────────────── -->
  <p>6. NESTED: the blockquote inset adds to the stanza inset.</p>

  <div class="stanza">
    <blockquote class="quote">
      <p class="line">Listen, listen</p>
      <p class="line">Remember the wane</p>
    </blockquote>
  </div>

  <!-- 7. The wrapper must not leak past its own end tag ────────────────────── -->
  <p>7. SIBLING: this paragraph follows the wrappers above and must be back at
  the text edge, indented only by the ordinary first-line indent.</p>

</body>
</html>
"""


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    with zipfile.ZipFile(OUT, "w") as z:
        # mimetype must be first and stored uncompressed
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/style.css", CSS, zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER, zipfile.ZIP_DEFLATED)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
