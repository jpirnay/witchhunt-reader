#!/usr/bin/env python3
"""Diff lib/OtShaper against HarfBuzz on Arabic, Persian and Urdu text, in every font given.

Each *.ttf of <fonts dir> is shaped with the corpus of generate_corpus_arabic.py (arabic.txt of <corpus dir>), with no
language and with each of ar, fa, ur, ps, sd, ug, ku, ckb and ks, at (2133, 33) and (1195, 19), by HarfBuzz (direction
rtl, script Arab) and by ot_shape_cli through the font's CPac (the `cpac` argument). Prints one line per run, "font
size lang: N/N", and exits non-zero on any difference.

    cmake --build build/test --target ot_shape_cli
    python3 test/ot_shaper/generate_corpus_arabic.py <corpus dir>
    python3 test/ot_shaper/oracle_arabic.py build/test/ot_shaper/ot_shape_cli <fonts dir> <corpus dir>

Needs uharfbuzz (the pinned pair is fontTools 4.63.0 and uharfbuzz 0.56.2: HarfBuzz 14.5.0).
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_harfbuzz import harfbuzz_lines  # noqa: E402

LANGUAGES = ["", "ar", "fa", "ur", "ps", "sd", "ug", "ku", "ckb", "ks"]
SIZES = [(2133, 33), (1195, 19)]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    cli, fonts, corpus = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    with open(corpus / "arabic.txt", encoding="utf-8") as f:
        lines = list(dict.fromkeys(l.strip("\n") for l in f if l.strip()))
    stdin = "\n".join(lines) + "\n"
    different = 0
    for font in sorted(fonts.glob("*.ttf")):
        for scale, ppem in SIZES:
            for lang in LANGUAGES:
                expected = harfbuzz_lines(str(font), lines, scale, ppem, lang)
                run = subprocess.run([cli, str(font), str(scale), str(ppem), lang or "-", "cpac"], input=stdin,
                                     capture_output=True, text=True, encoding="utf-8", check=True)
                actual = run.stdout.splitlines()
                bad = [(t, e, a) for t, e, a in zip(lines, expected, actual) if e != a]
                if len(actual) != len(lines):
                    bad.append(("<line count>", str(len(lines)), str(len(actual))))
                print(f"{font.stem} {ppem} {lang or '-'}: {len(lines) - len(bad)}/{len(lines)}", flush=True)
                different += len(bad)
                for text, exp, act in bad[:5]:
                    esc = "".join("\\u%04X" % ord(c) for c in text)
                    print(f"  MISMATCH {esc}\n    harfbuzz: {exp}\n    otshaper: {act}")
    print(f"{different} differences")
    return 1 if different else 0


if __name__ == "__main__":
    sys.exit(main())
