#!/usr/bin/env python3
"""Regenerate the JPEG decoder test fixtures.

Run from anywhere:  python3 generate_fixtures.py
Requires Pillow.  The committed .jpg / .pgm outputs are what the tests read; this
script exists so the fixtures are reproducible and their provenance is documented.

Fixtures:
  odd_420.jpg        Baseline 4:2:0 colour JPEG with ODD width & height (35x53).
                     Guards the floor-vs-ceil descale-width bug: at 1/2 DCT scale
                     TJpgDec emits floor(35/2)=17 columns, and the BMP converter's
                     MCU-row completion check must agree or it writes zero rows.
  contrast_420.jpg   Baseline 4:2:0 colour JPEG, sharp black shapes on white (96x64).
                     The hard edges make the IDCT overshoot past [0,255]; guards the
                     grayscale BYTECLIP fix (without it the overshoot wraps mod 256).
  contrast_420.gray.pgm  Reference grayscale decode of contrast_420.jpg (Pillow),
                     used as the golden image for the clamp test.
    progressive_420.jpg Progressive 4:2:0 JPEG with broad tonal regions. Its
                                         first DC scan is sufficient for a recognizable preview.
  progressive_tall.jpg   Progressive, 16x2712 (339 DC rows) with a vertical gradient.
                     Guards the RowScaler::mapCoordinate 32-bit overflow: scaled to a
                     540-row thumbnail, `outputIndex * (sourceSize-1) << 16` wrapped from
                     output row 194 on, and every row after it was emitted from the last
                     decoded row. 339 DC rows is what a real 2708px-tall cover produces.
  progressive_wide.jpg   Progressive, 1920x16 (240 DC columns) with a horizontal gradient.
                     Same overflow on the COLUMN axis: at 382 output columns it wrapped
                     from column 275 on.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))

def save_baseline_420(im, path, quality=85):
    # subsampling=2 -> 4:2:0; progressive omitted -> baseline (SOF0).
    im.convert("RGB").save(path, "JPEG", quality=quality, subsampling=2,
                           progressive=False, optimize=False)

# --- odd_420.jpg : odd dimensions, some low-frequency content ---
W, H = 35, 53
im = Image.new("RGB", (W, H), (255, 255, 255))
d = ImageDraw.Draw(im)
for y in range(H):           # vertical gradient
    g = int(255 * y / (H - 1))
    d.line([(0, y), (W, y)], fill=(g, g // 2, 255 - g))
d.rectangle([8, 12, 26, 40], fill=(0, 0, 0))
save_baseline_420(im, os.path.join(HERE, "odd_420.jpg"))

# --- contrast_420.jpg : sharp high-contrast edges (Gibbs overshoot) ---
W, H = 96, 64
im = Image.new("RGB", (W, H), (255, 255, 255))
d = ImageDraw.Draw(im)
d.rectangle([20, 16, 44, 48], fill=(0, 0, 0))     # solid black block
for i in range(6):                                 # thin black bars (max AC energy)
    x = 56 + i * 6
    d.rectangle([x, 8, x + 2, 56], fill=(0, 0, 0))
save_baseline_420(im, os.path.join(HERE, "contrast_420.jpg"))

# golden grayscale decode (reference decoder)
ref = Image.open(os.path.join(HERE, "contrast_420.jpg")).convert("L")
ref.save(os.path.join(HERE, "contrast_420.gray.pgm"))

# --- progressive_420.jpg : low-frequency regions survive the DC-only preview ---
W, H = 96, 64
im = Image.new("RGB", (W, H), (255, 255, 255))
d = ImageDraw.Draw(im)
for x in range(W // 2):
    g = int(255 * x / (W // 2 - 1))
    d.line([(x, 0), (x, H - 1)], fill=(g, g, g))
d.rectangle([W // 2, 0, W - 1, H // 2 - 1], fill=(0, 0, 0))
d.rectangle([W // 2, H // 2, W - 1, H - 1], fill=(255, 255, 255))
im.save(os.path.join(HERE, "progressive_420.jpg"), "JPEG", quality=92,
        subsampling=2, progressive=True, optimize=False)

# --- progressive_tall.jpg / progressive_wide.jpg : mapCoordinate overflow guards ---
# Narrow/short on the other axis so the DC block count that matters is large while the
# committed file stays a few KB. Smooth gradients keep every DC block distinct, which is
# what makes "this row repeats the previous one" detectable.
def save_progressive(im, name):
    im.convert("RGB").save(os.path.join(HERE, name), "JPEG", quality=92,
                           subsampling=2, progressive=True, optimize=False)

W, H = 16, 2712                                   # 339 DC rows, as a 2708px cover produces
im = Image.new("L", (W, H))
d = ImageDraw.Draw(im)
for y in range(H):
    d.line([(0, y), (W, y)], fill=int(255 * y / (H - 1)))
save_progressive(im, "progressive_tall.jpg")

W, H = 1920, 16                                   # 240 DC columns
im = Image.new("L", (W, H))
d = ImageDraw.Draw(im)
for x in range(W):
    d.line([(x, 0), (x, H - 1)], fill=int(255 * x / (W - 1)))
save_progressive(im, "progressive_wide.jpg")

print("Fixtures written to", HERE)
for f in sorted(os.listdir(HERE)):
    if f.endswith((".jpg", ".pgm")):
        print(" ", f, os.path.getsize(os.path.join(HERE, f)), "bytes")
