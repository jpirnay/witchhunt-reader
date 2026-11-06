"""Merge Noto's Latin into a Noto script font (the Indic TrueType families of sd-fonts.yaml) and check the merge.

The merge, for one style:
- the script font with the Latin letters A-Z and a-z taken out of its cmap (its glyphs stay, so its glyph IDs and its
  own shaping are unchanged; only Noto Sans Sinhala maps any), so that Latin letters always come from the Latin font;
- a fontTools.subset of the Latin font (Noto Sans or Noto Serif): U+0020-007E, U+00A0-024F, U+2000-206F,
  U+20A0-20CF, U+2122 and U+FFFD, less every code point the script font maps other than A-Z and a-z (space, digits,
  punctuation and the rest come from the script font); scaled to the script font's upem when they differ;
- fontTools.merge.Merger with vhea and vmtx dropped, the script font first.
The checks (HarfBuzz, scale 2133, ppem 33):
- equal against the script's own font: every corpus line, glyph IDs, advances and offsets, with no language and each
  of the script's languages;
- Latin kept whole: each Latin line restricted to Latin letters (its longest runs of A-Z and a-z), each run shaped
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

# The languages the corpus is shaped with besides none, by script (the key of test/ot_shaper/generate_corpus.py).
LANGS = {"devanagari": ["hi", "mr", "ne", "sa"], "bengali": ["bn", "as"], "gurmukhi": ["pa"], "gujarati": ["gu"],
         "oriya": ["or"], "tamil": ["ta"], "telugu": ["te"], "kannada": ["kn"], "malayalam": ["ml"],
         "sinhala": ["si"]}
ISO = ["Deva", "Beng", "Guru", "Gujr", "Orya", "Taml", "Telu", "Knda", "Mlym", "Sinh"]
LATIN_RANGES = [(0x0020, 0x007E), (0x00A0, 0x024F), (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2122, 0x2122),
                (0xFFFD, 0xFFFD)]
LETTERS = set(range(0x41, 0x5B)) | set(range(0x61, 0x7B))
LATIN_LINES = ["The quick brown fox jumps over the lazy dog.", "AVATAR Wave fi fl ffi",
               "\u201cQuotes\u201d \u2014 1,234.56 (x) [y] {z} \u20ac"]
DROPPED_TABLES = ["vhea", "vmtx"]
SCALE, PPEM = 2133, 33


def script_of(text):
    """The ISO 15924 tag of the first Indic character of a corpus line (its block, 128 code points each)."""
    for ch in text:
        cp = ord(ch)
        if 0x0900 <= cp <= 0x0DFF and not (0x0951 <= cp <= 0x0954 or cp in (0x0964, 0x0965)):
            return ISO[(cp - 0x0900) >> 7]
    return "Deva"


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


def _letter_runs(text):
    out, cur = [], ""
    for ch in text:
        if ord(ch) in LETTERS:
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
            if shape(own, text, "ltr", script_of(text), lang) != shape(both, text, "ltr", script_of(text), lang):
                differences.append(f"script font differs, language {lang or 'none'}: "
                                   f"{' '.join(f'U+{ord(c):04X}' for c in text)}")

    with tempfile.TemporaryDirectory() as tmp:
        script_input, latin_subset = _prepare(script, latin, tmp)
        orders = [list(TTFont(script_input).getGlyphOrder()), list(TTFont(latin_subset).getGlyphOrder())]
        latin_original = list(orders[1])
        computeMegaGlyphOrder(_Order(), orders)
        back = {renamed: original for original, renamed in zip(latin_original, orders[1])}
    merged_order = list(TTFont(merged).getGlyphOrder())
    latin_order = list(TTFont(latin).getGlyphOrder())
    full = hb_font(Path(latin).read_bytes())
    for text in LATIN_LINES:
        for run in _letter_runs(text):
            a = [(latin_order[g], *rest) for g, *rest in shape(full, run)]
            b = [(back.get(merged_order[g], "SCRIPT:" + merged_order[g]), *rest) for g, *rest in shape(both, run)]
            if a != b:
                differences.append(f"Latin font differs: {run!r}")
    return differences
