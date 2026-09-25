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
  thin_lines_gray.jpg    Baseline grayscale, 600x32, white with a 1-px black vertical
                     line every 7 px (x = 3, 10, 17, ...). Downscaled to 420 wide, the
                     residual 0.7 scale runs at 1/1 DCT, and nearest-neighbour sampling
                     never lands on ~30% of the lines -- they vanish. Guards the area-
                     average downscale in JpegToFramebufferConverter (epub_pipeline test).
  prog_full_420.jpg      Progressive 4:2:0 colour, 203x141 (not a multiple of the 16-px MCU),
                     libjpeg's successive-approximation scan script (DC and AC refinement
                     scans, per-scan optimised Huffman tables). prog_full_420.y.pgm is its
                     exact luma, decoded by libjpeg in YCbCr mode: the reference for the
                     full progressive decoder (ProgressiveJpeg) at every 1/2^n scale.
  prog_full_gray.jpg     The same script on a one-component (grayscale) frame, 157x99, where
                     every scan is non-interleaved. Reference: prog_full_gray.y.pgm.
  prog_full_444_rst.jpg  4:4:4, 150x90, restart marker every 3 blocks/MCUs, so every scan
                     crosses restart boundaries inside and between bands.
  thin_lines_prog.jpg    thin_lines_gray.jpg's picture as a progressive JPEG (libjpeg script).
                     Through JpegToFramebufferConverter it must keep every line: the DC-only
                     preview it replaced shows 1/8 resolution, where they all blur away.
  thin_hlines_gray.jpg   The same, transposed (32x600, horizontal lines): lines in the last
                     row of an MCU row need the carry across MCU rows, not just blocks.
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

# --- thin_lines_gray.jpg : 1-px strokes a point-sampling downscale drops ---
W, H = 600, 32
im = Image.new("L", (W, H), 255)
d = ImageDraw.Draw(im)
for x in range(3, W, 7):
    d.line([(x, 0), (x, H - 1)], fill=0)
im.save(os.path.join(HERE, "thin_lines_gray.jpg"), "JPEG", quality=95, progressive=False, optimize=False)
im.save(os.path.join(HERE, "thin_lines_prog.jpg"), "JPEG", quality=95, progressive=True, optimize=True)
im.transpose(Image.Transpose.TRANSPOSE).save(os.path.join(HERE, "thin_hlines_gray.jpg"), "JPEG", quality=95,
                                             progressive=False, optimize=False)

# --- prog_full_*.jpg : full progressive decode references ---
def busy_picture(w, h, mode="RGB"):
    """Hard edges, 1-px strokes, text, a gradient and noise: every frequency gets used."""
    import random
    rnd = random.Random(7)
    im = Image.new("RGB", (w, h), (250, 248, 240))
    d = ImageDraw.Draw(im)
    for x in range(w):
        d.line([(x, h * 2 // 3), (x, h - 1)], fill=(x * 255 // w, 90, 255 - x * 255 // w))
    for i in range(12):
        x0, y0 = rnd.randrange(w), rnd.randrange(h * 2 // 3)
        d.rectangle([x0, y0, x0 + rnd.randrange(8, 40), y0 + rnd.randrange(4, 24)],
                    outline=(0, 0, 0), fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    for x in range(3, w, 7):
        d.line([(x, 0), (x, h // 4)], fill=(0, 0, 0))
    d.text((6, h // 3), "Real alibi period 14:00", fill=(0, 0, 0))
    px = im.load()
    for _ in range(w * h // 20):
        x, y = rnd.randrange(w), rnd.randrange(h)
        px[x, y] = tuple(rnd.randrange(256) for _ in range(3))
    return im.convert(mode)

def save_reference(jpg, pgm):
    ref = Image.open(os.path.join(HERE, jpg))
    if ref.mode != "L":
        ref.draft("YCbCr", ref.size)  # libjpeg's own luma, no RGB round trip
        ref = ref.getchannel(0)
    ref.save(os.path.join(HERE, pgm))

busy_picture(203, 141).save(os.path.join(HERE, "prog_full_420.jpg"), "JPEG", quality=90, subsampling=2,
                             progressive=True, optimize=True)
save_reference("prog_full_420.jpg", "prog_full_420.y.pgm")
busy_picture(157, 99, "L").save(os.path.join(HERE, "prog_full_gray.jpg"), "JPEG", quality=90,
                                 progressive=True, optimize=True)
save_reference("prog_full_gray.jpg", "prog_full_gray.y.pgm")
busy_picture(150, 90).save(os.path.join(HERE, "prog_full_444_rst.jpg"), "JPEG", quality=85, subsampling=0,
                            progressive=True, optimize=True, restart_marker_blocks=3)
save_reference("prog_full_444_rst.jpg", "prog_full_444_rst.y.pgm")

print("Fixtures written to", HERE)
for f in sorted(os.listdir(HERE)):
    if f.endswith((".jpg", ".pgm")):
        print(" ", f, os.path.getsize(os.path.join(HERE, f)), "bytes")
