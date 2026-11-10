#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/font-sizes.sh"
cd "$SCRIPT_DIR"

# The built-in Serif: Source Serif 4 by Adobe (SIL OFL 1.1), its Caption optical size, from Adobe's static release
# 4.005 (github.com/adobe-fonts/source-serif/releases), the four files unmodified in source/SourceSerif4Caption beside
# the licence. sha256 Regular fc964ede48743f21da94fb3a80253d30dd4e4752fb416a09fd64cb17fae89cd2, It
# 8c705747dbfe5736cf947aff520b822dc4c3ca301f35bfea473c37d17e93e1f7, Bold
# 92b317ac248565b5be9c3086a7e7743f44c9db35d49d5adeca67f3391c3a4cf2, BoldIt
# 7af31fb47abe9b4318238fc8c24130953baf70fd36e53eae867bb642f268d022.
#
# The faces are serif_* and the reader shows the family as "Serif": the licence reserves the font name "Source", and
# a face converted and scaled here is a Modified Version of the font, which may not carry a reserved name. CREDITS.md
# says where it comes from.
#
# Scaled to 95 %. At the same nominal size the Caption cut holds about 82 pages of a chapter where Bookerly, the
# built-in serif before it, held 74 (measured on an X3). What a page holds goes with the square of the type size, so
# 0.95 ~= sqrt(74 / 82) gives a chapter Bookerly's page count back at every size of the reader's ladder. The scale
# is the font's own: unitsPerEm is set to round(1000 / 0.95) = 1053 in a temporary copy (the committed files stay
# Adobe's), and FreeType scales the outlines, the advances and the line metrics by it, as fontconvert.py scales the
# kerning by the same head table.
SERIF_UNITS_PER_EM=1053
declare -A SERIF_SOURCE_STYLE=([Regular]=Regular [Italic]=It [Bold]=Bold [BoldItalic]=BoldIt)
# Behind it in fontconvert.py's font stack, Noto Sans of the same style draws what Bookerly drew within
# fontconvert.py's intervals and Caption has no glyph for, and U+FFFD, the glyph the renderer draws for a character no
# face has. It is subset to exactly those characters, as the UI faces' Hebrew is below: whole, Noto Sans would fill
# every other gap of the intervals too, 177 more characters (combining marks, historic Cyrillic, rare punctuation,
# super- and subscripts, currency signs) that no built-in serif has drawn. Noto Sans has no glyph for 55 of the
# characters it is cut to: the arrows and the mathematical operators (U+2194-22EF below). The subset leaves them out,
# and they draw through lib/EpdFont/GlyphFallback.h, U+FFFD failing that, as they do in the built-in Noto Sans. The C0
# controls Bookerly mapped (U+0000, U+0008, tab, CR, U+001D) are not carried.
SERIF_FALLBACK_INTERVALS=(
  "0x012C,0x012D"  # I with breve
  "0x0132,0x0133"  # ligature IJ
  "0x0166,0x0167"  # T with stroke
  "0x01C4,0x01CC"  # the digraphs DZ with caron, LJ and NJ
  "0x01DD,0x01E5"  # turned e; A with diaeresis and macron to G with stroke
  "0x01E8,0x01F5"  # K with caron to G with acute: O with ogonek, ezh with caron, J with caron, DZ
  "0x01FA,0x0217"  # A with ring and acute to U with inverted breve: the double grave and inverted breve letters
  "0x021C,0x021F"  # yogh, H with caron
  "0x030F,0x030F"  # combining double grave
  "0x0311,0x0315"  # combining inverted breve, turned comma, comma, reversed comma, comma above right
  "0x0325,0x0325"  # combining ring below
  "0x032D,0x032D"  # combining circumflex below
  "0x0330,0x0330"  # combining tilde below
  "0x0342,0x0342"  # combining Greek perispomeni
  "0x0344,0x0345"  # combining Greek dialytika tonos, ypogegrammeni
  "0x048A,0x048F"  # Cyrillic: short I with tail, semisoft sign, er with tick
  "0x0494,0x0495"  # Cyrillic: ghe with middle hook
  "0x049C,0x049F"  # Cyrillic: ka with vertical stroke, ka with stroke
  "0x04A4,0x04A9"  # Cyrillic: ligature en ghe, pe with middle hook, Abkhasian ha
  "0x04AC,0x04AD"  # Cyrillic: te with descender
  "0x04B4,0x04B5"  # Cyrillic: ligature te tse
  "0x04B8,0x04B9"  # Cyrillic: che with vertical stroke
  "0x04BC,0x04BF"  # Cyrillic: Abkhasian che, with descender
  "0x04C3,0x04CE"  # Cyrillic: ka with hook to em with tail, palochka
  "0x04D2,0x04D3"  # Cyrillic: a with diaeresis
  "0x04DA,0x04E1"  # Cyrillic: schwa with diaeresis to Abkhasian dze
  "0x04E4,0x04E5"  # Cyrillic: i with diaeresis
  "0x04EA,0x04ED"  # Cyrillic: barred o with diaeresis, e with diaeresis
  "0x04F0,0x04F1"  # Cyrillic: u with diaeresis
  "0x04F4,0x04FF"  # Cyrillic: che with diaeresis to ha with stroke
  "0x2000,0x2001"  # en quad, em quad
  "0x2008,0x2008"  # punctuation space
  "0x200C,0x200F"  # ZWNJ, ZWJ, LRM, RLM (fontconvert.py writes them empty whatever the font has)
  "0x2016,0x2017"  # double vertical line, double low line
  "0x201B,0x201B"  # single high-reversed-9 quotation mark
  "0x201F,0x201F"  # double high-reversed-9 quotation mark
  "0x2023,0x2024"  # triangular bullet, one dot leader
  "0x2027,0x202E"  # hyphenation point, line and paragraph separators, the bidi embeddings and overrides
  "0x2034,0x2038"  # triple prime, the reversed primes, caret
  "0x203B,0x203B"  # reference mark
  "0x203E,0x203E"  # overline
  "0x2043,0x2043"  # hyphen bullet
  "0x2053,0x2053"  # swung dash
  "0x2057,0x2057"  # quadruple prime
  "0x205F,0x2064"  # medium mathematical space, word joiner, the invisible operators
  "0x20A3,0x20A3"  # French franc sign
  "0x2194,0x2195"  # left right arrow, up down arrow
  "0x21A8,0x21A8"  # up down arrow with base
  "0x21B2,0x21B3"  # downwards arrows with tip left and right
  "0x21B5,0x21B5"  # downwards arrow with corner leftwards
  "0x21D0,0x21D4"  # the double arrows
  "0x2200,0x2200"  # for all
  "0x2203,0x2203"  # there exists
  "0x2205,0x2205"  # empty set
  "0x2207,0x220D"  # nabla, element of and its variants
  "0x2216,0x2218"  # set minus, asterisk operator, ring operator
  "0x221D,0x221D"  # proportional to
  "0x221F,0x2220"  # right angle, angle
  "0x2223,0x2223"  # divides
  "0x2225,0x2225"  # parallel to
  "0x2227,0x222A"  # logical and, or; intersection, union
  "0x2234,0x2237"  # therefore, because, ratio, proportion
  "0x223C,0x223C"  # tilde operator
  "0x2245,0x2245"  # approximately equal to
  "0x224D,0x224D"  # equivalent to
  "0x2261,0x2261"  # identical to
  "0x226A,0x226B"  # much less-than, much greater-than
  "0x2283,0x2287"  # superset of and its variants
  "0x2295,0x2298"  # circled plus, minus, times, division slash
  "0x22A5,0x22A5"  # up tack
  "0x22C5,0x22C5"  # dot operator
  "0x22EF,0x22EF"  # midline horizontal ellipsis
  "0xFB05,0xFB06"  # ligatures long s t, s t
  "0xFFFD,0xFFFD"  # replacement character
)
serif_fallback_unicodes=""
for interval in "${SERIF_FALLBACK_INTERVALS[@]}"; do
  first="${interval%,*}"
  last="${interval#*,}"
  serif_fallback_unicodes+="${serif_fallback_unicodes:+,}U+${first#0x}-${last#0x}"
