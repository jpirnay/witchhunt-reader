#!/usr/bin/env python3
"""Generate compact German hybrid-hyphenation validation tables.

The firmware's GermanHybridHyphenator first creates cheap candidates from the
German syllable rules. This script learns only the small amount of data needed
to decide which candidates are safe and which high-confidence breaks are
missing.

Source format: DANTE languages-german/wortliste.
Training split is deterministic:
  SHA-1 bucket 0..7: generate tables
  SHA-1 bucket 8:    validation only
  SHA-1 bucket 9:    held-out test fixture only
"""
from __future__ import annotations

import argparse
import hashlib
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

MIN_PREFIX = 2
MIN_SUFFIX = 2
MAX_WORD_CHARS = 70
SYMBOL_OTHER = 30

VOWELS = {ord(c) for c in "aeiouyäöüàáâãåèéêëìíîïòóôõùúûýÿ"}
PROTECTED_DIPHTHONGS = {
    (ord("a"), ord("i")),
    (ord("a"), ord("u")),
    (ord("ä"), ord("u")),
    (ord("e"), ord("i")),
    (ord("e"), ord("u")),
    (ord("o"), ord("i")),
    (ord("i"), ord("e")),
}
PROTECTED_CONSONANT_PAIRS = {
    (ord("c"), ord("h")),
    (ord("c"), ord("k")),
    (ord("p"), ord("h")),
    (ord("r"), ord("h")),
    (ord("s"), ord("h")),
    (ord("t"), ord("h")),
}
MARKER_CHARS = frozenset("-=<>·.")
BREAK_MARKER_CHARS = frozenset("-=<>·")
MORPHEME_MARKER_CHARS = frozenset("=<>")


@dataclass(frozen=True)
class WordEntry:
    word: str
    cps: tuple[int, ...]
    legal: frozenset[int]
    preferred: frozenset[int]
    undesirable: frozenset[int]
    ordinary: frozenset[int]       # -
    compound: frozenset[int]       # =
    prefix: frozenset[int]         # <
    suffix: frozenset[int]         # >
    uncategorized: frozenset[int]  # ·
    bucket: int


def lower_latin(cp: int) -> int:
    """Mirror HyphenationCommon.cpp::toLowerLatinImpl()."""
    if ord("A") <= cp <= ord("Z"):
        return cp + 0x20
    if (0x00C0 <= cp <= 0x00D6) or (0x00D8 <= cp <= 0x00DE):
        return cp + 0x20
    if (
        (0x0100 <= cp <= 0x0137 and cp % 2 == 0)
        or (0x0139 <= cp <= 0x0148 and cp % 2 == 1)
        or (0x014A <= cp <= 0x0177 and cp % 2 == 0)
        or (0x0179 <= cp <= 0x017E and cp % 2 == 1)
    ):
        return cp + 1
    if cp == 0x0178:
        return 0x00FF
    if cp == 0x1E9E:
        return 0x00DF
    return cp


def is_latin_letter(cp: int) -> bool:
    """Mirror HyphenationCommon.cpp::isLatinLetter()."""
    if ord("A") <= cp <= ord("Z") or ord("a") <= cp <= ord("z"):
        return True
    if (
        (0x00C0 <= cp <= 0x00D6)
        or (0x00D8 <= cp <= 0x00F6)
        or (0x00F8 <= cp <= 0x00FF)
    ) and cp not in (0x00D7, 0x00F7):
        return True
    if 0x0100 <= cp <= 0x017F:
        return True
    return cp == 0x1E9E


def is_vowel(cp: int) -> bool:
    return lower_latin(cp) in VOWELS


def vowel_nucleus_end(cps: tuple[int, ...], pos: int) -> int:
    if pos + 1 < len(cps):
        pair = (lower_latin(cps[pos]), lower_latin(cps[pos + 1]))
        if pair in PROTECTED_DIPHTHONGS:
            return pos + 2
    return pos + 1


def consonant_unit_length(cps: tuple[int, ...], pos: int, end: int) -> int:
    if (
        pos + 2 < end
        and lower_latin(cps[pos]) == ord("s")
        and lower_latin(cps[pos + 1]) == ord("c")
        and lower_latin(cps[pos + 2]) == ord("h")
    ):
        return 3
    if pos + 1 < end:
        pair = (lower_latin(cps[pos]), lower_latin(cps[pos + 1]))
        if pair in PROTECTED_CONSONANT_PAIRS:
            return 2
    return 1


