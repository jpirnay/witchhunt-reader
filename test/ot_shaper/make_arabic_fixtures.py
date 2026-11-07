#!/usr/bin/env python3
"""Builds the Arabic shaping fixtures of step 2 PR 4a and records what HarfBuzz does with them.

    python -I -X utf8 pyrun.py <pydeps> test/ot_shaper/make_arabic_fixtures.py <sources dir> <fonts out dir> \
        [--header-out test/ot_shaper/ArabicExpected.h]

<sources dir> holds the eight pinned Noto sources (notofonts.github.io at 5c82cef, unhinted TTFs). NotoNaskhArabic-Regular,
NotoSerif-Regular, NotoSansArabic-Bold and NotoSans-Bold are merged here into two fonts; the oracle's other four (Naskh
Bold, Sans Arabic Regular, Serif Bold, Sans Regular) are checked when present. Each is checked against the sha256
below; the script does not download. It writes three fonts into <fonts out dir>:

- NotoNaskhArabic-Shaping-Regular.ttf: Noto Naskh Arabic Regular merged with Noto Serif's Latin by
  lib/EpdFont/scripts/merge_noto_latin.py (the recipe that builds the shipped Arabic families), then cut with
  fontTools.subset to the code points of the words below (every layout feature kept).
- NotoSansArabic-Shaping-Bold.ttf: the same for Noto Sans Arabic Bold with Noto Sans.
- ArabicRtlTest-Regular.ttf: a synthetic font (fontTools.fontBuilder and feaLib, this repository's own data) holding
  what neither Noto family has: cursive attachment with and without the RightToLeft lookup flag, isol/init/medi/fina
  on a few letters, the lam-alef rlig, calt under a 'dflt' language system without rclt and a URD one with it, mark to
  base and mark to mark, U+0627 and U+0653 mapped with U+0622 left unmapped.

Into --header-out it writes HarfBuzz 14.5's own answers (uharfbuzz): per font and language the lookups it plans (read
from its buffer messages, "start lookup N", the method of measure-first item 7) and the GSUB stage count, and per
word the glyph run for direction rtl, script Arab, at scale 2133 and ppem 33. The output is byte-identical on every
run.

Needs fonttools 4.63.0 and uharfbuzz 0.56.2 (the versions lib/EpdFont/scripts/sd-fonts.yaml pins).
"""

import argparse
import hashlib
import io
import re
import sys
import tempfile
from pathlib import Path

import uharfbuzz as hb
from fontTools import subset
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "lib" / "EpdFont" / "scripts"))
import merge_noto_latin  # noqa: E402

# notofonts/notofonts.github.io at this commit, fonts/<Family>/unhinted/ttf/<File>.
SOURCE_COMMIT = "5c82cef0a8f08806f932062e18c02be62be278f0"
SOURCES = {
    "NotoNaskhArabic-Regular.ttf": "c34fdbd98af4dbc45ca192a23d2eeb77032add83086f5fe32957d23b3f36b221",
    "NotoSerif-Regular.ttf": "a15cfbbc1539d707115111d672d590a3d70d4f74b4c0a315956da20ae19a14e1",
    "NotoSansArabic-Bold.ttf": "f4cb79f842a07d1bcf597879574358fb7010f73761396d1d0f2445dfedb4d2ca",
    "NotoSans-Bold.ttf": "87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9",
    # The other four merge sources of the oracle (oracle_arabic.py); not merged here, checked when present.
    "NotoNaskhArabic-Bold.ttf": "380ac34b249d336e1a43c21053df440594e1e3db8f75fabd4d85b649ca1ea4c6",
    "NotoSansArabic-Regular.ttf": "bd86ca02f087d7f3c3788ba458fb6b73744c7639ed276b8d870dba6def6c40d0",
    "NotoSerif-Bold.ttf": "24ad531e6b05ddad8c3d89572d2c93eb86a6b74e652ce7ee3c3e171de68e84c3",
    "NotoSans-Regular.ttf": "f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8",
}
MERGED = {
    "NotoNaskhArabic-Shaping-Regular.ttf": ("NotoNaskhArabic-Regular.ttf", "NotoSerif-Regular.ttf"),
    "NotoSansArabic-Shaping-Bold.ttf": ("NotoSansArabic-Bold.ttf", "NotoSans-Bold.ttf"),
}
SYNTHETIC = "ArabicRtlTest-Regular.ttf"

