#!/usr/bin/env python3
"""Builds the Noto Sans Devanagari shaping fixtures (see test/fixtures/fonts/README.md).

The words, the conversion and `round_px` are crosspoint-reader PR #3787's `generate_fixtures.py`
(SSafayet / @SSafayet). This script is this repository's own.

    python lib/EpdFont/scripts/make_shaping_fixtures.py --source-dir <dir with the two Noto Sans Devanagari TTFs> \
        --fonts-out test/fixtures/fonts --header-out test/ttf_font/ExpectedGlyphRuns.h

It writes NotoSansDevanagari-Shaping-Regular.ttf and -Bold.ttf, subsets of the unhinted Noto Sans Devanagari (the
code points of the words below, plus U+0020, U+200C, U+200D and U+25CC; GDEF, GSUB and GPOS kept whole), and, into the
header, HarfBuzz's own glyph runs for each word over each subset at 1867 (14 pt at 150 DPI) and 2560 (a 40 px native
size) in 26.6, so a test can check that lib/OtShaper shapes the subsets exactly as HarfBuzz does.

The script does not download: --source-dir holds NotoSansDevanagari-Regular.ttf and -Bold.ttf, each checked against
the sha256 pinned below. The output is byte-identical on every run, and the script writes nothing else.
"""

import argparse
import hashlib
import io
import sys
from pathlib import Path

import uharfbuzz as hb
from fontTools import subset

# notofonts/notofonts.github.io at this commit.
SOURCE_COMMIT = "5c82cef0a8f08806f932062e18c02be62be278f0"
SOURCES = {
    "Regular": (
        "NotoSansDevanagari-Regular.ttf",
        "9c7d935139ea6a1e6ad9dbac4f6d27ece1e04bca8123c8888d00a0f9df4724cd",
    ),
    "Bold": (
        "NotoSansDevanagari-Bold.ttf",
        "ff2f76a23aad41e0608c2d7dbc4bacd247ff3bec78f0ec2a8fb106b561636e58",
    ),
}
URL = (
    "https://github.com/notofonts/notofonts.github.io/raw/" + SOURCE_COMMIT
    + "/fonts/NotoSansDevanagari/unhinted/ttf/{name}"
)