def base_breaks(cps: tuple[int, ...]) -> set[int]:
    """Mirror markBaseSyllableBreaks() in GermanHybridHyphenator.cpp."""
    n = len(cps)
    if n < MIN_PREFIX + MIN_SUFFIX or n > MAX_WORD_CHARS:
        return set()
    if not all(is_latin_letter(cp) for cp in cps):
        return set()

    first_vowel = 0
    while first_vowel < n and not is_vowel(cps[first_vowel]):
        first_vowel += 1
    if first_vowel == n:
        return set()

    result: set[int] = set()
    left_nucleus_end = vowel_nucleus_end(cps, first_vowel)
    scan = left_nucleus_end

    while scan < n:
        next_vowel = scan
        while next_vowel < n and not is_vowel(cps[next_vowel]):
            next_vowel += 1
        if next_vowel == n:
            break

        if next_vowel == left_nucleus_end:
            candidate = next_vowel
        else:
            pos = left_nucleus_end
            last_unit_start = pos
            while pos < next_vowel:
                last_unit_start = pos
                pos += consonant_unit_length(cps, pos, next_vowel)
            candidate = last_unit_start

        if candidate >= MIN_PREFIX and n - candidate >= MIN_SUFFIX:
            result.add(candidate)

        left_nucleus_end = vowel_nucleus_end(cps, next_vowel)
        scan = left_nucleus_end

    return result


def german_symbol(cp: int) -> int:
    """Return the 5-bit symbol used by GermanHybridRules.cpp."""
    cp = lower_latin(cp)
    if ord("a") <= cp <= ord("z"):
        return cp - ord("a")
    if cp == 0x00E4:
        return 26
    if cp == 0x00F6:
        return 27
    if cp == 0x00FC:
        return 28
    if cp == 0x00DF:
        return 29

    fold_to_ascii = {
        0x00E0: "a", 0x00E1: "a", 0x00E2: "a", 0x00E3: "a", 0x00E5: "a",
        0x00E8: "e", 0x00E9: "e", 0x00EA: "e", 0x00EB: "e",
        0x00EC: "i", 0x00ED: "i", 0x00EE: "i", 0x00EF: "i",
        0x00F2: "o", 0x00F3: "o", 0x00F4: "o", 0x00F5: "o",
        0x00F9: "u", 0x00FA: "u", 0x00FB: "u",
        0x00FD: "y", 0x00FF: "y",
    }
    folded = fold_to_ascii.get(cp)
    if folded is not None:
        return ord(folded) - ord("a")
    return SYMBOL_OTHER


def pair_key(cps: tuple[int, ...], boundary: int) -> int | None:
    left = german_symbol(cps[boundary - 1])
    right = german_symbol(cps[boundary])
    if left == SYMBOL_OTHER or right == SYMBOL_OTHER:
        return None
    return (left << 5) | right


def context_key(cps: tuple[int, ...], boundary: int) -> int | None:
    """Pack two codepoints left + two right into a 20-bit integer."""
    symbols = (
        german_symbol(cps[boundary - 2]),
        german_symbol(cps[boundary - 1]),
        german_symbol(cps[boundary]),
        german_symbol(cps[boundary + 1]),
    )
    if SYMBOL_OTHER in symbols:
        return None
    key = 0
    for symbol in symbols:
        key = (key << 5) | symbol
    return key


def split_bucket(word: str) -> int:
    return hashlib.sha1(word.casefold().encode("utf-8")).digest()[0] % 10


def select_reformed_field(fields: list[str]) -> str | None:
    # DANTE README.wortliste:
    # field 2 if all orthographies agree, otherwise field 4 (reformed 2006).
    if len(fields) >= 2 and fields[1] and fields[1] != "-2-":
        return fields[1]
    if len(fields) >= 4 and fields[3] and fields[3] != "-4-":
        return fields[3]
    return None


def parse_annotation(annotation: str) -> tuple[str, dict[int, str]] | None:
    # Version 1 deliberately excludes alternative/special DANTE forms.
    if any(ch in annotation for ch in "{}[]/"):
        return None

    plain: list[str] = []
    markers: dict[int, str] = {}
    position = 0
    i = 0
    while i < len(annotation):
        if annotation[i] in MARKER_CHARS:
            start = i
            while i < len(annotation) and annotation[i] in MARKER_CHARS:
                i += 1
            markers[position] = markers.get(position, "") + annotation[start:i]
            continue
        plain.append(annotation[i])
        position += 1
        i += 1
    return "".join(plain), markers


