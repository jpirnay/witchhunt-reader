#!/usr/bin/env python3
"""Generate the CJK UI packs: what the built-in UI faces lack of a script, as .cpfont files the Font Manager offers.

A pack is a 1-bit .cpfont family on the card that the UI faces draw from and the reader never does (the registry leaves
it out). The glyphs are rendered exactly as the UI faces' own are, by fontconvert.py, 1-bit, autohinted, so they stand
beside the faces' Latin at matching metrics; fontconvert.py writes a C header, which this reads back and writes as a
1-bit v4 .cpfont (fontconvert_sdcard.write_cpfont). No kerning and no ligatures.

ChineseUI, ChineseTradUI, JapaneseUI, KoreanUI: the CJK characters a book title may hold, at the CJK UI faces' sizes
  (10 and 12 pt, regular: font-sizes.sh CJK_UI_FONT_SIZES), from the Noto Sans CJK region the forms belong to, the
  sources sd-fonts.yaml pins for NotoSansSC/TC/JP/KR. The Han packs hold the whole CJK Unified Ideographs block as
  Noto draws it, Japanese the kana and half-width forms too, Korean every Hangul syllable and the compatibility jamo;
  each the CJK punctuation and the full-width forms. Whole blocks rather than a frequency list: SdCardFont keeps a
  font's interval table in RAM, and a frequency list of 3,500 characters is 2,500 intervals, 30 KB; whole blocks are a
  handful. Each is one file, <name>_10.cpfont, whose styles are the sizes (style 0 10 pt, style 1 12 pt): one
  SdCardFont serves both, where a file per size cost the heap two (2.5 KB each, the X3 measured about 6 KB a pack).
  src/CjkUiPack.cpp opens them; the CJK fallback (FallbackFaceProvider) serves from them.

Usage:  python build_ui_script_pack.py [--pack NAME] [--output-dir DIR]
        --output-dir is the pack's own folder (default assets/sd-fonts/<NAME>). build-sd-fonts.py runs it for
        sd-fonts.yaml's `format: uipack` families.
Needs:  the fontconvert.py requirements (PyYAML for the CJK sources, which it downloads into
        downloaded_fonts/NotoSansCJK/ on first use, as build_cjk_ui_fonts.py does, checked against the pins).
"""

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
sys.path.insert(0, str(SCRIPT_DIR))
from fontconvert_sdcard import GlyphProps, StyleRasterData, write_cpfont  # noqa: E402

SD_FONTS_YAML = SCRIPT_DIR / "sd-fonts.yaml"
CJK_DOWNLOAD_DIR = SCRIPT_DIR / "downloaded_fonts" / "NotoSansCJK"

# The CJK UI faces' sizes (font-sizes.sh CJK_UI_FONT_SIZES): every UI face draws CJK at one of them, regular.
CJK_SIZES = {10: ("Regular",), 12: ("Regular",)}

# fontconvert.py caps an uncompressed face at 64 KB of bitmap (uint16 offsets), which a CJK pack passes many times
# over: it renders a pack in runs of this many characters, each glyph exactly as it would alone, and the runs are
# joined. 600 characters stay under the cap at 12 pt (about 80 B a glyph).
CHUNK = 600

# The CJK packs' blocks. Noto Sans CJK draws all of them but U+9FF0-9FFF and U+3097-3098, which are left out.
URO = [(0x4E00, 0x9FEF)]                      # CJK Unified Ideographs
CJK_PUNCTUATION = [(0x3000, 0x303F)]          # CJK symbols and punctuation, the ideographic space among them
FULL_WIDTH = [(0xFF01, 0xFF60)]               # full-width ASCII and brackets
KANA = [(0x3041, 0x3096), (0x3099, 0x30FF), (0x31F0, 0x31FF), (0xFF61, 0xFF9F)]  # and the half-width forms
HANGUL = [(0x3131, 0x318E), (0xAC00, 0xD7A3)]  # compatibility jamo, every syllable

