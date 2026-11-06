#!/usr/bin/env python3
"""Builds the fonts of the four shaped-Indic fixture books (see test/fixtures/fonts/README.md).

    python lib/EpdFont/scripts/make_shaped_book_fonts.py --source-dir <dir with the four Noto Sans TTFs> \
        --books test/epubs --fonts-out test/fixtures/fonts

It writes NotoSansDevanagari-Book-Regular.ttf, NotoSansBengali-Book-Regular.ttf, NotoSansTamil-Book-Regular.ttf and
NotoSansMalayalam-Book-Regular.ttf: subsets of the unhinted Noto Sans Regular of each script holding the code points of
its book's XHTML (test_shaped_<script>.epub, written by test/epubs/make_test_shaped_indic.py) plus U+0020, U+200C,
U+200D and U+25CC, subset as make_shaping_fixtures.py subsets its fonts (every layout feature, GDEF, GSUB and GPOS
kept whole, no hinting). EpubPipelineTest lays each book out with its font as the shaping face of every font and
style, and ShapingRulesTest checks the pages.

The script does not download: --source-dir holds the four TTFs, each checked against the sha256 pinned below. A font of
131,072 bytes or more fails it. The output is byte-identical on every run, and the script writes nothing else.
"""

import argparse
import hashlib
import io
import sys
import zipfile
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

# notofonts/notofonts.github.io at this commit.
SOURCE_COMMIT = "5c82cef0a8f08806f932062e18c02be62be278f0"
URL = (
    "https://github.com/notofonts/notofonts.github.io/raw/" + SOURCE_COMMIT
    + "/fonts/NotoSans{script}/unhinted/ttf/NotoSans{script}-Regular.ttf"
)

# The script, its source's sha256, and its book.
SCRIPTS = [
    ("Devanagari", "9c7d935139ea6a1e6ad9dbac4f6d27ece1e04bca8123c8888d00a0f9df4724cd", "test_shaped_devanagari.epub"),
    ("Bengali", "5dceda02816fece18ea6796f474f5d3170f1d862f21b1e809cdc94b1caa4b6ec", "test_shaped_bengali.epub"),
    ("Tamil", "6634d9cc97a726e670df41281dd32b167a6b9b71f2036e19671ff08fdde0c292", "test_shaped_tamil.epub"),
    ("Malayalam", "cc39bb1f7d63b582a2aec1abf2469a43b805a3523a27e0a0eee8cd32702541d5", "test_shaped_malayalam.epub"),
]

MAX_FONT_BYTES = 131072

# Kept in the subset beside the book's code points: space, ZWNJ, ZWJ and the dotted circle.
EXTRA_CODEPOINTS = [0x0020, 0x200C, 0x200D, 0x25CC]


def verify_source(source_dir: Path, script: str, expected: str) -> Path:
    path = source_dir / f"NotoSans{script}-Regular.ttf"
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: {URL.format(script=script)}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected:
        sys.exit(f"{path}: sha256 {digest}, expected {expected}")
    return path


def book_codepoints(book: Path) -> list:
    found = set(EXTRA_CODEPOINTS)
    with zipfile.ZipFile(book) as z:
        names = sorted(n for n in z.namelist() if n.endswith(".xhtml"))
        if not names:
            sys.exit(f"{book}: no XHTML")
        for name in names:
            found.update(ord(c) for c in z.read(name).decode("utf-8"))
    return sorted(found)


def subset_font(path: Path, unicodes: list) -> bytes:
    # make_shaping_fixtures.py's options.
    options = subset.Options()
    options.layout_features = ["*"]
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.recalc_timestamp = False
    font = subset.load_font(str(path), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    for table in ("GDEF", "GSUB", "GPOS"):
        if table not in font:
            sys.exit(f"{path}: the subset lost {table}")
    out = io.BytesIO()
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", required=True, type=Path, help="holds the four pinned Noto Sans TTFs")
    parser.add_argument("--books", required=True, type=Path, help="the directory of the four books")
    parser.add_argument("--fonts-out", required=True, type=Path, help="output directory for the fonts")
    args = parser.parse_args()

    fonts = []
    for script, sha256, book in SCRIPTS:
        data = subset_font(verify_source(args.source_dir, script, sha256), book_codepoints(args.books / book))
        if len(data) >= MAX_FONT_BYTES:
            sys.exit(f"the {script} subset is {len(data)} bytes, not under {MAX_FONT_BYTES}")
        fonts.append((script, f"NotoSans{script}-Book-Regular.ttf", data))

    args.fonts_out.mkdir(parents=True, exist_ok=True)
    for script, file, data in fonts:
        (args.fonts_out / file).write_bytes(data)
        copyright_notice = TTFont(io.BytesIO(data))["name"].getDebugName(0)
        print(f"{file}: {len(data)} bytes; copyright (name ID 0): {copyright_notice}")


if __name__ == "__main__":
    main()