def parse_word_line(line: str) -> WordEntry | None:
    fields = line.rstrip("\r\n").split(";")
    if not fields or not fields[0]:
        return None
    annotation = select_reformed_field(fields)
    if annotation is None:
        return None
    parsed = parse_annotation(annotation)
    if parsed is None:
        return None
    plain, markers = parsed
    if plain.casefold() != fields[0].casefold():
        return None

    cps = tuple(ord(ch) for ch in plain)
    if not (MIN_PREFIX + MIN_SUFFIX <= len(cps) <= MAX_WORD_CHARS):
        return None
    if not all(is_latin_letter(cp) for cp in cps):
        return None

    legal: set[int] = set()
    preferred: set[int] = set()
    undesirable: set[int] = set()
    ordinary: set[int] = set()
    compound: set[int] = set()
    prefix: set[int] = set()
    suffix: set[int] = set()
    uncategorized: set[int] = set()

    for boundary, marker in markers.items():
        if boundary < MIN_PREFIX or len(cps) - boundary < MIN_SUFFIX:
            continue
        if not any(ch in BREAK_MARKER_CHARS for ch in marker):
            continue
        if "." in marker:
            undesirable.add(boundary)
            continue

        legal.add(boundary)

        # Keep the original DANTE marker categories for diagnostics. Mixed
        # markers deliberately put the same boundary into more than one
        # category; these recalls are diagnostics, not a partition of legal.
        if "-" in marker:
            ordinary.add(boundary)
        if "=" in marker:
            compound.add(boundary)
        if "<" in marker:
            prefix.add(boundary)
        if ">" in marker:
            suffix.add(boundary)
        if "·" in marker:
            uncategorized.add(boundary)

        if any(ch in MORPHEME_MARKER_CHARS for ch in marker):
            preferred.add(boundary)

    return WordEntry(
        word=plain,
        cps=cps,
        legal=frozenset(legal),
        preferred=frozenset(preferred),
        undesirable=frozenset(undesirable),
        ordinary=frozenset(ordinary),
        compound=frozenset(compound),
        prefix=frozenset(prefix),
        suffix=frozenset(suffix),
        uncategorized=frozenset(uncategorized),
        bucket=split_bucket(plain),
    )


def entries(path: Path) -> Iterator[WordEntry]:
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            if not line or line.startswith("#"):
                continue
            entry = parse_word_line(line)
            if entry is not None:
                yield entry


def select_keys(stats: dict[int, list[int]], support: int, confidence: float) -> set[int]:
    return {
        key
        for key, (correct, total) in stats.items()
        if total >= support and correct / total >= confidence
    }


def build_tables(
    source: Path,
    pair_support: int,
    pair_confidence: float,
    context_support: int,
    context_confidence: float,
    add_support: int,
    add_confidence: float,
) -> tuple[set[int], set[int], set[int]]:
    # Pass 1: find safe immediate boundary classes, and collect high-confidence
    # additions for breaks the base rules never generated.
    pair_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    add_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])

    for entry in entries(source):
        if entry.bucket > 7:
            continue
        base = base_breaks(entry.cps)
        for boundary in range(MIN_PREFIX, len(entry.cps) - MIN_SUFFIX + 1):
            if boundary in base:
                key = pair_key(entry.cps, boundary)
                if key is not None:
                    stat = pair_stats[key]
                    stat[1] += 1
                    if boundary in entry.legal:
                        stat[0] += 1
            else:
                key = context_key(entry.cps, boundary)
                if key is not None:
                    stat = add_stats[key]
                    stat[1] += 1
                    if boundary in entry.legal:
                        stat[0] += 1

    safe_pairs = select_keys(pair_stats, pair_support, pair_confidence)
    add_contexts = select_keys(add_stats, add_support, add_confidence)

    # Pass 2: only ambiguous base candidates need the larger 2+2 context.
    safe_context_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    for entry in entries(source):
        if entry.bucket > 7:
            continue
        base = base_breaks(entry.cps)
        for boundary in base:
            pair = pair_key(entry.cps, boundary)
            if pair is not None and pair in safe_pairs:
                continue
            key = context_key(entry.cps, boundary)
            if key is None:
                continue
            stat = safe_context_stats[key]
            stat[1] += 1
            if boundary in entry.legal:
                stat[0] += 1

    safe_contexts = select_keys(safe_context_stats, context_support, context_confidence)
    return safe_pairs, safe_contexts, add_contexts


