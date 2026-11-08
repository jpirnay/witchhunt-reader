"""Merge Noto's Latin into a Noto script font (the Indic, Arabic and Hebrew TrueType families of sd-fonts.yaml) and check
the merge.

The merge, for one style:
- the script font with the Latin letters A-Z and a-z taken out of its cmap (its glyphs stay, so its glyph IDs and its
  own shaping are unchanged; only Noto Sans Sinhala maps any), so that Latin letters always come from the Latin font;
- a fontTools.subset of the Latin font (Noto Sans or Noto Serif): U+0020-007E, U+00A0-024F, U+2000-206F,
  U+20A0-20CF, U+2122 and U+FFFD, plus the IAST letters (24 precomposed letters of Latin Extended Additional,
  U+1E0C/D, 1E24/5, 1E36-9, 1E40-7, 1E5A-D, 1E62/3, 1E6C/D, and the 6 combining marks their decomposed forms use,
  U+0301, 0303, 0304, 0307, 0310, 0323), less every code point the script font maps other
  than A-Z and a-z (space, digits, punctuation and the rest come from the script font); scaled to the script font's
  upem when they differ;
- fontTools.merge.Merger with vhea and vmtx dropped, the script font first.
The checks (HarfBuzz, scale 2133, ppem 33; an Indic line is shaped left to right under its script's tag, a line with
an Arabic-block letter right to left under Arab, a line with a Hebrew-block letter right to left under Hebr):
- equal against the script's own font: every corpus line, glyph IDs, advances and offsets, with no language and each
  of the script's languages;
- Latin kept whole: each Latin line restricted to the code points the merged font takes from the Latin font (A-Z, a-z
  and every other code point of the subset that the script font does not map, such as the IAST letters and marks; its longest runs of them), each run shaped
  by the merged font and by Noto Sans/Serif itself: glyph names (through the merger's own renaming), advances and
  offsets equal, and every glyph one of the Latin font's.

The bytes depend on fontTools, so the build pins it (sd-fonts.yaml's `fonttools`); the checks also need uharfbuzz:
    pip install fonttools==4.63.0 uharfbuzz==0.56.2
"""
import tempfile
from pathlib import Path

from fontTools import subset
from fontTools.merge import Merger
from fontTools.merge.cmap import computeMegaGlyphOrder
from fontTools.merge.options import Options
from fontTools.ttLib import TTFont
from fontTools.ttLib.scaleUpem import scale_upem

# The languages the corpus is shaped with besides none, by script (the key of test/ot_shaper/generate_corpus.py): the
# merge check's set. test/ot_shaper/oracle_shipped_fonts.py covers every language the reader maps (LanguageTag.h).
LANGS = {"devanagari": ["hi", "mr", "ne", "sa"], "bengali": ["bn", "as"], "gurmukhi": ["pa"], "gujarati": ["gu"],
         "oriya": ["or"], "tamil": ["ta"], "telugu": ["te"], "kannada": ["kn"], "malayalam": ["ml"],
         "sinhala": ["si"],
         # Arabic: the set of test/ot_shaper/oracle_arabic.py
         "arabic": ["ar", "fa", "ur", "ps", "sd", "ug", "ku", "ckb", "ks"],
         # Hebrew: the languages the reader sets right to left in the Hebrew script (TypographyProfile)
         "hebrew": ["he", "yi"]}
ISO = ["Deva", "Beng", "Guru", "Gujr", "Orya", "Taml", "Telu", "Knda", "Mlym", "Sinh"]
ARABIC_BLOCKS = [(0x0600, 0x06FF), (0x0750, 0x077F), (0x0870, 0x089F), (0x08A0, 0x08FF)]
HEBREW_BLOCKS = [(0x0590, 0x05FF), (0xFB1D, 0xFB4F)]
LATIN_RANGES = [(0x0020, 0x007E), (0x00A0, 0x024F), (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2122, 0x2122),
                (0xFFFD, 0xFFFD)]
