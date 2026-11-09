#!/usr/bin/env python3
r"""Cut the committed corpus sample from generate_corpus.py's output.

The first 200 lines of each Indic script's file go to <out dir>/indic_corpus_<script>.txt, and 200 lines of
generate_corpus_arabic.py's arabic.txt, evenly spaced through the sorted file (its first lines are single code points),
to <out dir>/arabic_corpus.txt, and 200 lines of generate_corpus_myanmar.py's myanmar.txt the same way to
<out dir>/myanmar_corpus.txt; every code point of Unicode category Cf
(ZWJ, ZWNJ, ...) written as a literal XXXX escape, so no joiner sits in the repo; the test decodes them.
The full corpus depends on the Python's unicodedata version, which is why CI reads this sample instead.

    python3 test/ot_shaper/generate_corpus.py <corpus dir>
    python3 test/ot_shaper/generate_corpus_arabic.py <corpus dir>
    python3 test/ot_shaper/generate_corpus_myanmar.py <corpus dir>
    python3 test/ot_shaper/make_corpus_sample.py <corpus dir> test/ttf_font/data
"""

import sys
import unicodedata
from pathlib import Path

SCRIPTS = ["devanagari", "bengali", "gurmukhi", "gujarati", "oriya", "tamil", "telugu", "kannada", "malayalam",
           "sinhala"]
LINES = 200


def escape(line):
    return "".join("\\u%04X" % ord(c) if unicodedata.category(c) == "Cf" else c for c in line)


def evenly(lines, count):
    """count lines spread through the list, the first and then every len//count-th."""
    step = max(1, len(lines) // count)
    return lines[::step][:count]


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    for name in ("arabic", "myanmar"):
        lines = (src / f"{name}.txt").read_text(encoding="utf-8").splitlines()
        text = "\n".join(escape(l) for l in evenly(lines, LINES)) + "\n"
        (dst / f"{name}_corpus.txt").write_text(text, encoding="utf-8", newline="\n")
    for name in SCRIPTS:
        lines = (src / f"{name}.txt").read_text(encoding="utf-8").splitlines()[:LINES]
        (dst / f"indic_corpus_{name}.txt").write_text("\n".join(escape(l) for l in lines) + "\n", encoding="utf-8",
                                                      newline="\n")


if __name__ == "__main__":
    main()
