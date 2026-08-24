#!/usr/bin/env python3
"""Generate compact German hybrid-hyphenation tables from the DANTE word list.

The generated firmware format contains the existing 2+2 tables and an optional
3+3 correction layer.  The 3+3 layer is trained on buckets 0..7, selected on
bucket 8, and evaluated on held-out bucket 9.
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
SYMBOL_PAD = 31
MARKER_CHARS = frozenset("-=<>·.")
BREAK_MARKER_CHARS = frozenset("-=<>·")
MORPHEME_MARKER_CHARS = frozenset("=<>")
VOWELS = {ord(c) for c in "aeiouyäöüàáâãåèéêëìíîïòóôõùúûýÿ"}
PROTECTED_DIPHTHONGS = {
    (ord("a"), ord("i")), (ord("a"), ord("u")), (ord("ä"), ord("u")),
    (ord("e"), ord("i")), (ord("e"), ord("u")), (ord("o"), ord("i")),
    (ord("i"), ord("e")),
}
PROTECTED_CONSONANT_PAIRS = {
    (ord("c"), ord("h")), (ord("c"), ord("k")), (ord("p"), ord("h")),
    (ord("r"), ord("h")), (ord("s"), ord("h")), (ord("t"), ord("h")),
}

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


def lower_latin(cp: int) -> int:
    if ord("A") <= cp <= ord("Z"):
        return cp + 0x20
    if (0x00C0 <= cp <= 0x00D6) or (0x00D8 <= cp <= 0x00DE):
        return cp + 0x20
    if (0x0100 <= cp <= 0x0137 and cp % 2 == 0) or (
        0x0139 <= cp <= 0x0148 and cp % 2 == 1
    ) or (0x014A <= cp <= 0x0177 and cp % 2 == 0) or (
        0x0179 <= cp <= 0x017E and cp % 2 == 1
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
    if (0x00C0 <= cp <= 0x00D6) or (0x00D8 <= cp <= 0x00F6) or (
        0x00F8 <= cp <= 0x00FF
    ):
        return cp not in (0x00D7, 0x00F7)
    if 0x0100 <= cp <= 0x017F:
        return True
    return cp == 0x1E9E


def is_vowel(cp: int) -> bool:
    return lower_latin(cp) in VOWELS


def vowel_nucleus_end(cps: tuple[int, ...], pos: int) -> int:
    if pos + 1 < len(cps):
        if (lower_latin(cps[pos]), lower_latin(cps[pos + 1])) in PROTECTED_DIPHTHONGS:
            return pos + 2
    return pos + 1


def consonant_unit_length(cps: tuple[int, ...], pos: int, end: int) -> int:
    if pos + 2 < end and (
        lower_latin(cps[pos]), lower_latin(cps[pos + 1]), lower_latin(cps[pos + 2])
    ) == (ord("s"), ord("c"), ord("h")):
        return 3
    if pos + 1 < end and (lower_latin(cps[pos]), lower_latin(cps[pos + 1])) in PROTECTED_CONSONANT_PAIRS:
        return 2
    return 1


def base_breaks(cps: tuple[int, ...]) -> set[int]:
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
    cp = lower_latin(cp)
    if ord("a") <= cp <= ord("z"):
        return cp - ord("a")
    direct = {0x00E4: 26, 0x00F6: 27, 0x00FC: 28, 0x00DF: 29}
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
    ch = fold.get(cp)
    return ord(ch) - ord("a") if ch else SYMBOL_OTHER


def pair_key(cps: tuple[int, ...], boundary: int) -> int | None:
    left, right = german_symbol(cps[boundary - 1]), german_symbol(cps[boundary])
    if left == SYMBOL_OTHER or right == SYMBOL_OTHER:
        return None
    return (left << 5) | right


def context_key(cps: tuple[int, ...], boundary: int) -> int | None:
    if boundary < 2 or boundary + 1 >= len(cps):
        return None
    symbols = tuple(german_symbol(cps[i]) for i in range(boundary - 2, boundary + 2))
    if SYMBOL_OTHER in symbols:
        return None
    key = 0
    for s in symbols:
        key = (key << 5) | s
    return key


def context_key3(cps: tuple[int, ...], boundary: int) -> int | None:
    if boundary < 1 or boundary >= len(cps):
        return None
    symbols = []
    for i in range(boundary - 3, boundary + 3):
        symbols.append(SYMBOL_PAD if i < 0 or i >= len(cps) else german_symbol(cps[i]))
    if SYMBOL_OTHER in symbols:
        return None
    key = 0
    for s in symbols:
        key = (key << 5) | s
    return key


def split_bucket(word: str) -> int:
    return hashlib.sha1(word.casefold().encode("utf-8")).digest()[0] % 10


def parse_annotation(annotation: str) -> tuple[str, dict[int, str]] | None:
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
    legal: set[int] = set(); preferred: set[int] = set(); undesirable: set[int] = set()
    ordinary: set[int] = set(); compound: set[int] = set(); prefix: set[int] = set()
    suffix: set[int] = set(); uncategorized: set[int] = set()
    for boundary, marker in markers.items():
        if boundary < MIN_PREFIX or len(cps) - boundary < MIN_SUFFIX:
            continue
        if not any(ch in BREAK_MARKER_CHARS for ch in marker):
            continue
        if "." in marker:
            undesirable.add(boundary)
            continue
        legal.add(boundary)
        if "-" in marker: ordinary.add(boundary)
        if "=" in marker: compound.add(boundary)
        if "<" in marker: prefix.add(boundary)
        if ">" in marker: suffix.add(boundary)
        if "·" in marker: uncategorized.add(boundary)
        if any(ch in MORPHEME_MARKER_CHARS for ch in marker):
            preferred.add(boundary)
    return WordEntry(plain, cps, frozenset(legal), frozenset(preferred), frozenset(undesirable),
                     frozenset(ordinary), frozenset(compound), frozenset(prefix), frozenset(suffix),
                     frozenset(uncategorized), split_bucket(plain))


def entries(path: Path) -> Iterator[WordEntry]:
    with path.open("r", encoding="utf-8") as src:
        for line in src:
            if not line or line.startswith("#"):
                continue
            entry = parse_word_line(line)
            if entry is not None:
                yield entry


def select_keys(stats: dict[int, list[int]], support: int, confidence: float) -> set[int]:
    return {k for k, (good, total) in stats.items() if total >= support and good / total >= confidence}


def build_base_tables(source: Path, args: argparse.Namespace) -> tuple[set[int], set[int], set[int]]:
    pair_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    add_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    context_stats: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    for entry in entries(source):
        if entry.bucket > 7:
            continue
        base = base_breaks(entry.cps)
        for b in range(MIN_PREFIX, len(entry.cps) - MIN_SUFFIX + 1):
            if b in base:
                pk = pair_key(entry.cps, b)
                if pk is not None:
                    pair_stats[pk][1] += 1
                    pair_stats[pk][0] += int(b in entry.legal)
            else:
                ck = context_key(entry.cps, b)
                if ck is not None:
                    add_stats[ck][1] += 1
                    add_stats[ck][0] += int(b in entry.legal)
        for b in base:
            pk = pair_key(entry.cps, b)
            if pk is not None and pk in select_keys(pair_stats, args.pair_support, args.pair_confidence):
                continue
            ck = context_key(entry.cps, b)
            if ck is not None:
                context_stats[ck][1] += 1
                context_stats[ck][0] += int(b in entry.legal)
    safe_pairs = select_keys(pair_stats, args.pair_support, args.pair_confidence)
    safe_contexts = select_keys(context_stats, args.context_support, args.context_confidence)
    add_contexts = select_keys(add_stats, args.add_support, args.add_confidence)
    return safe_pairs, safe_contexts, add_contexts


def apply_base(entry: WordEntry, safe_pairs: set[int], safe_contexts: set[int], add_contexts: set[int]) -> set[int]:
    base = base_breaks(entry.cps)
    out: set[int] = set()
    for b in range(MIN_PREFIX, len(entry.cps) - MIN_SUFFIX + 1):
        ck = context_key(entry.cps, b)
        if b in base:
            pk = pair_key(entry.cps, b)
            if (pk is not None and pk in safe_pairs) or (ck is not None and ck in safe_contexts):
                out.add(b)
        elif ck is not None and ck in add_contexts:
            out.add(b)
    return out


def collect_3x3_candidates(source: Path, safe_pairs: set[int], safe_contexts: set[int], add_contexts: set[int]):
    block: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    add: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    for entry in entries(source):
        if entry.bucket > 7:
            continue
        baseline = apply_base(entry, safe_pairs, safe_contexts, add_contexts)
        for b in range(MIN_PREFIX, len(entry.cps) - MIN_SUFFIX + 1):
            k = context_key3(entry.cps, b)
            if k is None:
                continue
            if b in baseline:
                block[k][1] += 1
                block[k][0] += int(b not in entry.legal)
            elif b in entry.legal:
                add[k][1] += 1
                add[k][0] += 1
    return block, add


def rank_block(stats, min_support: int, min_conf: float):
    out = []
    for k, (bad, total) in stats.items():
        if total >= min_support and bad / total >= min_conf:
            out.append((k, bad, total, bad / total))
    return sorted(out, key=lambda x: (x[1], x[3], x[2]), reverse=True)


def rank_add(stats, min_support: int, min_conf: float):
    out = []
    for k, (good, total) in stats.items():
        if total >= min_support and good / total >= min_conf:
            out.append((k, good, total, good / total))
    return sorted(out, key=lambda x: (x[1], x[3], x[2]), reverse=True)


def apply_3x3(entry: WordEntry, baseline: set[int], block: set[int], add: set[int]) -> set[int]:
    out = set(baseline)
    for b in range(MIN_PREFIX, len(entry.cps) - MIN_SUFFIX + 1):
        k = context_key3(entry.cps, b)
        if k is None:
            continue
        if k in block:
            out.discard(b)
        elif b not in out and k in add:
            out.add(b)
    return out


def metrics(cases, safe_pairs, safe_contexts, add_contexts, block, add):
    m = {"words":0,"tp":0,"fp":0,"fn":0,"exact":0,"preferred_found":0,"preferred_total":0,"undesirable_used":0, "false_positives":0}
    for e in cases:
        actual = apply_3x3(e, apply_base(e, safe_pairs, safe_contexts, add_contexts), block, add)
        m["words"] += 1
        m["tp"] += len(actual & e.legal); m["fp"] += len(actual - e.legal); m["fn"] += len(e.legal - actual)
        m["exact"] += int(actual == e.legal)
        m["preferred_found"] += len(actual & e.preferred); m["preferred_total"] += len(e.preferred)
        m["undesirable_used"] += len(actual & e.undesirable)
        m["false_positives"] += len(actual & e.false_positives) if hasattr(e, "false_positives") else 0
    p = m["tp"] / (m["tp"] + m["fp"]) if m["tp"] + m["fp"] else 1.0
    r = m["tp"] / (m["tp"] + m["fn"]) if m["tp"] + m["fn"] else 1.0
    f = 2*p*r/(p+r) if p+r else 0.0
    return {**m,"precision":p,"recall":r,"f1":f,"exact":m["exact"]/m["words"],"preferred_recall":m["preferred_found"]/m["preferred_total"] if m["preferred_total"] else 1.0}


def read_cases(source: Path, bucket: int) -> list[WordEntry]:
    return [e for e in entries(source) if e.bucket == bucket]


def pack20(keys):
    out=bytearray()
    for k in sorted(keys): out.extend((k&255,(k>>8)&255,(k>>16)&15))
    return bytes(out)


def pack30(keys):
    out=bytearray()
    for k in sorted(keys): out.extend((k&255,(k>>8)&255,(k>>16)&255,(k>>24)&63))
    return bytes(out)


def pair_bitset(keys):
    out=bytearray(128)
    for k in keys: out[k>>3] |= 1 << (k&7)
    return bytes(out)


def format_bytes(blob: bytes, per_line=16):
    if not blob: return ""
    return "\n".join("    " + ", ".join(f"0x{x:02X}" for x in blob[i:i+per_line]) + "," for i in range(0,len(blob),per_line))


def write_header(path, source, safe_pairs, safe_contexts, add_contexts, block, add):
    sha=hashlib.sha256(source.read_bytes()).hexdigest()
    pb=pair_bitset(safe_pairs); sb=pack20(safe_contexts); ab=pack20(add_contexts)
    bb=pack30(block); xb=pack30(add)
    macro = "#define GERMAN_HYBRID_HAS_3X3 1\n" if (block or add) else ""
    text=f'''#pragma once\n#include <array>\n#include <cstddef>\n#include <cstdint>\n// Generated. Source SHA-256: {sha}\n{macro}inline constexpr std::array<uint8_t, {len(pb)}> kGermanSafePairBits = {{\n{format_bytes(pb)}\n}};\ninline constexpr std::array<uint8_t, {len(sb)}> kGermanSafeContexts = {{\n{format_bytes(sb)}\n}};\ninline constexpr size_t kGermanSafeContextCount = {len(safe_contexts)};\ninline constexpr std::array<uint8_t, {len(ab)}> kGermanAddContexts = {{\n{format_bytes(ab)}\n}};\ninline constexpr size_t kGermanAddContextCount = {len(add_contexts)};\n'''
    if block or add:
        text += f'''\ninline constexpr std::array<uint8_t, {len(bb)}> kGermanBlockContexts3 = {{\n{format_bytes(bb)}\n}};\ninline constexpr size_t kGermanBlockContext3Count = {len(block)};\ninline constexpr std::array<uint8_t, {len(xb)}> kGermanAddContexts3 = {{\n{format_bytes(xb)}\n}};\ninline constexpr size_t kGermanAddContext3Count = {len(add)};\n'''
    path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text,encoding="utf-8")


def pos(values): return ",".join(str(x) for x in sorted(values))


def write_fixture(path, cases):
    path.parent.mkdir(parents=True,exist_ok=True)
    cases=sorted(cases,key=lambda e: hashlib.sha1(("eval:"+e.word.casefold()).encode()).digest())[:5000]
    with path.open("w",encoding="utf-8") as f:
        f.write("# Format: word|legal|preferred|undesirable|ordinary|compound|prefix|suffix|uncategorized\n")
        for e in cases:
            f.write(f"{e.word}|{pos(e.legal)}|{pos(e.preferred)}|{pos(e.undesirable)}|{pos(e.ordinary)}|{pos(e.compound)}|{pos(e.prefix)}|{pos(e.suffix)}|{pos(e.uncategorized)}\n")


def main():
    p=argparse.ArgumentParser()
    p.add_argument("--input",required=True,type=Path); p.add_argument("--output-header",required=True,type=Path); p.add_argument("--output-eval",required=True,type=Path)
    p.add_argument("--pair-support",type=int,default=50); p.add_argument("--pair-confidence",type=float,default=.99)
    p.add_argument("--context-support",type=int,default=3); p.add_argument("--context-confidence",type=float,default=.99)
    p.add_argument("--add-support",type=int,default=3); p.add_argument("--add-confidence",type=float,default=.999)
    p.add_argument("--max-3x3-payload",type=int,default=20000); p.add_argument("--target-precision",type=float,default=.997)
    p.add_argument("--max-3x3-undesirable-delta",type=int,default=0)
    args=p.parse_args()

    safe_pairs,safe_contexts,add_contexts=build_base_tables(args.input,args)
    train3=read_cases(args.input,0)+read_cases(args.input,1)+read_cases(args.input,2)+read_cases(args.input,3)+read_cases(args.input,4)+read_cases(args.input,5)+read_cases(args.input,6)+read_cases(args.input,7)
    validation=read_cases(args.input,8); test=read_cases(args.input,9)
    block_stats,add_stats=collect_3x3_candidates(args.input,safe_pairs,safe_contexts,add_contexts)

    baseline_val=metrics(validation,safe_pairs,safe_contexts,add_contexts,set(),set())
    baseline_test=metrics(test,safe_pairs,safe_contexts,add_contexts,set(),set())
    budget=max(0,args.max_3x3_payload)//4

    profiles=[]
    for bconf in (.99,.995,.999):
        for aconf in (.995,.997,.999,.9995):
            br=rank_block(block_stats,2,bconf); ar=rank_add(add_stats,2,aconf)
            # Reserve up to 100 entries for BLOCK, then use every remaining byte for ADD.
            bsel=br[:min(100,budget)]
            rem=budget-len(bsel); asel=ar[:rem]
            # If fewer blockers are useful, free their slots to ADD.
            if len(bsel) < 100 and len(asel) < rem:
                bsel=br[:min(len(br),budget)]; rem=budget-len(bsel); asel=ar[:rem]
            bs={x[0] for x in bsel}; ads={x[0] for x in asel}
            vm=metrics(validation,safe_pairs,safe_contexts,add_contexts,bs,ads)
            profiles.append((vm["recall"],vm["precision"],vm["undesirable_used"],bs,ads,vm,bconf,aconf))

    safe_profiles=[x for x in profiles if x[1]>=args.target_precision and x[2] <= baseline_val["undesirable_used"] + args.max_3x3_undesirable_delta]
    if safe_profiles:
        chosen=max(safe_profiles,key=lambda x:(x[0],x[1],-x[2]))
    else:
        # Never exceed payload. Prefer highest precision, then recall, then fewer undesirable breaks.
        chosen=max(profiles,key=lambda x:(x[1],x[0],-x[2]))
        print("WARNING: no capped 3+3 profile meets validation constraints; selecting safest capped profile.")
    _,_,_,block,add,vm,bconf,aconf=chosen

    payload=128+3*(len(safe_contexts)+len(add_contexts))+4*(len(block)+len(add))
    print(f"safe pair classes: {len(safe_pairs)} / 1024")
    print(f"safe 2+2 contexts: {len(safe_contexts)}")
    print(f"add 2+2 contexts:  {len(add_contexts)}")
    print(f"3+3 candidates: block={len(block_stats)} add={len(add_stats)}")
    print(f"selected 3+3: block={len(block)} add={len(add)}")
    print(f"3+3 payload: {4*(len(block)+len(add))} bytes / {args.max_3x3_payload} max")
    print(f"generated payload: {payload} bytes (2+2={128+3*(len(safe_contexts)+len(add_contexts))}, 3+3={4*(len(block)+len(add))})")
    print(
        f"validation-selected: "
        f"words={vm['words']} "
        f"precision={vm['precision']*100:.3f}% "
        f"recall={vm['recall']*100:.3f}% "
        f"F1={vm['f1']*100:.3f}% "
        f"exact={vm['exact']*100:.3f}% "
        f"preferred-recall={vm['preferred_recall']*100:.3f}% "
        f"undesirable-used={vm['undesirable_used']} "
        f"false-positives={vm.get('false_positives', 0)}"
    )
    tm=metrics(test,safe_pairs,safe_contexts,add_contexts,block,add)
    print(
        f"test: words={tm['words']} "
        f"precision={tm['precision']*100:.3f}% "
        f"recall={tm['recall']*100:.3f}% "
        f"F1={tm['f1']*100:.3f}% "
        f"exact={tm['exact']*100:.3f}% "
        f"preferred-recall={tm['preferred_recall']*100:.3f}% "
        f"undesirable-used={tm['undesirable_used']} "
        f"false-positives={tm.get('false_positives', 0)}")

    write_header(args.output_header,args.input,safe_pairs,safe_contexts,add_contexts,block,add)
    write_fixture(args.output_eval,test)

if __name__ == "__main__":
    main()
