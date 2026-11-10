"""The language packs gen_i18n.py writes for the card (.cplang), and the key-hash check that guards them.

    python3 scripts/test_gen_i18n.py

ctest runs it too (test/i18n_lang_pack). Reads the repository's own translations; writes only to a temporary folder.
"""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_i18n  # noqa: E402

TRANSLATIONS = str(Path(__file__).resolve().parent.parent / "lib" / "I18n" / "translations")


class KeyHashes(unittest.TestCase):
    def test_hash_is_fnv1a_of_the_name(self):
        self.assertEqual(gen_i18n.key_hash(""), 0x811C9DC5)
        self.assertEqual(gen_i18n.key_hash("STR_SETTINGS_TITLE"), 0xE88925E1)

    def test_two_names_with_one_hash_fail(self):
        # A real FNV-1a collision: a pack could not tell these two keys' strings apart.
        self.assertEqual(gen_i18n.key_hash("STR_T132789"), gen_i18n.key_hash("STR_T729192"))
        with self.assertRaises(ValueError) as caught:
            gen_i18n.check_key_hashes(["STR_A", "STR_T132789", "STR_T729192"])
        self.assertIn("STR_T132789 and STR_T729192", str(caught.exception))

    def test_the_shipped_keys_do_not_collide(self):
        _, _, keys, _, _ = gen_i18n.load_translations(TRANSLATIONS)
        gen_i18n.check_key_hashes(keys)

    def test_the_layout_stamp_follows_the_key_order(self):
        a = gen_i18n.key_layout_stamp(["STR_A", "STR_B"])
        self.assertNotEqual(a, gen_i18n.key_layout_stamp(["STR_B", "STR_A"]))
        self.assertNotEqual(a, gen_i18n.key_layout_stamp(["STR_A", "STR_B", "STR_C"]))


class Packs(unittest.TestCase):
    def test_a_pack_reads_back_as_written(self):
        keys = ["STR_A", "STR_B", "STR_C", "STR_D"]
        payload, entries = gen_i18n.pack_payload(keys, ["Eins", "B", "", "Vier ü"], ["One", "B", "Three", "Four"])
        # B equals English and C is empty: both read as English, so neither is in the pack.
        self.assertEqual(entries, 2)
        data = gen_i18n.build_pack_file("DE", "Deutsch", payload, entries)
        header, values, ext = gen_i18n.read_pack_file(data)
        self.assertEqual(header["code"], "de")
        self.assertEqual(header["name"], "Deutsch")
        self.assertEqual(values, {gen_i18n.key_hash("STR_A"): "Eins", gen_i18n.key_hash("STR_D"): "Vier ü"})
        self.assertEqual(ext, b"")

    def test_the_header_is_the_documented_one(self):
        payload, entries = gen_i18n.pack_payload(["STR_A"], ["x"], ["y"])
        data = gen_i18n.build_pack_file("fr", "Français", payload, entries, extension=b"\x01" * 100)
        magic, version, _, count, code = struct.unpack_from("<4sBBH4s", data)
        self.assertEqual((magic, version, count, code), (b"CPLG", 1, 1, b"fr\x00\x00"))
        (plen, rawlen, elen, erawlen) = struct.unpack_from("<IIII", data, 16)
        self.assertEqual(rawlen, len(payload))
        self.assertEqual(erawlen, 100)
        self.assertEqual(64 + plen + elen, len(data))
        self.assertEqual(gen_i18n.read_pack_file(data)[2], b"\x01" * 100)

    def test_a_changed_byte_is_refused(self):
        payload, entries = gen_i18n.pack_payload(["STR_A"], ["xyz"], ["y"])
        data = bytearray(gen_i18n.build_pack_file("de", "Deutsch", payload, entries))
        data[-2] ^= 1
        with self.assertRaises(ValueError):
            gen_i18n.read_pack_file(bytes(data))

    def test_family_names(self):
        self.assertEqual(gen_i18n.pack_family_name("german"), "Language-German")
        self.assertEqual(gen_i18n.pack_family_name("portuguese_br"), "Language-PortugueseBR")
        self.assertEqual(gen_i18n.pack_family_name("chinese_simplified"), "Language-ChineseSimplified")

    def test_every_language_but_english_gets_a_pack_and_rewrites_only_what_changed(self):
        packs = gen_i18n.language_packs(TRANSLATIONS)
        names = {family for family, _, _ in packs}
        self.assertIn("Language-German", names)
        self.assertNotIn("Language-English", names)
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(len(gen_i18n.write_language_packs(TRANSLATIONS, tmp)), len(packs))
            self.assertEqual(gen_i18n.write_language_packs(TRANSLATIONS, tmp), [])
            self.assertEqual(gen_i18n.stale_language_packs(TRANSLATIONS, tmp), [])
            german = Path(tmp) / "Language-German" / "de.cplang"
            german.write_bytes(german.read_bytes()[:-1])
            self.assertEqual(gen_i18n.stale_language_packs(TRANSLATIONS, tmp), ["Language-German/de.cplang"])


if __name__ == "__main__":
    unittest.main()
