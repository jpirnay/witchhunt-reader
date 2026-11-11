"""Kerning is read the way OpenType applies it, by the Python converters and the browser one.

    python3 lib/EpdFont/scripts/test_kern_extract.py

Needs fontTools (requirements.txt). The browser converter's twin (tools/ftwasm/js) is checked
against the same fonts when node is on the PATH, and skipped otherwise.

The fonts are built here, with feaLib, so each one holds exactly the lookups a case needs.
"""

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr

sys.dont_write_bytecode = True
SCRIPTS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(SCRIPTS, '..', '..', '..'))
sys.path.insert(0, SCRIPTS)

from fontTools.feaLib.builder import addOpenTypeFeaturesFromString  # noqa: E402
from fontTools.fontBuilder import FontBuilder  # noqa: E402
from fontTools.pens.ttGlyphPen import TTGlyphPen  # noqa: E402
from fontTools.ttLib import TTFont, newTable  # noqa: E402
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0  # noqa: E402

from kern_extract import extract_raw_kerning  # noqa: E402

GLYPHS = ['.notdef', 'A', 'T', 'V', 'a', 'o', 'y']

# feaLib turns lookup PAIRS into: format 1 {V a -80, V o 0}; format 2 over [A T V] (A and V in
# class 0) x [a o] (y in class 0); then, after the break, format 2 over [A y] with A y -50 and
# y a -20. EXT is an Extension lookup holding a format 1 and a format 2 subtable; SINGLE is not
# pair kerning at all.
GPOS_KERN = """
languagesystem DFLT dflt;
lookup PAIRS {
  pos V a -80;
  pos V o 0;
  pos [A V] [a o] -70;
  pos [T] [a o] -30;
  subtable;
  pos [A] [y] -50;
  pos [y] [a] -20;
} PAIRS;
lookup MORE {
  pos V a -10;
} MORE;
lookup EXT useExtension {
  pos T y -40;
  pos [T] [y] -15;
} EXT;
lookup SINGLE {
  pos a -5;
} SINGLE;
feature kern { lookup PAIRS; lookup MORE; lookup EXT; lookup SINGLE; } kern;
"""

LEGACY_KERN = {('A', 'V'): -40, ('V', 'a'): -25}

# Lookup flags against GDEF classes: A V a base glyphs, fi a ligature, acute grave dot marks (acute
# and grave in the attachment class @TOP, grave alone in the mark set @SET). Each lookup kerns one
# pair its flags skip and one they keep; V is given class 5 after the build (GdefFlagsTest).
GDEF_GLYPHS = ['.notdef', 'A', 'V', 'a', 'fi', 'acute', 'grave', 'dot']
GDEF_CMAP = {0x41: 'A', 0x56: 'V', 0x61: 'a', 0xFB01: 'fi', 0x301: 'acute', 0x300: 'grave', 0x307: 'dot'}
GDEF_KERN = """
languagesystem DFLT dflt;
@TOP = [acute grave];
@SET = [grave];
table GDEF {
  GlyphClassDef [A V a], [fi], [acute grave dot], ;
} GDEF;
lookup BASE {
  lookupflag IgnoreBaseGlyphs;
  pos A V -100;
  pos V V -8;
} BASE;
lookup LIGS {
  lookupflag IgnoreLigatures;
  pos fi a -30;
  pos V a -20;
} LIGS;
lookup MARKS {
  lookupflag IgnoreMarks;
  pos A acute -40;
  pos a a -7;
} MARKS;
lookup ATTACH {
  lookupflag MarkAttachmentType @TOP;
  pos acute a -11;
  pos dot a -12;
} ATTACH;
lookup SET {
  lookupflag UseMarkFilteringSet @SET;
  pos grave a -13;
  pos acute A -14;
} SET;
lookup RESERVED {
  pos A a -9;
} RESERVED;
feature kern { lookup BASE; lookup LIGS; lookup MARKS; lookup ATTACH; lookup SET; lookup RESERVED; } kern;
"""


def build_gdef_font(with_gdef=True):
    """GDEF_KERN, V moved to the undefined class 5 and the reserved bit 0x0020 set on RESERVED: what
    HarfBuzz (and the firmware's reader) skip nothing for. Without GDEF: the same lookups, nothing to
    skip glyphs by."""
    font = TTFont(io.BytesIO(build_font(GDEF_KERN, glyphs=GDEF_GLYPHS, cmap=GDEF_CMAP)))
    font['GDEF'].table.GlyphClassDef.classDefs['V'] = 5
    reserved = [lookup for lookup in font['GPOS'].table.LookupList.Lookup
                if lookup.SubTable[0].Coverage.glyphs == ['A'] and lookup.LookupFlag == 0]
    assert len(reserved) == 1, 'RESERVED is the one unflagged lookup of A'
    reserved[0].LookupFlag = 0x0020
    if not with_gdef:
        del font['GDEF']
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def build_font(gpos_fea=None, legacy_kern=None, glyphs=None, cmap=None):
    glyphs = glyphs or GLYPHS
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(glyphs)
    fb.setupCharacterMap(cmap or {ord(name): name for name in glyphs[1:]})
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0))
    pen.lineTo((0, 500))
    pen.lineTo((400, 500))
    pen.closePath()
    glyph = pen.glyph()
    fb.setupGlyf({name: glyph for name in glyphs})
    fb.setupHorizontalMetrics({name: (500, 0) for name in glyphs})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'KernTest', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    if gpos_fea:
        addOpenTypeFeaturesFromString(fb.font, gpos_fea)
    if legacy_kern:
        subtable = KernTable_format_0()
        subtable.version, subtable.coverage, subtable.format, subtable.tupleIndex = 0, 1, 0, None
        subtable.kernTable = dict(legacy_kern)
        kern = newTable('kern')
        kern.version = 0
        kern.kernTables = [subtable]
        fb.font['kern'] = kern
    out = io.BytesIO()
    fb.font.save(out)
    return out.getvalue()


