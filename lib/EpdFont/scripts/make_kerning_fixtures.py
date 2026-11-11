#!/usr/bin/env python3
"""Builds the GDEF kerning fixture, NotoSansGrantha-Kern-Regular.ttf (see test/fixtures/fonts/README.md).

    python -I lib/EpdFont/scripts/make_kerning_fixtures.py --source-dir <dir with NotoSansGrantha-Regular.ttf> \
        --out test/fixtures/fonts

Noto Sans Grantha 2.001 (notofonts/noto-fonts, unhinted) kerns sha (U+11336) before the anusvara (U+11302) by -40
units in a pair adjustment lookup of its 'kern' feature whose LookupFlag (0x0508) ignores marks and the anusvara is a
mark: a reader that honours the flag, as HarfBuzz does, kerns the pair 0. The fixture is a subset holding those two
code points and U+0020, every layout feature kept (GDEF, GSUB and GPOS cut to its glyphs), so the TTF engine's tests
can check a real font whose kerning changed with GDEF-aware lookup flags.

The script does not download: --source-dir holds NotoSansGrantha-Regular.ttf, checked against the sha256 pinned
below. The output is byte-identical on every run, and the script writes nothing else.
"""

import argparse
import hashlib
import io
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

# notofonts/noto-fonts at this commit (the repository's last, archived).
SOURCE_COMMIT = "ffebf8c1ee449e544955a7e813c54f9b73848eac"
SOURCE_NAME = "NotoSansGrantha-Regular.ttf"
SOURCE_SHA256 = "724d0b592571a0564c007ad5b47dcf62f5c7599a5efcdebcb839fe43ab6c6178"
URL = "https://github.com/notofonts/noto-fonts/raw/" + SOURCE_COMMIT + "/unhinted/ttf/NotoSansGrantha/" + SOURCE_NAME

OUT_NAME = "NotoSansGrantha-Kern-Regular.ttf"
CODEPOINTS = [0x0020, 0x11302, 0x11336]  # space, the anusvara, sha
MAX_FONT_BYTES = 65536


def verify_source(source_dir: Path) -> Path:
    path = source_dir / SOURCE_NAME
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: {URL}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != SOURCE_SHA256:
        sys.exit(f"{path}: sha256 {digest}, expected {SOURCE_SHA256}")
    return path


def subset_font(path: Path) -> bytes:
    options = subset.Options()
    options.layout_features = ["*"]
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.recalc_timestamp = False
    font = subset.load_font(str(path), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=CODEPOINTS)
    subsetter.subset(font)
    for table in ("GDEF", "GSUB", "GPOS"):
        if table not in font:
            sys.exit(f"{path}: the subset lost {table}")
    out = io.BytesIO()
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def check_flagged_pair(data: bytes):
    """The subset must keep what it is for: the flagged pair adjustment lookup and the anusvara as a mark."""
    font = TTFont(io.BytesIO(data))
    cmap = font.getBestCmap()
    sha, anusvara = cmap.get(0x11336), cmap.get(0x11302)
    if font["GDEF"].table.GlyphClassDef.classDefs.get(anusvara) != 3:
        sys.exit("the anusvara is no longer a mark in the subset's GDEF")
    gpos = font["GPOS"].table
    kern = {i for record in gpos.FeatureList.FeatureRecord if record.FeatureTag == "kern"
            for i in record.Feature.LookupListIndex}
    for index in kern:
        lookup = gpos.LookupList.Lookup[index]
        if lookup.LookupType != 2 or not lookup.LookupFlag & 0x0008:
            continue
        for st in lookup.SubTable:
            if st.Format != 2 or sha not in st.Coverage.glyphs:
                continue
            first = st.ClassDef1.classDefs.get(sha, 0)
            second = st.ClassDef2.classDefs.get(anusvara, 0)
            value = st.Class1Record[first].Class2Record[second].Value1
            if value is not None and getattr(value, "XAdvance", 0) == -40:
                return
    sys.exit("the subset lost the sha/anusvara pair of the lookup that ignores marks")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", required=True, type=Path, help="holds the pinned NotoSansGrantha-Regular.ttf")
    parser.add_argument("--out", required=True, type=Path, help="output directory for the font")
    args = parser.parse_args()

    data = subset_font(verify_source(args.source_dir))
    if len(data) >= MAX_FONT_BYTES:
        sys.exit(f"the subset is {len(data)} bytes, not under {MAX_FONT_BYTES}")
    check_flagged_pair(data)
    (args.out / OUT_NAME).write_bytes(data)
    print(f"{OUT_NAME}: {len(data)} bytes")


if __name__ == "__main__":
    main()
