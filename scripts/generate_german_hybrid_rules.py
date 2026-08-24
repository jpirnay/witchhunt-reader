#!/usr/bin/env python3
"""
Generate compact German hybrid-hyphenation data for Witch Reader.

Architecture
------------

The runtime does not store the complete German DANTE / Liang pattern trie.
Instead:

  1. A small deterministic German syllable-rule engine proposes candidates.
  2. Compact 2+2 context data validates/adds high-confidence breaks.
  3. An optional 3+3 residual layer adds/removes difficult cases.

This generator is deliberately optimized for reproducibility and build speed.

Data split
----------

  buckets 0..6 : training / candidate discovery
  bucket 7      : development / profile selection
  bucket 8      : independent validation / safety gate
  bucket 9      : final held-out test

The 3+3 selector searches a small precision/recall curve instead of running
hundreds of complete validation passes.  All base/2+2 results are cached.

The 3+3 flash budget is hard:
  4 bytes per 3+3 context
  20,000 bytes == 5,000 contexts

The generated header defines GERMAN_HYBRID_HAS_3X3 only when one or more
3+3 entries are emitted.
"""

from __future__ import annotations

import argparse
import hashlib
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


MIN_PREFIX = 2
MIN_SUFFIX = 2
MAX_WORD_CHARS = 70

SYMBOL_OTHER = 30
SYMBOL_PAD = 31

MARKER_CHARS = frozenset("-=<>·.")
BREAK_MARKER_CHARS = frozenset("-=<>·")
MORPHEME_MARKER_CHARS = frozenset("=<>")

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

# Existing 2+2 baseline settings. Keep these stable.
PAIR_SUPPORT_DEFAULT = 50
PAIR_CONFIDENCE_DEFAULT = 0.99
CONTEXT_SUPPORT_DEFAULT = 3
CONTEXT_CONFIDENCE_DEFAULT = 0.99
ADD_SUPPORT_DEFAULT = 3
ADD_CONFIDENCE_DEFAULT = 0.999

# Broad candidate admission thresholds. These are NOT production safety
# thresholds. The actual selection is evaluated on buckets 7 and 8.
BLOCK3_SUPPORT_DEFAULT = 2
BLOCK3_CONFIDENCE_DEFAULT = 0.90
ADD3_SUPPORT_DEFAULT = 2
ADD3_CONFIDENCE_DEFAULT = 0.90

MAX_3X3_PAYLOAD_DEFAULT = 20_000
THREE_X_THREE_KEY_BYTES = 4

# German morphology component list. These are learned from DANTE compound (=)
# boundaries, not hard-coded word exceptions. Runtime uses them only to prefer
# an already accepted compound boundary when a candidate is one or two
# characters away from that boundary.
MAX_MORPH_COMPONENTS_DEFAULT = 128
MIN_MORPH_COMPONENT_SUPPORT_DEFAULT = 15
MIN_MORPH_COMPONENT_LENGTH = 4
MAX_MORPH_COMPONENT_LENGTH = 18


@dataclass(frozen=True)
class WordEntry:
    word: str
    cps: tuple[int, ...]
    legal: frozenset[int]
    preferred: frozenset[int]
    undesirable: frozenset[int]
    ordinary: frozenset[int]
    compound: frozenset[int]
    prefix: frozenset[int]
    suffix: frozenset[int]
    uncategorized: frozenset[int]
    bucket: int


@dataclass
class AnalyzedEntry:
    entry: WordEntry
    base: frozenset[int]
    baseline: frozenset[int]


@dataclass(frozen=True)
class Metrics:
    words: int
    tp: int
    fp: int
    fn: int
    exact: int
    preferred_found: int
    preferred_total: int
    undesirable_used: int

    @property
    def precision(self) -> float:
        d = self.tp + self.fp
        return self.tp / d if d else 1.0

    @property
    def recall(self) -> float:
        d = self.tp + self.fn
        return self.tp / d if d else 1.0

    @property
    def f1(self) -> float:
        p = self.precision
        r = self.recall
        return 2.0 * p * r / (p + r) if p + r else 0.0

    @property
    def exact_rate(self) -> float:
        return self.exact / self.words if self.words else 0.0

    @property
    def preferred_recall(self) -> float:
        return (
            self.preferred_found / self.preferred_total
            if self.preferred_total else 1.0
        )

    def add(self, other: "Metrics") -> "Metrics":
        return Metrics(
            words=self.words + other.words,
            tp=self.tp + other.tp,
            fp=self.fp + other.fp,
            fn=self.fn + other.fn,
            exact=self.exact + other.exact,
            preferred_found=self.preferred_found + other.preferred_found,
            preferred_total=self.preferred_total + other.preferred_total,
            undesirable_used=self.undesirable_used + other.undesirable_used,
        )

    def delta(
        self,
        dtp: int,
        dfp: int,
        dundesirable: int,
    ) -> "Metrics":
        return Metrics(
            words=self.words,
            tp=self.tp + dtp,
            fp=self.fp + dfp,
            fn=self.fn - dtp,
            exact=self.exact,  # exact is recomputed for final selections
            preferred_found=self.preferred_found,
            preferred_total=self.preferred_total,
            undesirable_used=self.undesirable_used + dundesirable,
        )


@dataclass(frozen=True)
class CandidateImpact:
    key: int
    dtp: int
    dfp: int
    dundesirable: int
    occurrences: int


def lower_latin(cp: int) -> int:
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
        if (
            lower_latin(cps[pos]),
            lower_latin(cps[pos + 1]),
        ) in PROTECTED_DIPHTHONGS:
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

    if (
        pos + 1 < end
        and (
            lower_latin(cps[pos]),
            lower_latin(cps[pos + 1]),
        ) in PROTECTED_CONSONANT_PAIRS
    ):
        return 2

    return 1