def apply_tables(
    entry: WordEntry,
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
) -> set[int]:
    base = base_breaks(entry.cps)
    result: set[int] = set()

    for boundary in range(MIN_PREFIX, len(entry.cps) - MIN_SUFFIX + 1):
        context = context_key(entry.cps, boundary)
        if boundary in base:
            pair = pair_key(entry.cps, boundary)
            if (pair is not None and pair in safe_pairs) or (
                context is not None and context in safe_contexts
            ):
                result.add(boundary)
        elif context is not None and context in add_contexts:
            result.add(boundary)
    return result


def new_metrics() -> dict[str, int]:
    return {
        "words": 0,
        "tp": 0,
        "fp": 0,
        "fn": 0,
        "exact": 0,
        "preferred_found": 0,
        "preferred_total": 0,
        "undesirable_used": 0,
    }


def add_metrics(metrics: dict[str, int], entry: WordEntry, actual: set[int]) -> None:
    metrics["words"] += 1
    metrics["tp"] += len(actual & entry.legal)
    metrics["fp"] += len(actual - entry.legal)
    metrics["fn"] += len(entry.legal - actual)
    metrics["exact"] += int(actual == entry.legal)
    metrics["preferred_found"] += len(actual & entry.preferred)
    metrics["preferred_total"] += len(entry.preferred)
    metrics["undesirable_used"] += len(actual & entry.undesirable)


def finalized_metrics(metrics: dict[str, int]) -> dict[str, float | int]:
    tp = metrics["tp"]
    fp = metrics["fp"]
    fn = metrics["fn"]
    words = metrics["words"]
    preferred_found = metrics["preferred_found"]
    preferred_total = metrics["preferred_total"]

    precision = tp / (tp + fp) if tp + fp else 1.0
    recall = tp / (tp + fn) if tp + fn else 1.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {
        "words": words,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "exact": metrics["exact"] / words if words else 0.0,
        "preferred_recall": preferred_found / preferred_total if preferred_total else 1.0,
        "undesirable_used": metrics["undesirable_used"],
        "false_positives": fp,
        "false_negatives": fn,
    }


def build_pair_bitset(keys: set[int]) -> bytes:
    data = bytearray(128)  # 1024 possible 5-bit x 5-bit pairs.
    for key in keys:
        data[key >> 3] |= 1 << (key & 7)
    return bytes(data)


def pack_20bit_keys(keys: set[int]) -> bytes:
    # Three bytes per key. The top four bits of byte 2 remain zero.
    data = bytearray()
    for key in sorted(keys):
        data.extend((key & 0xFF, (key >> 8) & 0xFF, (key >> 16) & 0x0F))
    return bytes(data)


def format_bytes(blob: bytes, per_line: int = 16) -> str:
    if not blob:
        return ""
    lines = []
    for i in range(0, len(blob), per_line):
        chunk = ", ".join(f"0x{value:02X}" for value in blob[i : i + per_line])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def write_header(
    output: Path,
    source: Path,
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
    args: argparse.Namespace,
) -> None:
    source_sha = hashlib.sha256(source.read_bytes()).hexdigest()
    pair_blob = build_pair_bitset(safe_pairs)
    safe_blob = pack_20bit_keys(safe_contexts)
    add_blob = pack_20bit_keys(add_contexts)

    content = f'''#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Generated by scripts/generate_german_hybrid_rules.py. Do not edit manually.
// Source: DANTE languages-german/wortliste
// Source SHA-256: {source_sha}
// Training: deterministic SHA-1 buckets 0..7 only; buckets 8/9 are held out.
// pair: support>={args.pair_support}, confidence>={args.pair_confidence:.6f}
// context: support>={args.context_support}, confidence>={args.context_confidence:.6f}
// add: support>={args.add_support}, confidence>={args.add_confidence:.6f}

inline constexpr std::array<uint8_t, {len(pair_blob)}> kGermanSafePairBits = {{
{format_bytes(pair_blob)}
}};

// Sorted 20-bit keys, packed little-endian into three bytes each.
inline constexpr std::array<uint8_t, {len(safe_blob)}> kGermanSafeContexts = {{
{format_bytes(safe_blob)}
}};
inline constexpr size_t kGermanSafeContextCount = {len(safe_contexts)};

// High-confidence legal breaks not produced by the base rule engine.
inline constexpr std::array<uint8_t, {len(add_blob)}> kGermanAddContexts = {{
{format_bytes(add_blob)}
}};
inline constexpr size_t kGermanAddContextCount = {len(add_contexts)};
'''
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8")