def python_kerning(font_bytes, glyphs=None):
    """{(left, right): units} over `glyphs` (names; default: every glyph the cmap maps)."""
    font = TTFont(io.BytesIO(font_bytes))
    with redirect_stderr(io.StringIO()):  # the "skipping lookupType" note for SINGLE
        return extract_raw_kerning(font, set(glyphs) if glyphs else set(font.getBestCmap().values()))


def pair_list_hash(font_bytes, kern):
    """sha256 (16 hex digits) of the pair list as glyph IDs, sorted: 'left right units' lines."""
    font = TTFont(io.BytesIO(font_bytes))
    lines = sorted((font.getGlyphID(l), font.getGlyphID(r), v) for (l, r), v in kern.items())
    return hashlib.sha256('\n'.join(f'{l} {r} {v}' for l, r, v in lines).encode()).hexdigest()[:16]


def read_font(relative):
    with open(os.path.join(REPO, relative), 'rb') as f:
        return f.read()


SERIF = 'lib/EpdFont/builtinFonts/source/SourceSerif4Caption/SourceSerif4Caption-Regular.ttf'


def with_pair_lookup_flags(font_bytes, flags, drop_gdef=False):
    """Every pair adjustment lookup (type 2, Extension 9) given `flags`, as a font such as Caecilia LT
    ships them (0x000E: skip base glyphs, ligatures and marks); its GDEF dropped if asked."""
    font = TTFont(io.BytesIO(font_bytes))
    for lookup in font['GPOS'].table.LookupList.Lookup:
        if lookup.LookupType in (2, 9):
            lookup.LookupFlag = flags
    if drop_gdef:
        del font['GDEF']
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def browser_kerning(font_bytes, tmp):
    """The same table walk in tools/ftwasm/js, keyed by glyph name, over every glyph the cmap maps."""
    path = os.path.join(tmp, 'font.ttf')
    with open(path, 'wb') as f:
        f.write(font_bytes)
    script = """
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';
const [js, fontPath] = process.argv.slice(1);
const { SfntFont } = await import(pathToFileURL(js + '/otf-tables.mjs').href);
const { prepareKerning } = await import(pathToFileURL(js + '/cpfont-writer.mjs').href);
const font = new SfntFont(new Uint8Array(fs.readFileSync(fontPath)), fontPath);
const { raw } = prepareKerning(font, [...font.bestCmap().keys()]);
console.log(JSON.stringify([...raw].map(([k, v]) => [Math.floor(k / 65536), k % 65536, v])));
"""
    js = os.path.join(REPO, 'tools', 'ftwasm', 'js')
    r = subprocess.run(['node', '--input-type=module', '-e', script, js, path],
                       capture_output=True, text=True, check=True)
    order = TTFont(io.BytesIO(font_bytes)).getGlyphOrder()
    return {(order[l], order[r]): v for l, r, v in json.loads(r.stdout)}


class FirstMatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kern = python_kerning(build_font(GPOS_KERN))

    def test_pair_exception_wins_over_class_kerning(self):
        # -80 from the format 1 exception plus -10 from the next lookup; summing every subtable
        # also added the class value and gave -160.
        self.assertEqual(self.kern[('V', 'a')], -90)

    def test_zero_valued_exception_still_decides_its_pair(self):
        self.assertNotIn(('V', 'o'), self.kern)  # not the class value, -70

    def test_class_subtable_decides_every_pair_of_its_left_glyphs(self):
        self.assertEqual(self.kern[('A', 'a')], -70)  # A is in class 0 of ClassDef1
        self.assertEqual(self.kern[('T', 'o')], -30)
        # A y: the first class subtable covers A and has 0 for y (class 0 of ClassDef2), so the
        # later -50 never applies.
        self.assertNotIn(('A', 'y'), self.kern)
        # y is not covered by it, so the later subtable kerns y a.
        self.assertEqual(self.kern[('y', 'a')], -20)

    def test_extension_lookup_takes_its_first_match_too(self):
        self.assertEqual(self.kern[('T', 'y')], -40)  # not -55

    def test_no_other_pairs(self):
        self.assertEqual(set(self.kern), {('V', 'a'), ('A', 'a'), ('A', 'o'), ('T', 'a'), ('T', 'o'),
                                          ('y', 'a'), ('T', 'y')})


