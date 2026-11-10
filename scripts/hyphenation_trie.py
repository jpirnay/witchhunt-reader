#!/usr/bin/env python3
"""Liang hyphenation tries in Typst hypher's format, built from TeX pattern files.

Ported from Typst hypher (https://github.com/typst/hypher, tests/generate.rs: parse() and TrieBuilder, by Laurenz
Mädje; MIT OR Apache-2.0) at commit d81cc50. It builds the tries hypher ships byte for byte (--check), so
scripts/build_hyphenation_packs.py can make a pack from patterns hypher does not carry.

A trie is hypher's tries/<code>.bin: a 4-byte big-endian root address, the levels tape, then the nodes.

  python3 scripts/hyphenation_trie.py --check <hypher checkout>
      rebuilds every tries/<code>.bin from patterns/ and compares
  python3 scripts/hyphenation_trie.py <patterns.tex> [--cork] [--out <trie.bin>]
      builds one trie; --cork reads an 8-bit Cork (T1) file
"""

import argparse
import pathlib
import re
import sys

# Cork (T1) bytes whose letter is not the Latin-1 one; the rest of 0xC0-0xFF is Latin-1's.
_CORK = {0x8E: "Ő", 0x96: "Ű", 0xAE: "ő", 0xB6: "ű", 0xD7: "Œ", 0xDF: "SS", 0xF7: "œ", 0xFF: "ß"}


def cork_text(raw: bytes) -> str:
    """An 8-bit Cork (T1) TeX file as text: its bytes, and its ^^xx notations, as the letters they stand for."""
    text = "".join(_CORK.get(b, chr(b)) for b in raw)
    return re.sub(r"\^\^([0-9a-f]{2})", lambda m: _CORK.get(int(m[1], 16), chr(int(m[1], 16))), text)


def parse(tex: str):
    """The patterns of a TeX file, as hypher reads them: inside \\patterns{...}, with % comments, nothing else."""
    patterns = []
    i, n = 0, len(tex)
    while i < n:
        c = tex[i]
        i += 1
        if c == "%":
            while i < n and tex[i] != "\n":
                i += 1
        elif c == "\\" and tex.startswith("patterns{", i):
            i += len("patterns{")
            while True:
                start = i
                while i < n and tex[i] != "}" and tex[i] != "%" and not tex[i].isspace():
                    i += 1
                if i > start:
                    patterns.append(tex[start:i])
                if i >= n:
                    break
                c = tex[i]
                i += 1
                if c == "}":
                    break
                if c == "%":
                    while i < n and tex[i] != "\n":
                        i += 1
                else:
                    while i < n and tex[i].isspace():
                        i += 1
    return patterns


