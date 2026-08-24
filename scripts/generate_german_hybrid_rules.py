#!/usr/bin/env python3
"""
Generate compact German hybrid-hyphenation tables.

The embedded German hyphenator deliberately does NOT store the complete
DANTE/Hyphen pattern dictionary.  Instead it works in three stages:

  1. A small deterministic implementation of the basic German syllable
     rules creates candidate break positions.
  2. Compact 2+2 context tables validate/reject those candidates and recover
     a small set of high-confidence missing breaks.
  3. Optional 3+3 context tables provide a larger residual correction layer.

The important design goal is to spend flash only where it buys useful
information.  The 3+3 layer is therefore selected against the validation
split under a hard byte budget.

DANTE training split:
  SHA-1 bucket 0..7 -> training
  SHA-1 bucket 8    -> validation / profile selection
  SHA-1 bucket 9    -> held-out test fixture only

The generated header is consumed by GermanHybridRules.cpp.  If the selected
3+3 tables are empty, GERMAN_HYBRID_HAS_3X3 is not emitted and the runtime
stays in its 2+2-only mode.
"""

from __future__ import annotations

import argparse
import hashlib
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator


MIN_PREFIX = 2
MIN_SUFFIX = 2
MAX_WORD_CHARS = 70

# Symbol 30 means "other".
# Symbol 31 is reserved by the firmware runtime as an edge-padding symbol for
# the 3+3 context.  It is deliberately NOT used in ordinary 2+2 contexts.
SYMBOL_OTHER = 30
SYMBOL_PAD = 31

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

# The generator should preserve the existing 2+2 behavior unless explicitly
# changed later.  These defaults are the values that produced the original
# ~20.6 KB 2+2 payload.
PAIR_SUPPORT_DEFAULT = 50
PAIR_CONFIDENCE_DEFAULT = 0.99
CONTEXT_SUPPORT_DEFAULT = 3
CONTEXT_CONFIDENCE_DEFAULT = 0.99
ADD_SUPPORT_DEFAULT = 3
ADD_CONFIDENCE_DEFAULT = 0.999

# 3+3 is deliberately more conservative.  The selector will vary the number
# of emitted contexts, but these confidence floors stop clearly noisy contexts
# from entering the candidate pool.
BLOCK3_SUPPORT_DEFAULT = 2
BLOCK3_CONFIDENCE_DEFAULT = 0.90
ADD3_SUPPORT_DEFAULT = 2
ADD3_CONFIDENCE_DEFAULT = 0.90

# A 3+3 key occupies four bytes.
THREE_X_THREE_KEY_BYTES = 4
MAX_3X3_PAYLOAD_DEFAULT = 20_000


@dataclass(frozen=True)
class WordEntry:
    word: str
    cps: tuple[int, ...]
    legal: frozenset[int]
    preferred: frozenset[int]
    undesirable: frozenset[int]

    # DANTE diagnostic categories.  These are intentionally overlapping:
    # a boundary can carry more than one marker in the source.
    ordinary: frozenset[int]       # "-"
    compound: frozenset[int]       # "="
    prefix: frozenset[int]         # "<"
    suffix: frozenset[int]         # ">"
    uncategorized: frozenset[int]  # "·"

    bucket: int


def lower_latin(cp: int) -> int:
    """Mirror the lowercase mapping used by HyphenationCommon.cpp."""
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
    """Mirror the supported Latin-letter range in the firmware."""
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
        pair = (
            lower_latin(cps[pos]),
            lower_latin(cps[pos + 1]),
        )
        if pair in PROTECTED_DIPHTHONGS:
            return pos + 2

    return pos + 1


def consonant_unit_length(
    cps: tuple[int, ...],
    pos: int,
    end: int,
) -> int:
    if (
        pos + 2 < end
        and lower_latin(cps[pos]) == ord("s")
        and lower_latin(cps[pos + 1]) == ord("c")
        and lower_latin(cps[pos + 2]) == ord("h")
    ):
        return 3

    if pos + 1 < end:
        pair = (
            lower_latin(cps[pos]),
            lower_latin(cps[pos + 1]),
        )
        if pair in PROTECTED_CONSONANT_PAIRS:
            return 2

    return 1


def base_breaks(cps: tuple[int, ...]) -> set[int]:
    """
    Mirror markBaseSyllableBreaks() in GermanHybridHyphenator.cpp.

    This intentionally models only the small general German syllable rules.
    Morphological/lexical ambiguity is left to the learned correction tables.
    """
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
            # Two adjacent vowel nuclei: the break can occur between them.
            candidate = next_vowel
        else:
            # With multiple consonant units, only the final unit moves to the
            # following syllable.
            pos = left_nucleus_end
            last_unit_start = pos

            while pos < next_vowel:
                last_unit_start = pos
                pos += consonant_unit_length(cps, pos, next_vowel)

            candidate = last_unit_start

        if (
            candidate >= MIN_PREFIX
            and n - candidate >= MIN_SUFFIX
        ):
            result.add(candidate)

        left_nucleus_end = vowel_nucleus_end(cps, next_vowel)
        scan = left_nucleus_end

    return result


