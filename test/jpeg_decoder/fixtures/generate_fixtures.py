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
  progressive_tall_3200.jpg  Progressive, 64x3200, a vertical gradient. A cover only the 1/8
                     decode fits for is resized from it whatever its size: taller than the
                     3072 rows the baseline path takes, it must still convert.
  progressive_wide_8000.jpg  Progressive, 8000x16, a horizontal gradient. The 1/8 workspace grows
                     with the width; this one's is past the 4 KB the framebuffer converter
                     takes without a heap floor.
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
  prog_full_420_base.jpg The same picture as prog_full_420.jpg encoded BASELINE: through the
                     thumbnail converter both must come out alike (JpegToBmpConverter test).
  prog_full_gray.jpg     The same script on a one-component (grayscale) frame, 157x99, where
                     every scan is non-interleaved. Reference: prog_full_gray.y.pgm.
  prog_full_444_rst.jpg  4:4:4, 150x90, restart marker every 3 blocks/MCUs, so every scan
                     crosses restart boundaries inside and between bands.
  prog_full_420_base_rst.jpg  prog_full_420_base.jpg's picture, baseline, restart marker every 3
                     MCUs: with 13 MCUs a row, restarts fall inside rows and, every third row,
                     exactly on a row boundary -- where a parked TJpgDec decode resumes.
  thin_lines_prog.jpg    thin_lines_gray.jpg's picture as a progressive JPEG (libjpeg script).
                     Through JpegToFramebufferConverter it must keep every line: the DC-only
                     preview it replaced shows 1/8 resolution, where they all blur away.
  thin_hlines_gray.jpg   The same, transposed (32x600, horizontal lines): lines in the last
                     row of an MCU row need the carry across MCU rows, not just blocks.
  prog_420_dc_first.jpg  prog_full_420.jpg with its DC first scan (Al = 1) and its DC refinement merged
                     into one DC scan at Al = 0, every other scan as it was: DC complete in the first
                     scan, as in the real covers measured, where libjpeg's script completes it 55-80%
                     into the file. The same coefficients, so prog_full_420.y.pgm is its reference
                     (asserted below). A 1/8 decode reads it only to the end of that scan.

Progressive cases from the Free-Ink SDK decoder's test matrix (freeink-sdk c51246c8,
libs/assets/ProgressiveJpeg/test/host/run.py), each with its libjpeg luma as the reference
(<name>.y.pgm), checked like prog_full_*:
  prog_422_rst.jpg       4:2:2 (MCU 16x8), 117x61, restart marker every 3 MCUs.
  prog_420_rst_row.jpg   4:2:0, 133x77, restart marker at every MCU row.
  prog_gray_rst.jpg      Grayscale, 101x63, restart marker every 5 blocks.
  prog_tiny_9x5.jpg      4:2:0, 9x5: smaller than one MCU.
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

# --- progressive_420.jpg : low-frequency regions survive a 1/8 decode ---
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

W, H = 64, 3200                                   # taller than the baseline path's 3072 rows
im = Image.new("L", (W, H))
d = ImageDraw.Draw(im)
for y in range(H):
    d.line([(0, y), (W, y)], fill=int(255 * y / (H - 1)))
save_progressive(im, "progressive_tall_3200.jpg")

W, H = 8000, 16                                   # a 1/8 workspace well over 4 KB
im = Image.new("L", (W, H))
d = ImageDraw.Draw(im)
for x in range(W):
    d.line([(x, 0), (x, H - 1)], fill=int(255 * x / (W - 1)))
save_progressive(im, "progressive_wide_8000.jpg")

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
busy_picture(203, 141).save(os.path.join(HERE, "prog_full_420_base.jpg"), "JPEG", quality=90, subsampling=2,
                             progressive=False, optimize=False)
busy_picture(203, 141).save(os.path.join(HERE, "prog_full_420_base_rst.jpg"), "JPEG", quality=90, subsampling=2,
                             progressive=False, optimize=False, restart_marker_blocks=3)
busy_picture(157, 99, "L").save(os.path.join(HERE, "prog_full_gray.jpg"), "JPEG", quality=90,
                                 progressive=True, optimize=True)
save_reference("prog_full_gray.jpg", "prog_full_gray.y.pgm")
busy_picture(150, 90).save(os.path.join(HERE, "prog_full_444_rst.jpg"), "JPEG", quality=85, subsampling=0,
                            progressive=True, optimize=True, restart_marker_blocks=3)
save_reference("prog_full_444_rst.jpg", "prog_full_444_rst.y.pgm")

