"""build-sd-fonts.py: the pinned downloads, the `format: otf` dispatch and the real sd-fonts.yaml's CJK entries.

    python3 lib/EpdFont/scripts/test_build_sd_fonts.py

Needs fontTools and PyYAML (requirements.txt). No network: the build test cuts the Noto Sans JP fixture.
"""

import importlib.util
import os
import re
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import yaml

sys.dont_write_bytecode = True
SCRIPTS = Path(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, str(SCRIPTS))

import fontTools  # noqa: E402
from fontTools.ttLib import TTFont  # noqa: E402

import cjk_subset  # noqa: E402

REPO = SCRIPTS.parent.parent.parent
FIXTURE = REPO / "test" / "fixtures" / "fonts" / "NotoSansJP-Cff.otf"
OFL_NOTICE = "This Font Software is licensed under the SIL Open Font License, Version 1.1."

spec = importlib.util.spec_from_file_location("build_sd_fonts", SCRIPTS / "build-sd-fonts.py")
build_sd_fonts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_sd_fonts)


class PinnedDownloads(unittest.TestCase):
    def test_a_cached_source_must_have_its_pinned_sha256(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "x.otf"
            dest.write_bytes(b"font")
            url = "https://example.invalid/x.otf"
            self.assertEqual(build_sd_fonts.download_font(url, dest, build_sd_fonts.sha256_of(dest)), dest)
            with self.assertRaises(RuntimeError) as raised:
                build_sd_fonts.download_font(url, dest, "0" * 64)
            self.assertIn(url, str(raised.exception))


class OtfFamilies(unittest.TestCase):
    def test_an_otf_entry_is_built_by_cjk_subset(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            source = tmp / "source.otf"
            shutil.copy(FIXTURE, source)
            font = TTFont(str(source), recalcTimestamp=False)
            font["name"].setName(OFL_NOTICE, 13, 3, 1, 0x409)
            font.save(str(source), reorderTables=False)
            cmap = set(TTFont(str(source)).getBestCmap())
            unihan = tmp / "Unihan.zip"
            unihan.write_bytes(b"not read: family_set is patched")
            family = {"name": "TestSansJP", "format": "otf", "charset": "ja", "fontName": "Test Sans JP",
                      "source": {"path": str(source)}}
            with mock.patch.object(cjk_subset, "family_set", return_value=cmap):
                name, ok, message = build_sd_fonts.build_family(
                    family, tmp / "out", unihan=unihan, fonttools_pin=fontTools.version)
            self.assertEqual((name, ok), ("TestSansJP", True), message)
            self.assertIn("characters", message)
            self.assertTrue((tmp / "out" / "TestSansJP" / "TestSansJP-Regular.otf").is_file())


class SdFontsYaml(unittest.TestCase):
    def test_sd_fonts_yaml_pins_every_otf_family(self):
        text = (SCRIPTS / "sd-fonts.yaml").read_text(encoding="utf-8")
        config = yaml.safe_load(text)
        otf = [f for f in config["families"] if f.get("format") == "otf"]
        self.assertEqual([f["name"] for f in otf], ["NotoSansJP", "NotoSerifJP", "NotoSansSC", "NotoSerifSC",
                                                    "NotoSansTC", "NotoSerifTC", "NotoSansKR", "NotoSerifKR"])
        for f in otf:
            self.assertIn(f["charset"], cjk_subset.CHARSETS)
            self.assertIs(f["inDownloadAll"], False)
            self.assertLessEqual(len(f["description"]), 32)
            self.assertEqual(f["fontName"].replace(" ", ""), f["name"])
            self.assertRegex(f["source"]["url"],
                             r"^https://github\.com/notofonts/noto-cjk/raw/(Sans2\.004/Sans|Serif2\.003/Serif)/OTF/")
            self.assertRegex(f["source"]["sha256"], r"^[0-9a-f]{64}$")
        self.assertTrue(config["unihan"]["url"].endswith("/17.0.0/ucd/Unihan.zip"))
        self.assertRegex(config["unihan"]["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(config["fonttools"], "4.63.0")
        # the bitmap entries, commented out or not (the source URLs name NotoSansCJKjp... and stay)
        self.assertNotRegex(text, r"(?m)^\s*#?\s*-?\s*name:\s*(NotoSansCJK|NotoSerifCJK|NotoSansHangul)\s*$")


TTF_SCRIPTS = ["Devanagari", "Bengali", "Gurmukhi", "Gujarati", "Oriya", "Tamil", "Telugu", "Kannada", "Malayalam",
               "Sinhala"]
TTF_PIN = ("https://raw.githubusercontent.com/notofonts/notofonts.github.io/"
           "5c82cef0a8f08806f932062e18c02be62be278f0/fonts/")


def ttf_entries():
    config = yaml.safe_load((SCRIPTS / "sd-fonts.yaml").read_text(encoding="utf-8"))
    return [f for f in config["families"] if f.get("format") == "ttf"]


class TtfFamilies(unittest.TestCase):
    def test_the_ttf_families_are_the_twenty_indic_ones(self):
        entries = ttf_entries()
        self.assertEqual(sorted(f["name"] for f in entries),
                         sorted(f"Noto{kind}{x}" for kind in ("Sans", "Serif") for x in TTF_SCRIPTS))
        for f in entries:
            self.assertIs(f["inDownloadAll"], False, f["name"])
            self.assertTrue(f["description"], f["name"])
            kind = "NotoSans" if f["name"].startswith("NotoSans") else "NotoSerif"
            for part in ("script", "latin"):
                self.assertEqual(sorted(f["merge"][part]), ["Bold", "Regular"], f["name"])
                for style, source in f["merge"][part].items():
                    self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$", f"{f['name']} {part} {style}")
                    family = f["name"] if part == "script" else kind
                    self.assertTrue(source["url"].endswith(f"/{family}/unhinted/ttf/{family}-{style}.ttf"),
                                    source["url"])

    def test_ttf_sources_are_pinned_at_one_commit(self):
        for f in ttf_entries():
            for part in f["merge"].values():
                for source in part.values():
                    self.assertTrue(source["url"].startswith(TTF_PIN), source["url"])

    @unittest.skipUnless(fontTools.version == "4.63.0", "the merge is pinned to fontTools 4.63.0")
    def test_the_merge_keeps_glyph_ids_and_moves_latin_letters(self):
        import merge_noto_latin
        from fontTools.fontBuilder import FontBuilder
        from fontTools.pens.ttGlyphPen import TTGlyphPen

        def tiny(path, names, cmap, advance):
            fb = FontBuilder(1000, isTTF=True)
            fb.setupGlyphOrder([".notdef"] + names)
            fb.setupCharacterMap(cmap)
            pen = TTGlyphPen(None)
            pen.moveTo((0, 0))
            pen.lineTo((0, 100))
            pen.lineTo((100, 100))
            pen.closePath()
            glyph = pen.glyph()
            fb.setupGlyf({n: glyph for n in [".notdef"] + names})
            fb.setupHorizontalMetrics({n: (advance, 0) for n in [".notdef"] + names})
            fb.setupHorizontalHeader(ascent=800, descent=-200)
            fb.setupNameTable({"familyName": "T", "styleName": "Regular"})
            fb.setupOS2()
            fb.setupPost()
            fb.save(str(path))

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            script, latin = tmp / "script.ttf", tmp / "latin.ttf"
            tiny(script, ["A", "ka"], {0x41: "A", 0x915: "ka"}, 500)
            tiny(latin, [chr(c) for c in range(0x41, 0x5B)], {c: chr(c) for c in range(0x41, 0x5B)}, 700)
            out = tmp / "out.ttf"
            merge_noto_latin.merge(script, latin, out)
            merged = TTFont(str(out))
            cmap = merged.getBestCmap()
            order = merged.getGlyphOrder()
            script_order = TTFont(str(script)).getGlyphOrder()
            self.assertEqual(order.index(cmap[0x915]), script_order.index("ka"))
            self.assertGreaterEqual(order.index(cmap[0x41]), len(script_order))
            self.assertEqual(merged["hmtx"][cmap[0x41]][0], 700)
            self.assertNotIn("vhea", merged)
            self.assertNotIn("vmtx", merged)
            first = out.read_bytes()
            time.sleep(1.1)  # head.modified was once stamped by the second
            merge_noto_latin.merge(script, latin, out)
            self.assertEqual(out.read_bytes(), first)


class HostedFontsLicense(unittest.TestCase):
    def test_the_ofl_names_every_copyright_the_hosted_fonts_carry(self):
        fonts = REPO / "assets" / "sd-fonts"
        ofl = (fonts / "OFL.txt").read_text(encoding="utf-8")
        files = sorted(fonts.glob("*/*.otf"))
        self.assertEqual(len(files), 8)
        for path in files:
            font = TTFont(str(path))
            notice = font["name"].getDebugName(0)
            years = re.search(r"\d{4}-\d{4}", notice)
            self.assertIsNotNone(years, f"{path.name}: {notice}")
            self.assertIn(years.group(0), ofl, path.name)
        self.assertIn("Copyright 2022 The Noto Project Authors", ofl)


if __name__ == "__main__":
    unittest.main()