def german_symbol(cp: int) -> int:
    """
    Return the 5-bit symbol used by GermanHybridRules.cpp.

    0..25   a-z
    26..29  ä ö ü ß
    30      other
    31      reserved for 3+3 edge padding
    """
    cp = lower_latin(cp)

    if ord("a") <= cp <= ord("z"):
        return cp - ord("a")

    direct = {
        0x00E4: 26,  # ä
        0x00F6: 27,  # ö
        0x00FC: 28,  # ü
        0x00DF: 29,  # ß
    }

    if cp in direct:
        return direct[cp]

    # Fold common accented vowels in foreign vocabulary to ASCII vowels.
    fold = {
        0x00E0: "a",
        0x00E1: "a",
        0x00E2: "a",
        0x00E3: "a",
        0x00E5: "a",
        0x00E8: "e",
        0x00E9: "e",
        0x00EA: "e",
        0x00EB: "e",
        0x00EC: "i",
        0x00ED: "i",
        0x00EE: "i",
        0x00EF: "i",
        0x00F2: "o",
        0x00F3: "o",
        0x00F4: "o",
        0x00F5: "o",
        0x00F9: "u",
        0x00FA: "u",
        0x00FB: "u",
        0x00FD: "y",
        0x00FF: "y",
    }

    folded = fold.get(cp)
    return ord(folded) - ord("a") if folded is not None else SYMBOL_OTHER


def pair_key(cps: tuple[int, ...], boundary: int) -> int | None:
    left = german_symbol(cps[boundary - 1])
    right = german_symbol(cps[boundary])

    if left == SYMBOL_OTHER or right == SYMBOL_OTHER:
        return None

    return (left << 5) | right


def context_key(cps: tuple[int, ...], boundary: int) -> int | None:
    """Pack two codepoints left + two right into 20 bits."""
    if boundary < 2 or boundary + 1 >= len(cps):
        return None

    symbols = tuple(
        german_symbol(cps[i])
        for i in range(boundary - 2, boundary + 2)
    )

    if SYMBOL_OTHER in symbols:
        return None

    key = 0
    for symbol in symbols:
        key = (key << 5) | symbol

    return key


def context_key3(cps: tuple[int, ...], boundary: int) -> int | None:
    """
    Pack three codepoints left + three right into 30 bits.

    The firmware uses symbol 31 as an edge pad, so the first/last legal
    boundaries are still representable.  SYMBOL_OTHER deliberately remains
    a hard "do not learn this context" value.
    """
    if boundary <= 0 or boundary >= len(cps):
        return None

    symbols: list[int] = []

    for i in range(boundary - 3, boundary + 3):
        if i < 0 or i >= len(cps):
            symbol = SYMBOL_PAD
        else:
            symbol = german_symbol(cps[i])

        if symbol == SYMBOL_OTHER:
            return None

        symbols.append(symbol)

    key = 0
    for symbol in symbols:
        key = (key << 5) | symbol

    return key


def split_bucket(word: str) -> int:
    """Deterministic train/validation/test split."""
    return hashlib.sha1(
        word.casefold().encode("utf-8")
    ).digest()[0] % 10


def parse_annotation(
    annotation: str,
) -> tuple[str, dict[int, str]] | None:
    """
    Remove DANTE markers while remembering the marker(s) at each boundary.

    Version 1 intentionally skips alternative/special entries containing
    braces, brackets or slash alternatives because those cannot safely be
    reduced to one single preferred spelling.
    """
    if any(ch in annotation for ch in "{}[]/"):
        return None

    plain: list[str] = []
    markers: dict[int, str] = {}
    position = 0
    i = 0

    while i < len(annotation):
        if annotation[i] in MARKER_CHARS:
            start = i

            while (
                i < len(annotation)
                and annotation[i] in MARKER_CHARS
            ):
                i += 1

            markers[position] = (
                markers.get(position, "")
                + annotation[start:i]
            )
            continue

        plain.append(annotation[i])
        position += 1
        i += 1

    return "".join(plain), markers