# Each pack: its sizes and weights, its code points' ranges, and its source (the sd-fonts.yaml family whose pinned Noto
# Sans CJK region OTF it is cut from).
PACKS = {
    "ChineseUI": (CJK_SIZES, CJK_PUNCTUATION + URO + FULL_WIDTH, "NotoSansSC"),
    "ChineseTradUI": (CJK_SIZES, CJK_PUNCTUATION + URO + FULL_WIDTH, "NotoSansTC"),
    "JapaneseUI": (CJK_SIZES, CJK_PUNCTUATION + KANA + URO + FULL_WIDTH, "NotoSansJP"),
    "KoreanUI": (CJK_SIZES, CJK_PUNCTUATION + HANGUL + FULL_WIDTH, "NotoSansKR"),
}


def range_codepoints(ranges):
    cps = set()
    for first, last in ranges:
        cps |= set(range(first, last + 1))
    return sorted(cps)


def cjk_source(family):
    """The Noto Sans CJK region OTF sd-fonts.yaml pins for `family`, downloaded on first use and hash-checked."""
    import yaml  # the CJK packs only

    config = yaml.safe_load(SD_FONTS_YAML.read_text(encoding="utf-8"))
    spec = next(f for f in config["families"] if f["name"] == family)["source"]
    dest = CJK_DOWNLOAD_DIR / spec["url"].rsplit("/", 1)[1]
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"  Downloading {dest.name}...")
        urllib.request.urlretrieve(spec["url"], dest)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    if digest != spec["sha256"]:
        raise SystemExit(f"{dest} is not the source sd-fonts.yaml pins for {family} (sha256 {digest})")
    return dest


def array_body(header, name):
    match = re.search(re.escape(name) + r"\[\d*\] = \{(.*?)\};", header, re.S)
    if not match:
        raise SystemExit(f"fontconvert.py output has no {name}")
    return match.group(1)


def read_glyphs(header, name):
    """fontconvert.py's glyphs of face `name`, in code point order, each with its packed 1-bit bitmap, and the face's
    line metrics."""
    bitmap = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", array_body(header, f"{name}Bitmaps")))
    records = [tuple(int(v) for v in g) for g in
               re.findall(r"\{ *(-?\d+), *(-?\d+), *(-?\d+), *(-?\d+), *(-?\d+) *\}", array_body(header, f"{name}Glyphs"))]
    offsets = [int(v) for v in re.findall(r"^\s*(\d+),", array_body(header, f"{name}BitmapOffsets"), re.M)]
    intervals = [(int(a, 16), int(b, 16), int(o, 0)) for a, b, o in
                 re.findall(r"\{ *(0x[0-9A-Fa-f]+), *(0x[0-9A-Fa-f]+), *(0x[0-9A-Fa-f]+|\d+) *\}",
                            array_body(header, f"{name}Intervals"))]
    data = re.search(r"static const EpdFontData " + re.escape(name) + r" = \{(.*?)\};", header, re.S).group(1)
    fields = [line.split("//")[0].strip().rstrip(",") for line in data.split("\n")]
    fields = [f for f in fields if f]
    metrics = tuple(int(fields[i]) for i in (6, 7, 8))  # advanceY, ascender, descender
    if fields[9] != "false":
        raise SystemExit(f"{name}: fontconvert.py wrote a 2-bit face")
    if len(records) != len(offsets):
        raise SystemExit(f"{name}: {len(records)} glyphs but {len(offsets)} bitmap offsets")
    glyphs = []
    index = 0
    for first, last, offset in intervals:
        if offset != index:
            raise SystemExit(f"{name}: interval U+{first:04X} starts at glyph {offset}, expected {index}")
        for cp in range(first, last + 1):
            width, height, advance_x, left, top = records[index]
            length = (width * height + 7) // 8  # glyphDataBytes(), 1-bit
            glyphs.append((cp, (width, height, advance_x, left, top), bitmap[offsets[index]:offsets[index] + length]))
            index += 1
    return glyphs, metrics


