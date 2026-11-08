#!/usr/bin/env python3
"""Builds the fonts of the shaped fixture books (see test/fixtures/fonts/README.md).

    python lib/EpdFont/scripts/make_shaped_book_fonts.py --source-dir <dir with the pinned Noto TTFs> \
        --books test/epubs --fonts-out test/fixtures/fonts

It writes five fonts, each a subset holding the code points of its books' XHTML plus U+0020, U+200C, U+200D and
U+25CC, subset as make_shaping_fixtures.py subsets its fonts (every layout feature, GDEF, GSUB and GPOS kept whole, no
hinting). EpubPipelineTest lays each book out with its font as the shaping face of every font and style, and
ShapingRulesTest checks the pages.
  - NotoSansDevanagari-Book-Regular.ttf, NotoSansBengali-Book-Regular.ttf, NotoSansTamil-Book-Regular.ttf and
    NotoSansMalayalam-Book-Regular.ttf: the unhinted Noto Sans Regular of each script, over its book
    (test_shaped_<script>.epub, written by test/epubs/make_test_shaped_indic.py). The Devanagari font also takes the
    Devanagari code points of test_shaped_bidi_indic.epub (written by test/epubs/make_test_shaped_arabic.py), whose
    Hindi words are laid out with it: the harness has one shaping font a book.
  - NotoNaskhArabic-Book-Regular.ttf: Noto Naskh Arabic Regular merged with Noto Serif's Latin by
    merge_noto_latin.py, as the Font Manager's NotoNaskhArabic family is, over test_shaped_arabic.epub,
    test_shaped_persian.epub and test_shaped_urdu.epub (make_test_shaped_arabic.py), plus U+0640 (tatweel).

The script does not download: --source-dir holds the TTFs, each checked against the sha256 pinned below. A font of
131,072 bytes or more fails it. The output is byte-identical on every run, and the script writes nothing else.
"""

import argparse
import hashlib
import io
import sys
import tempfile
import zipfile
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import merge_noto_latin  # noqa: E402

# notofonts/notofonts.github.io at this commit.
SOURCE_COMMIT = "5c82cef0a8f08806f932062e18c02be62be278f0"
URL = (
    "https://github.com/notofonts/notofonts.github.io/raw/" + SOURCE_COMMIT
    + "/fonts/{family}/unhinted/ttf/{family}-Regular.ttf"
)

# Each source's sha256.
SOURCES = {
    "NotoSansDevanagari": "9c7d935139ea6a1e6ad9dbac4f6d27ece1e04bca8123c8888d00a0f9df4724cd",
    "NotoSansBengali": "5dceda02816fece18ea6796f474f5d3170f1d862f21b1e809cdc94b1caa4b6ec",
    "NotoSansTamil": "6634d9cc97a726e670df41281dd32b167a6b9b71f2036e19671ff08fdde0c292",
    "NotoSansMalayalam": "cc39bb1f7d63b582a2aec1abf2469a43b805a3523a27e0a0eee8cd32702541d5",
    "NotoNaskhArabic": "c34fdbd98af4dbc45ca192a23d2eeb77032add83086f5fe32957d23b3f36b221",
    "NotoSerif": "a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1",
}

DEVANAGARI = [(0x0900, 0x097F), (0x1CD0, 0x1CFF), (0xA8E0, 0xA8FF)]

# Kept in every subset beside the books' code points: space, ZWNJ, ZWJ and the dotted circle.
EXTRA_CODEPOINTS = [0x0020, 0x200C, 0x200D, 0x25CC]

# The output, its source, the Latin family merged into it (None: none), its books, each with the code point ranges it
# gives (None: every code point of its XHTML), and the code points kept beside EXTRA_CODEPOINTS.
FONTS = [
    ("NotoSansDevanagari-Book-Regular.ttf", "NotoSansDevanagari", None,
     [("test_shaped_devanagari.epub", None), ("test_shaped_bidi_indic.epub", DEVANAGARI)], []),
    ("NotoSansBengali-Book-Regular.ttf", "NotoSansBengali", None, [("test_shaped_bengali.epub", None)], []),
    ("NotoSansTamil-Book-Regular.ttf", "NotoSansTamil", None, [("test_shaped_tamil.epub", None)], []),
    ("NotoSansMalayalam-Book-Regular.ttf", "NotoSansMalayalam", None, [("test_shaped_malayalam.epub", None)], []),
    ("NotoNaskhArabic-Book-Regular.ttf", "NotoNaskhArabic", "NotoSerif",
     [("test_shaped_arabic.epub", None), ("test_shaped_persian.epub", None), ("test_shaped_urdu.epub", None)],
     [0x0640]),
]

MAX_FONT_BYTES = 131072


def verify_source(source_dir: Path, family: str) -> Path:
    path = source_dir / f"{family}-Regular.ttf"
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: {URL.format(family=family)}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != SOURCES[family]:
        sys.exit(f"{path}: sha256 {digest}, expected {SOURCES[family]}")
    return path


def book_codepoints(book: Path, ranges) -> set:
    found = set()
    with zipfile.ZipFile(book) as z:
        names = sorted(n for n in z.namelist() if n.endswith(".xhtml"))
        if not names:
            sys.exit(f"{book}: no XHTML")
        for name in names:
            found.update(ord(c) for c in z.read(name).decode("utf-8"))
    if ranges is not None:
        found = {cp for cp in found if any(lo <= cp <= hi for lo, hi in ranges)}
    return found


def subset_font(name: str, source: bytes, unicodes: list) -> bytes:
    # make_shaping_fixtures.py's options.
    options = subset.Options()
    options.layout_features = ["*"]
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.recalc_timestamp = False
    font = subset.load_font(io.BytesIO(source), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    for table in ("GDEF", "GSUB", "GPOS"):
        if table not in font:
            sys.exit(f"{name}: the subset lost {table}")
    out = io.BytesIO()
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", required=True, type=Path, help="holds the pinned Noto TTFs")
    parser.add_argument("--books", required=True, type=Path, help="the directory of the books")
    parser.add_argument("--fonts-out", required=True, type=Path, help="output directory for the fonts")
    args = parser.parse_args()

    fonts = []
    with tempfile.TemporaryDirectory() as tmp:
        for file, family, latin, books, extra in FONTS:
            path = verify_source(args.source_dir, family)
            if latin is not None:
                merged = Path(tmp) / f"{family}-Merged.ttf"
                merge_noto_latin.merge(path, verify_source(args.source_dir, latin), merged)
                path = merged
            # Read whole: a font fontTools reads from its path stays open, and the merge's directory goes.
            source = path.read_bytes()
            unicodes = set(EXTRA_CODEPOINTS) | set(extra)
            for book, ranges in books:
                unicodes |= book_codepoints(args.books / book, ranges)
            data = subset_font(file, source, sorted(unicodes))
            if len(data) >= MAX_FONT_BYTES:
                sys.exit(f"{file} is {len(data)} bytes, not under {MAX_FONT_BYTES}")
            fonts.append((file, data))

    args.fonts_out.mkdir(parents=True, exist_ok=True)
    for file, data in fonts:
        (args.fonts_out / file).write_bytes(data)
        copyright_notice = TTFont(io.BytesIO(data))["name"].getDebugName(0)
        print(f"{file}: {len(data)} bytes; copyright (name ID 0): {copyright_notice}")


if __name__ == "__main__":
    main()
