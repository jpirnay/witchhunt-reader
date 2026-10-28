"""Per-language CFF subsets of Noto CJK for the font manager.

build-sd-fonts.py calls build_family() for every sd-fonts.yaml entry with a `charset`. Each family is one
Regular file, assets/sd-fonts/<name>/<name>-Regular.otf, that the reader's CFF engine draws at every size: the
language's character set (charset()) plus the Latin and punctuation every CJK book mixes in (BASE), cut from the
Noto CJK region OTF. The outlines stay CFF charstrings, desubroutinized and unhinted: the engine reads neither
subroutines nor hints.

The output is reproducible: the same source, Unihan, fontTools and code give the same bytes, so a rebuild
without a change to its inputs leaves git with nothing to commit.

    python lib/EpdFont/scripts/cjk_subset.py <source.otf> <Unihan.zip> <charset> "<Font Name>" <out.otf>

builds one family by hand.
"""

import argparse
import re
import sys
import zipfile
from pathlib import Path

import fontTools
from fontTools import subset
from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import ControlBoundsPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont

# Latin and punctuation every family carries, for the Latin words, numbers and marks CJK books mix in:
# ASCII and Latin-1, general punctuation, CJK symbols and punctuation, full- and half-width forms.
BASE = (
    set(range(0x20, 0x7F))
    | set(range(0xA0, 0x100))
    | set(range(0x2010, 0x2028))
    | set(range(0x2030, 0x205F))
    | set(range(0x3000, 0x3040))
    | {0x30FC}
    # The CSS text-emphasis marks the reader draws: bullet U+2022 (already in 2010-2027), white bullet, circles,
    # double circles, triangles and the sesame dots. Noto CJK draws all ten; the region sets name none of them.
    | {0x2022, 0x25E6, 0x25CF, 0x25CB, 0x25C9, 0x25CE, 0x25B2, 0x25B3, 0xFE45, 0xFE46}
    | set(range(0xFF01, 0xFFA0))
)

CHARSETS = ("ja", "zh-Hans", "zh-Hant", "ko")

# What the sets name that Noto CJK does not draw (Sans2.004 and Serif2.003, every region alike), left out of
# them. Any other character a source lacks fails the build: a source that lost glyphs is never shipped short.
BASE_GAPS = {
    0x2017, 0x201B, 0x201F, 0x2023, 0x2024, 0x2031, 0x2034, 0x2036, 0x2037, 0x2038, 0x203D, 0x203E, 0x203F,
    0x2040, 0x2041, 0x2043, 0x2044, 0x2045, 0x2046, 0x204A, 0x204B, 0x204C, 0x204D, 0x204E, 0x204F, 0x2050,
    0x2052, 0x2053, 0x2054, 0x2055, 0x2056, 0x2057, 0x2058, 0x2059, 0x205A, 0x205B, 0x205C, 0x205D, 0x205E,
}
CHARSET_GAPS = {
    "ja": {0x2252},
    "zh-Hans": set(),
    "zh-Hant": {0x02CD, 0x203E, 0x223C, 0x2252},
    "ko": {
        0x0126, 0x0127, 0x0131, 0x0132, 0x0133, 0x0138, 0x013F, 0x0140, 0x0141, 0x0142, 0x0149, 0x014A, 0x014B,
        0x0166, 0x0167, 0x02D0, 0x02D8, 0x02DA, 0x02DB, 0x02DD, 0x207F, 0x2081, 0x2082, 0x2083, 0x2084, 0x2153,
        0x2154, 0x215B, 0x215C, 0x215D, 0x215E, 0x223C, 0x2252, 0x25C8,
    },
}

# Noto CJK has no U+FFFD, which the renderer draws for a character a font lacks: the build copies Noto
# Sans's (OFL too, bundled for the built-in fonts) into every family, at the same 1000-unit em.
REPLACEMENT = 0xFFFD
REPLACEMENT_DONOR = Path(__file__).resolve().parent.parent / "builtinFonts" / "source" / "NotoSans" / "NotoSans-Regular.ttf"