def base_breaks(cps: tuple[int, ...]) -> frozenset[int]:
    """Implement the small deterministic German syllable-rule layer."""
    n = len(cps)

    if n < MIN_PREFIX + MIN_SUFFIX or n > MAX_WORD_CHARS:
        return frozenset()
    if not all(is_latin_letter(cp) for cp in cps):
        return frozenset()

    first_vowel = 0
    while first_vowel < n and not is_vowel(cps[first_vowel]):
        first_vowel += 1

    if first_vowel == n:
        return frozenset()

    result: set[int] = set()
    left_end = vowel_nucleus_end(cps, first_vowel)
    scan = left_end

    while scan < n:
        next_vowel = scan
        while next_vowel < n and not is_vowel(cps[next_vowel]):
            next_vowel += 1

        if next_vowel == n:
            break

        if next_vowel == left_end:
            candidate = next_vowel
        else:
            pos = left_end
            last_unit_start = pos
            while pos < next_vowel:
                last_unit_start = pos
                pos += consonant_unit_length(
                    cps, pos, next_vowel
                )
            candidate = last_unit_start

        if candidate >= MIN_PREFIX and n - candidate >= MIN_SUFFIX:
            result.add(candidate)

        left_end = vowel_nucleus_end(cps, next_vowel)
        scan = left_end

    return frozenset(result)


def german_symbol(cp: int) -> int:
    cp = lower_latin(cp)

    if ord("a") <= cp <= ord("z"):
        return cp - ord("a")

    direct = {
        0x00E4: 26,
        0x00F6: 27,
        0x00FC: 28,
        0x00DF: 29,
    }
    if cp in direct:
        return direct[cp]

    fold = {
        0x00E0: "a", 0x00E1: "a", 0x00E2: "a", 0x00E3: "a", 0x00E5: "a",
        0x00E8: "e", 0x00E9: "e", 0x00EA: "e", 0x00EB: "e",
        0x00EC: "i", 0x00ED: "i", 0x00EE: "i", 0x00EF: "i",
        0x00F2: "o", 0x00F3: "o", 0x00F4: "o", 0x00F5: "o",
        0x00F9: "u", 0x00FA: "u", 0x00FB: "u",
        0x00FD: "y", 0x00FF: "y",
    }

    folded = fold.get(cp)
    return ord(folded) - ord("a") if folded else SYMBOL_OTHER


def pair_key(cps: tuple[int, ...], boundary: int) -> int | None:
    if boundary <= 0 or boundary >= len(cps):
        return None
    left = german_symbol(cps[boundary - 1])
    right = german_symbol(cps[boundary])
    if left == SYMBOL_OTHER or right == SYMBOL_OTHER:
        return None
    return (left << 5) | right


def context_key(cps: tuple[int, ...], boundary: int) -> int | None:
    if boundary < 2 or boundary + 1 >= len(cps):
        return None

    symbols = [
        german_symbol(cps[i])
        for i in range(boundary - 2, boundary + 2)
    ]
    if SYMBOL_OTHER in symbols:
        return None

    key = 0
    for symbol in symbols:
        key = (key << 5) | symbol
    return key


def context_key3(cps: tuple[int, ...], boundary: int) -> int | None:
    if boundary <= 0 or boundary >= len(cps):
        return None

    key = 0
    for i in range(boundary - 3, boundary + 3):
        symbol = (
            SYMBOL_PAD
            if i < 0 or i >= len(cps)
            else german_symbol(cps[i])
        )
        if symbol == SYMBOL_OTHER:
            return None
        key = (key << 5) | symbol

    return key


def split_bucket(word: str) -> int:
    return (
        hashlib.sha1(
            word.casefold().encode("utf-8")
        ).digest()[0] % 10
    )


def parse_annotation(
    annotation: str,
) -> tuple[str, dict[int, str]] | None:
    if any(ch in annotation for ch in "{}[]/"):
        return None

    plain: list[str] = []
    markers: dict[int, str] = {}
    pos = 0
    i = 0

    while i < len(annotation):
        if annotation[i] in MARKER_CHARS:
            start = i
            while i < len(annotation) and annotation[i] in MARKER_CHARS:
                i += 1
            markers[pos] = markers.get(pos, "") + annotation[start:i]
        else:
            plain.append(annotation[i])
            pos += 1
            i += 1

    return "".join(plain), markers


def select_reformed_field(fields: list[str]) -> str | None:
    if len(fields) >= 2 and fields[1] and fields[1] != "-2-":
        return fields[1]
    if len(fields) >= 4 and fields[3] and fields[3] != "-4-":
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


def parse_all_entries(source: Path) -> list[WordEntry]:
    """Read the DANTE file exactly once."""
    result: list[WordEntry] = []

    with source.open("r", encoding="utf-8") as file:
        for line in file:
            if not line or line.startswith("#"):
                continue
            entry = parse_word_line(line)
            if entry is not None:
                result.append(entry)

    return result


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


def build_2x2_tables(
    all_entries: list[WordEntry],
    args: argparse.Namespace,
) -> tuple[set[int], set[int], set[int]]:
    """
    Build the established 2+2 baseline.

    We intentionally keep this path close to the known-good implementation.
    """
    train = [
        entry for entry in all_entries
        if entry.bucket <= 6
    ]

    base_cache = [
        (entry, base_breaks(entry.cps))
        for entry in train
    ]

    pair_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    add_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    context_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])

    for entry, base in base_cache:
        for boundary in base:
            key = pair_key(entry.cps, boundary)
            if key is None:
                continue
            pair_stats[key][1] += 1
            pair_stats[key][0] += int(boundary in entry.legal)

        for boundary in range(
            MIN_PREFIX,
            len(entry.cps) - MIN_SUFFIX + 1,
        ):
            if boundary in base:
                continue
            key = context_key(entry.cps, boundary)
            if key is None:
                continue
            add_stats[key][1] += 1
            add_stats[key][0] += int(boundary in entry.legal)

    safe_pairs = select_keys(
        pair_stats,
        args.pair_support,
        args.pair_confidence,
    )

    for entry, base in base_cache:
        for boundary in base:
            pair = pair_key(entry.cps, boundary)
            if pair is not None and pair in safe_pairs:
                continue

            key = context_key(entry.cps, boundary)
            if key is None:
                continue

            context_stats[key][1] += 1
            context_stats[key][0] += int(boundary in entry.legal)

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


def apply_2x2(
    entry: WordEntry,
    base: frozenset[int],
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
) -> frozenset[int]:
    result: set[int] = set()

    for boundary in range(
        MIN_PREFIX,
        len(entry.cps) - MIN_SUFFIX + 1,
    ):
        if boundary in base:
            pair = pair_key(entry.cps, boundary)
            context = context_key(entry.cps, boundary)

            if (
                pair is not None and pair in safe_pairs
            ) or (
                context is not None and context in safe_contexts
            ):
                result.add(boundary)
        else:
            context = context_key(entry.cps, boundary)
            if context is not None and context in add_contexts:
                result.add(boundary)

    return frozenset(result)


