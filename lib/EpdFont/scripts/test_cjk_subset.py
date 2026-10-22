"""The CJK CFF subsets (cjk_subset.py): the character sets, the subset build and its checks.

    python3 lib/EpdFont/scripts/test_cjk_subset.py

Needs fontTools (requirements.txt). The Build tests cut the Noto Sans JP fixture (test/fixtures/fonts) and the
character-set tests make their own small Unihan, so neither needs the Noto CJK sources or the network; the real
set sizes are checked when the pinned Unihan is cached.
"""

import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
SCRIPTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPTS)

import fontTools  # noqa: E402
from fontTools.fontBuilder import FontBuilder  # noqa: E402
from fontTools.pens.boundsPen import ControlBoundsPen  # noqa: E402
from fontTools.pens.recordingPen import RecordingPen  # noqa: E402
from fontTools.pens.t2CharStringPen import T2CharStringPen  # noqa: E402
from fontTools.ttLib import TTFont  # noqa: E402

import cjk_subset  # noqa: E402

REPO = Path(SCRIPTS).parent.parent.parent
FIXTURES = REPO / "test" / "fixtures" / "fonts"
PINNED_UNIHAN = Path(SCRIPTS) / "downloaded_fonts" / "Unihan" / "Unihan-17.0.0.zip"
OFL_NOTICE = "This Font Software is licensed under the SIL Open Font License, Version 1.1."


def unihan_zip(path: Path, lines: list[str]) -> Path:
    """A Unihan.zip whose Unihan_OtherMappings.txt holds `lines` (code point, field, value; tab-separated)."""
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("Unihan_OtherMappings.txt", "# test\n" + "\n".join(lines) + "\n")
    return path