def select_reformed_field(fields: list[str]) -> str | None:
    """
    DANTE README.wortliste:
      field 2 when all orthographies agree,
      otherwise field 4 for the reformed/current spelling.
    """
    if (
        len(fields) >= 2
        and fields[1]
        and fields[1] != "-2-"
    ):
        return fields[1]

    if (
        len(fields) >= 4
        and fields[3]
        and fields[3] != "-4-"
    ):
        return fields[3]

    return None


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

    if not (
        MIN_PREFIX + MIN_SUFFIX
        <= len(cps)
        <= MAX_WORD_CHARS
    ):
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
        if (
            boundary < MIN_PREFIX
            or len(cps) - boundary < MIN_SUFFIX
        ):
            continue

        if not any(
            ch in BREAK_MARKER_CHARS
            for ch in marker
        ):
            continue

        # DANTE "." means a possible-looking but undesirable/meaning-changing
        # split.  Keep it separately; it is not a legal positive break.
        if "." in marker:
            undesirable.add(boundary)
            continue

        legal.add(boundary)

        # These categories intentionally overlap when DANTE uses mixed
        # markers such as "<=".
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

        if any(
            ch in MORPHEME_MARKER_CHARS
            for ch in marker
        ):
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


def select_keys(
    stats: dict[int, list[int]],
    support: int,
    confidence: float,
) -> set[int]:
    return {
        key
        for key, (good, total) in stats.items()
        if total >= support
        and good / total >= confidence
    }


def build_base_tables(
    source: Path,
    args: argparse.Namespace,
) -> tuple[set[int], set[int], set[int]]:
    """
    Build the original 2+2 data path.

    This section is intentionally kept structurally close to the known-good
    2+2 generator.  We do NOT want adding 3+3 to silently change the baseline
    payload or baseline quality.
    """
    pair_stats: dict[int, list[int]] = defaultdict(
        lambda: [0, 0]
    )
    add_stats: dict[int, list[int]] = defaultdict(
        lambda: [0, 0]
    )
    context_stats: dict[int, list[int]] = defaultdict(
        lambda: [0, 0]
    )

    train_entries = [
        entry
        for entry in entries(source)
        if entry.bucket <= 7
    ]

    # Pass 1: gather pair and possible-add statistics.
    for entry in train_entries:
        base = base_breaks(entry.cps)

        for boundary in range(
            MIN_PREFIX,
            len(entry.cps) - MIN_SUFFIX + 1,
        ):
            if boundary in base:
                key = pair_key(entry.cps, boundary)

                if key is not None:
                    pair_stats[key][1] += 1
                    if boundary in entry.legal:
                        pair_stats[key][0] += 1
            else:
                key = context_key(entry.cps, boundary)

                if key is not None:
                    add_stats[key][1] += 1
                    if boundary in entry.legal:
                        add_stats[key][0] += 1

    safe_pairs = select_keys(
        pair_stats,
        args.pair_support,
        args.pair_confidence,
    )

    # Pass 2: learn 2+2 validation only for base candidates that are not
    # already covered by the immediate pair class.
    for entry in train_entries:
        base = base_breaks(entry.cps)

        for boundary in base:
            pair = pair_key(entry.cps, boundary)

            if pair is not None and pair in safe_pairs:
                continue

            key = context_key(entry.cps, boundary)

            if key is None:
                continue

            context_stats[key][1] += 1
            if boundary in entry.legal:
                context_stats[key][0] += 1

    safe_contexts = select_keys(
        context_stats,
        args.context_support,
        args.context_confidence,
    )

    add_contexts = select_keys(
        add_stats,
        args.add_support,
        args.add_confidence,
    )

    return safe_pairs, safe_contexts, add_contexts


def apply_base(
    entry: WordEntry,
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
) -> set[int]:
    """
    Apply ONLY the established 2+2 hybrid layer.

    This function is also the baseline used to train the 3+3 correction
    tables, which lets us measure the incremental value of 3+3 independently.
    """
    base = base_breaks(entry.cps)
    result: set[int] = set()

    for boundary in range(
        MIN_PREFIX,
        len(entry.cps) - MIN_SUFFIX + 1,
    ):
        context = context_key(entry.cps, boundary)

        if boundary in base:
            pair = pair_key(entry.cps, boundary)

            if (
                pair is not None
                and pair in safe_pairs
            ) or (
                context is not None
                and context in safe_contexts
            ):
                result.add(boundary)

        elif (
            context is not None
            and context in add_contexts
        ):
            result.add(boundary)

    return result


