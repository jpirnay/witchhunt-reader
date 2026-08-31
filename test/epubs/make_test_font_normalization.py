#!/usr/bin/env python3
"""Build test_font_normalization.epub — the corpus book that pins the font-size
normalization dead zone.

Normalization widens the per-word dead zone from +/-3% (OFF) to +/-10% (ON), so
sizes are laid out here to straddle BOTH edges:

  * 98%, 102%      inside both bands  -> snap to 100 under OFF and ON
  * 92%, 95%, 108% inside ON only     -> the sizes that must differ OFF vs ON
  * 89%, 111%      outside both bands -> survive under OFF and ON
  * 80%, 70%       clearly distinct   -> footnote/caption sizes, never snapped

The whole-paragraph wrapper cases mirror the real publisher markup the feature
targets (<span style="font-size:0.92em"> around a full paragraph); the mid-line
cases confirm snapping happens per word without disturbing its neighbours.

The band applies to a size stated on the block itself as well (`p.body {
font-size: 1.1em }`, how a publisher states the same thing without a wrapper),
so cases 8 and 9 travel the block-style path instead of the per-word channel.
Headings are the one exemption (case 10): they are meant to stand apart, so a
heading's size survives the band under either setting.

Regenerate with:  python test/epubs/make_test_font_normalization.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_font_normalization.epub")

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
    <dc:title>Font Size Normalization Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-font-normalization-001</dc:identifier>
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
    <meta name="dtb:uid" content="urn:uuid:test-font-normalization-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Font Size Normalization Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>Normalization Test</text></navLabel>
      <content src="chapter1.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

CSS = """/* Dead-zone boundaries. OFF snaps +/-3%, ON snaps +/-10%. */

/* Inside BOTH bands - snap to 100 regardless of the setting. */
.p098 { font-size: 98% }
.p102 { font-size: 102% }

/* Inside the ON band only - these are what OFF and ON disagree about. */
.p092 { font-size: 92% }
.p095 { font-size: 95% }
.p108 { font-size: 108% }

/* Just outside BOTH bands - must survive under either setting. */
.p089 { font-size: 89% }
.p111 { font-size: 111% }

/* Clearly distinct sizes - footnote / caption territory, never snapped. */
.p080 { font-size: 80% }
.p070 { font-size: 70% }

/* Nested composition: 1.05 x 0.9 = 94.5% -> inside ON band only. */
.outer105 { font-size: 1.05em }
.inner090 { font-size: 0.9em }

/* Upper band EDGE, stated on the paragraph class - the shape that made a whole
   book's prose render as resampled glyphs. Bare `p` carries no size, so the
   main-text baseline never sees it. */
.p110 { font-size: 1.1em }

/* Inside the ON band but on a HEADING, which is exempt: a heading is meant to
   stand apart, so its author's size survives both settings. */
.h106 { font-size: 1.06em }
"""

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN"
  "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head>
  <title>Font Size Normalization Test</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>

  <h1>Font Size Normalization Test</h1>

  <!-- Control ───────────────────────────────────────────────────── -->
  <p>CONTROL: This whole paragraph is plain body text at one hundred percent.
  Every other section on this page is judged against this line.</p>

  <!-- 1. Whole-paragraph wrappers, ON band only ─────────────────── -->
  <p>1. PUBLISHER WRAPPERS (the real-world case: a whole paragraph wrapped in a
  near-body span). Under OFF each keeps its own size; under ON all three snap to
  body.</p>

  <p><span class="p092">This entire paragraph sits at ninety-two percent, the
  classic publisher body wrapper. It is below the just-noticeable size
  threshold yet forces per-glyph resampling across every page.</span></p>

  <p><span class="p095">This entire paragraph sits at ninety-five percent and is
  the same story one step closer to body size.</span></p>

  <p><span class="p108">This entire paragraph sits at one hundred eight percent,
  the same case on the upper side of body size.</span></p>

  <!-- 2. Inside both bands ──────────────────────────────────────── -->
  <p>2. BOTH BANDS: normal, <span class="p098">ninety-eight percent</span>,
  normal, <span class="p102">one hundred two percent</span>, normal. These snap
  to body under OFF and ON alike.</p>

  <!-- 3. Outside both bands ─────────────────────────────────────── -->
  <p>3. NEITHER BAND: normal, <span class="p089">eighty-nine percent</span>,
  normal, <span class="p111">one hundred eleven percent</span>, normal. These
  survive under OFF and ON alike.</p>

  <!-- 4. Distinct sizes must never be flattened ─────────────────── -->
  <p>4. DISTINCT: normal, <span class="p080">eighty percent footnote
  size</span>, normal, <span class="p070">seventy percent caption size</span>,
  normal. Neither setting may flatten these.</p>

  <!-- 5. Mid-line snapping must not disturb neighbours ──────────── -->
  <p>5. MID-LINE: plain words, <span class="p095">then a ninety-five percent run
  in the middle of the line</span>, then plain words again — under ON the middle
  run joins the body baseline without shifting the runs around it.</p>

  <!-- 6. Nested composition lands inside the ON band ────────────── -->
  <p>6. NESTING: <span class="outer105">outer at one hundred five percent
  <span class="inner090">inner composes to ninety-four and a half percent</span>
  back to outer</span> and normal.</p>

  <!-- 7. A deliberate fine gradient ─────────────────────────────── -->
  <p>7. GRADIENT (steps inside the ON band collapse, the wide ones remain):</p>
  <div>
    <span class="p098">ninety-eight</span><br/>
    <span class="p095">ninety-five</span><br/>
    <span class="p092">ninety-two</span><br/>
    <span class="p089">eighty-nine</span><br/>
    <span class="p080">eighty</span><br/>
    <span class="p070">seventy</span>
  </div>

  <!-- 8. Block-level font-size (BlockStyle path, not per-word) ──── -->
  <p class="p092">8. BLOCK LEVEL: the font-size sits on the paragraph itself
  rather than on an inner span, so this travels the block-style path.</p>

  <p class="p080">8b. BLOCK LEVEL DISTINCT: an eighty percent block, outside
  both bands, which must keep its size either way.</p>

  <!-- 9. Block level at the upper band edge, with a distinct nested run ── -->
  <p class="p110">9. BLOCK EDGE: a whole book of prose stated at one hundred ten
  percent on the paragraph class, which is exactly the upper edge of the ON
  band. It must snap to body under ON, and <span class="p080">this nested
  eighty percent run</span> must then measure eighty percent of body — not of
  the hundred and ten.</p>

  <!-- 10. Headings are exempt from the band ─────────────────────── -->
  <h3 class="h106">10. HEADING EDGE: one hundred six percent</h3>
  <p>A heading sized inside the ON band keeps its size under both settings — the
  band spares body text a scale nobody asked for, it does not flatten the
  page's hierarchy.</p>

</body>
</html>
"""


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        # mimetype must be first and STORED per OCF.
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        z.writestr("OEBPS/content.opf", OPF)
        z.writestr("OEBPS/toc.ncx", NCX)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
