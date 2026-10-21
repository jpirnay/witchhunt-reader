#!/usr/bin/env python3
"""Builds the CFF test fonts in test/fixtures/fonts (see the README there).

    python lib/EpdFont/scripts/make_cff_fixtures.py --source-dir <dir with the two Noto CJK OTFs> \
        --out test/fixtures/fonts

The script does not download: --source-dir holds NotoSansCJKjp-Regular.otf and NotoSerifCJKjp-Regular.otf, each
checked against the sha256 pinned below. The output is byte-identical on every run (fixed timestamps, no
randomness) and the script writes nothing but the seven fonts.
"""

import argparse
import hashlib
import io
import sys
from pathlib import Path

from fontTools import subset
from fontTools.cffLib import SubrsIndex
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable

# Sources, pinned. noto-cjk release tags Sans2.004 and Serif2.003.
SOURCES = {
    "NotoSansCJKjp-Regular.otf": (
        "https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf",
        "68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5",
    ),
    "NotoSerifCJKjp-Regular.otf": (
        "https://github.com/notofonts/noto-cjk/raw/Serif2.003/Serif/OTF/Japanese/NotoSerifCJKjp-Regular.otf",
        "d9854c7a8ef170b5a7932558856fd64eb8de0b007cd823fed6f9f514ad2803d3",
    ),
}

# ASCII, a few Japanese marks, hiragana, katakana and a spread of kanji from plain to dense.
KANJI = "日本語文字書読鬱慶議驚龍鑑響漢点線曲円角"
CODEPOINTS = (
    list(range(0x20, 0x7F))
    + [ord(c) for c in "、。「」・ー"]
    + list(range(0x3041, 0x3094))
    + list(range(0x30A1, 0x30F4))
    + [ord(c) for c in KANJI]
)

# 2000-01-01T00:00:00Z as seconds since 1904-01-01, the timestamp of the synthetic font.
FIXED_TIME = 3155673600


def verify_source(source_dir: Path, name: str) -> Path:
    path = source_dir / name
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: {SOURCES[name][0]}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != SOURCES[name][1]:
        sys.exit(f"{path}: sha256 {digest}, expected {SOURCES[name][1]}")
    return path


def subset_noto(path: Path) -> bytes:
    """The character set, no hinting, kern only, CID structure and subroutines kept."""
    options = subset.Options()
    options.hinting = False
    options.layout_features = ["kern"]
    options.recalc_timestamp = False
    options.desubroutinize = False
    options.notdef_outline = True
    font = subset.load_font(str(path), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=CODEPOINTS)
    subsetter.subset(font)
    out = io.BytesIO()
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def save(font: TTFont) -> bytes:
    out = io.BytesIO()
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def flatten(data: bytes) -> bytes:
    """The same font with every subroutine call inlined."""
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    font["CFF "].cff.desubroutinize()
    return save(font)


def truetype_twin(data: bytes) -> bytes:
    """The same glyphs as TrueType quadratics within one font unit (fontTools' otf2ttf recipe), for the device
    bench's CFF-against-TrueType run."""
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    order = font.getGlyphOrder()
    glyph_set = font.getGlyphSet()
    font["loca"] = newTable("loca")
    font["glyf"] = glyf = newTable("glyf")
    glyf.glyphOrder = order
    glyf.glyphs = {}
    for name in order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(Cu2QuPen(pen, 1.0, reverse_direction=True))
        glyf.glyphs[name] = pen.glyph()
    del font["CFF "]
    if "VORG" in font:
        del font["VORG"]
    glyf.compile(font)
    hmtx = font["hmtx"]
    for name, glyph in glyf.glyphs.items():
        if hasattr(glyph, "xMin"):
            hmtx[name] = (hmtx[name][0], glyph.xMin)
    font["maxp"] = maxp = newTable("maxp")
    maxp.tableVersion = 0x00010000
    for attr in ("maxZones", "maxTwilightPoints", "maxStorage", "maxFunctionDefs", "maxInstructionDefs",
                 "maxStackElements", "maxSizeOfInstructions", "maxComponentElements"):
        setattr(maxp, attr, 1 if attr == "maxZones" else 0)
    maxp.compile(font)
    font["post"].formatType = 3.0
    font.sfntVersion = "\x00\x01\x00\x00"
    return save(font)


def slant(data: bytes) -> bytes:
    """The flat font with a non-identity top-DICT FontMatrix (an oblique of 0.2)."""
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    font["CFF "].cff.topDictIndex[0].FontMatrix = [0.001, 0, 0.0002, 0.001, 0, 0]
    return save(font)


# ---------------------------------------------------------------------------------------------------------------
# CffOperators.otf: one glyph per Type 2 operator family. Programs are operator lists; a number is pushed, a
# string is an operator. Every glyph advances 1000 (the Private defaultWidthX) so none carries a width argument.