class CharacterSets(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.empty = unihan_zip(Path(self.dir.name) / "empty.zip", [])

    def tearDown(self):
        self.dir.cleanup()

    def test_unihan_fields_give_their_code_points(self):
        z = unihan_zip(Path(self.dir.name) / "u.zip", [
            "U+20B9F\tkJoyoKanji\t2010", "U+53F1\tkJoyoKanji\tU+20B9F", "U+4E00\tkTGH\t2013:1",
            "U+5000\tkOtherField\tx"])
        self.assertEqual(cjk_subset.unihan_field(z, "kJoyoKanji"), {0x20B9F, 0x53F1})
        self.assertEqual(cjk_subset.unihan_field(z, "kTGH"), {0x4E00})
        self.assertEqual(cjk_subset.unihan_field(z, "kJinmeiyoKanji"), set())

    def test_japanese_is_jis_x_0208_the_nec_row_and_the_joyo_and_jinmeiyo_kanji(self):
        z = unihan_zip(Path(self.dir.name) / "u.zip", ["U+20B9F\tkJoyoKanji\t2010", "U+5861\tkJinmeiyoKanji\t2010"])
        ja = cjk_subset.charset("ja", z)
        for cp in (0x3042, 0x30A2, 0x4E9C, 0x7199):  # あ ア 亜 熙: JIS X 0208's kana and both levels of kanji
            self.assertIn(cp, ja)
        self.assertIn(0x2460, ja)  # ①, cp932 row 13
        self.assertIn(0x2160, ja)  # Ⅰ, cp932 row 13
        self.assertIn(0x20B9F, ja)  # 𠮟: Joyo since 2010, beyond JIS X 0208
        self.assertIn(0x5861, ja)
        self.assertNotIn(0x20B9F, cjk_subset.charset("ja", self.empty))
        self.assertGreater(len(cjk_subset.charset("ja", self.empty)), 6800)

    def test_simplified_chinese_is_gb2312_and_the_general_standard_table(self):
        z = unihan_zip(Path(self.dir.name) / "u.zip", ["U+5570\tkTGH\t2013:3"])
        self.assertIn(0x4E2D, cjk_subset.charset("zh-Hans", self.empty))  # 中
        self.assertNotIn(0x5570, cjk_subset.charset("zh-Hans", self.empty))  # 啰: not in GB2312
        self.assertIn(0x5570, cjk_subset.charset("zh-Hans", z))
        self.assertGreater(len(cjk_subset.charset("zh-Hans", self.empty)), 7400)

    def test_traditional_chinese_is_big5_and_cp950(self):
        zh = cjk_subset.charset("zh-Hant", self.empty)
        self.assertIn(0x4E2D, zh)
        self.assertIn(0x20AC, zh)  # €: cp950 has it, Big5 does not
        self.assertGreater(len(zh), 13000)

    def test_korean_is_every_hangul_syllable_and_all_of_ks_x_1001(self):
        ko = cjk_subset.charset("ko", self.empty)
        self.assertTrue(all(cp in ko for cp in range(0xAC00, 0xD7A4)))
        self.assertIn(0x3131, ko)  # ㄱ, compatibility jamo
        self.assertIn(0x4E00, ko)  # 一: KS X 1001's Hanja
        self.assertIn(0x3260, ko)  # ㉠: its symbol rows
        self.assertIn(0x0391, ko)  # Α: Greek

    def test_a_family_holds_base_without_the_gaps_noto_cjk_has(self):
        ja = cjk_subset.family_set("ja", self.empty)
        self.assertTrue(cjk_subset.BASE - cjk_subset.BASE_GAPS <= ja)
        self.assertNotIn(0x203D, ja)  # a BASE gap
        self.assertNotIn(0x2252, ja)  # a Japanese gap
        self.assertNotIn(cjk_subset.REPLACEMENT, ja)  # copied in from Noto Sans, not cut from the source
        self.assertTrue(cjk_subset.CHARSET_GAPS["ko"].isdisjoint(cjk_subset.family_set("ko", self.empty)))

    def test_an_unknown_charset_fails(self):
        with self.assertRaises(cjk_subset.BuildError):
            cjk_subset.charset("vi", self.empty)

    def test_the_gap_lists_are_the_measured_ones(self):
        self.assertEqual(len(cjk_subset.BASE_GAPS), 39)
        self.assertEqual(cjk_subset.CHARSET_GAPS["ja"], {0x2252})
        self.assertEqual(cjk_subset.CHARSET_GAPS["zh-Hant"], {0x02CD, 0x203E, 0x223C, 0x2252})
        self.assertEqual(len(cjk_subset.CHARSET_GAPS["ko"]), 34)
        self.assertEqual(cjk_subset.CHARSET_GAPS["zh-Hans"], set())

    @unittest.skipUnless(PINNED_UNIHAN.exists(), "the pinned Unihan is not cached")
    def test_the_real_sets_have_their_measured_sizes(self):
        sizes = {name: len(cjk_subset.family_set(name, PINNED_UNIHAN)) for name in cjk_subset.CHARSETS}
        self.assertEqual(sizes, {"ja": 7367, "zh-Hans": 9214, "zh-Hant": 14052, "ko": 17304})


class Build(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name)
        self.source = self.path / "source.otf"
        shutil.copy(FIXTURES / "NotoSansJP-Cff.otf", self.source)
        font = TTFont(str(self.source), recalcTimestamp=False)
        font["name"].setName(OFL_NOTICE, 13, 3, 1, 0x409)
        font.save(str(self.source), reorderTables=False)
        self.CODEPOINTS = set(TTFont(str(self.source)).getBestCmap())

    def tearDown(self):
        self.dir.cleanup()

    def build(self, name="out.otf", codepoints=None):
        out = self.path / name
        report = cjk_subset.subset_cjk(self.source, set(codepoints or self.CODEPOINTS), "Test Sans JP", out)
        return out, report

    def test_the_subset_is_cff_without_subroutines(self):
        out, _ = self.build()
        self.assertEqual(out.read_bytes()[:4], b"OTTO")
        cff = TTFont(str(out))["CFF "].cff
        self.assertEqual(cff.major, 1)
        top = cff.topDictIndex[0]
        self.assertIn("ROS", top.rawDict)
        self.assertEqual(len(cff.GlobalSubrs), 0)
        for fd in top.FDArray:
            self.assertFalse(hasattr(fd.Private, "Subrs"))

    def test_only_kern_is_kept_and_the_listed_tables_are_dropped(self):
        out, report = self.build()
        font = TTFont(str(out))
        self.assertLessEqual(set(font.keys()) - {"GlyphOrder"}, cjk_subset.KEPT_TABLES)
        features = {r.FeatureTag for r in font["GPOS"].table.FeatureList.FeatureRecord}
        self.assertEqual(features, {"kern"})
        self.assertEqual(set(report["dropped"]), {"GSUB", "BASE", "vhea", "vmtx", "VORG"})
        self.assertTrue(all(size > 0 for size in report["dropped"].values()))

    def test_no_name_claims_to_be_noto_cjk(self):
        out, _ = self.build()
        font = TTFont(str(out))
        name = font["name"]
        self.assertEqual(name.getDebugName(1), "Test Sans JP")
        self.assertEqual(name.getDebugName(2), "Regular")
        self.assertEqual(name.getDebugName(4), "Test Sans JP")
        self.assertEqual(name.getDebugName(6), "TestSansJP-Regular")
        self.assertIn("TestSansJP-Regular", name.getDebugName(3))
        for name_id in (16, 17, 21, 22):
            self.assertIsNone(name.getDebugName(name_id))
        self.assertEqual(name.getDebugName(13), OFL_NOTICE)
        cff = font["CFF "].cff
        top = cff.topDictIndex[0]
        self.assertEqual(cff.fontNames, ["TestSansJP-Regular"])
        self.assertEqual(top.FamilyName, "Test Sans JP")
        self.assertEqual(top.FullName, "Test Sans JP Regular")
        names = [r.toUnicode() for r in name.names] + cff.fontNames + [top.FamilyName, top.FullName]
        for fd in top.FDArray:
            self.assertTrue(fd.FontName.startswith("TestSansJP-Regular-"), fd.FontName)
            names.append(fd.FontName)
        self.assertFalse([n for n in names if "CJK" in n])

    def test_the_replacement_glyph_is_noto_sans_as_a_cff_charstring(self):
        out, report = self.build()
        font = TTFont(str(out))
        top = font["CFF "].cff.topDictIndex[0]
        donor = TTFont(str(cjk_subset.REPLACEMENT_DONOR))
        donor_name = donor.getBestCmap()[cjk_subset.REPLACEMENT]
        donor_pen = ControlBoundsPen(donor.getGlyphSet())
        donor.getGlyphSet()[donor_name].draw(donor_pen)
        self.assertEqual(donor_pen.bounds, (41, -156, 958, 760))
        unicode_tables = [t for t in font["cmap"].tables if t.isUnicode()]
        self.assertTrue(unicode_tables)
        glyph_set = font.getGlyphSet()
        for table in unicode_tables:
            name = table.cmap[cjk_subset.REPLACEMENT]
            recording = RecordingPen()
            glyph_set[name].draw(recording)
            self.assertEqual(sum(1 for op, _ in recording.value if op == "moveTo"), 3)
            advance, lsb = font["hmtx"][name]
            self.assertEqual(advance, 1000)
            self.assertLessEqual(abs(lsb - 41), 1)
            bounds = ControlBoundsPen(glyph_set)
            glyph_set[name].draw(bounds)
            for got, want in zip(bounds.bounds, donor_pen.bounds):
                self.assertLessEqual(abs(got - want), 1)
            latin = font.getBestCmap()[0x41]
            self.assertEqual(top.FDArray[top.FDSelect[font.getGlyphID(name)]].FontName,
                             top.FDArray[top.FDSelect[font.getGlyphID(latin)]].FontName)
        self.assertEqual(report["cmap"], len(self.CODEPOINTS))

    def test_timestamps_are_the_sources_and_a_rebuild_writes_the_same_bytes(self):
        first, _ = self.build("first.otf")
        second, _ = self.build("second.otf")
        source = TTFont(str(self.source), recalcTimestamp=False)
        font = TTFont(str(first), recalcTimestamp=False)
        self.assertEqual(font["head"].created, source["head"].created)
        self.assertEqual(font["head"].modified, source["head"].modified)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_a_code_point_the_source_lacks_fails_the_build(self):
        with self.assertRaisesRegex(cjk_subset.BuildError, "U\\+732B"):
            self.build(codepoints=self.CODEPOINTS | {0x732B})

    def test_the_check_refuses_what_the_engine_does_not_read(self):
        with self.assertRaisesRegex(cjk_subset.BuildError, "subroutines"):
            cjk_subset.check(self.source, self.CODEPOINTS)
        with self.assertRaisesRegex(cjk_subset.BuildError, "OTTO"):
            cjk_subset.check(FIXTURES / "NotoSansJP-TrueType.ttf", self.CODEPOINTS)
        out, _ = self.build()
        for attribute, table, value in (("macStyle", "head", 1), ("usWeightClass", "OS/2", 600)):
            font = TTFont(str(out))
            setattr(font[table], attribute, value)
            changed = self.path / f"{attribute}.otf"
            font.save(str(changed))
            with self.assertRaisesRegex(cjk_subset.BuildError, "regular"):
                cjk_subset.check(changed, self.CODEPOINTS)
        with self.assertRaisesRegex(cjk_subset.BuildError, "lacks 1"):
            cjk_subset.check(out, self.CODEPOINTS | {0x732B})

    def test_a_glyph_over_the_engine_caps_fails_unless_listed(self):
        pen = T2CharStringPen(1000, None)
        pen.moveTo((0, 0))
        for i in range(171):
            pen.curveTo((i, 10), (i + 1, 20), (i + 2, 30))
        pen.closePath()
        notdef = T2CharStringPen(1000, None)
        notdef.moveTo((0, 0))
        notdef.lineTo((0, 100))
        notdef.lineTo((100, 0))
        notdef.closePath()
        order = [".notdef", "big"]
        fb = FontBuilder(1000, isTTF=False)
        fb.setupGlyphOrder(order)
        fb.setupCharacterMap({0xE000: "big"})
        fb.setupCFF("Big-Regular", {"FullName": "Big Regular"},
                    {".notdef": notdef.getCharString(), "big": pen.getCharString()}, {})
        fb.setupHorizontalMetrics({name: (1000, 0) for name in order})
        fb.setupHorizontalHeader(ascent=880, descent=-120)
        fb.setupNameTable({"familyName": "Big", "styleName": "Regular"})
        fb.setupOS2(usWeightClass=400)
        fb.setupPost()
        path = self.path / "big.otf"
        fb.save(str(path))
        with self.assertRaisesRegex(cjk_subset.BuildError, "points"):
            cjk_subset.check(path, {0xE000})
        cjk_subset.check(path, {0xE000}, oversize=frozenset({0xE000}))

    def test_another_fonttools_version_is_refused(self):
        with self.assertRaises(cjk_subset.BuildError) as raised:
            cjk_subset.check_fonttools("0.0.0")
        self.assertIn("0.0.0", str(raised.exception))
        self.assertIn(fontTools.version, str(raised.exception))
        self.assertIsNone(cjk_subset.check_fonttools(fontTools.version))


if __name__ == "__main__":
    unittest.main()
