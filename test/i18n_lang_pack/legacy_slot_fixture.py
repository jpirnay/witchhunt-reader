"""A German UI language slot as release 2.40 or 2.41RC left it in flash, for the conversion test (LangPackConvertTest).

Both inflated their in-image pack into the slot: one NUL-terminated entry per StrId in THEIR key order, empty where
the language equals English (gen_i18n.py build_langpack at the tags), and a table derived by walking that blob, an
entry's offset where it has one and 0x8000 | its English offset where it is empty (FlashLangPartition TableBuilder
at the tags). 2.40 put a 32 B format-1 header before them, 2.41RC a 40 B format-2 one with the CRC of the data, then
the table. All of it is repeated here as it stood at the tags. The key order is the release's
(lib/I18n/legacy/<release>.keys); the strings are this tree's, which is what a slot holds that matters, not which
words.

Writes two files: the slot's bytes, and the entries a correct conversion yields for it as a raw pack payload
([u32 key hash][value NUL]...) -- every key the release had a German string for.

    python3 legacy_slot_fixture.py <repo root> <release> <slot out> <expected out>
"""

import struct
import sys
import zlib
from pathlib import Path


def main(repo: Path, release: str, slot_out: Path, expected_out: Path) -> None:
    sys.path.insert(0, str(repo / "scripts"))
    import gen_i18n

    layouts = gen_i18n.load_legacy_layouts(repo / "lib" / "I18n" / "legacy")
    layout = next(l for l in layouts if l["release"] == release)
    keys = layout["keys"]
    english = gen_i18n.parse_yaml_file(str(repo / "lib" / "I18n" / "translations" / "english.yaml"))
    german = gen_i18n.parse_yaml_file(str(repo / "lib" / "I18n" / "translations" / "german.yaml"))
    # A key the release had and this tree dropped still had English text then; any will do.
    en_strings = [english.get(k) or f"English of {k}" for k in keys]
    de_strings = [german.get(k) or en for k, en in zip(keys, en_strings)]

    # build_langpack, at the tags
    blob = bytearray()
    for s, en in zip(de_strings, en_strings):
        if s != en:
            blob += s.encode("utf-8")
        blob += b"\x00"

    # English offsets and TableBuilder::feed, at the tags
    en_offsets = []
    at = 0
    for en in en_strings:
        en_offsets.append(at)
        at += len(en.encode("utf-8")) + 1
    table = []
    pos = 0
    for s, en, en_off in zip(de_strings, en_strings, en_offsets):
        if s != en:
            table.append(pos)
            pos += len(s.encode("utf-8")) + 1
        else:
            table.append(0x8000 | en_off)
            pos += 1

    lang_index = layout["languages"].index("DE")
    table_bytes = (len(keys) * 2 + 3) & ~3
    table_raw = b"".join(struct.pack("<H", t) for t in table).ljust(table_bytes, b"\x00")
    if layout["slot_format"] == 1:
        header = struct.pack("<4sBBHIIII8s", b"CPLP", 1, lang_index, len(keys), 0x00240000, 32, 32 + table_bytes,
                             len(blob), b"\x00" * 8)
    else:
        crc = zlib.crc32(table_raw, zlib.crc32(bytes(blob))) & 0xFFFFFFFF
        header = struct.pack("<4sBBHIIIIIIII", b"CPLP", 2, lang_index, len(keys), 0x00241000, 40, 40 + table_bytes,
                             len(blob), 0, 0, crc, 0)
    slot_out.write_bytes(header + table_raw + bytes(blob))

    expected = bytearray()
    for k, s, en in zip(keys, de_strings, en_strings):
        if s != en:
            expected += struct.pack("<I", gen_i18n.key_hash(k)) + s.encode("utf-8") + b"\x00"
    expected_out.write_bytes(bytes(expected))


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4]))