LOCAL_NEST = 15  # subroutines 0..14 call each other in a chain: 15 frames deep from the glyph


def local(i):
    return i - 107  # subroutine bias for fewer than 1240 subroutines


def glyphs():
    """name -> program, in glyph order."""
    g = {}
    g[".notdef"] = [50, 0, "rmoveto", 400, 0, "rlineto", 0, 600, "rlineto", -400, 0, "rlineto", "endchar"]
    g["A"] = [100, 0, "rmoveto", 300, 700, "rlineto", 300, -700, "rlineto", -120, 0, "rlineto", -180, 450, "rlineto",
              -180, -450, "rlineto", "endchar"]
    g["grave"] = [150, 800, "rmoveto", 100, 150, "rlineto", 80, 0, "rlineto", -60, -150, "rlineto", "endchar"]
    # seac: adx ady bchar achar endchar, over StandardEncoding A (65) and grave (193).
    g["Agrave"] = [20, 50, 65, 193, "endchar"]

    # Lines.
    g["rlineto_1"] = [100, 100, "rmoveto", 400, 300, "rlineto", "endchar"]
    g["rlineto_3"] = [100, 100, "rmoveto", 400, 0, 0, 300, -200, 100, "rlineto", "endchar"]
    g["hlineto_odd"] = [100, 100, "rmoveto", 400, 300, 200, "hlineto", "endchar"]
    g["hlineto_even"] = [100, 100, "rmoveto", 400, 300, -200, 100, "hlineto", "endchar"]
    g["vlineto_odd"] = [100, 100, "rmoveto", 300, 400, 200, "vlineto", "endchar"]
    g["vlineto_even"] = [100, 100, "rmoveto", 300, 400, -100, 200, "vlineto", "endchar"]
    g["hmoveto_vmoveto"] = [100, "hmoveto", 200, 0, "rlineto", 0, 200, "rlineto", 300, "vmoveto", 200, 0, "rlineto",
                            0, 100, "rlineto", "endchar"]

    # Curves.
    g["rrcurveto_1"] = [100, 100, "rmoveto", 100, 200, 150, 100, 100, -50, "rrcurveto", "endchar"]
    g["rrcurveto_2"] = [100, 100, "rmoveto", 100, 200, 150, 100, 100, -50, 100, -200, 150, -100, -100, 50,
                        "rrcurveto", "endchar"]
    g["hhcurveto_even"] = [100, 100, "rmoveto", 100, 150, 100, 150, "hhcurveto", "endchar"]
    g["hhcurveto_odd"] = [100, 100, "rmoveto", 50, 100, 150, 100, 150, "hhcurveto", "endchar"]
    g["hhcurveto_two"] = [100, 100, "rmoveto", -30, 100, 150, 100, 150, 100, 100, 100, 100, "hhcurveto", "endchar"]
    g["vvcurveto_even"] = [100, 100, "rmoveto", 100, 150, 100, 150, "vvcurveto", "endchar"]
    g["vvcurveto_odd"] = [100, 100, "rmoveto", 50, 100, 150, 100, 150, "vvcurveto", "endchar"]
    g["vvcurveto_two"] = [100, 100, "rmoveto", -30, 100, 150, 100, 150, 100, 100, 100, 100, "vvcurveto", "endchar"]
    g["hvcurveto_4"] = [100, 100, "rmoveto", 150, 100, 100, 150, "hvcurveto", "endchar"]
    g["hvcurveto_5"] = [100, 100, "rmoveto", 150, 100, 100, 150, 40, "hvcurveto", "endchar"]
    g["hvcurveto_8"] = [100, 100, "rmoveto", 150, 100, 100, 150, 100, 100, 100, 100, "hvcurveto", "endchar"]
    g["hvcurveto_9"] = [100, 100, "rmoveto", 150, 100, 100, 150, 100, 100, 100, 100, 40, "hvcurveto", "endchar"]
    g["vhcurveto_4"] = [100, 100, "rmoveto", 150, 100, 100, 150, "vhcurveto", "endchar"]
    g["vhcurveto_5"] = [100, 100, "rmoveto", 150, 100, 100, 150, 40, "vhcurveto", "endchar"]
    g["vhcurveto_8"] = [100, 100, "rmoveto", 150, 100, 100, 150, 100, 100, 100, 100, "vhcurveto", "endchar"]
    g["vhcurveto_9"] = [100, 100, "rmoveto", 150, 100, 100, 150, 100, 100, 100, 100, 40, "vhcurveto", "endchar"]
    g["rcurveline_1"] = [100, 100, "rmoveto", 100, 200, 150, 100, 100, -50, 100, -200, "rcurveline", "endchar"]
    g["rcurveline_2"] = [100, 100, "rmoveto", 100, 200, 150, 100, 100, -50, 100, 200, 150, -100, 100, -50,
                         100, -200, "rcurveline", "endchar"]
    g["rlinecurve_1"] = [100, 100, "rmoveto", 100, 200, 100, 100, 150, 100, 100, -50, "rlinecurve", "endchar"]
    g["rlinecurve_2"] = [100, 100, "rmoveto", 100, 200, 100, -100, 100, 100, 150, 100, 100, -50, "rlinecurve",
                         "endchar"]

    # Flex.
    g["flex"] = [100, 300, "rmoveto", 100, 30, 100, 40, 100, 30, 100, 0, 100, -30, 100, -40, 50, "flex",
                 "endchar"]
    g["hflex"] = [100, 300, "rmoveto", 100, 100, 40, 100, 100, 100, 100, "hflex", "endchar"]
    g["hflex1"] = [100, 300, "rmoveto", 100, 30, 100, 40, 100, 100, 100, -30, 100, "hflex1", "endchar"]
    # flex1: the last argument is dx6 when the five segments travel further in x than in y, dy6 otherwise.
    g["flex1_x"] = [100, 300, "rmoveto", 100, 30, 100, 40, 100, 30, 100, 0, 100, -20, 100, "flex1", "endchar"]
    g["flex1_y"] = [100, 300, "rmoveto", 30, 100, 40, 100, 30, 100, 0, 100, -20, 100, 100, "flex1", "endchar"]

    # Hints. 8 stems: the mask is one byte; 9 stems: two bytes.
    g["hintmask_8"] = [0, 50, 100, 50, 100, 50, 100, 50, "hstemhm", 0, 50, 100, 50, 100, 50, 100, 50, "vstemhm",
                       "hintmask", b"\xff", 100, 100, "rmoveto", 400, 0, "rlineto", 0, 400, "rlineto", "endchar"]
    # The vertical stems are the arguments left on the stack before the hintmask.
    g["hintmask_9"] = [0, 50, 100, 50, 100, 50, 100, 50, 100, 50, "hstemhm", 0, 50, 100, 50, 100, 50, 100, 50,
                       "hintmask", b"\xff\x80", 100, 100, "rmoveto", 400, 0, "rlineto", 0, 400, "rlineto",
                       "endchar"]
    g["cntrmask_8"] = [0, 50, 100, 50, 100, 50, 100, 50, "hstemhm", 0, 50, 100, 50, 100, 50, 100, 50, "vstemhm",
                       "cntrmask", b"\xa5", "hintmask", b"\x5a", 100, 100, "rmoveto", 400, 0, "rlineto", 0, 400,
                       "rlineto", "endchar"]
    g["cntrmask_9"] = [0, 50, 100, 50, 100, 50, 100, 50, 100, 50, "hstemhm", 0, 50, 100, 50, 100, 50, 100, 50,
                       "cntrmask", b"\xa5\x80", "hintmask", b"\x5a\x00", 100, 100, "rmoveto", 400, 0, "rlineto",
                       0, 400, "rlineto", "endchar"]
    g["hintmask_change"] = [0, 50, 100, 50, "hstemhm", 0, 50, 100, 50, "vstemhm", "hintmask", b"\xf0", 100, 100,
                            "rmoveto", 300, 0, "rlineto", 0, 300, "rlineto", "hintmask", b"\x0f", 300, 100,
                            "rmoveto", 200, 0, "rlineto", 0, 200, "rlineto", "endchar"]

    # Subroutines. Local subr 0..14 form the chain, 15 is a plain segment; global subr 0 is a plain segment.
    g["callsubr"] = [100, 100, "rmoveto", local(15), "callsubr", -150, 0, "rlineto", "endchar"]
    g["callgsubr"] = [100, 100, "rmoveto", local(0), "callgsubr", -150, 0, "rlineto", "endchar"]
    g["subr_nested"] = [100, 100, "rmoveto", local(0), "callsubr", 0, -100, "rlineto", "endchar"]

    # Arithmetic, stack and storage. Each yields an rlineto argument pair.
    g["arith"] = [100, 0, "rmoveto", 2, 3, "add", 100, "mul", 0, "rlineto", 0, 600, 400, "sub", "rlineto",
                  -300, "abs", "neg", 0, "rlineto", 0, 10000, 100, "div", "rlineto", 10000, "sqrt", 0, "rlineto",
                  "endchar"]
    g["stack"] = [100, 0, "rmoveto", 200, "dup", "rlineto", 100, 200, "exch", "rlineto", -80, 120, 1, "index", 1,
                  "index", "rlineto", 0, 40, 30, 60, 4, 1, "roll", "rlineto", 0, 40, 30, 60, 4, -1, "roll",
                  "rlineto", 120, 60, 99, "drop", "rlineto", "endchar"]
    g["storage"] = [100, 0, "rmoveto", 70, 0, "put", 90, 1, "put", 0, "get", 1, "get", "rlineto", 1, "get", 0, "get",
                    "rlineto", "endchar"]
    g["logic"] = [100, 0, "rmoveto", 100, 1, 1, "and", "mul", 0, "rlineto", 0, 80, 1, 0, "or", "mul", "rlineto",
                  -60, 0, "not", "mul", 0, "rlineto", 0, 90, 5, 5, "eq", "mul", "rlineto", 50, 90, 2, 1, "ifelse",
                  0, "rlineto", 0, 50, 90, 2, 1, "ifelse", "rlineto", 100, 200, 3, 5, "ifelse", 0, "rlineto",
                  "endchar"]

    # Numbers and degenerate contours.
    g["fractional"] = [100.5, 0.25, "rmoveto", 300.75, 100.5, "rlineto", -50.125, 200.25, "rlineto", -200.5, -0.75,
                       "rlineto", "endchar"]
    g["zero_length_line"] = [100, 100, "rmoveto", 0, 0, "rlineto", 200, 0, "rlineto", 0, 200, "rlineto", 0, "hlineto",
                             "endchar"]
    g["single_point"] = [300, 300, "rmoveto", "endchar"]
    g["point_then_shape"] = [100, 100, "rmoveto", 50, 50, "rmoveto", 200, 0, "rlineto", 0, 200, "rlineto", "endchar"]
    return g


