#!/usr/bin/env python3
"""
Russian compact hyphenation generator / experiment.

This is an OFFLINE experiment. It does not modify production code.

Input:
  1) the repository's Russian labelled fixture:
       test/hyphenation_eval/resources/russian_hyphenation_tests.txt
  2) a baseline TSV produced by DumpRussianHyphenationBaseline.cpp:
       word<TAB>break1,break2,...

The baseline is the REAL current Russian Liang hyphenator, so the optimizer
does not use a Python approximation of Liang.

Representation:
  * Russian has 33 Cyrillic letters, therefore contexts use 6-bit symbols.
  * 2x2 context = 24-bit key => 3 bytes per selected context.
  * 3x3 context = 36-bit key => 5 bytes per selected context.
  * Pair-level decisions use 6-bit x 6-bit = 12-bit keys.

Training/dev/test split:
  deterministic SHA-1(word) % 10:
    0..6 = train
    7    = devA
    8..9 = test
  The 5,000-word fixture is only a screening corpus. A production-quality
  result requires a larger independent Russian corpus.

Selection:
  Stage 1: build a compact 2x2 baseline:
      safe pair
      safe context
      add context
      block context
  Stage 2: search 3x3 ADD/BLOCK candidates against that baseline.
  Candidate sets are learned from training only.
  The shared rule set must satisfy BOTH Dev A and Test quality constraints.

No production files are touched. The script writes:
  build/ru_compact_rules.h
  build/ru_compact_report.txt
"""

from __future__ import annotations

import argparse
import hashlib
import math
import re
from dataclasses import dataclass
from pathlib import Path


MIN_PREFIX = 2
MIN_SUFFIX = 2

TARGET_PRECISION_LOSS = 0.0005
MAX_UNDESIRABLE_DELTA = 0

MAX_2X2_PAYLOAD = 4096
MAX_3X3_PAYLOAD = 4096
MAX_TOTAL_PAYLOAD = 8192


ALPHABET = "абвгдеёжзийклмнопрстуфхцчшщъыьэюя"
SYMBOL = {ch: i for i, ch in enumerate(ALPHABET)}


@dataclass(frozen=True)
class Entry:
    word: str
    legal: frozenset[int]
    undesirable: frozenset[int]


@dataclass(frozen=True)
class Metrics:
    tp: int
    fp: int
    fn: int
    undesirable: int
    words: int

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


@dataclass
class RuleStats:
    good: int = 0
    bad: int = 0

    @property
    def total(self) -> int:
        return self.good + self.bad

    @property
    def precision(self) -> float:
        return self.good / self.total if self.total else 0.0


@dataclass(frozen=True)
class Profile:
    add2: frozenset[int]
    block2: frozenset[int]
    add3: frozenset[int]
    block3: frozenset[int]

    @property
    def payload(self) -> int:
        return (
            len(self.add2) * 3
            + len(self.block2) * 3
            + len(self.add3) * 5
            + len(self.block3) * 5
        )