def render(name, size, source, cps, tmp):
    """Face `name` of `cps` at `size` from `source`, in runs of CHUNK characters, rendered in parallel and joined, as
    the StyleRasterData write_cpfont() takes (style_id left 0)."""
    runs = [cps[i:i + CHUNK] for i in range(0, len(cps), CHUNK)]

    def run(index):
        chars = tmp / f"{name}_{index}.txt"
        chars.write_text("".join(chr(cp) for cp in runs[index]), encoding="utf-8")
        header = subprocess.run([sys.executable, str(SCRIPT_DIR / "fontconvert.py"), name, str(size), str(source),
                                 "--codepoints-file", str(chars)],
                                check=True, capture_output=True, text=True, cwd=SCRIPT_DIR).stdout
        return read_glyphs(header, name)

    with ThreadPoolExecutor() as pool:
        results = list(pool.map(run, range(len(runs))))
    metrics = {m for _, m in results}
    if len(metrics) != 1:
        raise SystemExit(f"{name}: runs disagree on the line metrics {sorted(metrics)}")
    advance_y, ascender, descender = metrics.pop()
    glyphs = []
    offset = 0
    for run_glyphs, _ in results:
        for cp, (width, height, advance_x, left, top), packed in run_glyphs:
            glyphs.append((GlyphProps(width, height, advance_x, left, top, len(packed), offset, cp), packed))
            offset += len(packed)
    glyphs.sort(key=lambda g: g[0].code_point)
    missing = sorted(set(cps) - {g.code_point for g, _ in glyphs})
    if missing:
        raise SystemExit(f"{name}: no glyph for " + " ".join(f"U+{cp:04X}" for cp in missing))
    # The bitmaps are written in code point order: the offsets follow it.
    offset = 0
    placed = []
    for props, packed in glyphs:
        placed.append((props._replace(data_offset=offset), packed))
        offset += len(packed)
    intervals = []
    for props, _ in placed:
        cp = props.code_point
        if intervals and intervals[-1][1] == cp - 1:
            intervals[-1] = (intervals[-1][0], cp)
        else:
            intervals.append((cp, cp))
    return StyleRasterData(style_id=0, intervals=intervals, all_glyphs=placed, total_bitmap_size=offset,
                           advanceY=advance_y, ascender=ascender, descender=descender, kern_left_classes=[],
                           kern_right_classes=[], kern_matrix=[], kern_left_class_count=0, kern_right_class_count=0,
                           ligature_pairs=[])


def build(name, out_dir):
    sizes, ranges, cjk_family = PACKS[name]
    cps = range_codepoints(ranges)
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        source = cjk_source(cjk_family)
        # One file for both sizes, each a style: style i is the i-th size of CJK_SIZES. The UI then holds one
        # SdCardFont per pack rather than one per size, which is most of what an open pack costs on the heap.
        raster = {}
        for index, size in enumerate(sizes):
            face = render(f"{name.lower()}_{size}_regular", size, source, cps, tmp)
            raster[index] = face._replace(style_id=index)
        path = out_dir / f"{name}_{min(sizes)}.cpfont"
        write_cpfont(raster, str(path), two_bit=False)
        print(f"Generated {path} ({len(cps)} characters at {sorted(sizes)} pt, {path.stat().st_size} B)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pack", choices=sorted(PACKS), action="append",
                        help="the pack to build (repeatable; default: every pack)")
    parser.add_argument("--output-dir", help="the pack's folder (default: assets/sd-fonts/<pack>); one pack only")
    args = parser.parse_args()
    packs = args.pack or sorted(PACKS)
    if args.output_dir and len(packs) != 1:
        parser.error("--output-dir takes one --pack")
    for name in packs:
        out_dir = Path(args.output_dir) if args.output_dir else REPO_ROOT / "assets" / "sd-fonts" / name
        build(name, out_dir)


if __name__ == "__main__":
    main()
