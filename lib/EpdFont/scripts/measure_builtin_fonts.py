#!/usr/bin/env python3
"""
Measure the flash footprint of the builtin fonts that are ACTUALLY compiled in
(i.e. included by builtinFonts/all.h), so a before/after comparison can confirm
whether a compression change (e.g. zlib -> zopfli) really reduces flash.

Reports, per generated header:
  - bitmapBytes:  size of the compressed glyph-bitmap array (what compression changes)
  - totalBytes:   sum of every emitted data array (bitmaps + glyph/group/kern/interval
                  tables), i.e. the font's real contribution to the image

Usage:
  python measure_builtin_fonts.py                 # human-readable table
  python measure_builtin_fonts.py --json out.json # machine-readable snapshot (baseline)
"""
import argparse
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
BUILTIN_DIR = os.path.normpath(os.path.join(HERE, "..", "builtinFonts"))
ALL_H = os.path.join(BUILTIN_DIR, "all.h")

# Byte width of each array element type emitted by fontconvert.py.
ELEM_BYTES = {
    "uint8_t": 1, "int8_t": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
    "EpdGlyph": 16, "EpdFontGroup": 20, "EpdUnicodeInterval": 12,
    "EpdKernClassEntry": 3, "EpdLigaturePair": 8,
}
ARRAY_RE = re.compile(
    r"(uint8_t|int8_t|uint16_t|int16_t|uint32_t|int32_t|EpdGlyph|EpdFontGroup|"
    r"EpdUnicodeInterval|EpdKernClassEntry|EpdLigaturePair)\s+(\w+)\[(\d+)\]"
)


def included_headers():
    headers = []
    with open(ALL_H, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"builtinFonts/([A-Za-z0-9_]+\.h)", line)
            if m:
                headers.append(m.group(1))
    return headers


def measure(header):
    path = os.path.join(BUILTIN_DIR, header)
    text = open(path, encoding="utf-8", errors="replace").read()
    total = 0
    bitmap = 0
    for etype, name, count in ARRAY_RE.findall(text):
        nbytes = int(count) * ELEM_BYTES[etype]
        total += nbytes
        if etype == "uint8_t" and name.endswith("Bitmaps"):
            bitmap += nbytes
    return bitmap, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", help="write a machine-readable snapshot to this path")
    args = ap.parse_args()

    rows = {}
    tot_bitmap = tot_all = 0
    for h in included_headers():
        b, t = measure(h)
        rows[h] = {"bitmapBytes": b, "totalBytes": t}
        tot_bitmap += b
        tot_all += t

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"fonts": rows,
                       "totals": {"bitmapBytes": tot_bitmap, "totalBytes": tot_all}},
                      f, indent=2, sort_keys=True)
        print(f"Wrote snapshot: {args.json}")

    print(f"{'header':40} {'bitmap':>10} {'total':>10}")
    for h in sorted(rows):
        print(f"{h:40} {rows[h]['bitmapBytes']:>10} {rows[h]['totalBytes']:>10}")
    print("-" * 62)
    print(f"{'TOTAL ('+str(len(rows))+' fonts)':40} {tot_bitmap:>10} {tot_all:>10}")
    print(f"\nCompressed bitmap data: {tot_bitmap/1024:.1f} KB   "
          f"Total builtin flash: {tot_all/1024:.1f} KB ({tot_all/1024/1024:.2f} MB)")


if __name__ == "__main__":
    main()
