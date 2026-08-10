"""Generate a deterministic PNG fixture for PngStreamDecoder arena tests.

Hand-rolled (no PIL): writes a truecolour 8-bit PNG with a gradient + blocks so the
decoded grayscale has structure, and large enough that the inflate ring matters
(height * (rawRowBytes+1) well over the 32 KB ring cap).
"""
import struct
import sys
import zlib

W, H = 160, 120


def chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


raw = bytearray()
for y in range(H):
    raw.append(0)  # filter type 0 (None) — keeps the fixture reproducible
    for x in range(W):
        r = (x * 255) // (W - 1)
        g = (y * 255) // (H - 1)
        b = 255 if ((x // 16) + (y // 16)) % 2 == 0 else 0
        raw += bytes((r, g, b))

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))  # colorType 2 = RGB
png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
png += chunk(b"IEND", b"")

out = sys.argv[1]
with open(out, "wb") as f:
    f.write(png)
print("wrote", out, len(png), "bytes; raw =", len(raw))