def collect_3x3_candidates(
    train_entries: list[WordEntry],
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
) -> tuple[
    dict[int, list[int]],
    dict[int, list[int]],
]:
    """
    Learn residual 3+3 contexts from the already-established 2+2 baseline.

    BLOCK:
      baseline produces a break, but DANTE says it is illegal.

    ADD:
      baseline does not produce a break, but DANTE says it is legal.

    Statistics are [positive_count, total_count].
    """
    block_stats: dict[int, list[int]] = defaultdict(
        lambda: [0, 0]
    )
    add_stats: dict[int, list[int]] = defaultdict(
        lambda: [0, 0]
    )

    for entry in train_entries:
        baseline = apply_base(
            entry,
            safe_pairs,
            safe_contexts,
            add_contexts,
        )

    for boundary in range(
        MIN_PREFIX,
        len(entry.cps) - MIN_SUFFIX + 1,
    ):
        key = context_key3(entry.cps, boundary)
        if key is None:
            continue

        if boundary in baseline:
            # BLOCK:
            # baseline proposes a break. Positive means DANTE says that
            # break is illegal.
            block_stats[key][1] += 1

            if boundary not in entry.legal:
                block_stats[key][0] += 1

        else:
            # ADD:
            # baseline does not propose a break. Every occurrence counts
            # toward the denominator; only legal DANTE boundaries are positive.
            add_stats[key][1] += 1

            if boundary in entry.legal:
                add_stats[key][0] += 1

    return block_stats, add_stats


def rank_block(
    stats: dict[int, list[int]],
    min_support: int,
    min_confidence: float,
) -> list[tuple[int, int, int, float]]:
    """
    Rank BLOCK contexts.

    We prefer high confidence first, then the number of corrected training
    errors.  A context that fixes 200 errors but is 99.0% reliable is less
    attractive than one that fixes 50 errors at 100%.
    """
    ranked = []

    for key, (bad, total) in stats.items():
        if total < min_support:
            continue

        confidence = bad / total
        if confidence < min_confidence:
            continue

        ranked.append((key, bad, total, confidence))

    return sorted(
        ranked,
        key=lambda item: (
            item[1] * item[3], # confidence x corrected errors
            item[3], # confidence
            item[2], # support
        ),
        reverse=True,
    )


def rank_add(
    stats: dict[int, list[int]],
    min_support: int,
    min_confidence: float,
) -> list[tuple[int, int, int, float]]:
    """
    Rank ADD contexts by confidence first, then useful occurrence count.
    """
    ranked = []

    for key, (good, total) in stats.items():
        if total < min_support:
            continue

        confidence = good / total
        if confidence < min_confidence:
            continue

        ranked.append((key, good, total, confidence))

    return sorted(
        ranked,
        key=lambda item: (
            item[1] * item[3], # confidence x useful recoveries
            item[3], # confidence
            item[2], # support
        ),
        reverse=True,
    )


def apply_3x3(
    entry: WordEntry,
    baseline: set[int],
    block: set[int],
    add: set[int],
) -> set[int]:
    """
    Apply the residual 3+3 layer.

    BLOCK always wins over ADD.
    """
    result = set(baseline)

    for boundary in range(
        MIN_PREFIX,
        len(entry.cps) - MIN_SUFFIX + 1,
    ):
        key = context_key3(entry.cps, boundary)

        if key is None:
            continue

        if key in block:
            result.discard(boundary)
            continue

        if boundary not in result and key in add:
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


def add_metrics(
    metrics: dict[str, int],
    entry: WordEntry,
    actual: set[int],
) -> None:
    metrics["words"] += 1
    metrics["tp"] += len(actual & entry.legal)
    metrics["fp"] += len(actual - entry.legal)
    metrics["fn"] += len(entry.legal - actual)
    metrics["exact"] += int(actual == entry.legal)

    metrics["preferred_found"] += len(
        actual & entry.preferred
    )
    metrics["preferred_total"] += len(entry.preferred)

    metrics["undesirable_used"] += len(
        actual & entry.undesirable
    )


def finalize_metrics(
    metrics: dict[str, int],
) -> dict[str, float | int]:
    """
    Compute every metric from the same TP/FP/FN counters.

    The previous generator accidentally returned a separate false-positive
    field that always stayed zero.  This version intentionally uses metrics
    ["fp"] as the single source of truth and asserts the arithmetic.
    """
    tp = metrics["tp"]
    fp = metrics["fp"]
    fn = metrics["fn"]
    words = metrics["words"]

    precision = (
        tp / (tp + fp)
        if tp + fp
        else 1.0
    )
    recall = (
        tp / (tp + fn)
        if tp + fn
        else 1.0
    )
    f1 = (
        2.0 * precision * recall / (precision + recall)
        if precision + recall
        else 0.0
    )

    exact = (
        metrics["exact"] / words
        if words
        else 0.0
    )

    preferred_recall = (
        metrics["preferred_found"]
        / metrics["preferred_total"]
        if metrics["preferred_total"]
        else 1.0
    )

    # Internal consistency checks.  If these fail, the generator must stop
    # rather than print a plausible-looking but contradictory summary.
    calculated_precision = (
        tp / (tp + fp)
        if tp + fp
        else 1.0
    )
    calculated_recall = (
        tp / (tp + fn)
        if tp + fn
        else 1.0
    )

    assert abs(precision - calculated_precision) < 1e-12
    assert abs(recall - calculated_recall) < 1e-12

    return {
        "words": words,
        "true_positives": tp,
        "false_positives": fp,
        "false_negatives": fn,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "exact": exact,
        "preferred_recall": preferred_recall,
        "undesirable_used": metrics["undesirable_used"],
    }