# The letters of IAST beyond Latin-1 (a, i, u, s, n, e, o with macron, acute or tilde are in U+00A0-024F): the 24
# precomposed letters (D, H, L, L-macron, M, M, N, N, R, R-macron, S, T with a dot, lower and upper case) and the combining
# marks the decomposed forms use (acute, tilde, macron, dot above, candrabindu, dot below).
IAST = [cp for lower in (0x1E0D, 0x1E25, 0x1E37, 0x1E39, 0x1E41, 0x1E43, 0x1E45, 0x1E47, 0x1E5B, 0x1E5D, 0x1E63, 0x1E6D)
        for cp in (lower - 1, lower)] + [0x0301, 0x0303, 0x0304, 0x0307, 0x0310, 0x0323]
LETTERS = set(range(0x41, 0x5B)) | set(range(0x61, 0x7B))
LATIN_LINES = ["The quick brown fox jumps over the lazy dog.", "AVATAR Wave fi fl ffi",
               "\u201cQuotes\u201d \u2014 1,234.56 (x) [y] {z} \u20ac",
               # IAST, precomposed and decomposed, lower and upper case
               "karma\u1e47y ev\u0101dhik\u0101ras te m\u0101 phale\u1e63u kad\u0101cana",
               "dharmak\u1e63etre kuruk\u1e63etre", "sa\u1e41sk\u1e5btam", "\u1e6d\u012bk\u0101",
               "\u1e5agveda", "\u1e62a\u1e6d", "\u1e0ca\u1e25 \u1e36 \u1e38 \u1e40 \u1e42 \u1e44 \u1e46 \u1e5c \u1e6c",
               "t\u0323 s\u0301 n\u0303 a\u0304 n\u0307 m\u0310 r\u0323\u0304 d\u0323 h\u0323",
               "\u1e0d \u1e25 \u1e37 \u1e39 \u1e41 \u1e43 \u1e45 \u1e47 \u1e5b \u1e5d \u1e63 \u1e6d"]
DROPPED_TABLES = ["vhea", "vmtx"]
SCALE, PPEM = 2133, 33


def script_of(text):
    """The ISO 15924 tag of the first Arabic, Hebrew or Indic character of a corpus line (an Indic block has 128 code
    points)."""
    for ch in text:
        cp = ord(ch)
        if any(lo <= cp <= hi for lo, hi in ARABIC_BLOCKS):
            return "Arab"
        if any(lo <= cp <= hi for lo, hi in HEBREW_BLOCKS):
            return "Hebr"
        if 0x0900 <= cp <= 0x0DFF and not (0x0951 <= cp <= 0x0954 or cp in (0x0964, 0x0965)):
            return ISO[(cp - 0x0900) >> 7]
    return "Deva"


def direction_of(script_tag):
    """The direction the check shapes a script in: Arabic and Hebrew right to left, every other script left to right."""
    return "rtl" if script_tag in ("Arab", "Hebr") else "ltr"


def prepare_script(script_path, out_path):
    """The script font as the merge takes it: A-Z and a-z out of every cmap subtable. Returns the path to merge (the
    original file when nothing had to leave the cmap)."""
    font = TTFont(script_path)
    removed = False
    for table in font["cmap"].tables:
        for cp in [c for c in (getattr(table, "cmap", None) or {}) if c in LETTERS]:
            del table.cmap[cp]
            removed = True
    if not removed:
        return Path(script_path)
    font.save(out_path)
    return Path(out_path)


def make_latin(latin_path, script_cmap, script_upem, out_path):
    keep = set()
    for a, b in LATIN_RANGES:
        keep.update(range(a, b + 1))
    keep.update(IAST)
    keep -= set(script_cmap) - LETTERS
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.glyph_names = True
    opts.notdef_outline = True
    opts.name_IDs = ["*"]
    opts.hinting = False
    font = TTFont(latin_path)
    sub = subset.Subsetter(options=opts)
    sub.populate(unicodes=keep)
    sub.subset(font)
    if font["head"].unitsPerEm != script_upem:
        scale_upem(font, script_upem)
    font.save(out_path)


