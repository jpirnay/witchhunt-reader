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

print("Fixtures written to", HERE)
for f in sorted(os.listdir(HERE)):
    if f.endswith((".jpg", ".pgm")):
        print(" ", f, os.path.getsize(os.path.join(HERE, f)), "bytes")
