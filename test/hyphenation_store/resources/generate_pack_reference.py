#!/usr/bin/env python3
"""The expected breaks of the languages added with hyphenation packs, for HyphenationStoreTest.

Their tries come from Typst hypher (scripts/build_hyphenation_packs.py), compiled from hyph-utf8's TeX patterns. This
script runs Liang's algorithm straight over those patterns -- not over a trie -- so the test checks the packs, and the
firmware's walk of them, against the patterns themselves.

  python3 test/hyphenation_store/resources/generate_pack_reference.py <hypher checkout> <udhr2 dir> [--out <dir>]

<udhr2 dir> holds NLTK's udhr2 corpus (<iso639-3>.txt, UTF-8). Each <code>_reference.txt lists up to 400 lowercase
words of the language's Universal Declaration of Human Rights (none for Assamese, Odia and Telugu: the corpus has no
text of them), then 150 seeded random strings of the letters its patterns use. A line is

  word|i,j,...

with the code point indexes a break goes before, inside the language's minima. Only words of the letters the
firmware's language table accepts are listed: it does not hyphenate any other.
"""

import argparse
import pathlib
import random
import re
import sys

# code: (hypher pattern file, UDHR file or None, script, (lefthyphenmin, righthyphenmin)), as LanguageRegistry.cpp
LANGUAGES = {
    "nl": ("hyph-nl.tex", "nld", "latin", (2, 2)),
    "cs": ("hyph-cs-sojka.tex", "ces", "latin", (2, 2)),
    "da": ("hyph-da.tex", "dan", "latin", (2, 2)),
    "sl": ("hyph-sl.tex", "slv", "latin", (2, 2)),
    "lt": ("hyph-lt.tex", "lit", "latin", (2, 2)),
    "ca": ("hyph-ca.tex", "cat", "latin", (2, 2)),
    "be": ("hyph-be.tex", "bel", "cyrillic", (2, 2)),
    "fi": ("hyph-fi-x-school.tex", "fin", "latin", (2, 2)),
    "tr": ("hyph-tr.tex", "tur", "latin", (2, 2)),
    "as": ("hyph-as.tex", None, "indic", (2, 2)),
    "bn": ("hyph-bn.tex", "ben", "indic", (2, 2)),
    "gu": ("hyph-gu.tex", "guj", "indic", (2, 2)),
    "hi": ("hyph-hi.tex", "hin", "indic", (2, 2)),
    "kn": ("hyph-kn.tex", "kan", "indic", (2, 2)),
    "ml": ("hyph-ml.tex", "mal", "indic", (2, 2)),
    "mr": ("hyph-mr.tex", "mar", "indic", (2, 2)),
    "or": ("hyph-or.tex", None, "indic", (2, 2)),
    "pa": ("hyph-pa.tex", "pan", "indic", (2, 2)),
    "sa": ("hyph-sa.tex", "san", "indic", (2, 2)),
    "ta": ("hyph-ta.tex", "tam", "indic", (2, 2)),
    "te": ("hyph-te.tex", None, "indic", (2, 2)),
}
MAX_WORDS = 400
RANDOM_STRINGS = 150
MAX_CHARS = 60  # under LiangHyphenation.cpp's MAX_WORD_CHARS (70, sentinels included)


# The firmware's letters (HyphenationCommon.cpp).
def is_latin(c):
    cp = ord(c)
    return (0x41 <= cp <= 0x5A or 0x61 <= cp <= 0x7A or (0xC0 <= cp <= 0xFF and cp not in (0xD7, 0xF7))
            or 0x100 <= cp <= 0x17F or cp == 0x1E9E)


def is_cyrillic(c):
    return 0x400 <= ord(c) <= 0x52F


def is_indic(c):
    cp = ord(c)
    if cp in (0x200C, 0x200D):
        return True
    if not 0x900 <= cp <= 0xDFF or 0x951 <= cp <= 0x954 or cp in (0x964, 0x965):
        return False
    return not 0x66 <= (cp & 0x7F) <= 0x6F


LETTER = {"latin": is_latin, "cyrillic": is_cyrillic, "indic": is_indic}


def lower(word):
    """hypher's lowercasing: a char's lowercase when it is one char of the same UTF-8 length."""
    out = []
    for c in word:
        lc = c.lower()
        out.append(lc if len(lc) == 1 and len(lc.encode()) == len(c.encode()) else c)
    return "".join(out)


def patterns(tex):
    """The patterns of a hyph-utf8 file, as hypher's tests/generate.rs reads them: \\patterns{...}, % comments."""
    body = re.sub(r"%[^\n]*", "", tex)
    out = {}
    for block in re.findall(r"\\patterns\{(.*?)\}", body, re.S):
        for pat in block.split():
            letters, levels = "", [0]
            for ch in pat:
                if ch.isdigit():
                    levels[-1] = int(ch)
                else:
                    letters += ch
                    levels.append(0)
            out[letters] = levels
    return out


def breaks(word, pats, minima):
    dotted = "." + word + "."
    points = [0] * (len(dotted) + 1)
    for i in range(len(dotted)):
        for j in range(i + 1, len(dotted) + 1):
            levels = pats.get(dotted[i:j])
            if levels:
                for k, v in enumerate(levels):
                    points[i + k] = max(points[i + k], v)
    left, right = max(1, minima[0]), max(1, minima[1])
    return [b for b in range(left, len(word) - right + 1) if points[b + 1] % 2 == 1]


def udhr_words(path, letter):
    text = path.read_text(encoding="utf-8")
    counts = {}
    for raw in re.split(r"[\s\.,;:!?()\[\]«»\"'“”„‘’\-–—/।॥]+", text):
        w = lower(raw)
        # Lowercase only: a capital that lowercases to more bytes (Turkish İ) is kept by hypher, lowered by the firmware.
        if 4 <= len(w) <= MAX_CHARS and all(letter(c) and not c.isupper() for c in w):
            counts[w] = counts.get(w, 0) + 1
    return sorted(counts, key=lambda w: (-counts[w], w))[:MAX_WORDS]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("hypher", type=pathlib.Path)
    parser.add_argument("udhr2", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent)
    args = parser.parse_args(argv)
    for code, (tex, udhr, script, minima) in LANGUAGES.items():
        pats = patterns((args.hypher / "patterns" / tex).read_text(encoding="utf-8"))
        letter = LETTER[script]
        words = udhr_words(args.udhr2 / f"{udhr}.txt", letter) if udhr else []
        alphabet = sorted({c for p in pats for c in p if c != "." and letter(c)})
        rng = random.Random(f"{code}-cphyph")
        seen = set(words)
        randoms = []
        while len(randoms) < RANDOM_STRINGS:
            w = "".join(rng.choice(alphabet) for _ in range(rng.randint(4, 12)))
            if w not in seen:
                seen.add(w)
                randoms.append(w)
        words += randoms
        lines = [f"# {code}: {tex} from hypher; minima {minima[0]}/{minima[1]}; word|code point indexes of its breaks"]
        total = 0
        for w in words:
            b = breaks(w, pats, minima)
            total += len(b)
            lines.append(f"{w}|{','.join(map(str, b))}")
        (args.out / f"{code}_reference.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"{code}: {len(words)} words, {total} breaks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
