#!/usr/bin/env python3
"""Builds the Myanmar shaping fixture of the Myanmar design's PR 1 and records what HarfBuzz does with it.

    python -I test/ot_shaper/make_myanmar_fixtures.py <sources dir> <fonts out dir> \
        [--header-out test/ot_shaper/MyanmarExpected.h]

<sources dir> holds NotoSansMyanmar-Regular.ttf (notofonts.github.io at 5c82cef, unhinted TTF), checked against the
sha256 below; the script does not download it. It writes NotoSansMyanmar-Shaping-Regular.ttf into <fonts out dir>:
that font cut with fontTools.subset to the code points of the words below plus U+0020, U+200C, U+200D and U+25CC,
every layout feature kept.

Into --header-out it writes HarfBuzz 14.5's own answers (uharfbuzz): per language the lookups it plans (its buffer
messages, as make_arabic_fixtures.py reads them) and per word the glyph run for direction ltr, script Mymr, at scale
2133 and ppem 33. The output is byte-identical on every run.

The words of Somnius/CrossInk-Indic's Myanmar tests (commits 44b3c613 and 5fcd8ce4 by Lef, MIT) are among them.

Needs fonttools 4.63.0 and uharfbuzz 0.56.2 (the versions lib/EpdFont/scripts/sd-fonts.yaml pins).
"""

import argparse
import hashlib
import io
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_arabic_fixtures import c_hex_string, cut, font_constant, hb_font, planned_lookups, shape  # noqa: E402

SOURCE_COMMIT = "5c82cef0a8f08806f932062e18c02be62be278f0"
SOURCE = "NotoSansMyanmar-Regular.ttf"
SOURCE_SHA256 = "f4be6ef43871516d8347c5217a2033991b8ecda2dd1200007c2fb431a66d64db"
FIXTURE = "NotoSansMyanmar-Shaping-Regular.ttf"
EXTRA_CODEPOINTS = {0x0020, 0x200C, 0x200D, 0x25CC}
PLAN_LANGUAGES = ["", "my", "mnw", "ksw", "shn"]
# GSUB stages of every Myanmar plan: one per pause (after rvrn, setup_syllables_myanmar, reorder_myanmar, each of the
# four basic features and hb_syllabic_clear_var) and the last one hb_ot_map_builder_t::compile adds.
GSUB_STAGES = 1 + 1 + 1 + 4 + 1 + 1


def w(*codepoints):
    return "".join(chr(c) for c in codepoints)


KA, KHA, GA, NGA, TA, DA, DHA, NA, PA, BHA, MA, YA, LA, SA, A = (0x1000, 0x1001, 0x1002, 0x1004, 0x1010, 0x1012, 0x1013,
                                                                0x1014, 0x1015, 0x1018, 0x1019, 0x101A, 0x101C, 0x101E,
                                                                0x1021)
AA_TALL, AA, I, U, UU, E, AI, ANUSVARA, DOT_BELOW, VISARGA, VIRAMA, ASAT = (0x102B, 0x102C, 0x102D, 0x102F, 0x1030,
                                                                            0x1031, 0x1032, 0x1036, 0x1037, 0x1038,
                                                                            0x1039, 0x103A)
MEDIAL_YA, MEDIAL_RA, MEDIAL_WA, MEDIAL_HA = 0x103B, 0x103C, 0x103D, 0x103E
ZWNJ, ZWJ = 0x200C, 0x200D

