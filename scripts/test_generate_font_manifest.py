"""The font manifests (generate-font-manifest.py): fonts.json for firmware that reads versions 1 and 2, with the
.cpfont families alone, and fonts-v3.json with every family and its format.

    python3 scripts/test_generate_font_manifest.py

The .cpfont files are made here, as much of each as the generator reads; the outline font is a copy of a fixture.
Needs fontTools and PyYAML (the repo's font tooling).
"""

import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parent / "generate-font-manifest.py"
FIXTURE = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "fonts" / "NotoSansJP-CffFlat.otf"
BASE_URL = "https://example.com/fonts/"


def cpfont(path: Path, styles=(0, 1, 2, 3)) -> Path:
    """A .cpfont's global header and style table, as the generator reads them."""
    path.parent.mkdir(parents=True, exist_ok=True)
    data = struct.pack("<8sHHB19x", b"CPFONT\x00\x00", 4, 0, len(styles))
    for style in styles:
        data += struct.pack("<B31x", style)
    path.write_bytes(data + b"glyphs")
    return path


def outline(path: Path, mac_style=None) -> Path:
    """A copy of the fixture OTF, its head.macStyle set when given (a re-saved copy)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if mac_style is None:
        shutil.copyfile(FIXTURE, path)
    else:
        from fontTools.ttLib import TTFont

        font = TTFont(FIXTURE)
        font["head"].macStyle = mac_style
        font.save(path)
    return path


CONFIG = """families:
  - name: Alpha
    description: "A serif"
    format: cpfont
  - name: NotoSansJP
    description: "Japanese, sans serif (Noto Sans JP). CFF, every size"
    format: otf
    inDownloadAll: false
"""


class Manifests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.fonts = self.root / "sd-fonts"
        cpfont(self.fonts / "Alpha" / "Alpha_10.cpfont")
        cpfont(self.fonts / "Alpha" / "Alpha_12.cpfont")
        self.jp = outline(self.fonts / "NotoSansJP" / "NotoSansJP-Regular.otf")
        self.config = self.root / "sd-fonts.yaml"
        self.config.write_text(CONFIG, encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def run_generator(self, *extra, config=True):
        args = [sys.executable, str(SCRIPT), "--input", str(self.fonts), "--base-url", BASE_URL,
                "--output", str(self.root / "fonts.json"), "--output-v3", str(self.root / "fonts-v3.json")]
        if config:
            args += ["--config", str(self.config)]
        return subprocess.run(args + list(extra), capture_output=True, text=True, encoding="utf-8")

    def manifest(self, name):
        return json.loads((self.root / name).read_text(encoding="utf-8"))

    def test_fonts_json_lists_the_cpfont_families_as_version_2(self):
        self.assertEqual(self.run_generator().returncode, 0)
        v2 = self.manifest("fonts.json")
        self.assertEqual(v2["version"], 2)
        self.assertEqual(v2["baseUrl"], BASE_URL)
        self.assertEqual([f["name"] for f in v2["families"]], ["Alpha"])
        alpha = v2["families"][0]
        self.assertEqual(list(alpha), ["name", "description", "styles", "files"])
        self.assertEqual(alpha["description"], "A serif")
        self.assertEqual(alpha["styles"], ["regular", "bold", "italic", "bolditalic"])
        self.assertEqual([f["name"] for f in alpha["files"]], ["Alpha/Alpha_10.cpfont", "Alpha/Alpha_12.cpfont"])
        text = (self.root / "fonts.json").read_text(encoding="utf-8")
        self.assertNotIn("format", text)
        self.assertNotIn("inDownloadAll", text)

    def test_fonts_v3_lists_every_family_with_its_format(self):
        self.assertEqual(self.run_generator().returncode, 0)
        v3 = self.manifest("fonts-v3.json")
        self.assertEqual(v3["version"], 3)
        families = {f["name"]: f for f in v3["families"]}
        self.assertEqual(sorted(families), ["Alpha", "NotoSansJP"])
        self.assertEqual(families["Alpha"]["format"], "cpfont")
        self.assertNotIn("inDownloadAll", families["Alpha"])
        jp = families["NotoSansJP"]
        data = self.jp.read_bytes()
        size = self.jp.stat().st_size
        self.assertEqual((jp["format"], jp["inDownloadAll"], jp["styles"]), ("otf", False, ["regular"]))
        self.assertEqual(jp["files"], [{"name": "NotoSansJP/NotoSansJP-Regular.otf", "size": size,
                                        "crc32": zlib.crc32(data)}])
        self.assertEqual(list(jp), ["name", "description", "format", "inDownloadAll", "styles", "files"])

    def test_a_folder_the_config_does_not_list_is_reported_and_left_out(self):
        cpfont(self.fonts / "NotoSansCJK" / "NotoSansCJK_12.cpfont")
        run = self.run_generator()
        self.assertEqual(run.returncode, 0)
        self.assertIn("NotoSansCJK", run.stderr)
        for name in ("fonts.json", "fonts-v3.json"):
            self.assertNotIn("NotoSansCJK", [f["name"] for f in self.manifest(name)["families"]])

    def test_without_a_config_every_family_found_is_listed(self):
        # The generator on its own, for a folder of fonts: everything, named by itself.
        cpfont(self.fonts / "Beta" / "Beta_10.cpfont", styles=(0,))
        self.assertEqual(self.run_generator(config=False).returncode, 0)
        self.assertEqual([f["name"] for f in self.manifest("fonts.json")["families"]], ["Alpha", "Beta"])
        v3 = self.manifest("fonts-v3.json")
        self.assertEqual([f["name"] for f in v3["families"]], ["Alpha", "Beta", "NotoSansJP"])
        self.assertEqual(v3["families"][1]["description"], "Beta")

    def test_a_family_whose_files_are_not_its_configured_format_fails(self):
        self.config.write_text(CONFIG.replace('description: "A serif"\n    format: cpfont',
                                              'description: "A serif"\n    format: otf'), encoding="utf-8")
        self.assertEqual(self.run_generator().returncode, 1)

    def test_a_family_of_two_formats_fails(self):
        outline(self.fonts / "NotoSansJP" / "NotoSansJP-Bold.ttf")
        self.assertEqual(self.run_generator().returncode, 1)

    def test_an_outline_files_style_is_its_mac_style(self):
        outline(self.fonts / "NotoSansJP" / "NotoSansJP-Bold.otf", mac_style=1)
        self.assertEqual(self.run_generator().returncode, 0)
        jp = {f["name"]: f for f in self.manifest("fonts-v3.json")["families"]}["NotoSansJP"]
        self.assertEqual(jp["styles"], ["bold", "regular"])  # by file name

    def test_the_same_fonts_give_the_same_bytes(self):
        self.assertEqual(self.run_generator().returncode, 0)
        first = [(self.root / n).read_bytes() for n in ("fonts.json", "fonts-v3.json")]
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertEqual([(self.root / n).read_bytes() for n in ("fonts.json", "fonts-v3.json")], first)
        for data in first:
            self.assertNotIn(b"\r\n", data)


if __name__ == "__main__":
    unittest.main()