def analyze_entries(
    entries: list[WordEntry],
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
) -> list[AnalyzedEntry]:
    """
    Cache both the base rule result and the 2+2 result exactly once.
    """
    result: list[AnalyzedEntry] = []

    for entry in entries:
        base = base_breaks(entry.cps)
        baseline = apply_2x2(
            entry, base, safe_pairs, safe_contexts, add_contexts
        )
        result.append(
            AnalyzedEntry(
                entry=entry,
                base=base,
                baseline=baseline,
            )
        )

    return result


def collect_3x3_candidates(
    train: list[AnalyzedEntry],
    support: int,
    confidence: float,
) -> tuple[list[tuple[int, float, int, int]], list[tuple[int, float, int, int]]]:
    """
    Collect 3+3 BLOCK and ADD candidates over ALL training words.

    Returns tuples:
      (key, confidence, support, useful_occurrences)

    BLOCK:
      baseline contains the break, DANTE does not.

    ADD:
      baseline does not contain the break, DANTE does.

    Every occurrence contributes to the denominator. This is essential for
    meaningful confidence estimates.
    """
    block_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    add_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])

    for analyzed in train:
        entry = analyzed.entry
        baseline = analyzed.baseline

        for boundary in range(
            MIN_PREFIX,
            len(entry.cps) - MIN_SUFFIX + 1,
        ):
            key = context_key3(entry.cps, boundary)
            if key is None:
                continue

            if boundary in baseline:
                block_stats[key][1] += 1
                if boundary not in entry.legal:
                    block_stats[key][0] += 1
            else:
                add_stats[key][1] += 1
                if boundary in entry.legal:
                    add_stats[key][0] += 1

    def finalize(
        stats: dict[int, list[int]],
    ) -> list[tuple[int, float, int, int]]:
        result = []
        for key, (useful, total) in stats.items():
            if total < support:
                continue
            conf = useful / total
            if conf < confidence:
                continue
            result.append((key, conf, total, useful))
        return result

    return finalize(block_stats), finalize(add_stats)


def collect_impacts(
    validation: list[AnalyzedEntry],
    block_candidates: set[int],
    add_candidates: set[int],
) -> tuple[
    dict[int, CandidateImpact],
    dict[int, CandidateImpact],
]:
    """
    Precompute each candidate's exact aggregate effect on validation.

    This turns profile evaluation from "run 43k words again" into integer
    counter arithmetic.
    """
    block_acc: dict[int, list[int]] = defaultdict(lambda: [0, 0, 0, 0])
    add_acc: dict[int, list[int]] = defaultdict(lambda: [0, 0, 0, 0])

    for analyzed in validation:
        entry = analyzed.entry
        baseline = analyzed.baseline

        for boundary in range(
            MIN_PREFIX,
            len(entry.cps) - MIN_SUFFIX + 1,
        ):
            key = context_key3(entry.cps, boundary)
            if key is None:
                continue

            if boundary in baseline:
                if key not in block_candidates:
                    continue

                value = block_acc[key]
                value[3] += 1
                if boundary in entry.legal:
                    value[0] -= 1
                else:
                    value[1] -= 1
                if boundary in entry.undesirable:
                    value[2] -= 1
            else:
                if key not in add_candidates:
                    continue

                value = add_acc[key]
                value[3] += 1
                if boundary in entry.legal:
                    value[0] += 1
                else:
                    value[1] += 1
                if boundary in entry.undesirable:
                    value[2] += 1

    blocks = {
        key: CandidateImpact(
            key, value[0], value[1], value[2], value[3]
        )
        for key, value in block_acc.items()
        if value[3]
    }

    adds = {
        key: CandidateImpact(
            key, value[0], value[1], value[2], value[3]
        )
        for key, value in add_acc.items()
        if value[3]
    }

    return blocks, adds


def metrics_for_baseline(
    analyzed: list[AnalyzedEntry],
) -> Metrics:
    words = tp = fp = fn = exact = preferred_found = preferred_total = undesirable = 0

    for item in analyzed:
        entry = item.entry
        actual = item.baseline

        words += 1
        tp += len(actual & entry.legal)
        fp += len(actual - entry.legal)
        fn += len(entry.legal - actual)
        exact += int(actual == entry.legal)
        preferred_found += len(actual & entry.preferred)
        preferred_total += len(entry.preferred)
        undesirable += len(actual & entry.undesirable)

    return Metrics(
        words, tp, fp, fn, exact,
        preferred_found, preferred_total, undesirable
    )


def ranking(
    candidates: list[tuple[int, float, int, int]],
    impacts: dict[int, CandidateImpact],
    kind: str,
) -> list[int]:
    """
    Rank candidates by validation benefit.

    ADD:
      prioritize recall gained, then precision cost.

    BLOCK:
      prioritize false-positive reduction, then recall cost.

    Training confidence and support are deterministic tie-breakers.
    """
    rows = []

    for key, train_conf, support, useful in candidates:
        impact = impacts.get(key)
        if impact is None:
            continue

        if kind == "add":
            benefit = impact.dtp
            cost = impact.dfp
            # Prefer candidates with useful validation recall and low FP cost.
            score = benefit / max(1, cost)
        else:
            benefit = -impact.dfp
            cost = -impact.dtp
            # Prefer candidates that remove FPs with little recall loss.
            score = benefit / max(1, cost)

        if benefit <= 0:
            continue

        rows.append(
            (
                score,
                benefit,
                train_conf,
                support,
                useful,
                key,
            )
        )

    rows.sort(reverse=True)
    return [row[-1] for row in rows]


def apply_candidate_delta(
    metrics: Metrics,
    impact: CandidateImpact,
) -> Metrics:
    return metrics.delta(
        impact.dtp,
        impact.dfp,
        impact.dundesirable,
    )