def evaluate(
    cases: Iterable[WordEntry],
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
    block: set[int],
    add: set[int],
) -> dict[str, float | int]:
    metrics = new_metrics()

    for entry in cases:
        baseline = apply_base(
            entry,
            safe_pairs,
            safe_contexts,
            add_contexts,
        )

        actual = apply_3x3(
            entry,
            baseline,
            block,
            add,
        )

        add_metrics(metrics, entry, actual)

    return finalize_metrics(metrics)


def build_pair_bitset(keys: set[int]) -> bytes:
    """
    The pair alphabet has 32 * 32 = 1024 possible pairs.
    """
    data = bytearray(128)

    for key in keys:
        data[key >> 3] |= 1 << (key & 7)

    return bytes(data)


def pack_20bit_keys(keys: set[int]) -> bytes:
    """Three bytes per 20-bit 2+2 key."""
    data = bytearray()

    for key in sorted(keys):
        data.extend(
            (
                key & 0xFF,
                (key >> 8) & 0xFF,
                (key >> 16) & 0x0F,
            )
        )

    return bytes(data)


def pack_30bit_keys(keys: set[int]) -> bytes:
    """Four bytes per 30-bit 3+3 key."""
    data = bytearray()

    for key in sorted(keys):
        data.extend(
            (
                key & 0xFF,
                (key >> 8) & 0xFF,
                (key >> 16) & 0xFF,
                (key >> 24) & 0x3F,
            )
        )

    return bytes(data)


def format_bytes(
    blob: bytes,
    per_line: int = 16,
) -> str:
    if not blob:
        return ""

    lines = []

    for start in range(0, len(blob), per_line):
        chunk = ", ".join(
            f"0x{value:02X}"
            for value in blob[start : start + per_line]
        )
        lines.append(f"    {chunk},")

    return "\n".join(lines)


def write_header(
    output: Path,
    source: Path,
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
    block: set[int],
    add: set[int],
) -> None:
    """
    Emit the firmware data.

    The 3+3 macro is emitted only when at least one 3+3 table is non-empty.
    This is important because GermanHybridRules.cpp feature-gates the 3+3
    code on GERMAN_HYBRID_HAS_3X3.
    """
    source_sha = hashlib.sha256(
        source.read_bytes()
    ).hexdigest()

    pair_blob = build_pair_bitset(safe_pairs)
    safe_blob = pack_20bit_keys(safe_contexts)
    add_blob = pack_20bit_keys(add_contexts)
    block_blob = pack_30bit_keys(block)
    add3_blob = pack_30bit_keys(add)

    has_3x3 = bool(block or add)

    text = f"""#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Generated file. Do not edit manually.
// Source DANTE wortliste SHA-256: {source_sha}
"""

    if has_3x3:
        text += "\n#define GERMAN_HYBRID_HAS_3X3 1\n"

    text += f"""
inline constexpr std::array<uint8_t, {len(pair_blob)}>
kGermanSafePairBits = {{
{format_bytes(pair_blob)}
}};

inline constexpr std::array<uint8_t, {len(safe_blob)}>
kGermanSafeContexts = {{
{format_bytes(safe_blob)}
}};

inline constexpr size_t
kGermanSafeContextCount = {len(safe_contexts)};

inline constexpr std::array<uint8_t, {len(add_blob)}>
kGermanAddContexts = {{
{format_bytes(add_blob)}
}};

inline constexpr size_t
kGermanAddContextCount = {len(add_contexts)};
"""

    if has_3x3:
        text += f"""
inline constexpr std::array<uint8_t, {len(block_blob)}>
kGermanBlockContexts3 = {{
{format_bytes(block_blob)}
}};

inline constexpr size_t
kGermanBlockContext3Count = {len(block)};

inline constexpr std::array<uint8_t, {len(add3_blob)}>
kGermanAddContexts3 = {{
{format_bytes(add3_blob)}
}};

inline constexpr size_t
kGermanAddContext3Count = {len(add)};
"""

    output.parent.mkdir(
        parents=True,
        exist_ok=True,
    )
    output.write_text(
        text,
        encoding="utf-8",
    )


def position_string(values: frozenset[int]) -> str:
    return ",".join(
        str(value)
        for value in sorted(values)
    )


