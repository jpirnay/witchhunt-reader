#!/usr/bin/env python3
"""Generate the Arabic UI pack: the Arabic presentation forms at the built-in UI faces' sizes, as .cpfont files.

The UI draws Arabic in its presentation forms (BidiUtils::arabicUiForms forms a string before it is measured or
drawn), and the built-in UI faces do not carry them: flash has no room for the ~60 KB they cost. They come from this
pack instead, a download in the Font Manager like the SD families. src/UiScriptPack.cpp opens it from the card and the
UI faces' providers (UiFaceProvider) serve a code point the face lacks from it, before the last book's font.

The glyphs are rendered exactly as the UI faces' own are: by fontconvert.py, 1-bit, at the faces' sizes, from Noto Sans
Arabic cut to its own blocks, so their metrics match the faces they stand beside. fontconvert.py writes a C header;
this reads its arrays back and writes them as a 1-bit v4 .cpfont (fontconvert_sdcard.write_cpfont), one file per size
holding the regular and the bold style. No kerning and no ligatures: the forms are the joined letters already.

The code points are every form the mapper can emit (shape_logical in lib/MiniBidi/minibidi.c: all of Presentation
Forms-B U+FE80-FEFC, and the Forms-A forms of its extended table), the harakat and superscript alef, which overlay
their letter, and the Arabic and Persian digits and punctuation, which have no forms.

Usage:  python build_ui_script_pack.py [--output-dir DIR] [--source-dir DIR]
        build-sd-fonts.py runs it for sd-fonts.yaml's `format: uipack` family, after the NotoSansArabic family.
Needs:  the fontconvert.py requirements (fontTools for the subset).
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
sys.path.insert(0, str(SCRIPT_DIR))
from fontconvert_sdcard import GlyphProps, StyleRasterData, write_cpfont  # noqa: E402

FAMILY = "ArabicUI"
SOURCE_DIR = REPO_ROOT / "assets" / "sd-fonts" / "NotoSansArabic"
MINIBIDI = REPO_ROOT / "lib" / "MiniBidi" / "minibidi.c"

# The UI faces' sizes (font-sizes.sh UI_FONT_SIZES, and Noto Sans 6 and 8) and the weights each is drawn in: 6 pt is
# Murphy's small face, regular only.
SIZES = {6: ("Regular",), 8: ("Regular", "Bold"), 10: ("Regular", "Bold"), 12: ("Regular", "Bold"),
         14: ("Regular", "Bold")}
STYLE_IDS = {"Regular": 0, "Bold": 1}

# What the pack holds besides the presentation forms: marks drawn over their letter, and characters with no forms.
UNFORMED = [
    (0x060C, 0x060C),  # Arabic comma
    (0x061B, 0x061B),  # Arabic semicolon
    (0x061F, 0x061F),  # Arabic question mark
    (0x0640, 0x0640),  # tatweel
    (0x064B, 0x0652),  # harakat
    (0x0660, 0x066C),  # Arabic-Indic digits, percent, decimal and thousands separators
    (0x0670, 0x0670),  # superscript alef
    (0x06D4, 0x06D4),  # Urdu full stop
    (0x06F0, 0x06F9),  # Persian and Urdu digits
]
FORMS_B = (0xFE80, 0xFEFC)  # every form of U+0621-064A, and the Lam-Alef ligatures


def forms_a():
    """The Forms-A code points shape_logical() can emit: each letter of minibidi.c's xshapeforms table, from its
    isolated form on, as many forms as the table gives it."""
    text = MINIBIDI.read_text(encoding="utf-8")
    table = text[text.index("xshapeforms[] = {"):]
    table = table[:table.index("};")]
    cps = []
    for _, isolated, forms in re.findall(r"\{0x([0-9A-F]+), 0x([0-9A-F]+), (\d)\}", table):
        cps += range(int(isolated, 16), int(isolated, 16) + int(forms))
    if not cps:
        raise SystemExit(f"no forms found in {MINIBIDI}")
    return cps


def pack_codepoints():
    cps = set(forms_a()) | set(range(FORMS_B[0], FORMS_B[1] + 1))
    for first, last in UNFORMED:
        cps |= set(range(first, last + 1))
    return sorted(cps)


def subset_source(source_dir, style, out_dir):
    """Noto Sans Arabic cut to its own blocks, and '|', which fontconvert.py measures the line metrics by."""
    out = out_dir / f"Arabic-{style}.ttf"
    subprocess.run([sys.executable, "-m", "fontTools.subset", str(source_dir / f"NotoSansArabic-{style}.ttf"),
                    "--unicodes=U+007C,U+0600-06FF,U+FB50-FDFF,U+FE70-FEFF", "--layout-features=*",
                    f"--output-file={out}"], check=True)
    return out


def array_body(header, name):
    match = re.search(re.escape(name) + r"\[\d*\] = \{(.*?)\};", header, re.S)
    if not match:
        raise SystemExit(f"fontconvert.py output has no {name}")
    return match.group(1)


def read_face(header, name, style_id):
    """fontconvert.py's arrays for face `name`, as the StyleRasterData write_cpfont() takes."""
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
    advance_y, ascender, descender = (int(fields[i]) for i in (6, 7, 8))
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
            start = offsets[index]
            glyphs.append((GlyphProps(width, height, advance_x, left, top, length, start, cp),
                           bitmap[start:start + length]))
            index += 1
    return StyleRasterData(style_id=style_id, intervals=[(a, b) for a, b, _ in intervals], all_glyphs=glyphs,
                           total_bitmap_size=sum(len(p) for _, p in glyphs), advanceY=advance_y, ascender=ascender,
                           descender=descender, kern_left_classes=[], kern_right_classes=[], kern_matrix=[],
                           kern_left_class_count=0, kern_right_class_count=0, ligature_pairs=[])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--output-dir", default=str(REPO_ROOT / "assets" / "sd-fonts" / FAMILY))
    parser.add_argument("--source-dir", default=str(SOURCE_DIR),
                        help="the NotoSansArabic family's files (sd-fonts.yaml builds them; its Arabic is Noto's own)")
    args = parser.parse_args()
    out_dir = Path(args.output_dir)
    source_dir = Path(args.source_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cps = pack_codepoints()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        chars = tmp / "codepoints.txt"
        chars.write_text("".join(chr(cp) for cp in cps), encoding="utf-8")
        sources = {style: subset_source(source_dir, style, tmp) for style in ("Regular", "Bold")}
        for size, styles in SIZES.items():
            raster = {}
            for style in styles:
                name = f"arabic_ui_{size}_{style.lower()}"
                header = subprocess.run([sys.executable, str(SCRIPT_DIR / "fontconvert.py"), name, str(size),
                                         str(sources[style]), "--codepoints-file", str(chars)],
                                        check=True, capture_output=True, text=True, cwd=SCRIPT_DIR).stdout
                face = read_face(header, name, STYLE_IDS[style])
                missing = sorted(set(cps) - {g.code_point for g, _ in face.all_glyphs})
                if missing:
                    raise SystemExit(f"{name}: no glyph for " + " ".join(f"U+{cp:04X}" for cp in missing))
                raster[STYLE_IDS[style]] = face
            path = out_dir / f"{FAMILY}_{size}.cpfont"
            write_cpfont(raster, str(path), two_bit=False)
            print(f"Generated {path.relative_to(REPO_ROOT)} ({len(cps)} characters, {path.stat().st_size} B)")


if __name__ == "__main__":
    main()