def cumulative_profile(
    baseline: Metrics,
    block_keys: list[int],
    add_keys: list[int],
    block_impacts: dict[int, CandidateImpact],
    add_impacts: dict[int, CandidateImpact],
    block_count: int,
    add_count: int,
) -> Metrics:
    """
    Calculate a profile from cached deltas.

    No word-level hyphenation is performed here.
    """
    current = baseline

    for key in block_keys[:block_count]:
        current = apply_candidate_delta(
            current,
            block_impacts[key],
        )

    for key in add_keys[:add_count]:
        current = apply_candidate_delta(
            current,
            add_impacts[key],
        )

    return current


def select_profile(
    dev_a: list[AnalyzedEntry],
    dev_b: list[AnalyzedEntry],
    baseline_a: Metrics,
    baseline_b: Metrics,
    block_keys_a: list[int],
    add_keys_a: list[int],
    block_impacts_a: dict[int, CandidateImpact],
    add_impacts_a: dict[int, CandidateImpact],
    block_keys_b: list[int],
    add_keys_b: list[int],
    block_impacts_b: dict[int, CandidateImpact],
    add_impacts_b: dict[int, CandidateImpact],
    max_payload: int,
    precision_target: float,
    undesirable_delta: int,
) -> tuple[set[int], set[int], str, Metrics, Metrics]:
    """
    Select the best profile against TWO independent validation buckets.

    We evaluate a compact curve rather than thousands of combinations:

      BLOCK counts: 0, 10, 25, 50, 100, 200
      ADD counts:   0, 250, 500, ... up to the remaining budget

    Every point is evaluated using precomputed candidate impacts.

    Safety:
      precision(A) >= target
      precision(B) >= target
      undesirable(A) <= baseline(A) + delta
      undesirable(B) <= baseline(B) + delta

    Objective:
      maximize minimum(recall(A), recall(B))
      then maximize average recall
      then maximize minimum precision
      then minimize payload
    """
    max_slots = max_payload // THREE_X_THREE_KEY_BYTES

    block_counts = sorted({
        0,
        min(10, max_slots),
        min(25, max_slots),
        min(50, max_slots),
        min(100, max_slots),
        min(200, max_slots),
    })

    add_counts_base = [0]
    value = 250
    while value <= max_slots:
        add_counts_base.append(value)
        value += 250
    if max_slots not in add_counts_base:
        add_counts_base.append(max_slots)

    best = None

    for block_count in block_counts:
        max_add = max_slots - block_count
        if max_add < 0:
            continue

        add_counts = [
            count for count in add_counts_base
            if count <= max_add
        ]

        for add_count in add_counts:
            metrics_a = cumulative_profile(
                baseline_a,
                block_keys_a,
                add_keys_a,
                block_impacts_a,
                add_impacts_a,
                block_count,
                add_count,
            )
            metrics_b = cumulative_profile(
                baseline_b,
                block_keys_b,
                add_keys_b,
                block_impacts_b,
                add_impacts_b,
                block_count,
                add_count,
            )

            if metrics_a.precision < precision_target:
                continue
            if metrics_b.precision < precision_target:
                continue

            max_undesirable_a = (
                baseline_a.undesirable_used + undesirable_delta
            )
            max_undesirable_b = (
                baseline_b.undesirable_used + undesirable_delta
            )

            if metrics_a.undesirable_used > max_undesirable_a:
                continue
            if metrics_b.undesirable_used > max_undesirable_b:
                continue

            min_recall = min(
                metrics_a.recall,
                metrics_b.recall,
            )
            avg_recall = (
                metrics_a.recall + metrics_b.recall
            ) / 2.0
            min_precision = min(
                metrics_a.precision,
                metrics_b.precision,
            )

            payload = (
                block_count + add_count
            ) * THREE_X_THREE_KEY_BYTES

            score = (
                min_recall,
                avg_recall,
                min_precision,
                -payload,
            )

            if best is None or score > best[0]:
                best = (
                    score,
                    block_count,
                    add_count,
                    metrics_a,
                    metrics_b,
                )

    if best is None:
        # If no positive ADD profile satisfies the two validation sets,
        # retain only the strongest safe BLOCK profile. This preserves the
        # safety benefit without pretending we achieved a recall improvement.
        best_block_count = 0
        best_metrics = (baseline_a, baseline_b)

        for count in block_counts:
            metrics_a = cumulative_profile(
                baseline_a,
                block_keys_a,
                [],
                block_impacts_a,
                {},
                count,
                0,
            )
            metrics_b = cumulative_profile(
                baseline_b,
                block_keys_b,
                [],
                block_impacts_b,
                {},
                count,
                0,
            )

            if (
                metrics_a.precision >= baseline_a.precision
                and metrics_b.precision >= baseline_b.precision
                and metrics_a.undesirable_used <= baseline_a.undesirable_used + undesirable_delta
                and metrics_b.undesirable_used <= baseline_b.undesirable_used + undesirable_delta
                and (
                    metrics_a.precision > best_metrics[0].precision
                    or metrics_b.precision > best_metrics[1].precision
                )
            ):
                best_block_count = count
                best_metrics = (metrics_a, metrics_b)

        return (
            set(block_keys_a[:best_block_count]),
            set(),
            f"fallback-block={best_block_count}",
            best_metrics[0],
            best_metrics[1],
        )

    _, block_count, add_count, metrics_a, metrics_b = best

    return (
        set(block_keys_a[:block_count]),
        set(add_keys_a[:add_count]),
        f"curve(block={block_count},add={add_count})",
        metrics_a,
        metrics_b,
    )


def exact_metrics(
    analyzed: list[AnalyzedEntry],
    block: set[int],
    add: set[int],
) -> Metrics:
    words = tp = fp = fn = exact = preferred_found = preferred_total = undesirable = 0

    for item in analyzed:
        entry = item.entry
        actual = set(item.baseline)

        for boundary in range(
            MIN_PREFIX,
            len(entry.cps) - MIN_SUFFIX + 1,
        ):
            key = context_key3(entry.cps, boundary)
            if key is None:
                continue

            if key in block:
                actual.discard(boundary)
            elif boundary not in actual and key in add:
                actual.add(boundary)

        words += 1
        tp += len(actual & entry.legal)
        fp += len(actual - entry.legal)
        fn += len(entry.legal - actual)
        exact += int(actual == entry.legal)
        preferred_found += len(actual & entry.preferred)
        preferred_total += len(entry.preferred)
        undesirable += len(actual & entry.undesirable)

    return Metrics(
        words, tp, fp, fn, exact,
        preferred_found, preferred_total, undesirable
    )