class TrieBuilder:
    def __init__(self):
        # A node: [transition bytes, target nodes, (levels offset, count) or None].
        self.nodes = [[[], [], None]]
        self.levels = []  # (distance, level) pairs
        self.root = 0

    def insert(self, pattern: str):
        """A pattern like .a1bc2d."""
        state, dist, levels = 0, 0, []
        for b in pattern.encode("utf-8"):
            if 0x30 <= b <= 0x39:
                levels.append((dist, b - 0x30))
                dist = 0
            else:
                node = self.nodes[state]
                if b in node[0]:
                    state = node[1][node[0].index(b)]
                else:
                    node[0].append(b)
                    node[1].append(len(self.nodes))
                    state = len(self.nodes)
                    self.nodes.append([[], [], None])
                dist += 1
        # Reuse a run of the tape that starts with these levels, else append them.
        offset = 0
        while offset < len(self.levels) and self.levels[offset : offset + len(levels)] != levels:
            offset += 1
        if offset == len(self.levels):
            self.levels.extend(levels)
        self.nodes[state][2] = (offset, len(levels))

    def compress(self):
        """Suffix compression: equal subtrees become one node, numbered in post-order as hypher does."""
        seen, new = {}, []
        sys.setrecursionlimit(max(sys.getrecursionlimit(), 10000))

        def node(i):
            trans, targets, levels = self.nodes[i]
            key = (tuple(trans), tuple(node(t) for t in targets), levels)
            if key not in seen:
                seen[key] = len(new)
                new.append([list(key[0]), list(key[1]), key[2]])
            return seen[key]

        self.root = node(0)
        self.nodes = new

    def encode(self) -> bytes:
        def size(trans, levels, stride):
            return 1 + (len(trans) >= 31) + 2 * (levels is not None) + (1 + stride) * len(trans)

        start = 4 + len(self.levels)
        # Address estimates with the widest stride; the strides they give fix the final addresses.
        estimates, addr = [], start
        for trans, _, levels in self.nodes:
            estimates.append(addr)
            addr += size(trans, levels, 3)
        addrs, strides, addr = [], [], start
        for i, (trans, targets, levels) in enumerate(self.nodes):
            stride = max((_bytes_for(estimates[t] - estimates[i]) for t in targets), default=1)
            addrs.append(addr)
            strides.append(stride)
            addr += size(trans, levels, stride)

        out = bytearray(addrs[self.root].to_bytes(4, "big"))
        for dist, level in self.levels:
            assert dist <= 24 and level < 10, "level out of range"
            out.append(dist * 10 + level)
        for (trans, targets, levels), addr, stride in zip(self.nodes, addrs, strides):
            out.append((levels is not None) << 7 | stride << 5 | min(len(trans), 31))
            if len(trans) >= 31:
                out.append(len(trans))
            if levels is not None:
                offset = 4 + levels[0]
                assert offset < 4096 and levels[1] < 16, "levels out of range"
                out += bytes([offset >> 4, (offset & 15) << 4 | levels[1]])
            out += bytes(trans)
            for t in targets:
                delta = addrs[t] - addr
                if stride == 3:  # biased by 2^23, not two's complement
                    out += (delta + (1 << 23)).to_bytes(3, "big")
                else:
                    out += delta.to_bytes(stride, "big", signed=True)
        return bytes(out)


def _bytes_for(num: int) -> int:
    if -128 <= num < 128:
        return 1
    if -32768 <= num < 32768:
        return 2
    if -(1 << 23) <= num < (1 << 23):
        return 3
    raise ValueError("too large a distance")


def build(patterns) -> bytes:
    """A trie of `patterns`, as hypher's tries/<code>.bin."""
    builder = TrieBuilder()
    for pattern in patterns:
        builder.insert(pattern)
    builder.compress()
    return builder.encode()


def check(hypher: pathlib.Path) -> int:
    """Rebuilds every trie hypher ships from its patterns. 0 when all are byte-identical."""
    table = (hypher / "tests/generate.rs").read_text(encoding="utf-8")
    languages = re.findall(r'\(\s*"[^"]+",\s*"([a-z]{2})",\s*&\[[^\]]*\],\s*"\w+",\s*"([^"]+\.tex)"', table)
    if not languages:
        print("no languages found in tests/generate.rs", file=sys.stderr)
        return 1
    failed = 0
    for code, tex in languages:
        trie = build(parse((hypher / "patterns" / tex).read_text(encoding="utf-8")))
        same = trie == (hypher / "tries" / f"{code}.bin").read_bytes()
        failed += not same
        print(f"{code} {tex}: {len(trie):,} B {'identical' if same else 'DIFFERENT'}")
    print(f"{len(languages) - failed} of {len(languages)} identical")
    return 1 if failed else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("patterns", nargs="?", type=pathlib.Path)
    parser.add_argument("--cork", action="store_true", help="the patterns file is 8-bit Cork (T1)")
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--check", type=pathlib.Path, metavar="HYPHER")
    args = parser.parse_args(argv)
    if args.check:
        return check(args.check)
    if not args.patterns:
        parser.error("a patterns file or --check")
    raw = args.patterns.read_bytes()
    trie = build(parse(cork_text(raw) if args.cork else raw.decode("utf-8")))
    if args.out:
        args.out.write_bytes(trie)
    print(f"{args.patterns.name}: {len(trie):,} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