def positions(values: frozenset[int]) -> str:
    return ",".join(str(value) for value in sorted(values))


def write_eval_fixture(output: Path, candidates: list[tuple[bytes, WordEntry]]) -> None:
    candidates.sort(key=lambda item: item[0])
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as out:
        out.write("# Held-out DANTE evaluation for GermanHybridHyphenator\n")
        out.write("# Bucket 9 is never used to generate firmware tables.\n")
        out.write(
            "# Format: "
            "word|legal|preferred|undesirable|"
            "ordinary|compound|prefix|suffix|uncategorized\n"
        )
        for _, entry in candidates:
            out.write(
                f"{entry.word}|{positions(entry.legal)}|{positions(entry.preferred)}|"
                f"{positions(entry.undesirable)}|"
                f"{positions(entry.ordinary)}|"
                f"{positions(entry.compound)}|"
                f"{positions(entry.prefix)}|"
                f"{positions(entry.suffix)}|"
                f"{positions(entry.uncategorized)}\n"
            )


def evaluate_and_collect_fixture(
    source: Path,
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
    eval_limit: int,
) -> tuple[dict[str, float | int], dict[str, float | int], list[tuple[bytes, WordEntry]]]:
    # Pass 3: evaluate both held-out buckets and collect a deterministic bucket-9
    # fixture.  Keeping this in one pass avoids reparsing the ~15 MB source twice.
    validation = new_metrics()
    test = new_metrics()
    fixture: list[tuple[bytes, WordEntry]] = []

    for entry in entries(source):
        if entry.bucket not in (8, 9):
            continue
        actual = apply_tables(entry, safe_pairs, safe_contexts, add_contexts)
        add_metrics(validation if entry.bucket == 8 else test, entry, actual)

        if entry.bucket == 9:
            digest = hashlib.sha1(("eval:" + entry.word.casefold()).encode("utf-8")).digest()
            fixture.append((digest, entry))

    fixture.sort(key=lambda item: item[0])
    if eval_limit > 0:
        fixture = fixture[:eval_limit]

    return finalized_metrics(validation), finalized_metrics(test), fixture


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output-header", required=True, type=Path)
    parser.add_argument("--output-eval", required=True, type=Path)
    parser.add_argument("--pair-support", type=int, default=50)
    parser.add_argument("--pair-confidence", type=float, default=0.99)
    parser.add_argument("--context-support", type=int, default=3)
    parser.add_argument("--context-confidence", type=float, default=0.99)
    parser.add_argument("--add-support", type=int, default=3)
    parser.add_argument("--add-confidence", type=float, default=0.999)
    parser.add_argument("--eval-limit", type=int, default=5000)
    args = parser.parse_args()

    safe_pairs, safe_contexts, add_contexts = build_tables(
        args.input,
        args.pair_support,
        args.pair_confidence,
        args.context_support,
        args.context_confidence,
        args.add_support,
        args.add_confidence,
    )

    write_header(
        args.output_header,
        args.input,
        safe_pairs,
        safe_contexts,
        add_contexts,
        args,
    )

    validation, test, fixture = evaluate_and_collect_fixture(
        args.input,
        safe_pairs,
        safe_contexts,
        add_contexts,
        args.eval_limit,
    )
    write_eval_fixture(args.output_eval, fixture)

    payload = 128 + 3 * (len(safe_contexts) + len(add_contexts))
    print(f"safe pair classes: {len(safe_pairs)} / 1024")
    print(f"safe 2+2 contexts: {len(safe_contexts)}")
    print(f"add 2+2 contexts:  {len(add_contexts)}")
    print(f"generated payload: {payload} bytes")

    for label, metrics in (("validation", validation), ("test", test)):
        print(
            f"{label}: words={metrics['words']} "
            f"precision={metrics['precision'] * 100:.3f}% "
            f"recall={metrics['recall'] * 100:.3f}% "
            f"F1={metrics['f1'] * 100:.3f}% "
            f"exact={metrics['exact'] * 100:.3f}% "
            f"preferred-recall={metrics['preferred_recall'] * 100:.3f}% "
            f"undesirable-used={metrics['undesirable_used']}"
        )


if __name__ == "__main__":
    main()
