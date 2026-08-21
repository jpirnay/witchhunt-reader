#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(10 12 14 16 18)
NOTOSANS_FONT_SIZES=(10 12 14 16 18)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --zopfli > $output_path
    echo "Generated $output_path"
  done
done

# Noto Sans carries the phonetic alphabet a dictionary needs: IPA Extensions,
# the stress and length marks from Spacing Modifier Letters, and the three Greek
# letters IPA borrows. Only this family gets them -- it is the one built-in whose
# source actually has the glyphs (Bookerly is missing 9 of the 20 codepoints a
# real dictionary uses), and the range costs ~3.9 KB per font.
# The whole IPA Extensions block, but only the handful of Spacing Modifier
# Letters a transcription actually uses: taking that block whole cost 31 KB more
# across the family for ~60 codepoints no dictionary writes.
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
    for interval in "${NOTOSANS_IPA_INTERVALS[@]}"; do
      cmd+=(--additional-intervals "$interval")
    done
    "${cmd[@]}" > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")
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

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="inter_ui_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    inter_path="../builtinFonts/source/Inter/Inter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"

    cmd=(python fontconvert.py "$font_name" "$size" "$inter_path")
    for interval in "${UI_LANG_INTERVALS[@]}"; do
      cmd+=(--additional-intervals "$interval")
    done
    "${cmd[@]}" > "$output_path"

    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