# Kept in the Noto subsets beside the words' code points: space, ZWNJ, ZWJ, the dotted circle, and two Latin letters
# (from the Latin font of the merge).
EXTRA_CODEPOINTS = {0x0020, 0x200C, 0x200D, 0x25CC, 0x0041, 0x0061}

SCALE = 2133  # 26.6, as the other shaping fixtures
PPEM = (SCALE + 32) >> 6  # 33

# BCP 47 language -> OpenType language system tag (hb-ot-tag-table.hh); every other language takes the default.
LANGUAGE_TAGS = {"ar": "ARA ", "fa": "FAR ", "ur": "URD ", "sd": "SND "}
PLAN_LANGUAGES = ["", "ar", "fa", "ur", "sd"]

# GSUB pauses that every Arabic plan has: after rvrn, stch, ccmp/locl, each of the seven forms, the empty stage
# (deallocate_buffer_var) and rlig (arabic_fallback_shape), and the last one hb_ot_map_builder_t::compile adds.
# collect_features_arabic adds one more, after calt, unless the language system has rclt (see gsub_stages()).
FIXED_GSUB_PAUSES = 1 + 1 + 1 + 7 + 1 + 1 + 1


def w(*tokens):
    """A word from hex code points, space separated and/or ints; no literal Arabic or joiner is ever written."""
    out = []
    for token in tokens:
        if isinstance(token, int):
            out.append(chr(token))
        else:
            out.extend(chr(int(t, 16)) for t in token.split())
    return "".join(out)


BEH = 0x0628
ALEF, LAM, MEEM = 0x0627, 0x0644, 0x0645
ZWNJ, ZWJ = 0x200C, 0x200D
SPACE = 0x0020
TATWEEL = 0x0640
SHADDA, FATHA, KASRA, DAMMA, SUKUN, SUPALEF = 0x0651, 0x064E, 0x0650, 0x064F, 0x0652, 0x0670
HAMZA_ABOVE, HAMZA_BELOW, MADDA = 0x0654, 0x0655, 0x0653

# Every Arabic letter of the blocks the engine's tables cover, wherever both Noto fonts map it.
LETTER_RANGES = [(0x0621, 0x063F), (0x0641, 0x064A), (0x066E, 0x066F), (0x0671, 0x06D3), (0x06D5, 0x06D5),
                 (0x06EE, 0x06EF), (0x06FA, 0x06FC), (0x06FF, 0x06FF), (0x0750, 0x077F)]

LAM_ALEF = [0x0622, 0x0623, 0x0625, 0x0627]