def build_morphology_components(
    entries: list[WordEntry],
    min_support: int,
    max_components: int,
) -> list[bytes]:
    """
    Learn frequent German compound components from DANTE's '=' markers.

    We deliberately do not learn individual false-positive words. A component
    is useful only when DANTE repeatedly marks it as a compound constituent.
    This keeps the runtime rule lexical/morphological while remaining small.
    """
    counts: dict[str, int] = defaultdict(int)

    for entry in entries:
        if entry.bucket > 6 or not entry.compound:
            continue

        boundaries = sorted(entry.compound)
        points = [0, *boundaries, len(entry.cps)]

        for left, right in zip(points, points[1:]):
            length = right - left
            if not (
                MIN_MORPH_COMPONENT_LENGTH
                <= length
                <= MAX_MORPH_COMPONENT_LENGTH
            ):
                continue

            component = ''.join(chr(cp) for cp in entry.cps[left:right]).casefold()
            if not all(german_symbol(ord(ch)) != SYMBOL_OTHER for ch in component):
                continue
            counts[component] += 1

    selected = [
        (component, count)
        for component, count in counts.items()
        if count >= min_support
    ]

    selected.sort(
        key=lambda item: (item[1], len(item[0]), item[0]),
        reverse=True,
    )

    return [
        component.encode('utf-8')
        for component, _count in selected[:max_components]
    ]


def pack_morphology_components(
    components: list[bytes],
) -> tuple[bytes, list[int], list[int]]:
    """Store each component as one-byte German 5-bit symbols."""
    blob = bytearray()
    offsets: list[int] = []
    lengths: list[int] = []

    for component in components:
        text = component.decode('utf-8')
        symbols = [german_symbol(ord(ch)) for ch in text]
        if any(symbol == SYMBOL_OTHER for symbol in symbols):
            continue
        offsets.append(len(blob))
        lengths.append(len(symbols))
        blob.extend(symbols)

    return bytes(blob), offsets, lengths

def write_header(
    output: Path,
    source: Path,
    safe_pairs: set[int],
    safe_contexts: set[int],
    add_contexts: set[int],
    block: set[int],
    add: set[int],
    morphology_components: list[bytes],
) -> None:
    source_sha = hashlib.sha256(source.read_bytes()).hexdigest()

    pair_blob = bytearray(128)
    for key in safe_pairs:
        pair_blob[key >> 3] |= 1 << (key & 7)

    safe_blob = bytearray()
    for key in sorted(safe_contexts):
        safe_blob.extend((
            key & 0xFF,
            (key >> 8) & 0xFF,
            (key >> 16) & 0x0F,
        ))

    add_blob = bytearray()
    for key in sorted(add_contexts):
        add_blob.extend((
            key & 0xFF,
            (key >> 8) & 0xFF,
            (key >> 16) & 0x0F,
        ))

    block_blob = bytearray()
    for key in sorted(block):
        block_blob.extend((
            key & 0xFF,
            (key >> 8) & 0xFF,
            (key >> 16) & 0xFF,
            (key >> 24) & 0x3F,
        ))

    add3_blob = bytearray()
    for key in sorted(add):
        add3_blob.extend((
            key & 0xFF,
            (key >> 8) & 0xFF,
            (key >> 16) & 0xFF,
            (key >> 24) & 0x3F,
        ))

    morph_blob, morph_offsets, morph_lengths = pack_morphology_components(
        morphology_components
    )

    def fmt(blob: bytes) -> str:
        if not blob:
            return ""
        lines = []
        for start in range(0, len(blob), 16):
            lines.append(
                "    "
                + ", ".join(f"0x{x:02X}" for x in blob[start:start + 16])
                + ","
            )
        return "\n".join(lines)

    text = f"""#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Generated file. Do not edit.
// DANTE SHA-256: {source_sha}
"""

    if block or add:
        text += "\n#define GERMAN_HYBRID_HAS_3X3 1\n"

    text += f"""
inline constexpr std::array<uint8_t, {len(pair_blob)}>
kGermanSafePairBits = {{
{fmt(pair_blob)}
}};

inline constexpr std::array<uint8_t, {len(safe_blob)}>
kGermanSafeContexts = {{
{fmt(safe_blob)}
}};

inline constexpr size_t kGermanSafeContextCount = {len(safe_contexts)};

inline constexpr std::array<uint8_t, {len(add_blob)}>
kGermanAddContexts = {{
{fmt(add_blob)}
}};

inline constexpr size_t kGermanAddContextCount = {len(add_contexts)};
"""

    if block or add:
        text += f"""
inline constexpr std::array<uint8_t, {len(block_blob)}>
kGermanBlockContexts3 = {{
{fmt(block_blob)}
}};

inline constexpr size_t kGermanBlockContext3Count = {len(block)};

inline constexpr std::array<uint8_t, {len(add3_blob)}>
kGermanAddContexts3 = {{
{fmt(add3_blob)}
}};

inline constexpr size_t kGermanAddContext3Count = {len(add)};
"""

    text += f"""
inline constexpr std::array<uint8_t, {len(morph_blob)}>
kGermanMorphologyComponentBlob = {{
{fmt(morph_blob)}
}};

inline constexpr std::array<uint16_t, {len(morph_offsets)}>
kGermanMorphologyComponentOffsets = {{
{', '.join(str(x) for x in morph_offsets)}
}};

inline constexpr std::array<uint8_t, {len(morph_lengths)}>
kGermanMorphologyComponentLengths = {{
{', '.join(str(x) for x in morph_lengths)}
}};

inline constexpr size_t kGermanMorphologyComponentCount = {len(morph_offsets)};
"""

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


def write_fixture(
    output: Path,
    test_entries: list[AnalyzedEntry],
) -> None:
    ordered = sorted(
        test_entries,
        key=lambda x: hashlib.sha1(
            ("eval:" + x.entry.word.casefold()).encode("utf-8")
        ).digest(),
    )[:5000]

    output.parent.mkdir(parents=True, exist_ok=True)

    with output.open("w", encoding="utf-8") as file:
        file.write("# Held-out DANTE evaluation for GermanHybridHyphenator\n")
        file.write(
            "# Format: "
            "word|legal|preferred|undesirable|"
            "ordinary|compound|prefix|suffix|uncategorized\n"
        )

        for item in ordered:
            e = item.entry

            def pos(values: frozenset[int]) -> str:
                return ",".join(str(v) for v in sorted(values))

            file.write(
                f"{e.word}|"
                f"{pos(e.legal)}|"
                f"{pos(e.preferred)}|"
                f"{pos(e.undesirable)}|"
                f"{pos(e.ordinary)}|"
                f"{pos(e.compound)}|"
                f"{pos(e.prefix)}|"
                f"{pos(e.suffix)}|"
                f"{pos(e.uncategorized)}\n"
            )


