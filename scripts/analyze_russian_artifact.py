#!/usr/bin/env python3
"""
Inspect the actual Russian serialized Liang artifact and the existing 5,000-word
Russian evaluation resource.

This script deliberately does NOT change production code.

It answers:
  * exact ru.bin payload size
  * exact generated-header data payload size, when present
  * number of evaluation words and Cyrillic-only words
  * legal-break distribution
  * 2+2 / 3+3 context collision statistics based on the labelled fixture

The 2+2/3+3 numbers are screening statistics, not a quality result. The
C++ experiment in RussianCompactContextExperiment.cpp runs the actual current
Russian hyphenator and performs the quality comparison.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from pathlib import Path
import re

RUS_LETTERS = set("абвгдеёжзийклмнопрстуфхцчшщъыьэюя")


def parse_positions(annotated: str) -> tuple[str, list[int]]:
    word = []
    positions = []
    pos = 0
    for ch in annotated:
        if ch == "=":
            positions.append(pos)
        else:
            word.append(ch)
            pos += 1
    return "".join(word), positions


def is_cyrillic_word(word: str) -> bool:
    letters = [c.casefold() for c in word if c.isalpha()]
    return bool(letters) and all(c in RUS_LETTERS for c in letters)


def load_fixture(path: Path):
    rows = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) != 3:
                continue
            word = parts[0]
            annotated = parts[1]
            freq = int(parts[2])
            actual_word, positions = parse_positions(annotated)
            if actual_word != word:
                raise ValueError(
                    f"fixture mismatch: {word!r} != {actual_word!r}"
                )
            rows.append((word, positions, freq))
    return rows


def load_header_payload(path: Path) -> int | None:
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8")
    m = re.search(
        r"constexpr uint8_t\s+ru_trie_data\[\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if not m:
        return None
    return len(re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1)))


def symbol(c: str) -> int:
    # 5-bit alphabet. This is only for the experiment and is intentionally
    # independent of the firmware's UTF-8 byte representation.
    alphabet = "абвгдеёжзийклмнопрстуфхцчшщъыьэюя"
    try:
        return alphabet.index(c.casefold())
    except ValueError:
        return 31  # OTHER


def key_2x2(word: str, boundary: int) -> int | None:
    if boundary < 2 or boundary + 1 >= len(word):
        return None
    chars = word[boundary - 2:boundary + 2]
    key = 0
    for c in chars:
        s = symbol(c)
        if s >= 31:
            return None
        key = (key << 5) | s
    return key


def key_3x3(word: str, boundary: int) -> int | None:
    if boundary < 3 or boundary + 2 >= len(word):
        return None
    chars = word[boundary - 3:boundary + 3]
    key = 0
    for c in chars:
        s = symbol(c)
        if s >= 31:
            return None
        key = (key << 5) | s
    return key


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trie", type=Path, default=Path("build/ru.bin"))
    parser.add_argument(
        "--header",
        type=Path,
        default=Path(
            "lib/Epub/Epub/hyphenation/generated/hyph-ru.trie.h"
        ),
    )
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path(
            "test/hyphenation_eval/resources/russian_hyphenation_tests.txt"
        ),
    )
    args = parser.parse_args()

    if not args.trie.exists():
        raise SystemExit(
            f"Missing {args.trie}. Run scripts/update_hyphenation.sh first."
        )
    if not args.fixture.exists():
        raise SystemExit(f"Missing fixture: {args.fixture}")

    payload = args.trie.stat().st_size
    header_payload = load_header_payload(args.header)

    rows = load_fixture(args.fixture)
    cyr = [row for row in rows if is_cyrillic_word(row[0])]

    legal_breaks = sum(len(row[1]) for row in cyr)
    no_break_words = sum(not row[1] for row in cyr)

    stats2 = defaultdict(lambda: [0, 0])
    stats3 = defaultdict(lambda: [0, 0])

    for word, positions, _freq in cyr:
        legal = set(positions)
        for boundary in range(2, len(word) - 2):
            key = key_2x2(word, boundary)
            if key is not None:
                stats2[key][0] += boundary in legal
                stats2[key][1] += 1
            key = key_3x3(word, boundary)
            if key is not None:
                stats3[key][0] += boundary in legal
                stats3[key][1] += 1

    def summary(stats):
        total = len(stats)
        single = sum(1 for good, total_occ in stats.values() if total_occ == 1)
        high = sum(
            1
            for good, total_occ in stats.values()
            if total_occ >= 5 and good / total_occ >= 0.99
        )
        safe5 = sum(
            1
            for good, total_occ in stats.values()
            if total_occ >= 5 and good / total_occ >= 0.999
        )
        return total, single, high, safe5

    s2 = summary(stats2)
    s3 = summary(stats3)

    print("RUSSIAN ARTIFACT / SCREENING")
    print(f"ru.bin payload:            {payload:,} bytes")
    if header_payload is not None:
        print(f"generated header payload:   {header_payload:,} bytes")
    print(f"fixture words:              {len(rows):,}")
    print(f"Cyrillic-only words:        {len(cyr):,}")
    print(f"legal breaks:               {legal_breaks:,}")
    print(f"words without breaks:       {no_break_words:,}")
    print()
    print(
        f"2x2 contexts: unique={s2[0]:,} singleton={s2[1]:,} "
        f"safe>=99%@5={s2[2]:,} safe>=99.9%@5={s2[3]:,}"
    )
    print(
        f"3x3 contexts: unique={s3[0]:,} singleton={s3[1]:,} "
        f"safe>=99%@5={s3[2]:,} safe>=99.9%@5={s3[3]:,}"
    )
    print()
    print(
        "This is only a structural screening. The quality experiment uses the "
        "actual firmware Russian hyphenator and the same fixture."
    )


if __name__ == "__main__":
    main()