# --- the Free-Ink SDK test matrix (see the docstring) ---
busy_picture(117, 61).save(os.path.join(HERE, "prog_422_rst.jpg"), "JPEG", quality=85, subsampling=1,
                           progressive=True, optimize=True, restart_marker_blocks=3)
save_reference("prog_422_rst.jpg", "prog_422_rst.y.pgm")
busy_picture(133, 77).save(os.path.join(HERE, "prog_420_rst_row.jpg"), "JPEG", quality=85, subsampling=2,
                           progressive=True, optimize=True, restart_marker_rows=1)
save_reference("prog_420_rst_row.jpg", "prog_420_rst_row.y.pgm")
busy_picture(101, 63, "L").save(os.path.join(HERE, "prog_gray_rst.jpg"), "JPEG", quality=85, progressive=True,
                                optimize=True, restart_marker_blocks=5)
save_reference("prog_gray_rst.jpg", "prog_gray_rst.y.pgm")
# Soft content: busy_picture saturates nearly every pixel at this size, and the scaled references
# are derived from libjpeg's clamped pixels, which are then no longer the blocks' content.
tiny = Image.new("RGB", (9, 5))
for y in range(5):
    for x in range(9):
        tiny.putpixel((x, y), (60 + 15 * x, 90 + 20 * y, 200 - 10 * x))
ImageDraw.Draw(tiny).rectangle([3, 1, 5, 3], fill=(40, 40, 40))
tiny.save(os.path.join(HERE, "prog_tiny_9x5.jpg"), "JPEG", quality=85, subsampling=2, progressive=True,
          optimize=True)
save_reference("prog_tiny_9x5.jpg", "prog_tiny_9x5.y.pgm")


# --- prog_420_dc_first.jpg: the DC scans of prog_full_420.jpg merged into one at Al = 0 ---
def jpeg_segments(data):
    """[(marker, bytes)] after SOI; an SOS carries its entropy-coded data up to the next marker."""
    out, i = [], 2
    while i < len(data):
        marker = data[i + 1]
        if marker == 0xD9:
            out.append((marker, data[i:i + 2]))
            break
        end = i + 2 + ((data[i + 2] << 8) | data[i + 3])
        if marker == 0xDA:
            while not (data[end] == 0xFF and data[end + 1] != 0x00 and not 0xD0 <= data[end + 1] <= 0xD7):
                end += 1
        out.append((marker, data[i:end]))
        i = end
    return out


class EntropyReader:
    """Bits of entropy-coded data, FF 00 unstuffed."""

    def __init__(self, data):
        self.data, self.pos, self.acc, self.left = data, 0, 0, 0

    def bit(self):
        if self.left == 0:
            self.acc = self.data[self.pos]
            self.pos += 2 if self.acc == 0xFF else 1
            self.left = 8
        self.left -= 1
        return (self.acc >> self.left) & 1

    def bits(self, count):
        value = 0
        for _ in range(count):
            value = (value << 1) | self.bit()
        return value

    def symbol(self, table):
        code, length = 0, 0
        while (length, code) not in table:
            assert length < 16, "not a code of the table"
            code, length = (code << 1) | self.bit(), length + 1
        return table[(length, code)]


class EntropyWriter:
    """Bits into entropy-coded data, 0xFF stuffed, padded with ones."""

    def __init__(self):
        self.out, self.acc, self.count = bytearray(), 0, 0

    def bits(self, value, count):
        for k in range(count - 1, -1, -1):
            self.acc, self.count = (self.acc << 1) | ((value >> k) & 1), self.count + 1
            if self.count == 8:
                self.out += bytes([0xFF, 0x00]) if self.acc == 0xFF else bytes([self.acc])
                self.acc, self.count = 0, 0

    def data(self):
        while self.count:
            self.bits(1, 1)
        return bytes(self.out)


def canonical_codes(counts, symbols):
    """A Huffman table as {(length, code): symbol} and {symbol: (code, length)}."""
    decode, encode, code, k = {}, {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            decode[(length, code)] = symbols[k]
            encode[symbols[k]] = (code, length)
            code, k = code + 1, k + 1
        code <<= 1
    return decode, encode


# T.81 Annex K.3 DC tables: every size category of an 8-bit frame.
K3_LUMA_DC = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
K3_CHROMA_DC = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))