def write_fixture(
    output: Path,
    cases: list[WordEntry],
) -> None:
    """
    Write the deterministic 5,000-word held-out fixture.

    The SHA-1 selection here is deliberately independent from the train/valid
    bucket calculation; it only makes the 5,000 rows stable within bucket 9.
    """
    output.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    selected = sorted(
        cases,
        key=lambda entry: hashlib.sha1(
            (
                "eval:"
                + entry.word.casefold()
            ).encode("utf-8")
        ).digest(),
    )[:5000]

    with output.open(
        "w",
        encoding="utf-8",
    ) as file:
        file.write(
            "# Format: "
            "word|legal|preferred|undesirable|"
            "ordinary|compound|prefix|suffix|uncategorized\n"
        )

        for entry in selected:
            file.write(
                f"{entry.word}|"
                f"{position_string(entry.legal)}|"
                f"{position_string(entry.preferred)}|"
                f"{position_string(entry.undesirable)}|"
                f"{position_string(entry.ordinary)}|"
                f"{position_string(entry.compound)}|"
                f"{position_string(entry.prefix)}|"
                f"{position_string(entry.suffix)}|"
                f"{position_string(entry.uncategorized)}\n"
            )


def choose_profile(
    validation: list[WordEntry],
    baseline_validation: dict[str, float | int],
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
    block_stats: dict[int, list[int]],
    add_stats: dict[int, list[int]],
    args: argparse.Namespace,
) -> tuple[set[int], set[int], dict[str, float | int], str]:
    """
    Select the best 3+3 profile under a hard byte budget.

    The previous selector always filled the 20 KB budget and therefore pushed
    precision down to ~98.8%.  This selector varies both the number of BLOCK
    contexts and the number of ADD contexts and prefers the smallest profile
    that achieves the requested safety target before considering recall.

    Search is intentionally coarse.  The generated data is tiny and this
    avoids a complicated optimizer that would make the build process difficult
    to understand or reproduce.
    """
    max_entries = max(
        0,
        args.max_3x3_payload // THREE_X_THREE_KEY_BYTES,
    )

    if max_entries == 0:
        empty = evaluate(
            validation,
            safe_pairs,
            safe_contexts,
            add_contexts,
            set(),
            set(),
        )
        return set(), set(), empty, "baseline"

    block_ranked = rank_block(
        block_stats,
        args.block3_support,
        args.block3_confidence,
    )

    add_ranked = rank_add(
        add_stats,
        args.add3_support,
        args.add3_confidence,
    )

    # Several candidate confidence pairs.  Higher values produce fewer
    # candidates; lower values recover more recall but may be noisier.
    confidence_profiles = (
        (1.0000, 1.0000),
        (0.9999, 0.9999),
        (0.9995, 0.9995),
        (0.9990, 0.9990),
    )

    # Try a small set of BLOCK allocations.  BLOCKs are valuable for safety,
    # but we do not want them to consume the whole flash budget.
    block_limits = (
        0,
        min(25, max_entries),
        min(50, max_entries),
        min(100, max_entries),
        min(200, max_entries),
    )

    # ADD counts are deliberately varied.  This is the important difference
    # from the previous selector, which always took exactly 4,900 ADD entries.
    add_counts = sorted(
        {
            0,
            250,
            500,
            750,
            1000,
            1500,
            2000,
            2500,
            3000,
            3500,
            4000,
            max_entries,
        }
    )

    profiles: list[
        tuple[
            float,
            float,
            int,
            int,
            set[int],
            set[int],
            dict[str, float | int],
            str,
        ]
    ] = []

    for block_conf, add_conf in confidence_profiles:
        blocks_ranked = rank_block(
            block_stats,
            max(args.block3_support, 2),
            block_conf,
        )
        adds_ranked = rank_add(
            add_stats,
            max(args.add3_support, 2),
            add_conf,
        )

        for block_limit in block_limits:
            selected_block_rows = blocks_ranked[:block_limit]
            selected_block = {
                row[0]
                for row in selected_block_rows
            }

            remaining = max_entries - len(
                selected_block
            )

            for add_count in add_counts:
                actual_add_count = min(
                    add_count,
                    remaining,
                    len(adds_ranked),
                )

                selected_add_rows = adds_ranked[
                    :actual_add_count
                ]
                selected_add = {
                    row[0]
                    for row in selected_add_rows
                }

                metrics = evaluate(
                    validation,
                    safe_pairs,
                    safe_contexts,
                    add_contexts,
                    selected_block,
                    selected_add,
                )

                payload = (
                    THREE_X_THREE_KEY_BYTES
                    * (
                        len(selected_block)
                        + len(selected_add)
                    )
                )

                if payload > args.max_3x3_payload:
                    continue

                profiles.append(
                    (
                        float(metrics["precision"]),
                        float(metrics["recall"]),
                        int(metrics["undesirable_used"]),
                        payload,
                        selected_block,
                        selected_add,
                        metrics,
                        (
                            f"block_conf={block_conf:.4f},"
                            f"add_conf={add_conf:.4f},"
                            f"block={len(selected_block)},"
                            f"add={len(selected_add)}"
                        ),
                    )
                )

    if not profiles:
        baseline = baseline_validation
        return (
            set(),
            set(),
            baseline,
            "baseline-no-profile",
        )

    # An undesirable break already present in the 2+2 baseline should not be
    # made worse by 3+3 unless the user explicitly permits that delta.
    max_undesirable = (
        int(baseline_validation["undesirable_used"])
        + args.max_3x3_undesirable_delta
    )

    safe_profiles = [
        profile
        for profile in profiles
        if profile[0] >= args.target_precision
        and profile[2] <= max_undesirable
    ]

    if safe_profiles:
        # First maximize recall.  For equal recall choose higher precision,
        # then a smaller payload.
        chosen = max(
            safe_profiles,
            key=lambda profile: (
                profile[1],
                profile[0],
                -profile[3],
            ),
        )
        return (
            chosen[4],
            chosen[5],
            chosen[6],
            chosen[7],
        )

    # No profile met the hard safety constraints.  Select the best VALID
    # profile, but never pretend that its constraints were satisfied.
    # Prefer precision first, then fewer undesirable breaks, then recall,
    # then smaller payload.
    chosen = max(
        profiles,
        key=lambda profile: (
            profile[0],
            -profile[2],
            profile[1],
            -profile[3],
        ),
    )

    return (
        chosen[4],
        chosen[5],
        chosen[6],
        "FALLBACK " + chosen[7],
    )


