#!/usr/bin/env python3
"""Build the SD-card hyphenation packs (hyph-<code>.cphyph) the Font Manager offers.

Each pack goes to <out>/Hyphenation-<Language>/hyph-<code>.cphyph (by default assets/sd-fonts, where
generate-font-manifest.py lists it in fonts-v3.json with format "hyphenation"; sd-fonts.yaml names each folder).

- The nine languages whose tries were compiled in before packs: the payload is exactly the bytes of
  lib/Epub/Epub/hyphenation/generated/hyph-<code>.trie.h, so a pack hyphenates exactly as the built-in table did.
- The languages added with packs: straight from Typst hypher's tries/<code>.bin (their 4-byte big-endian root
  prefix stripped, as generate_hyphenation_trie.py does for the headers), with --hypher-tries. The packs in the repo
  were built from hypher at commit d81cc50.
- The languages whose patterns hypher does not carry: from a TeX pattern file in assets/hyphenation-patterns/, built
  into a hypher trie by hyphenation_trie.py (Hungarian: Huhyphn v20031107, GPL-2, see its README.md).

The file layout is CPHY v1 from crosspoint-reader PR #3706 ("feat: hyphenation manager", Uri Tauber), kept byte
compatible so packs move between CrossPoint, CrossInk and this firmware. See HyphenationPack.h for the layout.

  python3 scripts/build_hyphenation_packs.py [--out <dir>] [--hypher-tries <hypher>/tries]

Without --hypher-tries only the nine header languages and the "tex" ones are built.
"""

import argparse
import pathlib
import re
import struct
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hyphenation_trie  # noqa: E402  (beside this script)

ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATED = ROOT / "lib/Epub/Epub/hyphenation/generated"
DEFAULT_OUT = ROOT / "assets/sd-fonts"
# code: (folder language, source, (lefthyphenmin, righthyphenmin)). The minima are LanguageRegistry.cpp's: a pack whose
# minima disagree is refused by the firmware. English stays in the image.
LANGUAGES = {
    "de": ("German", "header", (2, 2)),
    "es": ("Spanish", "header", (2, 2)),
    "fr": ("French", "header", (2, 2)),
    "it": ("Italian", "header", (2, 2)),
    "pl": ("Polish", "header", (2, 2)),
    "pt": ("Portuguese", "header", (2, 2)),
    "ru": ("Russian", "header", (2, 2)),
    "sv": ("Swedish", "header", (2, 2)),
    "uk": ("Ukrainian", "header", (2, 2)),
    "nl": ("Dutch", "hypher", (2, 2)),
    "cs": ("Czech", "hypher", (2, 2)),
    "da": ("Danish", "hypher", (2, 2)),
    "sl": ("Slovenian", "hypher", (2, 2)),
    "lt": ("Lithuanian", "hypher", (2, 2)),
    "ca": ("Catalan", "hypher", (2, 2)),
    "be": ("Belarusian", "hypher", (2, 2)),
    "fi": ("Finnish", "hypher", (2, 2)),
    "tr": ("Turkish", "hypher", (2, 2)),
    "as": ("Assamese", "hypher", (2, 2)),
    "bn": ("Bengali", "hypher", (2, 2)),
    "gu": ("Gujarati", "hypher", (2, 2)),
    "hi": ("Hindi", "hypher", (2, 2)),
    "kn": ("Kannada", "hypher", (2, 2)),
    "ml": ("Malayalam", "hypher", (2, 2)),
    "mr": ("Marathi", "hypher", (2, 2)),
    "or": ("Odia", "hypher", (2, 2)),
    "pa": ("Punjabi", "hypher", (2, 2)),
    "sa": ("Sanskrit", "hypher", (2, 2)),
    "ta": ("Tamil", "hypher", (2, 2)),
    "te": ("Telugu", "hypher", (2, 2)),
    "hu": ("Hungarian", "tex", (2, 2)),
}
# The "tex" languages' pattern files, and whether each is 8-bit Cork (T1) rather than UTF-8.
PATTERN_FILES = {
    "hu": (ROOT / "assets/hyphenation-patterns/hu/huhyphen.tex", True),
}
# The languages whose tries the image used to carry.
CODES = tuple(code for code, (_, source, _) in LANGUAGES.items() if source == "header")
MINIMA = {code: minima for code, (_, _, minima) in LANGUAGES.items()}


def folder(code: str) -> str:
    """The Font Manager family a pack is listed under, and its folder: Hyphenation-German."""
    return f"Hyphenation-{LANGUAGES[code][0]}"


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


def trie_from_patterns(code: str):
    """(payload, root offset) of a "tex" language's pattern file, as hypher would build it."""
    path, cork = PATTERN_FILES[code]
    raw = path.read_bytes()
    blob = hyphenation_trie.build(hyphenation_trie.parse(hyphenation_trie.cork_text(raw) if cork else raw.decode()))
    root = int.from_bytes(blob[:4], "big")
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
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    parser.add_argument("--hypher-tries", type=pathlib.Path, help="hypher's tries/ directory, for the languages it adds")
    args = parser.parse_args(argv)

    built = []
    for code, (_, source, minima) in LANGUAGES.items():
        if source == "header":
            payload, root = trie_from_header(code)
        elif source == "tex":
            payload, root = trie_from_patterns(code)
        elif args.hypher_tries:
            payload, root = trie_from_hypher(args.hypher_tries / f"{code}.bin")
        else:
            continue
        data = pack(code, payload, root, minima)
        target = args.out / folder(code) / f"hyph-{code}.cphyph"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        built.append((target.relative_to(args.out), len(data)))
    for path, size in built:
        print(f"{path} {size:,} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
