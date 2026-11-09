#!/usr/bin/env python3
"""Diff lib/OtShaper against HarfBuzz, glyph for glyph.

Shapes every line of the given text files with HarfBuzz (uharfbuzz) and with
ot_shape_cli (built from test/ot_shaper/OtShapeCli.cpp), and reports every
line whose glyph IDs, advances or offsets differ.

    pip install uharfbuzz
    cmake --build build/test --target ot_shape_cli
    python3 test/ot_shaper/compare_harfbuzz.py --cli build/test/ot_shaper/ot_shape_cli \\
        --font NotoSansBengali-Regular.ttf words-bn.txt [--lang bn]

Exits non-zero when any line differs.
"""

import argparse
import subprocess
import sys

import uharfbuzz as hb

ISO = ["Deva", "Beng", "Guru", "Gujr", "Orya", "Taml", "Telu", "Knda", "Mlym", "Sinh"]


def is_arabic(cp):
    return 0x0600 <= cp <= 0x06FF or 0x0750 <= cp <= 0x077F or 0x0870 <= cp <= 0x08FF


def is_myanmar(cp):
    return 0x1000 <= cp <= 0x109F or 0xA9E0 <= cp <= 0xA9FF or 0xAA60 <= cp <= 0xAA7F


def script_of(text):
    if any(is_arabic(ord(ch)) for ch in text):
        return "Arab"
    if any(is_myanmar(ord(ch)) for ch in text):
        return "Mymr"
    for ch in text:
        cp = ord(ch)
        if 0x0900 <= cp <= 0x0DFF and not (0x0951 <= cp <= 0x0954 or cp in (0x0964, 0x0965)):
            return ISO[(cp - 0x0900) >> 7]
    return "Deva"


def harfbuzz_lines(font_path, lines, scale, ppem, lang):
    blob = hb.Blob.from_file_path(font_path)
    font = hb.Font(hb.Face(blob))
    font.scale = (scale, scale)
    font.ppem = (ppem, ppem)
    out = []
    for text in lines:
        buf = hb.Buffer()
        buf.add_str(text)
        buf.script = script_of(text)
        buf.direction = "rtl" if buf.script == "Arab" else "ltr"
        if lang:
            buf.language = lang
        hb.shape(font, buf, {})
        out.append(" ".join(f"{i.codepoint}+{p.x_advance}+{p.x_offset}+{p.y_offset}"
                            for i, p in zip(buf.glyph_infos, buf.glyph_positions)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--font", required=True)
    ap.add_argument("--lang", default="")
    ap.add_argument("--scale", type=int, default=2133)  # 16 pt at 150 DPI in 26.6
    ap.add_argument("--ppem", type=int, default=33)
    ap.add_argument("--show", type=int, default=20, help="mismatches to print")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    lines = []
    for path in args.files:
        with open(path, encoding="utf-8") as f:
            lines += [l.strip() for l in f if l.strip() and not l.startswith("#")]
    lines = list(dict.fromkeys(lines))

    expected = harfbuzz_lines(args.font, lines, args.scale, args.ppem, args.lang)
    run = subprocess.run([args.cli, args.font, str(args.scale), str(args.ppem), args.lang or "-"],
                         input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
    actual = run.stdout.splitlines()

    bad = 0
    for text, exp, act in zip(lines, expected, actual):
        if exp == act:
            continue
        bad += 1
        if bad <= args.show:
            print(f"MISMATCH {text!r} ({' '.join(f'U+{ord(c):04X}' for c in text)})")
            print(f"  harfbuzz: {exp}")
            print(f"  otshaper: {act}")
    print(f"{args.font}: {len(lines) - bad}/{len(lines)} lines identical")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