def subr_programs():
    """(local subrs, global subrs) as programs."""
    local_subrs = []
    for i in range(LOCAL_NEST - 1):
        local_subrs.append([20, 20, "rlineto", local(i + 1), "callsubr", "return"])
    local_subrs.append([20, 20, "rlineto", "return"])  # the last link of the chain
    local_subrs.append([200, 0, "rlineto", 0, 200, "rlineto", "return"])  # 15
    global_subrs = [[200, 0, "rlineto", 0, 200, "rlineto", "return"]]
    return local_subrs, global_subrs


def make_operators_font() -> bytes:
    programs = glyphs()
    order = list(programs)
    fb = FontBuilder(1000, isTTF=False)
    fb.font.recalcTimestamp = False
    fb.setupGlyphOrder(order)
    cmap = {0x41: "A", 0x60: "grave", 0xC0: "Agrave"}
    for i, name in enumerate(order[4:]):
        cmap[0xE000 + i] = name
    fb.setupCharacterMap(cmap)

    charstrings = {name: T2CharString(program=program) for name, program in programs.items()}
    fb.setupCFF(
        "CffOperatorsTest-Regular",
        {"FullName": "CFF Operators Test", "FamilyName": "CFF Operators Test", "Weight": "Regular"},
        charstrings,
        {"defaultWidthX": 1000, "nominalWidthX": 0},
    )
    top = fb.font["CFF "].cff.topDictIndex[0]
    private = top.Private
    local_programs, global_programs = subr_programs()
    private.Subrs = SubrsIndex()
    for program in local_programs:
        private.Subrs.append(T2CharString(program=program, private=private, globalSubrs=top.GlobalSubrs))
    for program in global_programs:
        top.GlobalSubrs.append(T2CharString(program=program, private=private, globalSubrs=top.GlobalSubrs))

    fb.setupHorizontalMetrics({name: (1000, 0) for name in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "CFF Operators Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost(keepGlyphNames=False)
    fb.setupHead(created=FIXED_TIME, modified=FIXED_TIME, xMin=0, yMin=0, xMax=1000, yMax=1000)
    top.FontBBox = [0, 0, 1000, 1000]
    # fontTools cannot trace the arithmetic operators, so it cannot recompute the bounding boxes: they are fixed.
    fb.font.recalcBBoxes = False
    return save(fb.font)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", required=True, type=Path, help="holds the two pinned Noto CJK JP OTFs")
    parser.add_argument("--out", required=True, type=Path, help="output directory (test/fixtures/fonts)")
    args = parser.parse_args()

    sans = subset_noto(verify_source(args.source_dir, "NotoSansCJKjp-Regular.otf"))
    serif = subset_noto(verify_source(args.source_dir, "NotoSerifCJKjp-Regular.otf"))
    flat_sans = flatten(sans)
    outputs = {
        "NotoSansJP-Cff.otf": sans,
        "NotoSerifJP-Cff.otf": serif,
        "NotoSansJP-CffFlat.otf": flat_sans,
        "NotoSerifJP-CffFlat.otf": flatten(serif),
        "NotoSansJP-CffSlanted.otf": slant(flat_sans),
        "NotoSansJP-TrueType.ttf": truetype_twin(flat_sans),
        "CffOperators.otf": make_operators_font(),
    }
    args.out.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(outputs.items()):
        (args.out / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
