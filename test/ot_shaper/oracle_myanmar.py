#!/usr/bin/env python3
"""Diff lib/OtShaper against HarfBuzz on Myanmar text, in every font given.

Each *.ttf of <fonts dir> is shaped with the corpus of generate_corpus_myanmar.py (myanmar.txt of <corpus dir>, and
myanmar-prose.txt when it is there), with no language and with each of my, mnw, ksw and shn, at (2133, 33) and
(1195, 19), by HarfBuzz (direction ltr, script Mymr) and by ot_shape_cli through the font's CPac (the `cpac` argument;
with --computed, on the filters the plan computes). Prints one line per run, "font size lang: N/N", and exits
non-zero on any difference.

    cmake --build build/test --target ot_shape_cli
    python3 test/ot_shaper/generate_corpus_myanmar.py <corpus dir> [<myajvb_readaloud.zip>]
    python3 test/ot_shaper/oracle_myanmar.py build/test/ot_shaper/ot_shape_cli <fonts dir> <corpus dir> [--computed]

The fonts are NotoSansMyanmar and NotoSerifMyanmar, Regular and Bold, from notofonts.github.io at 5c82cef
(fonts/<Family>/unhinted/ttf/). Needs uharfbuzz (the pinned pair is fontTools 4.63.0 and uharfbuzz 0.56.2: HarfBuzz
14.5.0).
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_harfbuzz import harfbuzz_lines  # noqa: E402

LANGUAGES = ["", "my", "mnw", "ksw", "shn"]
SIZES = [(2133, 33), (1195, 19)]
CORPUS_FILES = ["myanmar.txt", "myanmar-prose.txt"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cli")
    ap.add_argument("fonts", type=Path)
    ap.add_argument("corpus", type=Path)
    ap.add_argument("--computed", action="store_true", help="shape on computed filters instead of the CPac")
    args = ap.parse_args()
    lines = []
    for name in CORPUS_FILES:
        path = args.corpus / name
        if path.is_file():
            with open(path, encoding="utf-8") as f:
                lines += [l.strip("\n") for l in f if l.strip()]
        elif name == CORPUS_FILES[0]:
            sys.exit(f"missing {path}")
    lines = list(dict.fromkeys(lines))
    stdin = "\n".join(lines) + "\n"
    different = 0
    fonts = sorted(args.fonts.glob("*.ttf"))
    if not fonts:
        sys.exit(f"no fonts in {args.fonts}")
    for font in fonts:
        for scale, ppem in SIZES:
            for lang in LANGUAGES:
                expected = harfbuzz_lines(str(font), lines, scale, ppem, lang)
                command = [args.cli, str(font), str(scale), str(ppem), lang or "-"] + ([] if args.computed else ["cpac"])
                run = subprocess.run(command, input=stdin, capture_output=True, text=True, encoding="utf-8", check=True)
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
