#!/usr/bin/env python3
"""Generate Arabic, Persian and Urdu test strings for oracle_arabic.py.

Writes <out dir>/arabic.txt, one string per line, deterministic (random.Random(1757)):

- every assigned code point of the four Arabic ranges alone;
- each letter isolated, initial, medial and final (beside beh);
- 40 base letters, each with each of U+064B-0652 and U+0670, and with shadda and each vowel in both orders;
- the 14 modifier combining marks after beh, each with fatha and with kasra in both orders;
- lam with each alef (U+0622 0623 0625 0627 0671 0672 0673 0675), alone, after beh, with a harakat or tatweel between;
- the joiner variants (ZWNJ and ZWJ beside letters and in lam-alef);
- Persian and Urdu letters inside words;
- 2,000 random sequences of 2-8 code points from the letters, marks, joiners and tatweel.

    python3 test/ot_shaper/generate_corpus_arabic.py <out dir>

The joiners are written as escape sequences, never as literal characters.
"""

import random
import sys
import unicodedata
from pathlib import Path


def word(*codepoints):
    return "".join(chr(c) for c in codepoints)


ZWNJ, ZWJ = chr(0x200C), chr(0x200D)
BEH, LAM, TATWEEL, SPACE = chr(0x0628), chr(0x0644), chr(0x0640), " "
FATHA, KASRA, SHADDA = chr(0x064E), chr(0x0650), chr(0x0651)
RANGES = [(0x0600, 0x06FF), (0x0750, 0x077F), (0x0870, 0x089F), (0x08A0, 0x08FF)]
VOWELS = [chr(c) for c in range(0x064B, 0x0653)] + [chr(0x0670)]  # U+064B-0652 and 0670
MODIFIER_MARKS = [chr(c) for c in (0x0654, 0x0655, 0x0658, 0x06DC, 0x06E3, 0x06E7, 0x06E8, 0x08CA, 0x08CB, 0x08CD,
                                   0x08CE, 0x08CF, 0x08D3, 0x08F3)]
ALEFS = [chr(c) for c in (0x0622, 0x0623, 0x0625, 0x0627, 0x0671, 0x0672, 0x0673, 0x0675)]
PERSIAN = word(0x067E, 0x0686, 0x0698, 0x06AF, 0x06A9, 0x06CC)
URDU = word(0x067E, 0x0679, 0x0688, 0x0691, 0x06BA, 0x06BE, 0x06C1, 0x06D2, 0x06C3, 0x06CC)


def assigned():
    return [chr(c) for lo, hi in RANGES for c in range(lo, hi + 1) if unicodedata.category(chr(c)) != "Cn"]


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1757)
    chars = assigned()
    letters = [c for c in chars if unicodedata.category(c) == "Lo"]
    marks = [c for c in chars if unicodedata.category(c) in ("Mn", "Me", "Mc")]
    # 40 base letters: the 36 basic ones and four Persian/Urdu ones.
    base = [chr(c) for c in list(range(0x0621, 0x063B)) + list(range(0x0641, 0x064B))] + [chr(c) for c in (0x067E, 0x0686, 0x06A9, 0x06AF)]
    assert len(base) == 40

    lines = set(chars)
    for c in letters:
        lines.update([c + BEH, BEH + c + BEH, BEH + c, c + c])
    for c in base:
        for v in VOWELS:
            lines.update([c + v, BEH + c + v + BEH])
        for v in (x for x in VOWELS if x != SHADDA):
            lines.update([c + SHADDA + v, c + v + SHADDA, BEH + c + SHADDA + v + BEH, BEH + c + v + SHADDA + BEH])
    for m in MODIFIER_MARKS:
        for v in (FATHA, KASRA):
            lines.update([BEH + m + v, BEH + v + m, BEH + m + v + BEH, BEH + v + m + BEH, m + v])
    for a in ALEFS:
        for mid in ("", FATHA, KASRA, SHADDA, TATWEEL, ZWNJ, ZWJ):
            lines.update([LAM + mid + a, BEH + LAM + mid + a, BEH + LAM + mid + a + BEH, LAM + mid + a + LAM + mid + a])
    for c in base + list(PERSIAN) + list(URDU):
        lines.update([c + ZWNJ + BEH, c + ZWJ + BEH, BEH + ZWJ + c, BEH + ZWNJ + c, c + ZWJ, ZWJ + c, ZWJ + c + ZWJ,
                      BEH + ZWNJ + c + ZWNJ + BEH])
    for a in ALEFS:
        lines.update([LAM + ZWNJ + a, LAM + ZWJ + a, LAM + ZWJ + a + ZWJ, ZWJ + LAM + a, LAM + a + ZWJ])
    for i in range(len(PERSIAN)):
        lines.add(PERSIAN[i:] + BEH + PERSIAN[:i])
        lines.add(BEH + PERSIAN[i] + BEH + SPACE + PERSIAN[i])
    for i in range(len(URDU)):
        lines.add(URDU[i:] + BEH + URDU[:i])
        lines.add(BEH + URDU[i] + BEH + SPACE + URDU[i])
    mikhwaham = word(0x0645, 0x06CC) + ZWNJ + word(0x062E, 0x0648, 0x0627, 0x0647, 0x0645)
    urdu_sentence = word(0x0628, 0x0691, 0x06D2, 0x20, 0x0679, 0x06BE, 0x06CC, 0x06A9, 0x20, 0x0645, 0x06CC, 0x06BA,
                         0x20, 0x06C1, 0x06D2, 0x20, 0x0688, 0x06BE, 0x0648, 0x0644)
    lines.update([mikhwaham, PERSIAN, URDU, PERSIAN + SPACE + URDU, urdu_sentence])

    pool = letters + marks + [ZWNJ, ZWJ, TATWEEL]
    for _ in range(2000):
        lines.add("".join(rng.choice(pool) for _ in range(rng.randint(2, 8))))

    ordered = sorted(lines)
    (out / "arabic.txt").write_text("\n".join(ordered) + "\n", encoding="utf-8")
    print(f"{len(ordered)} lines")


if __name__ == "__main__":
    main()
