#!/usr/bin/env python3
"""Generate Myanmar test strings (Burmese, Mon, Shan, Karen and the rest of the script) for oracle_myanmar.py and for
the merge check of the Myanmar TrueType families (merge_noto_latin.py, through build-sd-fonts.py).

Writes <out dir>/myanmar.txt, one string per line, deterministic (random.Random(1757)):

- every assigned code point of the three blocks the engine's tables cover alone: Myanmar (U+1000-109F), Myanmar
  Extended-B (U+A9E0-A9FF) and Myanmar Extended-A (U+AA60-AA7F);
- each letter with each sign (vowel signs, medials, tones, asat, virama), and after the dotted circle;
- each Burmese consonant with each medial and medial combination in canonical order, alone and with each vowel
  (the two-part ones too); a dozen consonants also with each tone, dot below and asat;
- dot below before and after asat, on a consonant and on a killed final consonant;
- kinzi before each consonant, alone, with each vowel, and between two consonants;
- each stack C + U+1039 + C, and a dozen first consonants with each vowel after the stack;
- the Mon, Shan and Karen medials on their letters, and the Shan tones;
- ZWJ and ZWNJ beside letters, medials, the virama and asat; marks without a base, alone and in pairs;
- the words of Somnius/CrossInk-Indic's Myanmar tests;
- 2,000 random sequences of 2-8 code points from the three blocks, the joiners and the dotted circle.

With a second argument, the eBible.org "readaloud" text of the Judson Bible (1840, public domain; the zip
https://ebible.org/Scriptures/myajvb_readaloud.zip or a directory of its files), it also writes
<out dir>/myanmar-prose.txt: every line of Genesis that holds a Myanmar letter, whole, and each of its space-separated
phrases. That text is not committed here.

The variation selectors (U+FE00-FE0F) are left out: the tables do not cover them, and the parser drops them.

The consonant-medial-vowel-tone combinations, the stacks and kinzi with vowels and the words follow
Somnius/CrossInk-Indic's Myanmar corpus (commits dbfb96a24 and 44b3c613 by Lef, MIT).

    python3 test/ot_shaper/generate_corpus_myanmar.py <out dir> [<myajvb_readaloud.zip or its directory>]

The joiners are written as escape sequences, never as literal characters.
"""

import random
import sys
import unicodedata
import zipfile
from pathlib import Path

RANGES = [(0x1000, 0x109F), (0xA9E0, 0xA9FF), (0xAA60, 0xAA7F)]
ZWNJ, ZWJ, DOTTED_CIRCLE = "‌", "‍", "◌"
VIRAMA, ASAT, DOT_BELOW, ANUSVARA, VISARGA = "္", "်", "့", "ံ", "း"
KINZI = "င်္"
BURMESE = [chr(c) for c in range(0x1000, 0x1021 + 1)]
COMMON = BURMESE[:12]
# Burmese medials ya, ra, wa, ha: each non-empty combination in canonical order.
MEDIAL_LETTERS = ["ျ", "ြ", "ွ", "ှ"]
MEDIALS = ["".join(m for i, m in enumerate(MEDIAL_LETTERS) if mask >> i & 1) for mask in range(1, 16)]
# Vowels: the signs alone, and the two-part vowels as Burmese writes them.
VOWELS = ["", "ါ", "ာ", "ိ", "ီ", "ု", "ူ", "ေ", "ဲ", "ော",
          "ော်", "ေါ", "ေါ်", "ို", "ံ", "ုံ",
          "ိံ"]
TONES = ["", DOT_BELOW, VISARGA, ASAT, DOT_BELOW + ASAT, ASAT + DOT_BELOW, ANUSVARA + DOT_BELOW, ASAT + VISARGA]
# Mon (medial na, ma, la), Shan (medial wa) and the letters they go with.
MINORITY_MEDIALS = ["ၞ", "ၟ", "ၠ", "ႂ"]
MINORITY_LETTERS = [chr(c) for c in (0x1000, 0x1014, 0x105A, 0x105B, 0x105C, 0x105D, 0x1061, 0x1065, 0x1066, 0x106E,
                                     0x1075, 0x1078, 0x107E, 0x108E, 0xAA60, 0xAA6E, 0xA9E0, 0xA9FA)]
SHAN_TONES = [chr(c) for c in (0x1087, 0x1088, 0x1089, 0x108A, 0x108B, 0x108C, 0x108D, 0x108F, 0x109A, 0x109B)]
# Somnius/CrossInk-Indic's Myanmar test words (commit 44b3c613 and its ComplexShaper fixture).
WORDS = ["မင်္ဂလာပါ", "ကျေးဇူး",
         "မြန်မာ", "သင်္ဘော",
         "ကျွန်တော်", "ကမ္ဘာ",
         "ကြောင့်", "၂၀၂၆"]