# Tables the engine never reads, and the vertical-text data it has no use for.
DROPPED_TABLES = ["GSUB", "BASE", "vhea", "vmtx", "VORG", "DSIG"]
KEPT_TABLES = frozenset({"CFF ", "GPOS", "OS/2", "cmap", "head", "hhea", "hmtx", "maxp", "name", "post"})

# The engine's CFF scratch caps (spec 9.2): a glyph over either fails the build, unless it is listed here.
MAX_POINTS = 512
MAX_CONTOURS = 256
OVERSIZE = {"NotoSerifTC": frozenset({0x9F98})}  # 龘, 621 points


class BuildError(Exception):
    pass


def _encodable(codec: str, accept) -> set[int]:
    """The BMP characters `codec` writes as a byte sequence `accept` takes."""
    out = set()
    for cp in range(0xA0, 0x10000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        try:
            encoded = chr(cp).encode(codec)
        except UnicodeEncodeError:
            continue
        if accept(encoded):
            out.add(cp)
    return out


def _double_byte(lead_min: int, lead_max: int):
    """Two-byte EUC sequences whose lead byte is in range and whose trail byte is A1..FE."""
    return lambda b: len(b) == 2 and lead_min <= b[0] <= lead_max and 0xA1 <= b[1] <= 0xFE


def unihan_field(unihan_zip: Path, field: str) -> set[int]:
    """The code points Unihan gives a value for `field` (Unihan_OtherMappings.txt)."""
    with zipfile.ZipFile(unihan_zip) as z:
        text = z.read("Unihan_OtherMappings.txt").decode("utf-8")
    out = set()
    for line in text.splitlines():
        if not line.startswith("U+"):
            continue
        cp, name, _value = line.split("\t", 2)
        if name == field:
            out.add(int(cp[2:], 16))
    return out


def charset(name: str, unihan_zip: Path) -> set[int]:
    """A language's characters, BASE not included.

    ja: JIS X 0208, the NEC special characters of cp932 row 13 (circled numbers, Roman numerals, ...), and
        the Joyo and Jinmeiyo kanji (Unihan kJoyoKanji, kJinmeiyoKanji: the 2010 revision JIS X 0208 lacks).
    zh-Hans: GB2312, and the Table of General Standard Chinese Characters (Unihan kTGH).
    zh-Hant: Big5 and cp950.
    ko: every Hangul syllable, the compatibility jamo, and all of KS X 1001.
    """
    if name == "ja":
        nec_row13 = set()
        for trail in range(0x40, 0x9D):
            try:
                nec_row13.add(ord(bytes([0x87, trail]).decode("cp932")))
            except UnicodeDecodeError:
                pass
        return (
            # Keep the proven JIS X 0208 repertoire, then add only CJK
            # ideographs from JIS X 0213 Plane 1. The full Plane-1 codec
            # also contains Latin/other characters absent from Noto CJK JP.
            _encodable("euc_jp", _double_byte(0xA1, 0xFE))
            | {
                cp
                for cp in _encodable("euc_jis_2004", _double_byte(0xA1, 0xFE))
                if (
                    0x3400 <= cp <= 0x4DBF
                    or 0x4E00 <= cp <= 0x9FFF
                    or 0xF900 <= cp <= 0xFAFF
                )
            }
            | nec_row13
            | unihan_field(unihan_zip, "kJoyoKanji")
            | unihan_field(unihan_zip, "kJinmeiyoKanji")
        )
    if name == "zh-Hans":
        return _encodable("gb2312", _double_byte(0xA1, 0xF7)) | unihan_field(unihan_zip, "kTGH")
    if name == "zh-Hant":
        return _encodable("big5", lambda b: len(b) == 2) | _encodable("cp950", lambda b: len(b) == 2)
    if name == "ko":
        return (
            set(range(0xAC00, 0xD7A4))
            | set(range(0x3131, 0x318F))
            | _encodable("euc_kr", _double_byte(0xA1, 0xFE))
        )
    raise BuildError(f"unknown charset {name!r}; one of {', '.join(CHARSETS)}")


def family_set(name: str, unihan_zip: Path) -> set[int]:
    """What a family of charset `name` holds, U+FFFD aside: the language's characters and BASE, without the
    gaps Noto CJK has."""
    return (charset(name, unihan_zip) | BASE) - BASE_GAPS - CHARSET_GAPS[name]


def check_fonttools(pinned: str) -> None:
    """The subset's bytes depend on fontTools' subsetter and CFF writer, so the build pins its version."""
    if fontTools.version != pinned:
        raise BuildError(f"fontTools {fontTools.version} is installed, the build pins {pinned}")


def _private_dicts(top):
    """The Private dicts of a CFF font: one per FD of a CID-keyed font, else the Top DICT's."""
    if hasattr(top, "FDArray"):
        return [fd.Private for fd in top.FDArray]
    return [top.Private]


class _Count(BasePen):
    """Points and contours as the engine counts them: 1 per moveTo and lineTo, 3 per curveTo."""

    def __init__(self, glyph_set):
        super().__init__(glyph_set)
        self.points = 0
        self.contours = 0

    def _moveTo(self, p):
        self.points += 1
        self.contours += 1

    def _lineTo(self, p):
        self.points += 1

    def _curveToOne(self, a, b, c):
        self.points += 3

    def _qCurveToOne(self, a, b):
        self.points += 2

    def _closePath(self):
        pass


def _glyph_extremes(font: TTFont, skip: set[str] = frozenset()) -> tuple[int, int, str, str]:
    """(most points, most contours, the glyph with the most points, the glyph with the most contours) of the
    glyphs not in `skip`."""
    glyph_set = font.getGlyphSet()
    best_points = best_contours = 0
    points_name = contours_name = ""
    for name in font.getGlyphOrder():
        if name in skip:
            continue
        counter = _Count(glyph_set)
        glyph_set[name].draw(counter)
        if counter.points > best_points:
            best_points, points_name = counter.points, name
        if counter.contours > best_contours:
            best_contours, contours_name = counter.contours, name
    return best_points, best_contours, points_name, contours_name


def _kern_pairs(font: TTFont) -> int:
    """The glyph pairs the kept kerning names: what the glyph store's pair index may come to hold."""
    if "GPOS" not in font:
        return 0
    pairs = 0
    for lookup in font["GPOS"].table.LookupList.Lookup:
        for sub in lookup.SubTable:
            if getattr(sub, "ExtSubTable", None) is not None:
                sub = sub.ExtSubTable
            if lookup.LookupType not in (2, 9) or not hasattr(sub, "Format"):
                continue
            if sub.Format == 1:
                pairs += sum(len(ps.PairValueRecord) for ps in sub.PairSet)
            elif sub.Format == 2:
                firsts = len(sub.Coverage.glyphs)
                seconds = {g for g, c in sub.ClassDef2.classDefs.items() if c}
                pairs += firsts * len(seconds)
    return pairs


def _rename(font: TTFont, font_name: str) -> None:
    """The family and full names become `font_name` ("Noto Sans JP"), the PostScript name its spaceless form:
    no subset claims to be the whole Noto Sans CJK. The CFF's own names follow, each FD keeping its suffix."""
    name = font["name"]
    old_ps = name.getDebugName(6)
    new_ps = font_name.replace(" ", "") + "-Regular"
    unique = name.getDebugName(3)
    for record in list(name.names):
        if record.nameID in (16, 17, 21, 22):  # typographic and WWS names would keep the old family
            name.removeNames(nameID=record.nameID)
    name.setName(font_name, 1, 3, 1, 0x409)
    name.setName("Regular", 2, 3, 1, 0x409)
    name.setName(unique.replace(old_ps, new_ps) if unique and old_ps else new_ps, 3, 3, 1, 0x409)
    name.setName(font_name, 4, 3, 1, 0x409)
    name.setName(new_ps, 6, 3, 1, 0x409)

    cff = font["CFF "].cff
    old_cff = cff.fontNames[0]
    cff.fontNames[0] = new_ps
    top = cff.topDictIndex[0]
    top.FamilyName = font_name
    top.FullName = font_name + " Regular"
    for fd in getattr(top, "FDArray", []):
        suffix = fd.FontName[len(old_cff):] if fd.FontName.startswith(old_cff) else "-" + fd.FontName
        fd.FontName = new_ps + suffix


def _add_replacement_glyph(font: TTFont, donor_path: Path) -> None:
    """Draws the donor's U+FFFD into the CID-keyed CFF as a new glyph of the Latin proportional FD (the one
    U+0041 uses), with an advance of 1000 and its own left side bearing, mapped in every Unicode cmap."""
    donor = TTFont(str(donor_path))
    if donor["head"].unitsPerEm != 1000:
        raise BuildError(f"{donor_path.name}: em of {donor['head'].unitsPerEm}, not 1000")
    donor_name = donor.getBestCmap().get(REPLACEMENT)
    if donor_name is None or donor["glyf"][donor_name].isComposite():
        raise BuildError(f"{donor_path.name}: no simple U+FFFD glyph")

    top = font["CFF "].cff.topDictIndex[0]
    cff = font["CFF "].cff
    latin = font.getBestCmap().get(0x41)
    if latin is None:
        raise BuildError("the subset has no U+0041 to take the replacement glyph's font dict from")
    fd_index = top.FDSelect[font.getGlyphID(latin)]
    private = top.FDArray[fd_index].Private

    highest = max(int(m.group(1)) for m in (re.fullmatch(r"cid(\d+)", n) for n in font.getGlyphOrder()) if m)
    cid = highest + 1
    if cid >= top.CIDCount:
        raise BuildError(f"CID {cid} for the replacement glyph is beyond the font's CIDCount of {top.CIDCount}")
    new_name = f"cid{cid:05d}"

    donor_set = donor.getGlyphSet()
    width = None if private.defaultWidthX == 1000 else 1000 - private.nominalWidthX
    pen = T2CharStringPen(width, None)
    donor_set[donor_name].draw(pen)
    charstring = pen.getCharString(private=private, globalSubrs=cff.GlobalSubrs)
    charstring.fdSelectIndex = fd_index
    bounds = ControlBoundsPen(None)  # of the rounded cubics just made, so the bearing matches the CFF outline
    charstring.draw(bounds)
    x_min = round(bounds.bounds[0])

    order = font.getGlyphOrder() + [new_name]
    font.setGlyphOrder(order)
    top.charset.append(new_name)
    top.FDSelect.append(fd_index)
    strings = top.CharStrings
    if strings.charStringsAreIndexed:
        strings.charStrings[new_name] = len(strings.charStringsIndex)
        strings.charStringsIndex.append(charstring)
    else:
        strings.charStrings[new_name] = charstring
    font["hmtx"][new_name] = (1000, x_min)
    for table in font["cmap"].tables:
        if table.isUnicode():
            table.cmap[REPLACEMENT] = new_name


def subset_cjk(source: Path, codepoints: set[int], font_name: str, out: Path, *,
               donor: Path = REPLACEMENT_DONOR, oversize: frozenset[int] = frozenset()) -> dict:
    """Writes the CFF subset of `source` holding `codepoints` to `out`, and returns what the build log
    records. Fails when the source lacks a glyph for one of them."""
    font = TTFont(str(source), recalcTimestamp=False, lazy=False)
    cmap = font.getBestCmap()
    missing = sorted(cp for cp in codepoints if cp not in cmap)
    if missing:
        shown = " ".join(f"U+{cp:04X}" for cp in missing[:20])
        raise BuildError(f"{source.name} has no glyph for {len(missing)} code points of the set: {shown}")
    dropped = {tag: font.reader.tables[tag].length for tag in DROPPED_TABLES if tag in font.reader.tables}

    options = subset.Options()
    options.hinting = False
    options.desubroutinize = True
    options.layout_features = ["kern"]  # GPOS keeps Latin kerning only; nothing of GSUB is read
    options.drop_tables += DROPPED_TABLES
    options.name_IDs = list(range(15))  # 13 and 14 carry the OFL notice
    options.name_legacy = False
    options.glyph_names = False
    options.notdef_outline = True
    options.recalc_timestamp = False
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    for tag in DROPPED_TABLES:
        if tag in font:
            del font[tag]

    _rename(font, font_name)
    _add_replacement_glyph(font, donor)
    out.parent.mkdir(parents=True, exist_ok=True)
    font.save(str(out))
    check(out, codepoints | {REPLACEMENT}, oversize)

    built = TTFont(str(out))
    max_points, _, _, _ = _glyph_extremes(built, _oversize_names(built, oversize))
    return {
        "characters": len(codepoints),
        "cmap": len(set(built.getBestCmap()) - {REPLACEMENT}),
        "glyphs": len(built.getGlyphOrder()),
        "bytes": out.stat().st_size,
        "dropped": dropped,
        "kernPairs": _kern_pairs(built),
        "maxPoints": max_points,
    }


def _oversize_names(font: TTFont, oversize: frozenset[int]) -> set[str]:
    cmap = font.getBestCmap()
    return {cmap[cp] for cp in oversize if cp in cmap}


def check(path: Path, codepoints: set[int], oversize: frozenset[int] = frozenset()) -> None:
    """What the reader's engine needs of the file (lib/EpdFont/ttf): OpenType CFF (version 1, not CFF2, not a
    collection) without subroutines; a Unicode cmap of format 4 or 12 covering the set; the regular style, so
    discovery lists it as the family's regular file; only the tables the engine reads, with `kern` as the one
    GPOS feature; and no glyph beyond the engine's scratch caps, but those of the `oversize` code points."""
    with open(path, "rb") as f:
        version = f.read(4)
    if version != b"OTTO":
        raise BuildError(f"{path.name}: sfnt version {version!r}, not OTTO (OpenType CFF)")
    font = TTFont(str(path))
    if "CFF " not in font or "CFF2" in font:
        raise BuildError(f"{path.name}: not an OTTO font with a CFF table (CFF2 is not read)")
    cff = font["CFF "].cff
    if cff.major != 1:
        raise BuildError(f"{path.name}: CFF major version {cff.major}, not 1")
    top = cff.topDictIndex[0]
    if len(cff.GlobalSubrs) or any(hasattr(private, "Subrs") for private in _private_dicts(top)):
        raise BuildError(f"{path.name}: holds CFF subroutines (the build desubroutinizes)")
    if not any((t.platformID, t.format) in ((3, 4), (3, 12)) and t.platEncID in (1, 10)
               for t in font["cmap"].tables):
        raise BuildError(f"{path.name}: no (3,1) format 4 or (3,10) format 12 cmap")
    cmap = font.getBestCmap()
    uncovered = [cp for cp in codepoints if cp not in cmap]
    if uncovered:
        raise BuildError(f"{path.name}: the cmap lacks {len(uncovered)} code points of the set")
    if font["head"].macStyle & 3 or font["OS/2"].usWeightClass >= 600:
        raise BuildError(f"{path.name}: not a regular style (macStyle {font['head'].macStyle}, "
                         f"usWeightClass {font['OS/2'].usWeightClass})")
    extra = set(font.keys()) - {"GlyphOrder"} - KEPT_TABLES
    if extra:
        raise BuildError(f"{path.name}: tables the engine does not read: {', '.join(sorted(extra))}")
    if "GPOS" in font:
        features = {r.FeatureTag for r in font["GPOS"].table.FeatureList.FeatureRecord}
        if features - {"kern"}:
            raise BuildError(f"{path.name}: GPOS features beyond kern: {', '.join(sorted(features - {'kern'}))}")
    points, contours, points_name, contours_name = _glyph_extremes(font, _oversize_names(font, oversize))
    if points > MAX_POINTS:
        raise BuildError(f"{path.name}: glyph {points_name} has {points} points, over the engine's {MAX_POINTS}")
    if contours > MAX_CONTOURS:
        raise BuildError(f"{path.name}: glyph {contours_name} has {contours} contours, over the engine's {MAX_CONTOURS}")


def build_family(family: dict, output_dir: Path, source: Path, unihan_zip: Path) -> dict:
    """One sd-fonts.yaml entry with a charset: <output_dir>/<name>-Regular.otf."""
    out = output_dir / f"{family['name']}-Regular.otf"
    return subset_cjk(source, family_set(family["charset"], unihan_zip), family["fontName"], out,
                      oversize=OVERSIZE.get(family["name"], frozenset()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path)
    parser.add_argument("unihan", type=Path)
    parser.add_argument("charset", choices=CHARSETS)
    parser.add_argument("font_name")
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    try:
        report = subset_cjk(args.source, family_set(args.charset, args.unihan), args.font_name, args.out)
    except BuildError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    print(report)


if __name__ == "__main__":
    main()