def _prepare(script, latin, work):
    """The two inputs of the merge, written into the directory `work`: the prepared script font and the Latin subset."""
    work = Path(work)
    script_input = prepare_script(script, work / "script.ttf")
    script_font = TTFont(script_input)
    make_latin(latin, script_font.getBestCmap() or {}, script_font["head"].unitsPerEm, work / "latin.ttf")
    return script_input, work / "latin.ttf"


def merge(script: Path, latin: Path, out: Path) -> None:
    """Write the merge of the script font and the Latin font to `out` (same inputs, same bytes)."""
    with tempfile.TemporaryDirectory() as tmp:
        script_input, latin_subset = _prepare(script, latin, tmp)
        merged = Merger(options=Options(drop_tables=DROPPED_TABLES)).merge([str(script_input), str(latin_subset)])
        # The merger stamps head.created and modified with the time of the run: take the script font's, so the bytes
        # repeat.
        merged.recalcTimestamp = False
        head = TTFont(script)["head"]
        merged["head"].created, merged["head"].modified = head.created, head.modified
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        merged.save(out)


def _latin_runs(text, taken):
    """The longest runs of `text` made of code points in `taken` (those the merged font takes from the Latin font)."""
    out, cur = [], ""
    for ch in text:
        if ord(ch) in taken:
            cur += ch
        elif cur:
            out.append(cur)
            cur = ""
    if cur:
        out.append(cur)
    return out


class _Order:
    pass


def check(merged: Path, script: Path, latin: Path, corpus: list[str], languages: list[str]) -> list[str]:
    """Both checks of the module header. Returns one line per difference, empty when the merge holds."""
    import uharfbuzz as hb

    def hb_font(data):
        font = hb.Font(hb.Face(hb.Blob(data)))
        font.scale = (SCALE, SCALE)
        font.ppem = (PPEM, PPEM)
        return font

    def shape(font, text, direction=None, script_tag=None, lang=None):
        buf = hb.Buffer()
        buf.add_str(text)
        if direction:
            buf.direction = direction
            buf.script = script_tag
        else:
            buf.guess_segment_properties()
        if lang:
            buf.language = lang
        hb.shape(font, buf, {})
        return [(i.codepoint, p.x_advance, p.y_advance, p.x_offset, p.y_offset)
                for i, p in zip(buf.glyph_infos, buf.glyph_positions)]

    differences = []
    merged_data = Path(merged).read_bytes()
    own = hb_font(Path(script).read_bytes())
    both = hb_font(merged_data)
    for lang in [None] + list(languages):
        for text in corpus:
            tag = script_of(text)
            if shape(own, text, direction_of(tag), tag, lang) != shape(both, text, direction_of(tag), tag, lang):
                differences.append(f"script font differs, language {lang or 'none'}: "
                                   f"{' '.join(f'U+{ord(c):04X}' for c in text)}")

    with tempfile.TemporaryDirectory() as tmp:
        script_input, latin_subset = _prepare(script, latin, tmp)
        orders = [list(TTFont(script_input).getGlyphOrder()), list(TTFont(latin_subset).getGlyphOrder())]
        latin_original = list(orders[1])
        computeMegaGlyphOrder(_Order(), orders)
        back = {renamed: original for original, renamed in zip(latin_original, orders[1])}
        # What the merged font takes from the Latin font: A-Z and a-z, and every other code point of the subset (the
        # subset already lacks what the script font maps).
        taken = set(TTFont(latin_subset).getBestCmap() or {}) | LETTERS
    merged_order = list(TTFont(merged).getGlyphOrder())
    latin_order = list(TTFont(latin).getGlyphOrder())
    full = hb_font(Path(latin).read_bytes())
    for text in LATIN_LINES:
        for run in _latin_runs(text, taken):
            a = [(latin_order[g], *rest) for g, *rest in shape(full, run)]
            b = [(back.get(merged_order[g], "SCRIPT:" + merged_order[g]), *rest) for g, *rest in shape(both, run)]
            if a != b:
                differences.append(f"Latin font differs: {run!r}")
    return differences