def print_metrics(label: str, m: Metrics) -> None:
    print(
        f"{label}: "
        f"words={m.words} "
        f"precision={m.precision * 100:.3f}% "
        f"recall={m.recall * 100:.3f}% "
        f"F1={m.f1 * 100:.3f}% "
        f"exact={m.exact_rate * 100:.3f}% "
        f"preferred-recall={m.preferred_recall * 100:.3f}% "
        f"undesirable-used={m.undesirable_used} "
        f"false-positives={m.fp}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate compact German hybrid hyphenation tables."
    )

    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output-header", required=True, type=Path)
    parser.add_argument("--output-eval", required=True, type=Path)

    parser.add_argument("--pair-support", type=int, default=PAIR_SUPPORT_DEFAULT)
    parser.add_argument("--pair-confidence", type=float, default=PAIR_CONFIDENCE_DEFAULT)
    parser.add_argument("--context-support", type=int, default=CONTEXT_SUPPORT_DEFAULT)
    parser.add_argument("--context-confidence", type=float, default=CONTEXT_CONFIDENCE_DEFAULT)
    parser.add_argument("--add-support", type=int, default=ADD_SUPPORT_DEFAULT)
    parser.add_argument("--add-confidence", type=float, default=ADD_CONFIDENCE_DEFAULT)

    parser.add_argument("--block3-support", type=int, default=BLOCK3_SUPPORT_DEFAULT)
    parser.add_argument("--block3-confidence", type=float, default=BLOCK3_CONFIDENCE_DEFAULT)
    parser.add_argument("--add3-support", type=int, default=ADD3_SUPPORT_DEFAULT)
    parser.add_argument("--add3-confidence", type=float, default=ADD3_CONFIDENCE_DEFAULT)

    parser.add_argument(
        "--max-3x3-payload",
        type=int,
        default=MAX_3X3_PAYLOAD_DEFAULT,
    )
    parser.add_argument(
        "--max-precision-degradation",
        type=float,
        default=0.0005,
        help=(
            "Maximum allowed precision degradation versus each validation "
            "baseline (0.0005 = 0.05 percentage points)."
        ),
    )
    parser.add_argument(
        "--max-3x3-undesirable-delta",
        type=int,
        default=0,
        help="Maximum undesirable-break increase over each validation baseline.",
    )
    parser.add_argument(
        "--max-morph-components",
        type=int,
        default=MAX_MORPH_COMPONENTS_DEFAULT,
    )
    parser.add_argument(
        "--min-morph-component-support",
        type=int,
        default=MIN_MORPH_COMPONENT_SUPPORT_DEFAULT,
    )

    args = parser.parse_args()

    if args.max_3x3_payload < 0:
        parser.error("--max-3x3-payload must be >= 0")
    if args.max_3x3_payload % THREE_X_THREE_KEY_BYTES:
        parser.error("--max-3x3-payload must be divisible by 4")
    if args.max_precision_degradation < 0:
        parser.error("--max-precision-degradation must be >= 0")

    # ------------------------------------------------------------------
    # 1. Parse DANTE exactly once.
    # ------------------------------------------------------------------
    print("Parsing DANTE word list once...")
    all_entries = parse_all_entries(args.input)

    train = [e for e in all_entries if e.bucket <= 6]
    dev_a = [e for e in all_entries if e.bucket == 7]
    dev_b = [e for e in all_entries if e.bucket == 8]
    test = [e for e in all_entries if e.bucket == 9]

    print(
        "DANTE entries: "
        f"all={len(all_entries)} "
        f"train={len(train)} "
        f"devA={len(dev_a)} "
        f"devB={len(dev_b)} "
        f"test={len(test)}"
    )

    morphology_components = build_morphology_components(
        all_entries,
        args.min_morph_component_support,
        args.max_morph_components,
    )
    print(
        f"German morphology components: {len(morphology_components)} "
        f"(min-support={args.min_morph_component_support})"
    )

    # ------------------------------------------------------------------
    # 2. Build 2+2 tables from train only.
    # ------------------------------------------------------------------
    safe_pairs, safe_contexts, add_contexts = build_2x2_tables(
        all_entries,
        args,
    )

    print(f"safe pair classes: {len(safe_pairs)} / 1024")
    print(f"safe 2+2 contexts: {len(safe_contexts)}")
    print(f"add 2+2 contexts:  {len(add_contexts)}")

    # ------------------------------------------------------------------
    # 3. Cache base + 2+2 once for every split.
    # ------------------------------------------------------------------
    print("Caching base and 2+2 baseline...")

    analyzed_train = analyze_entries(
        train,
        safe_pairs,
        safe_contexts,
        add_contexts,
    )
    analyzed_a = analyze_entries(
        dev_a,
        safe_pairs,
        safe_contexts,
        add_contexts,
    )
    analyzed_b = analyze_entries(
        dev_b,
        safe_pairs,
        safe_contexts,
        add_contexts,
    )
    analyzed_test = analyze_entries(
        test,
        safe_pairs,
        safe_contexts,
        add_contexts,
    )

    baseline_a = metrics_for_baseline(analyzed_a)
    baseline_b = metrics_for_baseline(analyzed_b)
    baseline_test = metrics_for_baseline(analyzed_test)

    print_metrics("baseline-devA", baseline_a)
    print_metrics("baseline-devB", baseline_b)
    print_metrics("baseline-test", baseline_test)

    # ------------------------------------------------------------------
    # 4. Learn 3+3 candidates from training buckets 0..6.
    # ------------------------------------------------------------------
    print("Collecting 3+3 training statistics...")

    block_candidates, add_candidates = collect_3x3_candidates(
        analyzed_train,
        args.block3_support,
        args.block3_confidence,
    )

    print(
        "3+3 candidates: "
        f"block={len(block_candidates)} "
        f"add={len(add_candidates)}"
    )

    # ------------------------------------------------------------------
    # 5. Determine which candidates actually occur in the validation sets.
    # ------------------------------------------------------------------
    block_candidate_keys = {row[0] for row in block_candidates}
    add_candidate_keys = {row[0] for row in add_candidates}

    block_imp_a, add_imp_a = collect_impacts(
        analyzed_a,
        block_candidate_keys,
        add_candidate_keys,
    )
    block_imp_b, add_imp_b = collect_impacts(
        analyzed_b,
        block_candidate_keys,
        add_candidate_keys,
    )

    print(
        "3+3 validation-visible candidates: "
        f"devA block={len(block_imp_a)} add={len(add_imp_a)} "
        f"devB block={len(block_imp_b)} add={len(add_imp_b)}"
    )

    # Only candidates visible in BOTH development sets are allowed into the
    # final selector. This avoids selecting a context based on a single split.
    common_block = block_candidate_keys & block_imp_a.keys() & block_imp_b.keys()
    common_add = add_candidate_keys & add_imp_a.keys() & add_imp_b.keys()

    block_candidates_common = [
        row for row in block_candidates if row[0] in common_block
    ]
    add_candidates_common = [
        row for row in add_candidates if row[0] in common_add
    ]

    # ------------------------------------------------------------------
    # 6. Rank candidates separately on devA and devB.
    #
    # We keep a common candidate order by averaging the candidate's aggregate
    # validation benefit across the two folds.
    # ------------------------------------------------------------------
    def average_add_score(key: int) -> tuple:
        a = add_imp_a[key]
        b = add_imp_b[key]
        tp_gain = a.dtp + b.dtp
        fp_cost = a.dfp + b.dfp
        return (
            tp_gain / max(1, fp_cost),
            tp_gain,
            -fp_cost,
        )

    def average_block_score(key: int) -> tuple:
        a = block_imp_a[key]
        b = block_imp_b[key]
        fp_reduction = -(a.dfp + b.dfp)
        recall_loss = -(a.dtp + b.dtp)
        return (
            fp_reduction / max(1, recall_loss),
            fp_reduction,
            -recall_loss,
        )

    block_order = sorted(
        common_block,
        key=average_block_score,
        reverse=True,
    )
    add_order = sorted(
        common_add,
        key=average_add_score,
        reverse=True,
    )

    # ------------------------------------------------------------------
    # 7. Search a small, explicit precision/recall curve.
    #
    # Each 3+3 entry is exactly 4 bytes, so 5,000 entries is the full budget.
    #
    # This is intentionally NOT a large combinatorial optimizer.
    # 6 block checkpoints x ~21 ADD checkpoints = about 126 cheap trials,
    # and each trial is just cached counter arithmetic.
    # ------------------------------------------------------------------
    max_slots = args.max_3x3_payload // THREE_X_THREE_KEY_BYTES

    block_checkpoints = sorted({
        0,
        min(10, max_slots),
        min(25, max_slots),
        min(50, max_slots),
        min(100, max_slots),
        min(200, max_slots),
    })

    add_checkpoints = list(range(0, max_slots + 1, 250))
    if max_slots not in add_checkpoints:
        add_checkpoints.append(max_slots)

    best = None

    # Print the complete requested diagnostic frontier.  Unlike the earlier
    # version, this prints ALL four BLOCK settings explicitly, so we can see
    # whether the selected point is really on the Pareto frontier.
    frontier_block_counts = [
        count for count in (0, 10, 25, 50)
        if count <= max_slots
    ]
    print("3+3 precision/recall frontier (BLOCK = 0, 10, 25, 50):")
    print("  BLOCK ADD  KB   devA_P devA_R dU_A   devB_P devB_R dU_B")

    for block_count in frontier_block_counts:
        for add_count in add_checkpoints:
            if block_count + add_count > max_slots:
                continue

            metrics_a = baseline_a
            for key in block_order[:block_count]:
                metrics_a = metrics_a.delta(
                    block_imp_a[key].dtp,
                    block_imp_a[key].dfp,
                    block_imp_a[key].dundesirable,
                )
            for key in add_order[:add_count]:
                metrics_a = metrics_a.delta(
                    add_imp_a[key].dtp,
                    add_imp_a[key].dfp,
                    add_imp_a[key].dundesirable,
                )

            metrics_b = baseline_b
            for key in block_order[:block_count]:
                metrics_b = metrics_b.delta(
                    block_imp_b[key].dtp,
                    block_imp_b[key].dfp,
                    block_imp_b[key].dundesirable,
                )
            for key in add_order[:add_count]:
                metrics_b = metrics_b.delta(
                    add_imp_b[key].dtp,
                    add_imp_b[key].dfp,
                    add_imp_b[key].dundesirable,
                )

            if add_count == 0 or add_count % 250 == 0 or add_count == max_slots - block_count:
                print(
                    f"  {block_count:5d} {add_count:4d} "
                    f"{(block_count + add_count) * THREE_X_THREE_KEY_BYTES / 1000.0:4.1f} "
                    f"{metrics_a.precision * 100:6.3f} {metrics_a.recall * 100:6.3f} "
                    f"{metrics_a.undesirable_used - baseline_a.undesirable_used:4d} "
                    f"{metrics_b.precision * 100:6.3f} {metrics_b.recall * 100:6.3f} "
                    f"{metrics_b.undesirable_used - baseline_b.undesirable_used:4d}"
                )

    # The actual selection uses the full checkpoint set below.
    for block_count in block_checkpoints:
        for add_count in add_checkpoints:
            if block_count + add_count > max_slots:
                continue

            # Calculate the aggregate profile against devA.
            metrics_a = baseline_a
            for key in block_order[:block_count]:
                metrics_a = metrics_a.delta(
                    block_imp_a[key].dtp,
                    block_imp_a[key].dfp,
                    block_imp_a[key].dundesirable,
                )

            for key in add_order[:add_count]:
                metrics_a = metrics_a.delta(
                    add_imp_a[key].dtp,
                    add_imp_a[key].dfp,
                    add_imp_a[key].dundesirable,
                )

            # Same profile on devB.
            metrics_b = baseline_b
            for key in block_order[:block_count]:
                metrics_b = metrics_b.delta(
                    block_imp_b[key].dtp,
                    block_imp_b[key].dfp,
                    block_imp_b[key].dundesirable,
                )

            for key in add_order[:add_count]:
                metrics_b = metrics_b.delta(
                    add_imp_b[key].dtp,
                    add_imp_b[key].dfp,
                    add_imp_b[key].dundesirable,
                )

            # Both validation folds must pass.
            min_precision_a = baseline_a.precision - args.max_precision_degradation
            min_precision_b = baseline_b.precision - args.max_precision_degradation

            if metrics_a.precision < min_precision_a:
                continue
            if metrics_b.precision < min_precision_b:
                continue

            if (
                metrics_a.undesirable_used
                > baseline_a.undesirable_used
                + args.max_3x3_undesirable_delta
            ):
                continue

            if (
                metrics_b.undesirable_used
                > baseline_b.undesirable_used
                + args.max_3x3_undesirable_delta
            ):
                continue

            min_recall = min(
                metrics_a.recall,
                metrics_b.recall,
            )
            avg_recall = (
                metrics_a.recall + metrics_b.recall
            ) / 2.0
            min_precision = min(
                metrics_a.precision,
                metrics_b.precision,
            )
            payload = (
                block_count + add_count
            ) * THREE_X_THREE_KEY_BYTES

            score = (
                min_recall,
                avg_recall,
                min_precision,
                -payload,
            )

            if best is None or score > best[0]:
                best = (
                    score,
                    block_count,
                    add_count,
                    metrics_a,
                    metrics_b,
                )

    # ------------------------------------------------------------------
    # 8. If no 3+3 profile satisfies the safety rules, fall back to the
    # strongest safe BLOCK-only profile. Do not add risky ADDs.
    # ------------------------------------------------------------------
    if best is None:
        print(
            "WARNING: no two-fold 3+3 profile meets "
            "precision/undesirable constraints; "
            "using BLOCK-only safety fallback."
        )

        best_block_count = 0
        best_a = baseline_a
        best_b = baseline_b

        for block_count in block_checkpoints:
            trial_a = baseline_a
            trial_b = baseline_b

            for key in block_order[:block_count]:
                trial_a = trial_a.delta(
                    block_imp_a[key].dtp,
                    block_imp_a[key].dfp,
                    block_imp_a[key].dundesirable,
                )
                trial_b = trial_b.delta(
                    block_imp_b[key].dtp,
                    block_imp_b[key].dfp,
                    block_imp_b[key].dundesirable,
                )

            if (
                trial_a.precision >= baseline_a.precision
                and trial_b.precision >= baseline_b.precision
                and trial_a.undesirable_used <= baseline_a.undesirable_used
                and trial_b.undesirable_used <= baseline_b.undesirable_used
            ):
                if (
                    min(
                        trial_a.precision,
                        trial_b.precision,
                    )
                    >
                    min(
                        best_a.precision,
                        best_b.precision,
                    )
                ):
                    best_block_count = block_count
                    best_a = trial_a
                    best_b = trial_b

        selected_block = set(block_order[:best_block_count])
        selected_add = set()
        profile_name = (
            f"fallback-block={best_block_count}"
        )
        selected_dev_a = best_a
        selected_dev_b = best_b

    else:
        _, best_block_count, best_add_count, selected_dev_a, selected_dev_b = best
        selected_block = set(
            block_order[:best_block_count]
        )
        selected_add = set(
            add_order[:best_add_count]
        )
        profile_name = (
            f"curve(block={best_block_count},"
            f"add={best_add_count})"
        )

    payload_3x3 = (
        len(selected_block) + len(selected_add)
    ) * THREE_X_THREE_KEY_BYTES

    payload_2x2 = (
        128
        + 3 * (
            len(safe_contexts)
            + len(add_contexts)
        )
    )

    total_payload = payload_2x2 + payload_3x3

    if payload_3x3 > args.max_3x3_payload:
        raise RuntimeError(
            "Internal error: 3+3 payload exceeds hard budget"
        )

    print(
        f"selected 3+3: "
        f"block={len(selected_block)} "
        f"add={len(selected_add)}"
    )
    print(
        f"3+3 payload: "
        f"{payload_3x3} bytes / "
        f"{args.max_3x3_payload} max"
    )
    print(
        f"generated payload: "
        f"{total_payload} bytes "
        f"(2+2={payload_2x2}, "
        f"3+3={payload_3x3})"
    )
    print(
        f"selected profile: {profile_name}"
    )

    print_metrics(
        "devA-selected-fast",
        selected_dev_a,
    )
    print_metrics(
        "devB-selected-fast",
        selected_dev_b,
    )

    # ------------------------------------------------------------------
    # 9. Exact validation and held-out test.
    #
    # This is now only two validation passes and one final test pass.
    # ------------------------------------------------------------------
    exact_a = exact_metrics(
        analyzed_a,
        selected_block,
        selected_add,
    )
    exact_b = exact_metrics(
        analyzed_b,
        selected_block,
        selected_add,
    )
    exact_test = exact_metrics(
        analyzed_test,
        selected_block,
        selected_add,
    )

    if (
        abs(exact_a.precision - selected_dev_a.precision) > 1e-12
        or abs(exact_a.recall - selected_dev_a.recall) > 1e-12
        or exact_a.undesirable_used != selected_dev_a.undesirable_used
        or abs(exact_b.precision - selected_dev_b.precision) > 1e-12
        or abs(exact_b.recall - selected_dev_b.recall) > 1e-12
        or exact_b.undesirable_used != selected_dev_b.undesirable_used
    ):
        raise RuntimeError(
            "Cached validation model disagrees with exact evaluation"
        )

    print_metrics(
        "devA-selected",
        exact_a,
    )
    print_metrics(
        "devB-selected",
        exact_b,
    )
    print_metrics(
        "test",
        exact_test,
    )

    # ------------------------------------------------------------------
    # 10. Generate firmware data and the fixed held-out fixture.
    # ------------------------------------------------------------------
    write_header(
        args.output_header,
        args.input,
        safe_pairs,
        safe_contexts,
        add_contexts,
        selected_block,
        selected_add,
        morphology_components,
    )

    write_fixture(
        args.output_eval,
        analyzed_test,
    )

    print("Generation complete.")


if __name__ == "__main__":
    main()