# (text, language): words shaped by the Noto fonts besides the letter forms.
def noto_words():
    words = []
    for a in LAM_ALEF:
        words += [w(LAM, a), w(BEH, LAM, a, BEH), w(BEH, LAM, a), w(LAM, FATHA, a), w(LAM, SHADDA, FATHA, a),
                  w(LAM, ZWNJ, a), w(LAM, ZWJ, a)]
    words += [
        w(BEH, SHADDA, FATHA, BEH), w(BEH, FATHA, SHADDA, BEH), w(BEH, SHADDA, KASRA, BEH), w(BEH, KASRA, SHADDA, BEH),
        w(BEH, SUPALEF, BEH), w(BEH, SHADDA, SUPALEF, BEH), w(BEH, SHADDA, DAMMA), w(BEH, SUKUN, BEH),
        w(ALEF, HAMZA_ABOVE, FATHA), w(ALEF, FATHA, HAMZA_ABOVE), w(ALEF, HAMZA_BELOW, KASRA),
        w(ALEF, KASRA, HAMZA_BELOW), w(BEH, HAMZA_ABOVE, FATHA, BEH), w(BEH, FATHA, HAMZA_ABOVE, BEH),
        w(BEH, HAMZA_BELOW, KASRA, BEH), w(BEH, KASRA, HAMZA_BELOW, BEH), w(ALEF, MADDA), w(BEH, MADDA),
        w(0x064A, HAMZA_ABOVE), w(0x0648, HAMZA_ABOVE), w(0x06D5, HAMZA_ABOVE),
        w(BEH, ZWNJ, BEH), w(BEH, ZWJ), w(ZWJ, BEH), w(ZWJ, BEH, ZWJ), w(BEH, ZWJ, BEH), w(ALEF, ZWNJ, BEH),
        w(BEH, TATWEEL, BEH), w(BEH, TATWEEL, TATWEEL, BEH), w(LAM, TATWEEL, ALEF), w(TATWEEL, BEH), w(BEH, TATWEEL),
        w(0x0661, 0x0662, 0x0663, 0x0664), w(0x06F1, 0x06F2, 0x06F3, 0x06F4), w(0x0661, SPACE, BEH, SPACE, 0x0662),
        w(BEH, 0x0661, BEH),
        # real words: the basmala, a greeting, the Allah ligature, a vocalised word
        w("0628 0633 0645 0020 0627 0644 0644 0647 0020 0627 0644 0631 062D 0645 0646"),
        w("0627 0644 0633 0644 0627 0645 0020 0639 0644 064A 0643 0645"),
        w("0627 0644 0644 0647"), w("0645 064F 062D 064E 0645 0651 064E 062F 064C"),
        w("0627 0644 0652 0639 064E 0631 064E 0628 0650 064A 0651 064E 0629 064F"),
    ]
    # Persian, Urdu, Sindhi: every letter of the brief's list in a word, as a plain word and in its language.
    persian = w("0645 06CC 200C 062E 0648 0627 0647 0645 0020 067E 0686 0698 06AF 06A9 06CC")
    urdu = w("067E 0686 0698 06AF 06A9 06CC 06C1 06D2 06BA 0679 0688 0691 06BE")
    urdu2 = w("0628 0691 06D2 0020 0679 06BE 06CC 06A9 0020 0645 06CC 06BA 0020 06C1 06D2 0020 0688 06BE 0648 0644")
    sindhi = w("0680 067B 0684 0683 06A6 06AA 06B3 06B1 06BB 0020 0633 0646 068C 064A")
    return ([(t, "") for t in words] +
            [(persian, ""), (persian, "fa"), (urdu, ""), (urdu, "ur"), (urdu2, ""), (urdu2, "ur"), (sindhi, ""),
             (sindhi, "sd")])


def letter_words(cmap):
    out = []
    for lo, hi in LETTER_RANGES:
        for cp in range(lo, hi + 1):
            if cp in cmap:
                out += [w(cp), w(cp, BEH), w(BEH, cp, BEH), w(BEH, cp)]
    return out


# The synthetic font's words: each reaches a lookup, under no language and under ur (URD: rclt, curs without the flag).
def synthetic_words():
    texts = [
        w(BEH), w(BEH, BEH), w(BEH, BEH, BEH), w(BEH, BEH, BEH, BEH),  # forms, calt (init + fina), cursive attachment
        w(ALEF), w(BEH, ALEF), w(ALEF, BEH),
        w(LAM), w(LAM, BEH), w(LAM, LAM, BEH),  # rclt (URD): lam.init then beh.fina
        w(LAM, ALEF), w(BEH, LAM, ALEF), w(LAM, FATHA, ALEF), w(LAM, 0x0622), w(LAM, ALEF, 0x0653),  # rlig, ccmp
        w(ALEF, MADDA), w(BEH, ALEF, MADDA), w(0x0622), w(BEH, 0x0622),  # ccmp; U+0622 is unmapped
        w(BEH, FATHA), w(BEH, FATHA, BEH), w(BEH, FATHA, SHADDA, BEH), w(BEH, SHADDA, FATHA, BEH),  # mark, mkmk
        w(BEH, MADDA, BEH), w(ALEF, HAMZA_ABOVE, FATHA), w(ALEF, FATHA, HAMZA_ABOVE), w(BEH, HAMZA_ABOVE, SHADDA),
        w(LAM, SHADDA, ALEF), w(LAM, MADDA, ALEF),
        w(BEH, ZWNJ, BEH), w(BEH, ZWJ), w(ZWJ, BEH), w(BEH, ZWJ, BEH), w(LAM, ZWNJ, ALEF), w(LAM, ZWJ, ALEF),
        w(BEH, TATWEEL, BEH), w(TATWEEL, BEH), w(BEH, SPACE, BEH), w(LAM, ALEF, SPACE, BEH, BEH),
    ]
    return [(t, lang) for t in texts for lang in ("", "ur")]