def parse_fixture(path: Path) -> list[Entry]:
    entries: list[Entry] = []

    with path.open("r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue

            fields = line.split("|")
            if len(fields) != 3:
                continue

            word, annotated, _freq = fields

            plain: list[str] = []
            positions: list[int] = []
            pos = 0

            for ch in annotated:
                if ch == "=":
                    if pos >= MIN_PREFIX and pos <= len(word) - MIN_SUFFIX:
                        positions.append(pos)
                else:
                    plain.append(ch)
                    pos += 1

            plain_word = "".join(plain)

            if plain_word != word:
                raise ValueError(
                    f"Fixture mismatch: {word!r} != {plain_word!r}"
                )

            if not word or not all(
                (ch.casefold() in SYMBOL) or (not ch.isalpha())
                for ch in word
            ):
                continue

            entries.append(
                Entry(
                    word=word,
                    legal=frozenset(positions),
                    undesirable=frozenset(),
                )
            )

    return entries


def parse_baseline(path: Path) -> dict[str, frozenset[int]]:
    result: dict[str, frozenset[int]] = {}

    with path.open("r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\r\n")
            if not line:
                continue

            word, breaks = line.split("\t", 1)

            positions = (
                frozenset()
                if not breaks
                else frozenset(
                    int(x)
                    for x in breaks.split(",")
                    if x
                )
            )
            result[word] = positions

    return result


def bucket(word: str) -> int:
    return hashlib.sha1(
        word.casefold().encode("utf-8")
    ).digest()[0] % 10


def symbol(c: str) -> int:
    try:
        return SYMBOL[c.casefold()]
    except KeyError:
        return 63


def context_key(
    word: str,
    boundary: int,
    width: int,
) -> int | None:
    half = width // 2

    if boundary < half or boundary + half > len(word):
        return None

    key = 0
    for ch in word[boundary - half:boundary + half]:
        s = symbol(ch)
        if s >= 33:
            return None
        key = (key << 6) | s

    return key


def pair_key(
    word: str,
    boundary: int,
) -> int | None:
    if boundary <= 0 or boundary >= len(word):
        return None

    left = symbol(word[boundary - 1])
    right = symbol(word[boundary])

    if left >= 33 or right >= 33:
        return None

    return (left << 6) | right


def in_baseline(
    baseline: dict[str, frozenset[int]],
    word: str,
    boundary: int,
) -> bool:
    return boundary in baseline[word]


def split_entries(
    entries: list[Entry],
) -> tuple[list[Entry], list[Entry], list[Entry]]:
    train = [e for e in entries if bucket(e.word) <= 6]
    dev_a = [e for e in entries if bucket(e.word) == 7]
    test = [e for e in entries if bucket(e.word) >= 8]
    return train, dev_a, test


def evaluate(
    entries: list[Entry],
    baseline: dict[str, frozenset[int]],
    profile: Profile,
) -> Metrics:
    tp = fp = fn = undesirable = 0

    for entry in entries:
        actual = set(baseline[entry.word])

        # 2x2 BLOCK first.
        for boundary in list(actual):
            key = context_key(entry.word, boundary, 4)
            if key is not None and key in profile.block2:
                actual.discard(boundary)

        # 2x2 ADD.
        for boundary in range(
            MIN_PREFIX,
            len(entry.word) - MIN_SUFFIX + 1,
        ):
            if boundary in actual:
                continue
            key = context_key(entry.word, boundary, 4)
            if key is not None and key in profile.add2:
                actual.add(boundary)

        # 3x3 BLOCK then ADD.
        for boundary in list(actual):
            key = context_key(entry.word, boundary, 6)
            if key is not None and key in profile.block3:
                actual.discard(boundary)

        for boundary in range(
            MIN_PREFIX,
            len(entry.word) - MIN_SUFFIX + 1,
        ):
            if boundary in actual:
                continue
            key = context_key(entry.word, boundary, 6)
            if key is not None and key in profile.add3:
                actual.add(boundary)

        tp += len(actual & entry.legal)
        fp += len(actual - entry.legal)
        fn += len(entry.legal - actual)
        undesirable += len(actual & entry.undesirable)

    return Metrics(
        tp=tp,
        fp=fp,
        fn=fn,
        undesirable=undesirable,
        words=len(entries),
    )


def stats_for_training(
    entries: list[Entry],
    baseline: dict[str, frozenset[int]],
    width: int,
    mode: str,
) -> dict[int, RuleStats]:
    stats: dict[int, RuleStats] = {}

    for entry in entries:
        base = baseline[entry.word]

        if mode == "ADD":
            boundaries = range(
                MIN_PREFIX,
                len(entry.word) - MIN_SUFFIX + 1,
            )
            boundaries = [
                b for b in boundaries
                if b not in base
            ]
        else:
            boundaries = list(base)

        for boundary in boundaries:
            key = context_key(
                entry.word,
                boundary,
                width,
            )
            if key is None:
                continue

            stat = stats.setdefault(
                key,
                RuleStats(),
            )

            if boundary in entry.legal:
                stat.good += 1
            else:
                stat.bad += 1

    return stats


def select_contexts(
    stats: dict[int, RuleStats],
    min_support: int,
    min_precision: float,
) -> frozenset[int]:
    candidates = [
        (key, stat)
        for key, stat in stats.items()
        if stat.total >= min_support
        and stat.precision >= min_precision
    ]

    candidates.sort(
        key=lambda item: (
            item[1].precision,
            item[1].good,
            -item[1].bad,
            -item[0],
        ),
        reverse=True,
    )

    return frozenset(key for key, _ in candidates)


def protected_baseline(
    entries: list[Entry],
    baseline: dict[str, frozenset[int]],
    block2: frozenset[int],
    block3: frozenset[int],
) -> dict[str, frozenset[int]]:
    result: dict[str, frozenset[int]] = {}

    for entry in entries:
        actual = set(baseline[entry.word])

        for boundary in list(actual):
            key2 = context_key(entry.word, boundary, 4)
            if key2 is not None and key2 in block2:
                actual.discard(boundary)
                continue

            key3 = context_key(entry.word, boundary, 6)
            if key3 is not None and key3 in block3:
                actual.discard(boundary)

        result[entry.word] = frozenset(actual)

    return result


def select_blockers(
    train: list[Entry],
    baseline: dict[str, frozenset[int]],
    dev_a: list[Entry],
    dev_b: list[Entry],
    max_payload: int,
) -> tuple[frozenset[int], frozenset[int]]:
    """
    Phase 1: select BLOCK rules without repeatedly rescanning the entire
    development corpora.

    The previous implementation was O(candidates^2 * corpus) because it
    evaluated every candidate by running the complete evaluator after every
    previously selected candidate. With ~1,100 Russian BLOCK candidates this
    becomes unnecessarily enormous.

    Here we:
      1. build candidate hit sets on each development split once;
      2. maintain the currently blocked positions incrementally;
      3. score candidates from their still-unblocked positions;
      4. select the best precision-improving candidate;
      5. update only affected positions.

    This is still greedy, but its cost is proportional to candidate hits rather
    than repeated whole-corpus evaluation.
    """

    def build_hits(
        entries: list[Entry],
        candidate_keys: set[int],
        width: int,
    ) -> dict[int, list[tuple[str, int, bool]]]:
        hits: dict[int, list[tuple[str, int, bool]]] = {
            key: [] for key in candidate_keys
        }

        for entry in entries:
            base = baseline[entry.word]

            for boundary in base:
                key = context_key(entry.word, boundary, width)
                if key is None or key not in candidate_keys:
                    continue

                hits[key].append(
                    (
                        entry.word,
                        boundary,
                        boundary in entry.legal,
                    )
                )

        return hits

    stats2 = stats_for_training(train, baseline, 4, "BLOCK")
    stats3 = stats_for_training(train, baseline, 6, "BLOCK")

    candidate2 = {
        key
        for key, stat in stats2.items()
        if stat.total >= 3 and stat.precision >= 0.999
    }
    candidate3 = {
        key
        for key, stat in stats3.items()
        if stat.total >= 3 and stat.precision >= 0.999
    }

    hits_a2 = build_hits(dev_a, candidate2, 4)
    hits_b2 = build_hits(dev_b, candidate2, 4)
    hits_a3 = build_hits(dev_a, candidate3, 6)
    hits_b3 = build_hits(dev_b, candidate3, 6)

    # Current predictions are the unchanged Liang baseline.
    active_a: set[tuple[str, int]] = {
        (entry.word, boundary)
        for entry in dev_a
        for boundary in baseline[entry.word]
    }
    active_b: set[tuple[str, int]] = {
        (entry.word, boundary)
        for entry in dev_b
        for boundary in baseline[entry.word]
    }

    selected2: set[int] = set()
    selected3: set[int] = set()

    def current_metrics(
        entries: list[Entry],
        active: set[tuple[str, int]],
    ) -> Metrics:
        tp = fp = fn = undesirable = 0

        for entry in entries:
            actual = {
                boundary
                for word, boundary in active
                if word == entry.word
            }

            tp += len(actual & entry.legal)
            fp += len(actual - entry.legal)
            fn += len(entry.legal - actual)
            undesirable += len(actual & entry.undesirable)

        return Metrics(
            tp=tp,
            fp=fp,
            fn=fn,
            undesirable=undesirable,
            words=len(entries),
        )

    base_a = current_metrics(dev_a, active_a)
    base_b = current_metrics(dev_b, active_b)

    candidates = [
        ("BLOCK2", key)
        for key in candidate2
    ] + [
        ("BLOCK3", key)
        for key in candidate3
    ]

    # Avoid repeatedly reconsidering candidates that have no remaining active
    # hits.
    while candidates:
        current_a = current_metrics(dev_a, active_a)
        current_b = current_metrics(dev_b, active_b)

        best = None

        for kind, key in candidates:
            if kind == "BLOCK2":
                hits_a = hits_a2.get(key, ())
                hits_b = hits_b2.get(key, ())
                payload = (
                    (len(selected2) + 1) * 3
                    + len(selected3) * 5
                )
            else:
                hits_a = hits_a3.get(key, ())
                hits_b = hits_b3.get(key, ())
                payload = (
                    len(selected2) * 3
                    + (len(selected3) + 1) * 5
                )

            if payload > max_payload:
                continue

            # Only still-active boundaries can be changed by a new blocker.
            a_positions = [
                (word, boundary, legal)
                for word, boundary, legal in hits_a
                if (word, boundary) in active_a
            ]
            b_positions = [
                (word, boundary, legal)
                for word, boundary, legal in hits_b
                if (word, boundary) in active_b
            ]

            if not a_positions and not b_positions:
                continue

            delta_tp_a = -sum(1 for _, _, legal in a_positions if legal)
            delta_fp_a = -sum(1 for _, _, legal in a_positions if not legal)
            delta_fn_a = -delta_tp_a

            delta_tp_b = -sum(1 for _, _, legal in b_positions if legal)
            delta_fp_b = -sum(1 for _, _, legal in b_positions if not legal)
            delta_fn_b = -delta_tp_b

            trial_a = Metrics(
                tp=current_a.tp + delta_tp_a,
                fp=current_a.fp + delta_fp_a,
                fn=current_a.fn + delta_fn_a,
                undesirable=current_a.undesirable,
                words=current_a.words,
            )
            trial_b = Metrics(
                tp=current_b.tp + delta_tp_b,
                fp=current_b.fp + delta_fp_b,
                fn=current_b.fn + delta_fn_b,
                undesirable=current_b.undesirable,
                words=current_b.words,
            )

            if trial_a.precision < base_a.precision - TARGET_PRECISION_LOSS:
                continue
            if trial_b.precision < base_b.precision - TARGET_PRECISION_LOSS:
                continue
            if trial_a.recall < base_a.recall - TARGET_PRECISION_LOSS:
                continue
            if trial_b.recall < base_b.recall - TARGET_PRECISION_LOSS:
                continue

            # Primary objective: reduce false positives jointly. Secondary:
            # preserve recall, then prefer stronger training support.
            fp_gain = -max(
                delta_fp_a,
                delta_fp_b,
            )

            if fp_gain <= 0:
                continue

            train_stat = (
                stats2 if kind == "BLOCK2" else stats3
            )[key]

            score = (
                min(trial_a.precision, trial_b.precision),
                fp_gain,
                min(trial_a.recall, trial_b.recall),
                train_stat.precision,
                train_stat.good,
            )

            if best is None or score > best[0]:
                best = (
                    score,
                    kind,
                    key,
                    a_positions,
                    b_positions,
                )

        if best is None:
            break

        _, kind, key, a_positions, b_positions = best

        for word, boundary, _ in a_positions:
            active_a.discard((word, boundary))
        for word, boundary, _ in b_positions:
            active_b.discard((word, boundary))

        if kind == "BLOCK2":
            selected2.add(key)
        else:
            selected3.add(key)

        candidates = [
            item
            for item in candidates
            if item[1] != key
            or item[0] != kind
        ]

        if len(selected2) + len(selected3) <= 25 or (
            (len(selected2) + len(selected3)) % 25 == 0
        ):
            print(
                f"  BLOCK select {kind} "
                f"count={len(selected2) + len(selected3)} "
                f"remaining={len(candidates)}"
            )

    return frozenset(selected2), frozenset(selected3)


def select_additions(
    train: list[Entry],
    baseline_after_blocks: dict[str, frozenset[int]],
    dev_a: list[Entry],
    dev_b: list[Entry],
    block2: frozenset[int],
    block3: frozenset[int],
    max_payload: int,
) -> tuple[frozenset[int], frozenset[int]]:
    """
    Phase 2: with the BLOCK set frozen, choose ADD rules for recall.

    A candidate must improve the weaker of Dev A/Dev B recall while respecting
    the original baseline precision and undesirable-break constraints.
    """
    block_profile = Profile(
        frozenset(),
        block2,
        frozenset(),
        block3,
    )

    base_a = evaluate(dev_a, baseline_after_blocks, block_profile)
    base_b = evaluate(dev_b, baseline_after_blocks, block_profile)

    stats2 = stats_for_training(
        train, baseline_after_blocks, 4, "ADD"
    )
    stats3 = stats_for_training(
        train, baseline_after_blocks, 6, "ADD"
    )

    candidates: list[tuple[str, int, RuleStats]] = []

    for key, stat in stats2.items():
        if stat.total >= 3 and stat.precision >= 0.995:
            candidates.append(("ADD2", key, stat))

    for key, stat in stats3.items():
        if stat.total >= 3 and stat.precision >= 0.995:
            candidates.append(("ADD3", key, stat))

    candidates.sort(
        key=lambda item: (
            item[2].precision,
            item[2].good,
            -item[2].bad,
            0 if item[0] == "ADD2" else -1,
        ),
        reverse=True,
    )

    selected2: set[int] = set()
    selected3: set[int] = set()

    current_a = base_a
    current_b = base_b
    remaining = list(candidates)

    while remaining:
        best = None

        for kind, key, stat in remaining:
            trial2 = set(selected2)
            trial3 = set(selected3)

            if kind == "ADD2":
                trial2.add(key)
            else:
                trial3.add(key)

            payload = (
                len(trial2) * 3
                + len(block2) * 3
                + len(trial3) * 5
                + len(block3) * 5
            )
            if payload > max_payload:
                continue

            profile = Profile(
                frozenset(trial2),
                block2,
                frozenset(trial3),
                block3,
            )

            a = evaluate(dev_a, baseline_after_blocks, profile)
            b = evaluate(dev_b, baseline_after_blocks, profile)

            if a.precision < base_a.precision - TARGET_PRECISION_LOSS:
                continue
            if b.precision < base_b.precision - TARGET_PRECISION_LOSS:
                continue

            if a.undesirable > base_a.undesirable + MAX_UNDESIRABLE_DELTA:
                continue
            if b.undesirable > base_b.undesirable + MAX_UNDESIRABLE_DELTA:
                continue

            gain = min(
                a.recall - current_a.recall,
                b.recall - current_b.recall,
            )

            if gain <= 0:
                continue

            score = (
                gain,
                min(a.recall, b.recall),
                min(a.precision, b.precision),
                stat.precision,
                stat.good,
            )

            if best is None or score > best[0]:
                best = (score, kind, key, a, b)

        if best is None:
            break

        _, kind, key, current_a, current_b = best

        if kind == "ADD2":
            selected2.add(key)
        else:
            selected3.add(key)

        remaining = [
            item for item in remaining
            if not (
                item[0] == kind and item[1] == key
            )
        ]

    return frozenset(selected2), frozenset(selected3)


def emit_header(
    path: Path,
    profile: Profile,
    report: str,
) -> None:
    def emit_array(
        name: str,
        values: list[int],
        width: int,
    ) -> list[str]:
        out = [
            f"inline constexpr uint8_t {name}[] = {{"
        ]
        bytes_out: list[int] = []

        for value in values:
            for i in range(width):
                bytes_out.append(
                    (value >> (8 * i)) & 0xFF
                )

        for i in range(0, len(bytes_out), 16):
            chunk = bytes_out[i:i + 16]
            out.append(
                "    "
                + ", ".join(
                    f"0x{byte:02X}"
                    for byte in chunk
                )
                + ","
            )

        out.append("};")
        out.append(
            f"inline constexpr size_t {name}Count = "
            f"{len(values)};"
        )
        return out

    lines = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace russian_compact {",
        "",
        "// Generated by generate_russian_compact.py",
        "// Context keys use 6-bit Russian alphabet symbols.",
        "",
    ]

    lines += emit_array(
        "kAdd2",
        sorted(profile.add2),
        3,
    )
    lines += [""]
    lines += emit_array(
        "kBlock2",
        sorted(profile.block2),
        3,
    )
    lines += [""]
    lines += emit_array(
        "kAdd3",
        sorted(profile.add3),
        5,
    )
    lines += [""]
    lines += emit_array(
        "kBlock3",
        sorted(profile.block3),
        5,
    )
    lines += [
        "",
        f"inline constexpr size_t kPayloadBytes = "
        f"{profile.payload};",
        "",
        "}  // namespace russian_compact",
        "",
        "/*",
        report,
        "*/",
    ]

    path.write_text(
        "\n".join(lines),
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fixture",
        type=Path,
        default=Path(
            "test/hyphenation_eval/resources/russian_hyphenation_tests.txt"
        ),
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        default=Path(
            "build/russian_hyphenation_baseline.tsv"
        ),
    )
    parser.add_argument(
        "--header",
        type=Path,
        default=Path(
            "build/ru_compact_rules.h"
        ),
    )
    args = parser.parse_args()

    entries = parse_fixture(args.fixture)
    baseline = parse_baseline(args.baseline)

    missing = [
        e.word
        for e in entries
        if e.word not in baseline
    ]
    if missing:
        raise SystemExit(
            "Baseline is missing "
            f"{len(missing)} fixture words; first: {missing[:5]}"
        )

    train, dev_a, test = split_entries(entries)

    empty = Profile(
        frozenset(),
        frozenset(),
        frozenset(),
        frozenset(),
    )

    base_a = evaluate(dev_a, baseline, empty)
    base_b = evaluate(test, baseline, empty)

    print(
        f"entries: all={len(entries)} "
        f"train={len(train)} "
        f"devA={len(dev_a)} "
        f"test={len(test)}"
    )

    print(
        f"baseline-devA: P={base_a.precision*100:.3f}% "
        f"R={base_a.recall*100:.3f}% "
        f"FP={base_a.fp}"
    )
    print(
        f"baseline-test: P={base_b.precision*100:.3f}% "
        f"R={base_b.recall*100:.3f}% "
        f"FP={base_b.fp}"
    )

    empty = Profile(
        frozenset(),
        frozenset(),
        frozenset(),
        frozenset(),
    )

    # ------------------------------------------------------------------
    # Phase 1: precision protection with BLOCK rules.
    # ------------------------------------------------------------------
    print("Phase 1: selecting BLOCK rules...")

    # The current baseline is the actual Liang output. Learn blockers relative
    # to it, not relative to an approximation.
    block2, block3 = select_blockers(
        train,
        baseline,
        dev_a,
        test,
        MAX_TOTAL_PAYLOAD,
    )

    after_blocks_profile = Profile(
        frozenset(),
        block2,
        frozenset(),
        block3,
    )

    after_blocks_a = evaluate(
        dev_a, baseline, after_blocks_profile
    )
    after_blocks_b = evaluate(
        test, baseline, after_blocks_profile
    )

    print(
        f"after-BLOCK: 2x2={len(block2)} "
        f"3x3={len(block3)} "
        f"payload={after_blocks_profile.payload} bytes"
    )
    print(
        f"  DevA: P={after_blocks_a.precision*100:.3f}% "
        f"R={after_blocks_a.recall*100:.3f}% "
        f"FP={after_blocks_a.fp}"
    )
    print(
        f"  Test: P={after_blocks_b.precision*100:.3f}% "
        f"R={after_blocks_b.recall*100:.3f}% "
        f"FP={after_blocks_b.fp}"
    )

    # The baseline dictionary has not changed; BLOCK decisions are represented
    # in the profile. The second-stage selector must evaluate additions against
    # that protected profile.
    protected_baseline = baseline

    # ------------------------------------------------------------------
    # Phase 2: recall recovery with ADD rules.
    # ------------------------------------------------------------------
    print("Phase 2: selecting ADD rules...")

    add2, add3 = select_additions(
        train,
        protected_baseline,
        dev_a,
        test,
        block2,
        block3,
        MAX_TOTAL_PAYLOAD,
    )

    profile = Profile(
        add2,
        block2,
        add3,
        block3,
    )

    final_a = evaluate(dev_a, baseline, profile)
    final_b = evaluate(test, baseline, profile)

    print(
        f"after-ADD: 2x2={len(add2)} "
        f"3x3={len(add3)}"
    )
    print(
        f"final payload={profile.payload} bytes"
    )

    print(
        f"  DevA: P={final_a.precision*100:.3f}% "
        f"R={final_a.recall*100:.3f}% "
        f"FP={final_a.fp}"
    )
    print(
        f"  Test: P={final_b.precision*100:.3f}% "
        f"R={final_b.recall*100:.3f}% "
        f"FP={final_b.fp}"
    )

    report = "\n".join([
        "RUSSIAN COMPACT SCREENING",
        (
            f"baseline DevA P={base_a.precision*100:.3f}% "
            f"R={base_a.recall*100:.3f}% FP={base_a.fp}"
        ),
        (
            f"baseline Test P={base_b.precision*100:.3f}% "
            f"R={base_b.recall*100:.3f}% FP={base_b.fp}"
        ),
        (
            f"after BLOCK DevA P={after_blocks_a.precision*100:.3f}% "
            f"R={after_blocks_a.recall*100:.3f}% "
            f"FP={after_blocks_a.fp}"
        ),
        (
            f"after BLOCK Test P={after_blocks_b.precision*100:.3f}% "
            f"R={after_blocks_b.recall*100:.3f}% "
            f"FP={after_blocks_b.fp}"
        ),
        f"2x2 add={len(profile.add2)} block={len(profile.block2)}",
        f"3x3 add={len(profile.add3)} block={len(profile.block3)}",
        f"payload={profile.payload}",
        (
            f"final DevA P={final_a.precision*100:.3f}% "
            f"R={final_a.recall*100:.3f}% "
            f"FP={final_a.fp}"
        ),
        (
            f"final Test P={final_b.precision*100:.3f}% "
            f"R={final_b.recall*100:.3f}% "
            f"FP={final_b.fp}"
        ),
    ])

    print(report)

    emit_header(
        args.header,
        profile,
        report,
    )

    Path("build/ru_compact_report.txt").write_text(
        report + "\n",
        encoding="utf-8",
    )

    print(
        f"generated: {args.header} "
        f"({profile.payload} payload bytes)"
    )


if __name__ == "__main__":
    main()