# (text, language): what each one checks is in its comment.
WORDS = [
    (w(KA, E), ""),  # the pre-base vowel e before its consonant
    (w(KA, E, AA), ""),  # a two-part vowel around its consonant
    (w(KA, E, E), ""),  # two left matras, flipped back (harfbuzz#3863)
    (w(KA, MEDIAL_RA), ""),  # medial ra before its consonant
    (w(MA, MEDIAL_RA, NA, ASAT, MA, AA), ""),  # medial ra, asat (Myanmar)
    (w(KA, MEDIAL_RA, E, AA, NGA, DOT_BELOW, ASAT), ""),  # medial ra, two-part vowel, dot below and asat
    (w(KA, MEDIAL_YA, MEDIAL_RA, MEDIAL_WA, MEDIAL_HA, E), ""),  # all four medials and e
    (w(MA, NGA, ASAT, VIRAMA, GA, LA, AA, PA, AA_TALL), ""),  # kinzi after its base
    (w(SA, NGA, ASAT, VIRAMA, BHA, E, AA), ""),  # kinzi with a two-part vowel
    (w(A, NGA, ASAT, VIRAMA, GA, LA, I, PA, ASAT), ""),  # kinzi, then i
    (w(NGA, ASAT, VIRAMA, KA, MEDIAL_RA, E), ""),  # kinzi with medial ra and e
    (w(KA, MA, VIRAMA, BHA, AA), ""),  # a stack: bha under ma
    (w(SA, TA, VIRAMA, TA, MA), ""),  # a stack: ta under ta
    (w(0x1017, U, DA, VIRAMA, DHA), ""),  # a stack: dha under da
    (w(KA, VIRAMA, KA, E), ""),  # a stack with e before both
    (w(E), ""),  # e alone: a dotted circle before it
    (w(AA_TALL), ""),  # tall aa alone: a dotted circle
    (w(VIRAMA, VIRAMA), ""),  # two stackers: two broken clusters, two dotted circles
    (w(E, KA), ""),  # e before a consonant, as Zawgyi writes it: a dotted circle
    (w(MEDIAL_RA, KA), ""),  # medial ra first: a dotted circle
    (w(DOT_BELOW, MEDIAL_RA) * 4, ""),  # dot below and medial ra: a dotted circle each
    (w(KA, DOT_BELOW, ASAT), ""),  # dot below before asat
    (w(KA, ASAT, DOT_BELOW), ""),  # asat before dot below
    (w(KA, MEDIAL_YA, E, VISARGA, 0x1007, UU, VISARGA), ""),
    (w(KA, MEDIAL_YA, MEDIAL_WA, NA, ASAT, TA, E, AA, ASAT), ""),
    (w(KA, MA, VIRAMA, BHA, AA, 0x0020, 0x1019, MEDIAL_RA, NA, ASAT, 0x1019, AA), ""),  # two words
    (w(0x1042, 0x1040, 0x1042, 0x1046), ""),  # digits
    (w(KA, ZWJ, MEDIAL_YA), ""),  # ZWJ before a medial
    (w(KA, ZWNJ, E), ""),  # ZWNJ before e: a broken cluster
    (w(KA, 0x105E), "mnw"),  # Mon medial na
    (w(0x105A, E, AA, ASAT), "mnw"),  # a Mon letter
    (w(TA, 0x1086, VISARGA), "shn"),  # Shan
    (w(0x1075, 0x1082, 0x1087), "shn"),  # a Shan letter, medial wa, tone
    (w(KA, 0x1062), "ksw"),  # S'gaw Karen vowel
    (w(MA, MEDIAL_RA, NA, ASAT, MA, AA), "my"),
    (w(MA, MEDIAL_RA, NA, ASAT, MA, AA), "ksw"),
    (w(KA, MEDIAL_RA, E, AA, NGA, DOT_BELOW, ASAT), "mnw"),
]


def verify_source(source_dir: Path) -> Path:
    path = source_dir / SOURCE
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: https://github.com/notofonts/notofonts.github.io/raw/{SOURCE_COMMIT}/"
                 f"fonts/NotoSansMyanmar/unhinted/ttf/{SOURCE}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != SOURCE_SHA256:
        sys.exit(f"{path}: sha256 {digest}, expected {SOURCE_SHA256}")
    return path