FEATURES = """
languagesystem DFLT dflt;
languagesystem arab dflt;
languagesystem arab URD;

@BASES = [beh beh.isol beh.init beh.medi beh.fina beh.init.alt alef alef.isol alef.fina alef_madda
          lam lam.isol lam.init lam.medi lam.fina lam.init.alt2 tatweel];
@MARKS = [fatha shadda madda hamza];

markClass [fatha shadda madda hamza] <anchor 150 0> @MC_ALL;
markClass [fatha madda hamza] <anchor 150 0> @MC_UPPER;

table GDEF {
  GlyphClassDef @BASES, [lam_alef.isol lam_alef.fina], @MARKS, ;
} GDEF;

feature ccmp {
  lookup CCMP_MADDA { sub alef madda by alef_madda; } CCMP_MADDA;
} ccmp;

feature isol { sub [beh lam alef] by [beh.isol lam.isol alef.isol]; } isol;
feature fina { sub [beh lam alef] by [beh.fina lam.fina alef.fina]; } fina;
feature medi { sub [beh lam] by [beh.medi lam.medi]; } medi;
feature init { sub [beh lam] by [beh.init lam.init]; } init;

feature rlig {
  lookupflag IgnoreMarks;
  sub lam.init alef.fina by lam_alef.isol;
  sub lam.medi alef.fina by lam_alef.fina;
} rlig;

feature calt { sub beh.init' beh.fina by beh.init.alt; } calt;

feature rclt {
  script arab;
  language URD exclude_dflt;
  sub lam.init' beh.fina by lam.init.alt2;
} rclt;

feature curs {
  script arab;
  language dflt;
  lookup CURS_RTL {
    lookupflag RightToLeft IgnoreMarks;
    position cursive beh.init <anchor NULL> <anchor 100 300>;
    position cursive beh.medi <anchor 550 320> <anchor 50 280>;
    position cursive beh.fina <anchor 520 300> <anchor NULL>;
  } CURS_RTL;
  language URD exclude_dflt;
  lookup CURS_LTR {
    lookupflag IgnoreMarks;
    position cursive beh.init <anchor NULL> <anchor 100 300>;
    position cursive beh.medi <anchor 550 320> <anchor 50 280>;
    position cursive beh.fina <anchor 520 300> <anchor NULL>;
  } CURS_LTR;
} curs;

feature mark {
  position base [beh beh.isol beh.init beh.medi beh.fina beh.init.alt] <anchor 300 700> mark @MC_ALL;
  position base [alef alef.isol alef.fina alef_madda lam lam.isol lam.init lam.medi lam.fina lam.init.alt2 tatweel]
      <anchor 200 700> mark @MC_ALL;
  position ligature [lam_alef.isol lam_alef.fina] <anchor 250 700> mark @MC_ALL;
} mark;

feature mkmk {
  position mark shadda <anchor 150 300> mark @MC_UPPER;
} mkmk;
"""

# (glyph, advance, box (x0, y0, x1, y1) or None, unicode or None)
GLYPHS = [
    (".notdef", 500, (50, 0, 450, 700), None),
    ("space", 250, None, 0x0020),
    ("alef", 300, (100, 0, 200, 700), 0x0627),
    ("alef.isol", 300, (100, 0, 220, 700), None),
    ("alef.fina", 300, (80, 0, 200, 700), None),
    ("alef_madda", 300, (100, 0, 200, 900), None),
    ("beh", 600, (50, 0, 550, 300), 0x0628),
    ("beh.isol", 600, (60, 0, 540, 300), None),
    ("beh.init", 600, (0, 0, 560, 300), None),
    ("beh.medi", 600, (0, 0, 600, 300), None),
    ("beh.fina", 600, (40, 0, 600, 300), None),
    ("beh.init.alt", 600, (0, 0, 560, 320), None),
    ("lam", 400, (100, 0, 300, 700), 0x0644),
    ("lam.isol", 400, (90, 0, 310, 700), None),
    ("lam.init", 400, (0, 0, 300, 700), None),
    ("lam.medi", 400, (0, 0, 400, 700), None),
    ("lam.fina", 400, (50, 0, 400, 700), None),
    ("lam.init.alt2", 400, (0, 0, 300, 720), None),
    ("lam_alef.isol", 700, (50, 0, 650, 700), None),
    ("lam_alef.fina", 700, (0, 0, 650, 700), None),
    ("tatweel", 300, (0, 100, 300, 160), 0x0640),
    ("fatha", 0, (50, 800, 250, 850), 0x064E),
    ("shadda", 0, (50, 900, 250, 980), 0x0651),
    ("madda", 0, (50, 1000, 250, 1060), 0x0653),
    ("hamza", 0, (80, 1000, 220, 1040), 0x0654),
]


