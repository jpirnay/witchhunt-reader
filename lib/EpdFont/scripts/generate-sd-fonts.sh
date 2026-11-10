#!/bin/bash
# Generate recommended SD card font packs for CrossPoint.
#
# Prerequisites:
#   pip install freetype-py fonttools
#
# Source fonts are in ../builtinFonts/source/:
#   SourceSerif4Caption/, NotoSans/, OpenDyslexic/, Ubuntu/ — committed to git
#
# Output goes to ./output/ (copy to SD card at /.crosspoint/fonts/)

set -e
cd "$(dirname "$0")"

SCRIPT="./fontconvert_sdcard.py"
FONT_DIR="../builtinFonts/source"
OUTPUT_BASE="./output"

SIZES="12,14,16,18"

# Clean output directories to ensure a fresh build
echo "Cleaning output directories..."
rm -rf "$OUTPUT_BASE/NotoSansExtended/" "$OUTPUT_BASE/Serif-SD/"

# Run both font families in parallel
echo "=== Starting parallel font generation ==="

echo "[1/2] NotoSansExtended (Latin-ext + Greek + Cyrillic + Georgian + Armenian + Ethiopic)"
python3 "$SCRIPT" \
  "$FONT_DIR/NotoSans/NotoSans-Regular.ttf" \
  --intervals latin-ext,greek,cyrillic,georgian,armenian,ethiopic \
  --sizes "$SIZES" --style regular \
  --name NotoSansExtended \
  --output-dir "$OUTPUT_BASE/NotoSansExtended/" &
PID_NOTO=$!

# The built-in Serif's source, Source Serif 4 Caption, at its own size (the built-in faces are it at 95 %). Named
# Serif-SD: its licence reserves the font name "Source", and a converted font is a Modified Version.
echo "[2/2] Serif-SD (multi-style)"
python3 "$SCRIPT" \
  --regular "$FONT_DIR/SourceSerif4Caption/SourceSerif4Caption-Regular.ttf" \
  --bold "$FONT_DIR/SourceSerif4Caption/SourceSerif4Caption-Bold.ttf" \
  --italic "$FONT_DIR/SourceSerif4Caption/SourceSerif4Caption-It.ttf" \
  --bolditalic "$FONT_DIR/SourceSerif4Caption/SourceSerif4Caption-BoldIt.ttf" \
  --intervals builtin \
  --sizes "$SIZES" \
  --name Serif-SD \
  --output-dir "$OUTPUT_BASE/Serif-SD/" &
PID_SERIF=$!

# Wait for all and track failures
FAILED=0
wait $PID_NOTO || { echo "ERROR: NotoSansExtended generation failed"; FAILED=1; }
wait $PID_SERIF || { echo "ERROR: Serif-SD generation failed"; FAILED=1; }

if [ $FAILED -ne 0 ]; then
  echo "=== Some font generations failed ==="
  exit 1
fi

echo ""
echo "=== Done ==="
echo "Copy the contents of $OUTPUT_BASE/ to your SD card at /.crosspoint/fonts/"