# 14 pt at 150 DPI in 26.6, as the converter and TtfEpdFont compute it, and a 40 px native size.
PPEMS_26_6 = [(14 * 150 * 64 + 36) // 72, 40 * 64]

MAX_FONT_BYTES = 65536

# A word is text, or (text, BCP 47 language) for a word shaped in a specific language.
WORDS = [
    "हिन्दी",  # i-matra before a n-da conjunct
    "पत्रिका",  # t-ra with the i-matra before the conjunct
    "श्री",  # sh-ra with ii-matra
    "प्रार्थना",  # ra below, reph
    "कृष्ण",  # vocalic r, ss-nna
    "ज्ञान",  # j-nya
    "द्ध", "त्र्य",  # stacked and chained conjuncts
    "क़िला",  # nukta consonant under an i-matra
    "र्‍य",  # eyelash ra (ZWJ)
    "संस्कृतम्",  # anusvara, final virama
    "लक्ष", ("लक्ष", "mr"),  # Marathi la and k-ssa
    "झाड", ("झाड", "ne"),  # Nepali jha
    ("९८५", "ne"),  # Nepali digits
]

# Kept in the subset beside the words' code points: space, ZWNJ, ZWJ and the dotted circle.
EXTRA_CODEPOINTS = [0x0020, 0x200C, 0x200D, 0x25CC]


def verify_source(source_dir: Path, style: str) -> Path:
    name, expected = SOURCES[style]
    path = source_dir / name
    if not path.is_file():
        sys.exit(f"missing {path}\n  download: {URL.format(name=name)}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected:
        sys.exit(f"{path}: sha256 {digest}, expected {expected}")
    return path


def codepoints() -> list:
    found = set(EXTRA_CODEPOINTS)
    for word in WORDS:
        found.update(ord(c) for c in (word[0] if isinstance(word, tuple) else word))
    return sorted(found)


def subset_font(path: Path) -> bytes:
    options = subset.Options()
    options.layout_features = ["*"]
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.recalc_timestamp = False
    font = subset.load_font(str(path), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=codepoints())
    subsetter.subset(font)
    for table in ("GDEF", "GSUB", "GPOS"):
        if table not in font:
            sys.exit(f"{path}: the subset lost {table}")
    out = io.BytesIO()
    font.recalcTimestamp = False
    font.save(out)
    return out.getvalue()


def round_px(v):
    return (v + 32) // 64 if v >= 0 else -((-v + 32) // 64)


def c_string(text):
    return "".join(c if ord(c) < 0x80 else "".join(f"\\x{b:02X}" for b in c.encode()) for c in text)


def shape_runs(data: bytes, ppem: int) -> list:
    """(text, language, [(gid, advance12_4, dx, dy)]) for every word."""
    font = hb.Font(hb.Face(data))
    font.scale = (ppem, ppem)
    font.ppem = ((ppem + 32) >> 6,) * 2
    runs = []
    for word in WORDS:
        text, language = word if isinstance(word, tuple) else (word, "")
        buf = hb.Buffer()
        buf.add_str(text)
        buf.direction = "ltr"
        buf.script = "Deva"
        if language:
            buf.language = language
        hb.shape(font, buf, {})
        glyphs = [
            (info.codepoint, (pos.x_advance + 2) >> 2, round_px(pos.x_offset), -round_px(pos.y_offset))
            for info, pos in zip(buf.glyph_infos, buf.glyph_positions)
        ]
        runs.append((text, language, glyphs))
    return runs


def header_text(fonts: dict) -> str:
    lines = [
        "#pragma once",
        "// Generated by lib/EpdFont/scripts/make_shaping_fixtures.py from test/fixtures/fonts/. Do not edit.",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "struct RunGlyph {",
        "  uint16_t gid;",
        "  int16_t advance12_4;",
        "  int8_t dx;",
        "  int8_t dy;",
        "};",
        "",
        "struct ExpectedRun {",
        "  const char* utf8;",
        '  const char* language;  // BCP 47; "" = none',
        "  const RunGlyph* glyphs;",
        "  uint8_t count;",
        "};",
        "",
        "struct RunSet {",
        "  const char* file;  // in test/fixtures/fonts",
        "  uint32_t ppem26_6;",
        "  const ExpectedRun* runs;",
        "  size_t count;",
        "};",
        "",        "// clang-format off",
    ]
    sets = []
    for style, (file, data) in fonts.items():
        for ppem in PPEMS_26_6:
            prefix = f"k{style}{ppem}"
            table = []
            for i, (text, language, glyphs) in enumerate(shape_runs(data, ppem)):
                body = ", ".join(f"{{{g}, {a}, {dx}, {dy}}}" for g, a, dx, dy in glyphs)
                lines.append(f"constexpr RunGlyph {prefix}Glyphs{i}[] = {{{body}}};")
                comment = f"{text} ({language})" if language else text
                table.append(f'    {{"{c_string(text)}", "{language}", {prefix}Glyphs{i}, {len(glyphs)}}},  // {comment}')
            lines += ["", f"constexpr ExpectedRun {prefix}Runs[] = {{", *table, "};", ""]
            sets.append(f'    {{"{file}", {ppem}, {prefix}Runs, sizeof({prefix}Runs) / sizeof({prefix}Runs[0])}},')
    lines += [f"constexpr RunSet kDevanagariRunSets[{len(sets)}] = {{", *sets, "};", "", "// clang-format on", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", required=True, type=Path, help="holds the two pinned Noto Sans Devanagari TTFs")
    parser.add_argument("--fonts-out", required=True, type=Path, help="output directory for the fonts")
    parser.add_argument("--header-out", required=True, type=Path, help="output path of ExpectedGlyphRuns.h")
    args = parser.parse_args()

    fonts = {}
    for style in SOURCES:
        data = subset_font(verify_source(args.source_dir, style))
        if len(data) >= MAX_FONT_BYTES:
            sys.exit(f"the {style} subset is {len(data)} bytes, not under {MAX_FONT_BYTES}")
        fonts[style] = (f"NotoSansDevanagari-Shaping-{style}.ttf", data)

    from fontTools.ttLib import TTFont

    print("copyright (name ID 0):", TTFont(io.BytesIO(fonts["Regular"][1]))["name"].getDebugName(0))

    header = header_text(fonts)
    args.fonts_out.mkdir(parents=True, exist_ok=True)
    args.header_out.parent.mkdir(parents=True, exist_ok=True)
    for file, data in fonts.values():
        (args.fonts_out / file).write_bytes(data)
        print(f"{file}: {len(data)} bytes")
    runs = len(WORDS) * len(fonts) * len(PPEMS_26_6)
    with open(args.header_out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"{args.header_out}: {runs} runs")


if __name__ == "__main__":
    main()