def build_synthetic():
    order = [g[0] for g in GLYPHS]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({g[3]: g[0] for g in GLYPHS if g[3] is not None})
    glyphs = {}
    for name, _, box, _ in GLYPHS:
        pen = TTGlyphPen(None)
        if box:
            x0, y0, x1, y1 = box
            pen.moveTo((x0, y0))
            pen.lineTo((x0, y1))
            pen.lineTo((x1, y1))
            pen.lineTo((x1, y0))
            pen.closePath()
        glyphs[name] = pen.glyph()
    fb.setupGlyf(glyphs)
    metrics = {}
    for name, advance, _, _ in GLYPHS:
        glyph = glyphs[name]
        glyph.recalcBounds(fb.font["glyf"])
        metrics[name] = (advance, getattr(glyph, "xMin", 0))
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=1100, descent=-300)
    fb.setupNameTable({"familyName": "ArabicRtlTest", "styleName": "Regular",
                       "copyright": "This font is this repository's own test data (test/ot_shaper/"
                                    "make_arabic_fixtures.py); it contains no third-party data.",
                       "uniqueFontIdentifier": "ArabicRtlTest-Regular", "fullName": "ArabicRtlTest Regular",
                       "psName": "ArabicRtlTest-Regular", "version": "Version 1.000"})
    fb.setupOS2(sTypoAscender=1100, sTypoDescender=-300, usWinAscent=1100, usWinDescent=300)
    fb.setupPost()
    # Fixed timestamps (seconds since 1904-01-01: 2026-01-01T00:00:00Z), so the bytes repeat.
    fb.updateHead(created=3_944_678_400, modified=3_944_678_400)
    fb.font.recalcTimestamp = False
    addOpenTypeFeaturesFromString(fb.font, FEATURES)
    out = io.BytesIO()
    fb.font.save(out)
    return out.getvalue()


def verify_source(source_dir: Path, name: str) -> Path:
    path = source_dir / name
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: https://github.com/notofonts/notofonts.github.io/raw/{SOURCE_COMMIT}/"
                 f"fonts/<Family>/unhinted/ttf/{name}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != SOURCES[name]:
        sys.exit(f"{path}: sha256 {digest}, expected {SOURCES[name]}")
    return path


def merged_font(source_dir: Path, tmp: Path, script_name: str, latin_name: str) -> Path:
    out = tmp / script_name
    merge_noto_latin.merge(verify_source(source_dir, script_name), verify_source(source_dir, latin_name), out)
    return out


def mapped_codepoints(path):
    return set(TTFont(io.BytesIO(path.read_bytes())).getBestCmap())


def cut(merged: Path, codepoints) -> bytes:
    options = subset.Options()
    options.layout_features = ["*"]
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.recalc_timestamp = False
    font = subset.load_font(io.BytesIO(merged.read_bytes()), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=sorted(codepoints))
    subsetter.subset(font)
    for table in ("GDEF", "GSUB", "GPOS"):
        if table not in font:
            sys.exit(f"{merged}: the subset lost {table}")
    font.recalcTimestamp = False
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def round_px(v):
    return (v + 32) // 64 if v >= 0 else -((-v + 32) // 64)


def hb_font(data):
    font = hb.Font(hb.Face(data))
    font.scale = (SCALE, SCALE)
    font.ppem = (PPEM, PPEM)
    return font


def shape(font, text, language):
    buf = hb.Buffer()
    buf.add_str(text)
    buf.direction = "rtl"
    buf.script = "Arab"
    if language:
        buf.language = language
    hb.shape(font, buf, {})
    glyphs = []
    for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
        advance = (pos.x_advance + 2) >> 2
        dx, dy = round_px(pos.x_offset), -round_px(pos.y_offset)
        if not (0 <= advance < 65536 and -128 <= dx < 128 and -128 <= dy < 128 and info.codepoint < 65536):
            sys.exit(f"a glyph of {text!r} does not fit ExpectedGlyph: {info.codepoint} {advance} {dx} {dy}")
        glyphs.append((info.codepoint, advance, dx, dy))
    return glyphs