def read_bucket(
    source: Path,
    bucket: int,
) -> list[WordEntry]:
    return [
        entry
        for entry in entries(source)
        if entry.bucket == bucket
    ]


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Generate compact German hybrid "
            "hyphenation rules from DANTE."
        )
    )

    parser.add_argument(
        "--input",
        required=True,
        type=Path,
    )
    parser.add_argument(
        "--output-header",
        required=True,
        type=Path,
    )
    parser.add_argument(
        "--output-eval",
        required=True,
        type=Path,
    )

    # Keep these defaults compatible with the established 2+2 generator.
    parser.add_argument(
        "--pair-support",
        type=int,
        default=PAIR_SUPPORT_DEFAULT,
    )
    parser.add_argument(
        "--pair-confidence",
        type=float,
        default=PAIR_CONFIDENCE_DEFAULT,
    )
    parser.add_argument(
        "--context-support",
        type=int,
        default=CONTEXT_SUPPORT_DEFAULT,
    )
    parser.add_argument(
        "--context-confidence",
        type=float,
        default=CONTEXT_CONFIDENCE_DEFAULT,
    )
    parser.add_argument(
        "--add-support",
        type=int,
        default=ADD_SUPPORT_DEFAULT,
    )
    parser.add_argument(
        "--add-confidence",
        type=float,
        default=ADD_CONFIDENCE_DEFAULT,
    )

    # 3+3 safety thresholds for candidate generation.
    parser.add_argument(
        "--block3-support",
        type=int,
        default=BLOCK3_SUPPORT_DEFAULT,
        help=(
            "Minimum training support for a 3+3 BLOCK context."
        ),
    )
    parser.add_argument(
        "--block3-confidence",
        type=float,
        default=BLOCK3_CONFIDENCE_DEFAULT,
        help=(
            "Minimum illegal-break confidence for a 3+3 BLOCK."
        ),
    )
    parser.add_argument(
        "--add3-support",
        type=int,
        default=ADD3_SUPPORT_DEFAULT,
        help=(
            "Minimum training support for a 3+3 ADD context."
        ),
    )
    parser.add_argument(
        "--add3-confidence",
        type=float,
        default=ADD3_CONFIDENCE_DEFAULT,
        help=(
            "Minimum legal-break confidence for a 3+3 ADD."
        ),
    )

    parser.add_argument(
        "--max-3x3-payload",
        type=int,
        default=MAX_3X3_PAYLOAD_DEFAULT,
        help=(
            "Hard maximum number of bytes spent on 3+3 "
            "tables. Four bytes per key."
        ),
    )
    parser.add_argument(
        "--target-precision",
        type=float,
        default=0.997,
        help=(
            "Minimum validation precision required for a "
            "profile to be considered safe."
        ),
    )
    parser.add_argument(
        "--max-3x3-undesirable-delta",
        type=int,
        default=0,
        help=(
            "Maximum increase in DANTE-undesirable breaks "
            "relative to the 2+2 baseline."
        ),
    )

    args = parser.parse_args()

    if args.max_3x3_payload < 0:
        parser.error("--max-3x3-payload must be >= 0")

    train = [
        entry
        for entry in entries(args.input)
        if entry.bucket <= 7
    ]
    validation = read_bucket(args.input, 8)
    test = read_bucket(args.input, 9)

    safe_pairs, safe_contexts, add_contexts = (
        build_base_tables(
            args.input,
            args,
        )
    )

    baseline_validation = evaluate(
        validation,
        safe_pairs,
        safe_contexts,
        add_contexts,
        set(),
        set(),
    )

    baseline_test = evaluate(
        test,
        safe_pairs,
        safe_contexts,
        add_contexts,
        set(),
        set(),
    )

    block_stats, add_stats = collect_3x3_candidates(
        train,
        safe_pairs,
        safe_contexts,
        add_contexts,
    )

    block, add, selected_validation, profile_name = (
        choose_profile(
            validation=validation,
            baseline_validation=baseline_validation,
            safe_pairs=safe_pairs,
            safe_contexts=safe_contexts,
            add_contexts=add_contexts,
            block_stats=block_stats,
            add_stats=add_stats,
            args=args,
        )
    )

    # If the selector chose a real 3+3 profile, use it for the final test.
    # Otherwise the test below is simply the 2+2 baseline.
    selected_test = evaluate(
        test,
        safe_pairs,
        safe_contexts,
        add_contexts,
        block,
        add,
    )

    baseline_payload = (
        128
        + 3 * (
            len(safe_contexts)
            + len(add_contexts)
        )
    )

    three_x_three_payload = (
        THREE_X_THREE_KEY_BYTES
        * (len(block) + len(add))
    )

    total_payload = (
        baseline_payload
        + three_x_three_payload
    )

    print(
        f"safe pair classes: "
        f"{len(safe_pairs)} / 1024"
    )
    print(
        f"safe 2+2 contexts: "
        f"{len(safe_contexts)}"
    )
    print(
        f"add 2+2 contexts:  "
        f"{len(add_contexts)}"
    )

    print(
        f"3+3 candidates: "
        f"block={len(block_stats)} "
        f"add={len(add_stats)}"
    )

    print(
        f"selected 3+3: "
        f"block={len(block)} "
        f"add={len(add)}"
    )

    print(
        f"3+3 payload: "
        f"{three_x_three_payload} bytes / "
        f"{args.max_3x3_payload} max"
    )

    print(
        f"generated payload: "
        f"{total_payload} bytes "
        f"(2+2={baseline_payload}, "
        f"3+3={three_x_three_payload})"
    )

    print(
        f"baseline-validation: "
        f"words={baseline_validation['words']} "
        f"precision="
        f"{baseline_validation['precision'] * 100:.3f}% "
        f"recall="
        f"{baseline_validation['recall'] * 100:.3f}% "
        f"F1="
        f"{baseline_validation['f1'] * 100:.3f}% "
        f"undesirable-used="
        f"{baseline_validation['undesirable_used']} "
        f"false-positives="
        f"{baseline_validation['false_positives']}"
    )

    print(
        f"selected profile: {profile_name}"
    )

    print(
        f"validation-selected: "
        f"words={selected_validation['words']} "
        f"precision="
        f"{selected_validation['precision'] * 100:.3f}% "
        f"recall="
        f"{selected_validation['recall'] * 100:.3f}% "
        f"F1="
        f"{selected_validation['f1'] * 100:.3f}% "
        f"exact="
        f"{selected_validation['exact'] * 100:.3f}% "
        f"preferred-recall="
        f"{selected_validation['preferred_recall'] * 100:.3f}% "
        f"undesirable-used="
        f"{selected_validation['undesirable_used']} "
        f"false-positives="
        f"{selected_validation['false_positives']}"
    )

    print(
        f"baseline-test: "
        f"words={baseline_test['words']} "
        f"precision="
        f"{baseline_test['precision'] * 100:.3f}% "
        f"recall="
        f"{baseline_test['recall'] * 100:.3f}% "
        f"undesirable-used="
        f"{baseline_test['undesirable_used']} "
        f"false-positives="
        f"{baseline_test['false_positives']}"
    )

    print(
        f"test: "
        f"words={selected_test['words']} "
        f"precision="
        f"{selected_test['precision'] * 100:.3f}% "
        f"recall="
        f"{selected_test['recall'] * 100:.3f}% "
        f"F1="
        f"{selected_test['f1'] * 100:.3f}% "
        f"exact="
        f"{selected_test['exact'] * 100:.3f}% "
        f"preferred-recall="
        f"{selected_test['preferred_recall'] * 100:.3f}% "
        f"undesirable-used="
        f"{selected_test['undesirable_used']} "
        f"false-positives="
        f"{selected_test['false_positives']}"
    )

    write_header(
        args.output_header,
        args.input,
        safe_pairs,
        safe_contexts,
        add_contexts,
        block,
        add,
    )

    write_fixture(
        args.output_eval,
        test,
    )


if __name__ == "__main__":
    main()