class LegacyKernTableTest(unittest.TestCase):
    def test_ignored_when_gpos_has_kern(self):
        kern = python_kerning(build_font(GPOS_KERN, LEGACY_KERN))
        self.assertNotIn(('A', 'V'), kern)
        self.assertEqual(kern[('V', 'a')], -90)  # not also the table's -25

    def test_used_without_gpos_kern(self):
        self.assertEqual(python_kerning(build_font(None, LEGACY_KERN)), LEGACY_KERN)


class GdefFlagsTest(unittest.TestCase):
    """A lookup whose flags skip a glyph by its GDEF class kerns no pair holding it, as HarfBuzz and
    the firmware's GPOS reader (freeink-sdk Gpos.cpp) apply the flags to adjacent pairs."""

    def test_each_flag_skips_what_it_names(self):
        self.assertEqual(python_kerning(build_gdef_font()), {
            ('V', 'V'): -8,      # BASE skips base glyphs; V is in class 5, unclassified as in HarfBuzz
            ('V', 'a'): -20,     # LIGS skips fi, not these
            ('a', 'a'): -7,      # MARKS skips acute
            ('acute', 'a'): -11,  # ATTACH keeps @TOP's marks, skips dot
            ('grave', 'a'): -13,  # SET keeps @SET's marks, skips acute
            ('A', 'a'): -9,      # the reserved bit 0x0020 changes nothing
        })

    def test_without_gdef_nothing_is_skipped(self):
        self.assertEqual(python_kerning(build_gdef_font(with_gdef=False)), {
            ('A', 'V'): -100, ('V', 'V'): -8, ('fi', 'a'): -30, ('V', 'a'): -20, ('A', 'acute'): -40,
            ('a', 'a'): -7, ('acute', 'a'): -11, ('dot', 'a'): -12, ('grave', 'a'): -13, ('acute', 'A'): -14,
            ('A', 'a'): -9})

    def test_caecilia_flags_take_letter_kerning_away(self):
        # Source Serif kerns A/V by -120 units and T/o by -40; given Caecilia LT's flags (0x000E) its
        # GDEF's base glyphs are skipped and neither pair kerns. Without GDEF the flags skip nothing.
        serif = read_font(SERIF)
        pairs = ['A', 'V', 'T', 'o']
        kern = python_kerning(serif, pairs)
        self.assertEqual((kern[('A', 'V')], kern[('T', 'o')]), (-120, -40))
        caecilia = python_kerning(with_pair_lookup_flags(serif, 0x000E), pairs)
        self.assertNotIn(('A', 'V'), caecilia)
        self.assertNotIn(('T', 'o'), caecilia)
        self.assertEqual(python_kerning(with_pair_lookup_flags(serif, 0x000E, drop_gdef=True), pairs), kern)


class FullPairListTest(unittest.TestCase):
    """The whole pair list (every glyph the cmap maps) as before GDEF-aware kerning, for fonts whose
    lookups skip nothing a pair holds: Ubuntu has no GDEF; Source Serif's and Noto Sans's lookups
    skip only marks, which they kern with nothing. The hashes are those of kern_extract.py before."""

    BEFORE = {
        'lib/EpdFont/builtinFonts/source/Ubuntu/Ubuntu-Regular.ttf': (56975, '5dfdacbdfcbe250a'),
        SERIF: (115665, '795b2f38f93a3a1b'),
        'lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf': (36062, '39ca7c4a0d09b2a9'),
    }

    def test_pair_lists_are_as_before(self):
        for path, (count, digest) in self.BEFORE.items():
            font = read_font(path)
            kern = python_kerning(font)
            with self.subTest(font=os.path.basename(path)):
                self.assertEqual((len(kern), pair_list_hash(font, kern)), (count, digest))

    def test_flags_without_gdef_change_no_pair(self):
        font = with_pair_lookup_flags(read_font(SERIF), 0x000E, drop_gdef=True)
        self.assertEqual(pair_list_hash(font, python_kerning(font)), self.BEFORE[SERIF][1])


@unittest.skipUnless(shutil.which('node'), 'node is not on the PATH')
class BrowserConverterTest(unittest.TestCase):
    def test_reads_the_same_kerning(self):
        with tempfile.TemporaryDirectory() as tmp:
            for fea, legacy in ((GPOS_KERN, None), (GPOS_KERN, LEGACY_KERN), (None, LEGACY_KERN)):
                font = build_font(fea, legacy)
                with self.subTest(gpos=bool(fea), legacy=bool(legacy)):
                    self.assertEqual(browser_kerning(font, tmp), python_kerning(font))

    def test_skips_what_the_lookup_flags_skip_as_the_python_one(self):
        serif = read_font(SERIF)
        with tempfile.TemporaryDirectory() as tmp:
            for name, font in (('flags', build_gdef_font()), ('flags without GDEF', build_gdef_font(False)),
                               ('Caecilia flags', with_pair_lookup_flags(serif, 0x000E))):
                with self.subTest(font=name):
                    self.assertEqual(browser_kerning(font, tmp), python_kerning(font))


if __name__ == '__main__':
    unittest.main()