START_TABLE = re.compile(r"start table (GSUB|GPOS) script tag '([^']*)'")
START_LOOKUP = re.compile(r"start lookup (\d+) feature '(....)'")


def planned_lookups(data, language):
    """HarfBuzz's planned GSUB and GPOS lookups: hb_ot_map_t::apply() sends "start lookup N feature 'tag'" for every
    planned lookup of every stage, in plan order, before it tests the lookup's digest. A lookup planned in two stages
    is listed twice, as our plan holds it twice."""
    font = hb_font(data)
    buf = hb.Buffer()
    buf.add_str(w(BEH, BEH))
    buf.direction = "rtl"
    buf.script = "Arab"
    if language:
        buf.language = language
    state = {"table": None, "GSUB": [], "GPOS": []}

    def message(msg):
        m = START_TABLE.match(msg)
        if m:
            state["table"] = m.group(1)
            return True
        m = START_LOOKUP.match(msg)
        if m and state["table"]:
            state[state["table"]].append(int(m.group(1)))
        return True

    buf.set_message_func(message)
    hb.shape(font, buf, {})
    return state["GSUB"], state["GPOS"]


def language_system_features(font, tag, language):
    """The feature tags of the language system HarfBuzz picks for script 'arab' (else DFLT, dflt, latn) and the
    language, in table `tag`: hb_ot_layout_table_select_script, hb_ot_layout_script_select_language."""
    table = font[tag].table
    scripts = {r.ScriptTag: r.Script for r in table.ScriptList.ScriptRecord}
    script = next((scripts[t] for t in ("arab", "DFLT", "dflt", "latn") if t in scripts), None)
    if script is None:
        return set()
    langs = {r.LangSysTag: r.LangSys for r in script.LangSysRecord}
    langsys = langs.get(LANGUAGE_TAGS.get(language, ""))
    if langsys is None:
        langsys = langs.get("dflt", script.DefaultLangSys)
    if langsys is None:
        return set()
    return {table.FeatureList.FeatureRecord[i].FeatureTag for i in langsys.FeatureIndex}


def gsub_stages(data, language):
    """GSUB stages: one per pause (hb_ot_map_builder_t::add_gsub_pause; the features after a pause go in the next
    stage, and compile() closes the last), so FIXED_GSUB_PAUSES plus the pause after calt, which only a language
    system without rclt gets (HarfBuzz #1573, has_feature over GSUB and GPOS)."""
    font = TTFont(io.BytesIO(data))
    has_rclt = any("rclt" in language_system_features(font, t, language) for t in ("GSUB", "GPOS") if t in font)
    return FIXED_GSUB_PAUSES + (0 if has_rclt else 1)


def c_hex_string(text):
    """UTF-8 as \\x escapes with no run of hex digits glued to a following literal character."""
    return "".join(f"\\x{b:02X}" for b in text.encode("utf-8"))


def font_constant(file):
    return "kFont" + re.sub(r"[^A-Za-z0-9]", "", file.rsplit(".", 1)[0])


