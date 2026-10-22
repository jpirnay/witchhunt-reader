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