def header_text(data):
    constant = font_constant(FIXTURE)
    lines = [
        "#pragma once",
        "// Generated by test/ot_shaper/make_myanmar_fixtures.py from test/fixtures/fonts/. Do not edit.",
        "// HarfBuzz 14.5.0 (uharfbuzz 0.56.2): the lookups it plans and the glyph runs it shapes, left to right,",
        "// script Mymr, at scale 2133 and ppem 33.",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        '#include "../complex_shaper/ExpectedShaping.h"',
        "",
        "struct MyanmarPlanExpectation {",
        '  const char* language;  // BCP 47; "" = none',
        "  const uint16_t* gsub;  // planned GSUB lookup indices, in plan order",
        "  uint8_t gsubCount;",
        "  const uint16_t* gpos;",
        "  uint8_t gposCount;",
        "};",
        "",
        "struct MyanmarShapingExpectation {",
        "  const char* utf8;",
        '  const char* language;  // BCP 47; "" = none',
        "  const ExpectedGlyph* glyphs;",
        "  uint8_t count;",
        "};",
        "",
        "// clang-format off",
        f'constexpr const char* {constant} = "{FIXTURE}";',
        f"constexpr size_t kMyanmarGsubStages = {GSUB_STAGES};",
        "",
    ]
    plan_rows = []
    for lang in PLAN_LANGUAGES:
        gsub, gpos = planned_lookups(data, lang, text=w(KA), direction="ltr", script="Mymr")
        if len(gsub) > 255 or len(gpos) > 255:
            sys.exit(f"lang={lang}: more planned lookups than a uint8_t counts")
        name = f"kMyanmarPlan{lang or 'None'}"
        lines.append(f"constexpr uint16_t {name}Gsub[] = {{{', '.join(map(str, gsub)) or '0'}}};")
        lines.append(f"constexpr uint16_t {name}Gpos[] = {{{', '.join(map(str, gpos)) or '0'}}};")
        plan_rows.append(f'    {{"{lang}", {name}Gsub, {len(gsub)}, {name}Gpos, {len(gpos)}}},')
        print(f"  plan lang={lang or 'none'}: GSUB {len(gsub)} GPOS {len(gpos)}")
    lines += ["", "constexpr MyanmarPlanExpectation kMyanmarPlans[] = {", *plan_rows, "};", ""]
    font = hb_font(data)
    pool, rows = [], []
    for text, lang in WORDS:
        glyphs = shape(font, text, lang, direction="ltr", script="Mymr")
        rows.append((text, lang, len(pool), len(glyphs)))
        pool += glyphs
    lines.append("constexpr ExpectedGlyph kMyanmarGlyphPool[] = {")
    for i in range(0, len(pool), 8):
        lines.append("    " + ",".join(f"{{{g},{a},{dx},{dy}}}" for g, a, dx, dy in pool[i:i + 8]) + ",")
    lines += ["};", "", "constexpr MyanmarShapingExpectation kMyanmarShaping[] = {"]
    for text, lang, offset, count in rows:
        lines.append(f'    {{"{c_hex_string(text)}", "{lang}", kMyanmarGlyphPool + {offset}, {count}}},')
    lines += ["};", "", "// clang-format on", ""]
    return "\n".join(lines), len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", type=Path, help=f"holds the pinned {SOURCE}")
    parser.add_argument("out", type=Path, help="output directory of the font")
    parser.add_argument("--header-out", type=Path, default=Path(__file__).resolve().parent / "MyanmarExpected.h")
    args = parser.parse_args()

    source = verify_source(args.sources)
    cps = {ord(c) for text, _ in WORDS for c in text} | EXTRA_CODEPOINTS
    data = cut(source, cps)
    mapped = set(TTFont(io.BytesIO(data)).getBestCmap())
    missing = sorted(c for c in cps if c not in mapped and c not in (ZWNJ, ZWJ))
    if missing:
        sys.exit(f"{SOURCE}: unmapped code points in the words: {[hex(c) for c in missing]}")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / FIXTURE).write_bytes(data)
    print(f"{FIXTURE}: {len(data)} bytes")
    header, runs = header_text(data)
    with open(args.header_out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"{args.header_out}: {runs} recorded words, {len(PLAN_LANGUAGES)} plans")


if __name__ == "__main__":
    main()
