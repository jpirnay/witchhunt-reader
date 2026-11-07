#!/usr/bin/env python3
"""Diff lib/OtShaper against HarfBuzz on every Indic font the Font Manager ships.

For each of the 20 families (Noto Sans and Noto Serif of ten scripts) and both styles, the script's corpus
(generate_corpus.py's output) is shaped with no language and with each language written in the script, at
(2133, 33) and (1195, 19), by HarfBuzz and by ot_shape_cli through the font's CPac (the `cpac` argument). Prints one
line per run, "family style size lang: N/N", and exits non-zero on any difference.

    python3 test/ot_shaper/generate_corpus.py <corpus dir>
    python3 test/ot_shaper/oracle_shipped_fonts.py <ot_shape_cli> <assets/sd-fonts> <corpus dir>

Needs uharfbuzz (the pinned pair is fontTools 4.63.0 and uharfbuzz 0.56.2: HarfBuzz 14.5.0).
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_harfbuzz import harfbuzz_lines  # noqa: E402

# script name -> (corpus file, languages written in it), as lib/Epub/Epub/LanguageTag.h maps them
SCRIPTS = {
    "Devanagari": ("devanagari", ["hi", "mr", "ne", "sa", "kok", "mai", "bho", "awa", "mag", "raj", "doi", "brx",
                                  "new"]),
    "Bengali": ("bengali", ["bn", "as", "mni"]),
    "Gurmukhi": ("gurmukhi", ["pa"]),
    "Gujarati": ("gujarati", ["gu"]),
    "Oriya": ("oriya", ["or"]),
    "Tamil": ("tamil", ["ta"]),
    "Telugu": ("telugu", ["te"]),
    "Kannada": ("kannada", ["kn"]),
    "Malayalam": ("malayalam", ["ml"]),
    "Sinhala": ("sinhala", ["si"]),
}
SIZES = [(2133, 33), (1195, 19)]
STYLES = ["Regular", "Bold"]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    cli, assets, corpus = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    different = 0
    for script, (corpus_name, languages) in SCRIPTS.items():
        with open(corpus / f"{corpus_name}.txt", encoding="utf-8") as f:
            lines = list(dict.fromkeys(l.strip() for l in f if l.strip()))
        stdin = "\n".join(lines) + "\n"
        for family in ("NotoSans" + script, "NotoSerif" + script):
            for style in STYLES:
                font = str(assets / family / f"{family}-{style}.ttf")
                for scale, ppem in SIZES:
                    for lang in [""] + languages:
                        expected = harfbuzz_lines(font, lines, scale, ppem, lang)
                        run = subprocess.run([cli, font, str(scale), str(ppem), lang or "-", "cpac"], input=stdin,
                                             capture_output=True, text=True, encoding="utf-8", check=True)
                        actual = run.stdout.splitlines()
                        bad = [(t, e, a) for t, e, a in zip(lines, expected, actual) if e != a]
                        if len(actual) != len(lines):
                            bad.append(("<line count>", str(len(lines)), str(len(actual))))
                        print(f"{family} {style} {ppem} {lang or '-'}: {len(lines) - len(bad)}/{len(lines)}",
                              flush=True)
                        different += len(bad)
                        for text, exp, act in bad[:5]:
                            esc = "".join("\\u%04X" % ord(c) for c in text)
                            print(f"  MISMATCH {esc}\n    harfbuzz: {exp}\n    otshaper: {act}")
    print(f"{different} differences")
    return 1 if different else 0


if __name__ == "__main__":
    sys.exit(main())