def header_text(fonts, words_by_font):
    lines = [
        "#pragma once",
        "// Generated by test/ot_shaper/make_arabic_fixtures.py from test/fixtures/fonts/. Do not edit.",
        "// HarfBuzz 14.5.0 (uharfbuzz 0.56.2): the lookups it plans and the glyph runs it shapes, right to left,",
        "// script Arab, at scale 2133 and ppem 33.",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        '#include "../complex_shaper/ExpectedShaping.h"',
        "",
        "struct ArabicPlanExpectation {",
        "  const char* font;      // in test/fixtures/fonts",
        '  const char* language;  // BCP 47; "" = none',
        "  const uint16_t* gsub;  // planned GSUB lookup indices, in plan order",
        "  uint8_t gsubCount;",
        "  const uint16_t* gpos;",
        "  uint8_t gposCount;",
        "  uint8_t gsubStages;",
        "};",
        "",
        "struct ArabicShapingExpectation {",
        "  const char* font;",
        "  const char* utf8;",
        '  const char* language;  // BCP 47; "" = none',
        "  const ExpectedGlyph* glyphs;",
        "  uint8_t count;",
        "};",
        "",
        "// clang-format off",
        *[f'constexpr const char* {font_constant(file)} = "{file}";' for file, _ in fonts],
        "",
    ]
    # The plans.
    plan_rows = []
    for index, (file, data) in enumerate(fonts):
        for lang in PLAN_LANGUAGES:
            gsub, gpos = planned_lookups(data, lang)
            stages = gsub_stages(data, lang)
            name = f"kPlan{index}{lang or 'None'}"
            lines.append(f"constexpr uint16_t {name}Gsub[] = {{{', '.join(map(str, gsub)) or '0'}}};")
            lines.append(f"constexpr uint16_t {name}Gpos[] = {{{', '.join(map(str, gpos)) or '0'}}};")
            plan_rows.append(f'    {{{font_constant(file)}, "{lang}", {name}Gsub, {len(gsub)}, {name}Gpos, {len(gpos)}, {stages}}},')
            print(f"  plan {file} lang={lang or 'none'}: GSUB {len(gsub)} GPOS {len(gpos)} stages {stages}")
    lines += ["", "constexpr ArabicPlanExpectation kArabicPlans[] = {", *plan_rows, "};", ""]
    # The glyph pool and the words.
    pool, rows = [], []
    for (file, data), words in zip(fonts, words_by_font):
        font = hb_font(data)
        for text, lang in words:
            glyphs = shape(font, text, lang)
            rows.append((file, text, lang, len(pool), len(glyphs)))
            pool += glyphs
    lines.append("constexpr ExpectedGlyph kArabicGlyphPool[] = {")
    for i in range(0, len(pool), 8):
        lines.append("    " + ",".join(f"{{{g},{a},{dx},{dy}}}" for g, a, dx, dy in pool[i:i + 8]) + ",")
    lines += ["};", "", "constexpr ArabicShapingExpectation kArabicShaping[] = {"]
    for file, text, lang, offset, count in rows:
        lines.append(f'    {{{font_constant(file)}, "{c_hex_string(text)}", "{lang}", kArabicGlyphPool + {offset}, {count}}},')
    lines += ["};", "", "// clang-format on", ""]
    return "\n".join(lines), len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", type=Path, help="holds the eight pinned Noto TTFs (four are merged into two fonts, four only checked when present)")
    parser.add_argument("out", type=Path, help="output directory of the three fonts")
    parser.add_argument("--header-out", type=Path, default=Path(__file__).resolve().parent / "ArabicExpected.h")
    args = parser.parse_args()

    for name in SOURCES:
        if (args.sources / name).is_file():
            verify_source(args.sources, name)

    fonts, words_by_font = [], []
    with tempfile.TemporaryDirectory() as tmp:
        merged = {name: merged_font(args.sources, Path(tmp), *src) for name, src in MERGED.items()}
        cmaps = {name: mapped_codepoints(path) for name, path in merged.items()}
        common = set.intersection(*cmaps.values())
        words = None
        for name, path in merged.items():
            # The letters both fonts map, so both fonts shape the same words.
            words = [(t, "") for t in letter_words(common)] + noto_words()
            cps = {ord(c) for t, _ in words for c in t} | EXTRA_CODEPOINTS
            data = cut(path, cps)
            mapped = set(TTFont(io.BytesIO(data)).getBestCmap())
            missing = sorted(c for t, _ in words for c in t if ord(c) not in mapped and ord(c) not in (0x200C, 0x200D))
            if missing:
                sys.exit(f"{name}: unmapped code points in the words: {sorted({hex(ord(c)) for c in missing})}")
            fonts.append((name, data))
            words_by_font.append(words)
    synthetic = build_synthetic()
    fonts.append((SYNTHETIC, synthetic))
    words_by_font.append(synthetic_words())

    args.out.mkdir(parents=True, exist_ok=True)
    for name, data in fonts:
        (args.out / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    header, runs = header_text(fonts, words_by_font)
    with open(args.header_out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"{args.header_out}: {runs} recorded words, {len(fonts) * len(PLAN_LANGUAGES)} plans")


if __name__ == "__main__":
    main()