def merge_dc_scans(data):
    """The file with its interleaved DC first scan (Al = 1) and DC refinement (Al = 0) as one
    interleaved DC scan at Al = 0, with the K.3 DC tables. No restart markers."""
    segments = jpeg_segments(data)
    dc_tables, comps, size, first, refine = {}, None, None, None, None
    for index, (marker, seg) in enumerate(segments):
        assert marker != 0xDD, "restart markers are not handled"
        if marker == 0xC2:
            size = ((seg[5] << 8) | seg[6], (seg[7] << 8) | seg[8])
            comps = [(seg[10 + 3 * k], seg[11 + 3 * k] >> 4, seg[11 + 3 * k] & 15) for k in range(seg[9])]
        elif marker == 0xC4:
            p, end = 4, 2 + ((seg[2] << 8) | seg[3])
            while p < end:
                counts = list(seg[p + 1:p + 17])
                if seg[p] >> 4 == 0:
                    dc_tables[seg[p] & 15] = canonical_codes(counts, list(seg[p + 17:p + 17 + sum(counts)]))[0]
                p += 17 + sum(counts)
        elif marker == 0xDA:
            n, header_end = seg[4], 2 + ((seg[2] << 8) | seg[3])
            ss, ah, al = seg[5 + 2 * n], seg[7 + 2 * n] >> 4, seg[7 + 2 * n] & 15
            if ss == 0 and ah == 0:
                assert first is None and n == len(comps) and al == 1, "one interleaved DC first scan at Al = 1"
                tables = {seg[5 + 2 * k]: dc_tables[seg[6 + 2 * k] >> 4] for k in range(n)}
                first = (index, tables, seg[header_end:])
            elif ss == 0:
                assert refine is None and n == len(comps) and al == 0, "one interleaved DC refinement to Al = 0"
                refine = (index, seg[header_end:])
    height, width = size
    hmax, vmax = max(c[1] for c in comps), max(c[2] for c in comps)
    mcus = -(-width // (8 * hmax)) * -(-height // (8 * vmax))
    order = [c[0] for c in comps for _ in range(c[1] * c[2])]  # the component of each block of an MCU

    reader, pred, dc = EntropyReader(first[2]), {c[0]: 0 for c in comps}, []
    for _ in range(mcus):
        for cid in order:
            category = reader.symbol(first[1][cid])
            diff = reader.bits(category)
            if category and diff < 1 << (category - 1):
                diff -= (1 << category) - 1
            pred[cid] += diff
            dc.append(pred[cid])
    reader = EntropyReader(refine[1])
    dc = [(value << 1) | reader.bit() for value in dc]

    k3 = [K3_LUMA_DC, K3_CHROMA_DC]
    encoders = {cid: canonical_codes(*k3[min(k, 1)])[1] for k, (cid, _, _) in enumerate(comps)}
    writer, pred = EntropyWriter(), {c[0]: 0 for c in comps}
    values = iter(dc)
    for _ in range(mcus):
        for cid in order:
            value = next(values)
            diff, pred[cid] = value - pred[cid], value
            category = abs(diff).bit_length()
            writer.bits(*encoders[cid][category])
            writer.bits(diff if diff >= 0 else diff - 1, category)

    tables = b"".join(bytes([k]) + bytes(t[0]) + bytes(t[1]) for k, t in enumerate(k3[:min(len(comps), 2)]))
    dht = bytes([0xFF, 0xC4]) + (2 + len(tables)).to_bytes(2, "big") + tables
    header = bytes([len(comps)]) + b"".join(bytes([cid, min(k, 1) << 4]) for k, (cid, _, _) in enumerate(comps))
    sos = bytes([0xFF, 0xDA]) + (5 + len(header)).to_bytes(2, "big") + header + bytes([0, 0, 0])
    out = bytearray(data[:2])
    for index, (_, seg) in enumerate(segments):
        if index == first[0]:
            out += dht + sos + writer.data()
        elif index != refine[0]:
            out += seg
    return bytes(out)


with open(os.path.join(HERE, "prog_full_420.jpg"), "rb") as f:
    merged = merge_dc_scans(f.read())
with open(os.path.join(HERE, "prog_420_dc_first.jpg"), "wb") as f:
    f.write(merged)
# The same coefficients: libjpeg decodes the same picture, so prog_full_420.y.pgm is the reference.
assert (Image.open(os.path.join(HERE, "prog_420_dc_first.jpg")).convert("RGB").tobytes() ==
        Image.open(os.path.join(HERE, "prog_full_420.jpg")).convert("RGB").tobytes())

print("Fixtures written to", HERE)
for f in sorted(os.listdir(HERE)):
    if f.endswith((".jpg", ".pgm")):
        print(" ", f, os.path.getsize(os.path.join(HERE, f)), "bytes")