def assigned():
    return [chr(c) for lo, hi in RANGES for c in range(lo, hi + 1) if unicodedata.category(chr(c)) != "Cn"]


def prose_lines(source):
    """Genesis of the Judson readaloud text: the lines that hold a Myanmar letter, and their phrases."""
    texts = []
    if source.is_dir():
        texts = [p.read_text(encoding="utf-8-sig") for p in sorted(source.glob("*_GEN_*_read.txt"))]
    else:
        with zipfile.ZipFile(source) as z:
            texts = [z.read(n).decode("utf-8-sig") for n in sorted(z.namelist()) if "_GEN_" in n]
    if not texts:
        sys.exit(f"{source}: no Genesis files (*_GEN_*_read.txt)")
    lines = set()
    for text in texts:
        for line in text.splitlines():
            line = line.strip()
            if not any(0x1000 <= ord(c) <= 0x109F for c in line):
                continue
            lines.add(line)
            lines.update(p for p in line.split() if any(0x1000 <= ord(c) <= 0x109F for c in p))
    return lines


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1757)
    chars = assigned()
    letters = [c for c in chars if unicodedata.category(c) == "Lo"]
    signs = [c for c in chars if unicodedata.category(c) in ("Mn", "Mc")]

    lines = set(chars)
    for c in letters:
        lines.update(c + s for s in signs)
    lines.update(DOTTED_CIRCLE + s for s in signs)
    for c in BURMESE:
        lines.update(c + m + v for m in [""] + MEDIALS for v in VOWELS)
    lines.update(c + m + v + t for c in COMMON for m in [""] + MEDIALS[:4] for v in VOWELS for t in TONES)
    # Dot below and asat in both orders, on a consonant and on a killed final after a syllable.
    for a in COMMON:
        for b in BURMESE:
            lines.update([a + b + ASAT + t for t in TONES] + [a + b + DOT_BELOW + ASAT, a + b + ASAT + DOT_BELOW])
            lines.update([a + "ိ" + b + ASAT + DOT_BELOW, a + "ော" + b + DOT_BELOW + ASAT])
    # Kinzi.
    for c in BURMESE:
        lines.update(KINZI + c + v for v in VOWELS)
    lines.update(a + KINZI + b + v for a in COMMON for b in BURMESE for v in VOWELS[:6])
    lines.update(a + KINZI + b + m for a in COMMON for b in COMMON for m in MEDIALS)
    # Stacks.
    lines.update(a + VIRAMA + b for a in BURMESE for b in BURMESE)
    lines.update(a + VIRAMA + b + v for a in COMMON for b in BURMESE for v in VOWELS[1:])
    lines.update(a + VIRAMA + b + m for a in COMMON for b in COMMON for m in MEDIALS)
    lines.update(a + b + VIRAMA + c for a in COMMON for b in COMMON for c in COMMON[:6])
    # The other languages' medials and tones.
    for c in MINORITY_LETTERS:
        lines.update(c + m for m in MINORITY_MEDIALS)
        lines.update(c + m + v for m in MINORITY_MEDIALS for v in VOWELS[1:9])
        lines.update(c + t for t in SHAN_TONES)
        lines.update(c + v + t for v in ("ၢ", "ႃ", "ႄ", "ႅ", "ႆ") for t in SHAN_TONES)
    # Joiners.
    for c in BURMESE:
        for j in (ZWJ, ZWNJ):
            lines.update([c + j, j + c, c + j + ASAT, c + ASAT + j, c + VIRAMA + j + c, c + j + VIRAMA + c,
                          c + j + "ြ", c + "ျ" + j + "ွ", KINZI[:2] + j + KINZI[2] + c,
                          c + j + "ေ", j + "ေ" + c])
    # Marks without a base.
    lines.update(a + b for a in signs for b in signs)
    lines.update(WORDS)

    pool = chars + [ZWJ, ZWNJ, DOTTED_CIRCLE]
    for _ in range(2000):
        lines.add("".join(rng.choice(pool) for _ in range(rng.randint(2, 8))))

    ordered = sorted(lines)
    (out / "myanmar.txt").write_text("\n".join(ordered) + "\n", encoding="utf-8")
    print(f"myanmar.txt: {len(ordered)} lines")
    if len(sys.argv) == 3:
        prose = sorted(prose_lines(Path(sys.argv[2])))
        (out / "myanmar-prose.txt").write_text("\n".join(prose) + "\n", encoding="utf-8")
        print(f"myanmar-prose.txt: {len(prose)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
