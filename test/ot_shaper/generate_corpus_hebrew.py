#!/usr/bin/env python3
"""Generate Hebrew and Yiddish test strings for the merge check of the Hebrew TrueType families (merge_noto_latin.py).

Writes <out dir>/hebrew.txt, one string per line, deterministic (random.Random(1757)):
- every assigned code point of the Hebrew block (U+0591-05F4) and of the Hebrew presentation forms (U+FB1D-FB4F)
  alone;
- each letter alone, doubled, beside bet, and in its final form where it has one;
- each letter with each point (sheva to qamats qatan, U+05B0-05BD, 05BF, 05C1, 05C2, 05C4, 05C5 and 05C7), and with
  dagesh and each vowel in both orders;
- shin with its dots and a vowel in both orders;
- each cantillation mark (U+0591-05AF) on a letter, alone and with a vowel in both orders;
- the divine name, bare and pointed, and the maqaf, geresh, gershayim, sof pasuq and paseq between words;
- the Yiddish digraphs (U+05F0-05F2) with their points;
- 2,000 random sequences of 2-8 code points from the letters and the marks.
    python3 test/ot_shaper/generate_corpus_hebrew.py <out dir>
"""
import random
import sys
import unicodedata
from pathlib import Path


def word(*codepoints):
    return "".join(chr(c) for c in codepoints)


RANGES = [(0x0591, 0x05F4), (0xFB1D, 0xFB4F)]
BET, SHIN, YOD, HE, VAV, SPACE = chr(0x05D1), chr(0x05E9), chr(0x05D9), chr(0x05D4), chr(0x05D5), " "
DAGESH, SHIN_DOT, SIN_DOT = chr(0x05BC), chr(0x05C1), chr(0x05C2)
POINTS = [chr(c) for c in list(range(0x05B0, 0x05BE)) + [0x05BF, 0x05C1, 0x05C2, 0x05C4, 0x05C5, 0x05C7]]
VOWELS = [chr(c) for c in list(range(0x05B0, 0x05BC)) + [0x05C7]]
ACCENTS = [chr(c) for c in range(0x0591, 0x05B0)]
LETTERS = [chr(c) for c in range(0x05D0, 0x05EB)]
FINALS = {0x05DB: 0x05DA, 0x05DE: 0x05DD, 0x05E0: 0x05DF, 0x05E4: 0x05E3, 0x05E6: 0x05E5}
PUNCTUATION = [chr(c) for c in (0x05BE, 0x05F3, 0x05F4, 0x05C3, 0x05C0)]  # maqaf, geresh, gershayim, sof pasuq, paseq
YIDDISH = [chr(c) for c in (0x05F0, 0x05F1, 0x05F2)]


def assigned():
    return [chr(c) for lo, hi in RANGES for c in range(lo, hi + 1) if unicodedata.category(chr(c)) != "Cn"]


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1757)
    chars = assigned()
    marks = [c for c in chars if unicodedata.category(c) in ("Mn", "Me", "Mc")]

    lines = set(chars)
    for c in LETTERS + YIDDISH:
        lines.update([c, c + c, c + BET, BET + c + BET, BET + c])
        for p in POINTS:
            lines.update([c + p, BET + c + p + BET])
        for v in VOWELS:
            lines.update([c + DAGESH + v, c + v + DAGESH, BET + c + DAGESH + v + BET])
    for medial, final in FINALS.items():
        lines.update([BET + chr(medial) + chr(final), BET + chr(final), chr(medial) + chr(final)])
    for dot in (SHIN_DOT, SIN_DOT):
        for v in VOWELS:
            lines.update([SHIN + dot + v, SHIN + v + dot, BET + SHIN + dot + v + BET, SHIN + DAGESH + dot + v])
    for a in ACCENTS:
        lines.update([BET + a, BET + a + BET])
        for v in (chr(0x05B8), chr(0x05B7), chr(0x05B4)):  # qamats, patah, hiriq
            lines.update([BET + v + a, BET + a + v, BET + v + a + BET])
    name = YOD + HE + VAV + HE
    pointed = YOD + chr(0x05B0) + HE + VAV + chr(0x05B8) + HE
    shalom = SHIN + SHIN_DOT + chr(0x05B8) + chr(0x05DC) + VAV + chr(0x05B9) + chr(0x05DD)
    lines.update([name, pointed, name + SPACE + shalom, BET + name, YOD + chr(0x0591) + HE + VAV + chr(0x05B8) + HE])
    for p in PUNCTUATION:
        lines.update([shalom + p + shalom, shalom + p, p + shalom, shalom + SPACE + p + SPACE + shalom])
    lines.update([shalom, shalom + SPACE + shalom, word(0x05E2, 0x05D5, 0x05DC, 0x05DD) + SPACE + shalom])

    pool = LETTERS + marks
    for _ in range(2000):
        lines.add("".join(rng.choice(pool) for _ in range(rng.randint(2, 8))))

    ordered = sorted(lines)
    (out / "hebrew.txt").write_text("\n".join(ordered) + "\n", encoding="utf-8")
    print(f"{len(ordered)} lines")


if __name__ == "__main__":
    main()