done
SERIF_SOURCE_DIR="$(mktemp -d)"
for style in ${READER_FONT_STYLES[@]}; do
  python -c 'import sys
from fontTools.ttLib import TTFont
font = TTFont(sys.argv[1])
font["head"].unitsPerEm = int(sys.argv[3])
font.save(sys.argv[2])' "../builtinFonts/source/SourceSerif4Caption/SourceSerif4Caption-${SERIF_SOURCE_STYLE[$style]}.ttf" \
    "$SERIF_SOURCE_DIR/Serif-${style}.ttf" "$SERIF_UNITS_PER_EM"
  python -m fontTools.subset "../builtinFonts/source/NotoSans/NotoSans-${style}.ttf" \
    --unicodes="$serif_fallback_unicodes" --output-file="$SERIF_SOURCE_DIR/NotoSans-${style}.ttf"
done

for size in ${SERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="serif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size "$SERIF_SOURCE_DIR/Serif-${style}.ttf" \
      "$SERIF_SOURCE_DIR/NotoSans-${style}.ttf" --2bit --compress --zopfli > $output_path
    echo "Generated $output_path"
  done
done
rm -rf "$SERIF_SOURCE_DIR"

# The phonetic alphabet a dictionary needs: IPA Extensions, the stress and
# length marks from Spacing Modifier Letters, and the three Greek letters IPA
# borrows.
#
# Only ONE face gets them, notosans_14, because the dictionary viewer pins
# itself to that font (see DICTIONARY_FONT_ID in
# src/activities/reader/DictionaryDefinitionActivity.h) and a definition is the
# only text that needs the range. Giving it to all twenty Noto faces cost 124 KB
# of flash; one size costs 26 KB. Noto Sans is also the only built-in reader
# family whose source actually HAS the glyphs -- Source Serif 4 Caption, the
# Serif's, lacks 102 of the 119 codepoints below (Bookerly, the serif before it,
# lacked 9 of the 20 a real dictionary uses), which is why every other font
# relies on the lookalike substitution in lib/EpdFont/GlyphFallback.h instead.
# The whole IPA Extensions block, but only the handful of Spacing Modifier
# Letters a transcription actually uses: taking that block whole cost 31 KB more
# across the family for ~60 codepoints no dictionary writes.
NOTOSANS_IPA_SIZE=14
NOTOSANS_IPA_INTERVALS=(
  "0x0250,0x02AF"  # IPA Extensions
  "0x02B0,0x02B7"  # superscript modifiers, aspiration and friends
  "0x02BC,0x02BC"  # modifier apostrophe
  "0x02C8,0x02D1"  # stress marks and length marks
  "0x02E1,0x02E1"  # modifier l
  "0x03B2,0x03B2"  # beta
  "0x03B8,0x03B8"  # theta
  "0x03C7,0x03C7"  # chi
)

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    cmd=(python fontconvert.py $font_name $size $font_path --2bit --compress --zopfli)
    if [ "$size" = "$NOTOSANS_IPA_SIZE" ]; then
      for interval in "${NOTOSANS_IPA_INTERVALS[@]}"; do
        cmd+=(--additional-intervals "$interval")
      done
    fi
    "${cmd[@]}" > $output_path
    echo "Generated $output_path"
  done
done

UI_LANG_INTERVALS=(
  "0x0000,0x007F"
  "0x0080,0x00FF"
  "0x0100,0x017F"
  "0x01A0,0x01A1"
  "0x01AF,0x01B0"
  "0x01C4,0x021F"
  "0x0300,0x036F"
  "0x0400,0x04FF"
  "0x1EA0,0x1EF9"
  "0x2010,0x206F"
  "0x20A0,0x20CF"
  "0xFB00,0xFB06"
  "0xFFFD,0xFFFD"
)

# Greek and Hebrew in every UI face, so a title, an author or a typed search in either draws whatever book was read
# last, and the keyboard's Greek and Hebrew layouts can label their keys. Greek comes from each face's own font (Inter,
# Noto Sans): modern Greek, its letters with tonos and dialytika and its punctuation, and no more. The whole Greek and
# Coptic block cost 8.7 KB more, Greek Extended (polytonic) 81 KB more. Hebrew comes from
# Noto Sans Hebrew, placed behind the face's font in fontconvert.py's font stack and cut to the Hebrew block first:
# whole, it also maps punctuation, U+FFFD and more the UI fonts lack, which would slip into the faces. Its letters sit
# on Noto Sans' x-height and cap height, which Inter's are within 2% of, so it is drawn at the face's own size. The
# points come with the letters; the cantillation marks (U+0591-05AF) do not: 2.4 KB for text no title carries.
UI_SCRIPT_INTERVALS=(
  "0x037E,0x037E"  # Greek question mark
  "0x0384,0x038A"  # tonos, dialytika tonos, the capitals with tonos, ano teleia
  "0x038C,0x038C"  # capital omicron with tonos
  "0x038E,0x03A1"  # capitals upsilon and omega with tonos, iota with both marks, capitals alpha to rho
  "0x03A3,0x03CE"  # capitals sigma to omega, the letters with tonos and dialytika, small alpha to omega, final sigma
  "0x05B0,0x05C7"  # Hebrew points (niqqud), maqaf, paseq, sof pasuq
  "0x05D0,0x05EA"  # Hebrew letters, final forms included
  "0x05F3,0x05F4"  # geresh, gershayim
)
# Arabic in every UI face too, from Noto Sans Arabic UI behind Hebrew (source/NotoSansArabicUI, notofonts.github.io at
# 5c82cef0a8f08806f932062e18c02be62be278f0, fonts/NotoSansArabicUI/unhinted/ttf; sha256 Regular
# f4fe1866a64cba1e8b8d08a67cd29070f919edf778ca5de983c1f2a690fc953b, Bold
# efb903b545a00a557c9d2dd739725157c04bf44215ba31b8ca3a805f099fdc33). The UI draws Arabic in its contextual presentation
# forms (BidiUtils::arabicUiForms), so the faces carry what that mapper writes rather than the letters: every form
# shape_logical() can give (Presentation Forms-B U+FE80-FEFC and the Forms-A forms of lib/MiniBidi/minibidi.c's
# xshapeforms table), the harakat and superscript alef, Arabic punctuation and tatweel, and the Arabic-Indic and
# Persian digits; the host test ui_face_scripts checks that every form the mapper writes is there. The UI cut of Noto
# Sans Arabic, because its ink stays on the faces' line: at 12 pt no glyph reaches more than 1 px below the descender,
# where Noto Sans Arabic draws meem and yeh 4 px below it and Pashto's e 7 px, and it costs no more. The subset holds
# these characters alone and none of the layout tables: the UI kerns a pair in the order it draws it, left to right,
# and an Arabic pair is kerned in reading order, so its 120 pairs would land on the wrong letters. Measured on
# fd48c5b79: default env +84,448 B, Murphy +104,080 B. inter_ui_14_bold, the largest face, then holds 64,937 B of
# bitmap, 598 B short of the 64 KB an uncompressed face's uint16 offsets reach: the next script it takes needs that
# face compressed or its offsets widened.
UI_ARABIC_INTERVALS=(
  "0x060C,0x060C"  # Arabic comma
  "0x061B,0x061B"  # Arabic semicolon
  "0x061F,0x061F"  # Arabic question mark
  "0x0640,0x0640"  # tatweel
  "0x064B,0x0652"  # harakat, fathatan to sukun
  "0x0660,0x066C"  # Arabic-Indic digits, percent sign, decimal and thousands separators
  "0x0670,0x0670"  # superscript alef
  "0x06D4,0x06D4"  # Urdu full stop
  "0x06F0,0x06F9"  # Persian and Urdu digits
  "0xFB50,0xFBB1"  # Forms-A: alef wasla to yeh barree with hamza above
  "0xFBD3,0xFBDC"  # Forms-A: ng, u, oe, yu
  "0xFBDE,0xFBE7"  # Forms-A: ve, Kirghiz oe and yu, Pashto e
  "0xFBFC,0xFBFF"  # Forms-A: Farsi yeh
  "0xFE80,0xFEFC"  # Forms-B: every form of U+0621-064A, and the Lam-Alef ligatures
)
UI_SCRIPT_INTERVALS+=("${UI_ARABIC_INTERVALS[@]}")
arabic_unicodes=""
for interval in "${UI_ARABIC_INTERVALS[@]}"; do
  first="${interval%,*}"
  last="${interval#*,}"
  arabic_unicodes+="${arabic_unicodes:+,}U+${first#0x}-${last#0x}"
done
UI_SCRIPT_DIR="$(mktemp -d)"
for style in ${UI_FONT_STYLES[@]}; do
  python -m fontTools.subset "../../../assets/sd-fonts/NotoSansHebrew/NotoSansHebrew-${style}.ttf" \
    --unicodes="U+0591-05F4" --layout-features='*' --output-file="$UI_SCRIPT_DIR/Hebrew-${style}.ttf"
  python -m fontTools.subset "../builtinFonts/source/NotoSansArabicUI/NotoSansArabicUI-${style}.ttf" \
    --unicodes="$arabic_unicodes" --drop-tables+=GSUB,GPOS,GDEF --output-file="$UI_SCRIPT_DIR/Arabic-${style}.ttf"
done

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="inter_ui_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    inter_path="../builtinFonts/source/Inter/Inter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"

    cmd=(python fontconvert.py "$font_name" "$size" "$inter_path" "$UI_SCRIPT_DIR/Hebrew-${style}.ttf"
      "$UI_SCRIPT_DIR/Arabic-${style}.ttf")
    for interval in "${UI_LANG_INTERVALS[@]}" "${UI_SCRIPT_INTERVALS[@]}"; do
      cmd+=(--additional-intervals "$interval")
    done
    "${cmd[@]}" > "$output_path"

    echo "Generated $output_path"
  done
done

ui_script_args=()
for interval in "${UI_SCRIPT_INTERVALS[@]}"; do
  ui_script_args+=(--additional-intervals "$interval")
done
python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  "$UI_SCRIPT_DIR/Hebrew-Regular.ttf" "$UI_SCRIPT_DIR/Arabic-Regular.ttf" "${ui_script_args[@]}" \
  > ../builtinFonts/notosans_8_regular.h
# Murphy uses a dedicated 6 pt face for the Small UI role at its Normal size setting.
python fontconvert.py notosans_6_regular 6 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  "$UI_SCRIPT_DIR/Hebrew-Regular.ttf" "$UI_SCRIPT_DIR/Arabic-Regular.ttf" "${ui_script_args[@]}" \
  > ../builtinFonts/notosans_6_regular.h
rm -rf "$UI_SCRIPT_DIR"

# The CJK UI faces hold each CJK translation's characters that the UI faces above lack, so they
# come after them. The script dedupes and verifies by itself; the pass below repeats that over the
# whole set, which is harmless.
python build_cjk_ui_fonts.py

echo ""
echo "Deduplicating tables shared between faces..."
# fontconvert.py writes each face independently, so every size of a family repeats the tables
# that describe the TYPEFACE rather than the size -- codepoint coverage, kern class assignment,
# the ligature set. Only the kern values scale with size. This pass runs over the finished set
# and keeps one copy of each; it never computes a table, so it cannot change any bytes.
# Idempotent, so re-running the conversion is safe.
# Snapshot first so the dedup can be PROVEN byte-equivalent rather than assumed. The verifier
# is a separate program that re-reads both trees from disk: one that shared the transformer's
# parsing would agree with it about a misparse.
PREDEDUPE="$(mktemp -d)"
cp ../builtinFonts/*.h "$PREDEDUPE/"
python dedupe_font_tables.py ../builtinFonts/
python verify_font_dedup.py ../builtinFonts/ "$PREDEDUPE"
rm -rf "$PREDEDUPE"

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
