#!/usr/bin/env python3
"""Build the SD-card hyphenation packs (hyph-<code>.cphyph) from the generated trie headers.

The payload is exactly the bytes of lib/Epub/Epub/hyphenation/generated/hyph-<code>.trie.h, the trie the firmware
compiled in before packs, so a pack hyphenates exactly as the built-in table did.

The file layout is CPHY v1 from crosspoint-reader PR #3706 ("feat: hyphenation manager", Uri Tauber), kept byte
compatible so packs move between CrossPoint, CrossInk and this firmware. See HyphenationPack.h for the layout.

  python3 scripts/build_hyphenation_packs.py --out <dir> [--hypher-tries <dir> --codes nl,cs]

--hypher-tries builds packs for further languages straight from Typst hypher's tries/<code>.bin (their 4-byte
big-endian root prefix stripped, as generate_hyphenation_trie.py does for the headers).
"""

import argparse
import pathlib
import re
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATED = ROOT / "lib/Epub/Epub/hyphenation/generated"
# The languages whose tries used to be compiled in; English stays in the image.
CODES = ("de", "es", "fr", "it", "pl", "pt", "ru", "sv", "uk")
# lefthyphenmin / righthyphenmin per language, as LanguageRegistry.cpp holds them: a pack whose minima disagree is
# refused by the firmware.
MINIMA = {code: (2, 2) for code in CODES}
HEADER = struct.Struct("<4sB2sBBBHIII")
MAX_PAYLOAD = 512 * 1024


def trie_from_header(code: str, root: pathlib.Path = GENERATED):
    """(payload, root offset) of hyph-<code>.trie.h."""
    source = (root / f"hyph-{code}.trie.h").read_text(encoding="utf-8")
    array = re.search(rf"{code}_trie_data\[\]\s*=\s*\{{(.*?)\}};", source, re.S)
    descriptor = re.search(rf"{code}_patterns\s*=\s*\{{\s*(0x[0-9A-Fa-f]+)u,", source)
    if not array or not descriptor:
        raise ValueError(f"unrecognised generated trie: hyph-{code}.trie.h")
    payload = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})\b", array[1]))
    return payload, int(descriptor[1], 16)


def trie_from_hypher(path: pathlib.Path):
    """(payload, root offset) of a hypher tries/<code>.bin: a 4-byte big-endian root address, then the trie."""
    blob = path.read_bytes()
    if len(blob) < 5:
        raise ValueError(f"{path}: too small")
    root = int.from_bytes(blob[:4], "big")
    if root < 4 or root >= len(blob):
        raise ValueError(f"{path}: root {root} outside the blob")
    return blob[4:], root - 4


def pack(code: str, payload: bytes, root_offset: int, minima=(2, 2)) -> bytes:
    if not re.fullmatch(r"[a-z]{2}", code):
        raise ValueError(f"not a two-letter code: {code!r}")
    if not payload or len(payload) > MAX_PAYLOAD or root_offset >= len(payload):
        raise ValueError(f"{code}: payload of {len(payload)} B with root {root_offset}")
    header = HEADER.pack(b"CPHY", 1, code.encode("ascii"), minima[0], minima[1], 0, 0, root_offset, len(payload),
                         zlib.crc32(payload) & 0xFFFFFFFF)
    return header + payload


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--hypher-tries", type=pathlib.Path, help="hypher's tries/ directory, for --codes")
    parser.add_argument("--codes", default="", help="comma-separated codes to build from --hypher-tries")
    args = parser.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    built = []
    for code in CODES:
        payload, root = trie_from_header(code)
        data = pack(code, payload, root, MINIMA[code])
        (args.out / f"hyph-{code}.cphyph").write_bytes(data)
        built.append((code, len(data)))
    if args.codes:
        if not args.hypher_tries:
            parser.error("--codes needs --hypher-tries")
        for code in filter(None, args.codes.split(",")):
            payload, root = trie_from_hypher(args.hypher_tries / f"{code}.bin")
            data = pack(code, payload, root)
            (args.out / f"hyph-{code}.cphyph").write_bytes(data)
            built.append((code, len(data)))
    for code, size in built:
        print(f"hyph-{code}.cphyph {size:,} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
