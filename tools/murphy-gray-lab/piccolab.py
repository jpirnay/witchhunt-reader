#!/usr/bin/env python3
"""The gray lab on the BOOX Picco (SSD2677 + SE0400NQW47), through the reader firmware's CMD:LAB.

Like x4lab.py: a development build of the reader draws both planes from rectangles and refreshes them with
a 535-byte LUT built here. The pixel code is RED * 2 + BW and selects pixel table 1 + code. Every table is
the stock reader FAST table (lut_picco_se0400_fast: one group, phases of 6, 1, 6, -, 6, 4, 1, - frames,
the fourth phase of each half never driven) with its drive levels replaced.

  ./piccolab.py white | black | marks       a normal full refresh, for cam.py's baselines and calibration
  ./piccolab.py t11                         Test 11: bands held black 1-500 pages, watched an hour
  ./piccolab.py t11-cancel [--pages N]      pay back what it drove
  ./piccolab.py check                       offline: the rows and what a run leaves

Test 11 families, what a page that keeps a pixel black gives it:
  redrive  24 frames to black, as Free-Ink PR #213's one-pass text table does for every black pixel
  stay     the stock FAST table's 1 frame for black that stays
  rest     nothing
Set MURPHY_PORT to the Picco's port; cam.py keeps its shots apart (LAB_BOARD=picco, set here).
"""

import argparse
import os
import re
import sys
import time
from pathlib import Path

os.environ["LAB_BOARD"] = "picco"
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # tools/, for devctl
import cam  # noqa: E402
import devctl  # noqa: E402

WIDTH, HEIGHT = 800, 480  # panel RAM, as cam.py's PANEL_W, PANEL_H for the Picco
TABLE = Path(__file__).resolve().parents[2] / "freeink-sdk/libs/display/FreeInkDisplay/src/lut/Ssd2677Luts.h"
BASE = "lut_picco_se0400_fast"
# Drivable phases of the group: (level byte 1 or 2, phase within the half, frames).
PHASES = [(0, 0, 6), (0, 1, 1), (0, 2, 6), (1, 0, 6), (1, 1, 4), (1, 2, 1)]
WHITE_BIT, BLACK_BIT = (5, 3, 1), (2, 0, 6)  # active-low drive bits per phase of a half
K, W = "black", "white"

BLACK = (0xBA, 0xBA)  # +24: stock FAST to-black, PR #213 text code 00
WHITE = (0xD5, 0xD5)  # -24: stock FAST to-white, PR #213 text code 11
STAY = (0xFF, 0xBF)  # +1: stock FAST black-stays
REST = (0xFF, 0xFF)

HOLDS = (1, 50, 200, 500)
FAMILIES = ("redrive", "stay", "rest")
BANDS = 2 + len(HOLDS) * len(FAMILIES)
BAND = HEIGHT // BANDS
WATCH_MINUTES = (0, 10, 20, 30, 40, 50, 60)
CODE = {"redrive": 0, "stay": 2, "rest": 3}
ROWS = (BLACK, WHITE, STAY, REST)  # codes 0-3
# Pay back no faster than the test drove (24 frames a page), then full black and white refreshes: on the
# X4 (2026-11-08) a faster cancel left a stripe that only full refreshes cleared.
CANCEL_STEP = 24
CLEAN_CYCLES = 5


def base_table():
    text = TABLE.read_text()
    body = text[text.index(BASE + "[535]") :]
    body = body[body.index("{") + 1 : body.index("}")]
    data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    if len(data) != 535:
        raise SystemExit(f"{TABLE}: expected 535 bytes, read {len(data)}")
    return data


def drive(row):
    """Net frames of a row: black positive."""
    total = 0
    for half, phase, frames in PHASES:
        level = row[half]
        black = not (level >> BLACK_BIT[phase]) & 1
        white = not (level >> WHITE_BIT[phase]) & 1
        total += frames * (black - white)
    return total


def row(level, frames):
    """Two level bytes that drive `level` for exactly `frames` frames."""
    for mask in range(1 << len(PHASES)):
        if sum(PHASES[i][2] for i in range(len(PHASES)) if mask >> i & 1) == frames:
            out = [0xFF, 0xFF]
            for i, (half, phase, _) in enumerate(PHASES):
                if mask >> i & 1:
                    out[half] &= ~(1 << (BLACK_BIT if level == K else WHITE_BIT)[phase]) & 0xFF
            return tuple(out)
    raise ValueError(f"{frames} frames do not fit the phases")


def reachable(limit):
    """The largest frame count up to `limit` that the phases can make."""
    sums = {sum(PHASES[i][2] for i in range(len(PHASES)) if m >> i & 1) for m in range(1 << len(PHASES))}
    return max(s for s in sums if s <= limit)


def lut_hex(rows):
    table = base_table()
    for code, (first, second) in enumerate(rows):
        at = 7 + (code + 1) * 88  # pixel table 1 + code, group 0
        table[at + 1], table[at + 2] = first, second
    return bytes(table).hex()


class Picco:
    def __init__(self, port):
        self.device = devctl.Device(port)
        self.lut = None

    def command(self, text, timeout=30):
        reply = self.device.command(text, timeout=timeout, until=re.compile(r"^LAB_(OK|ERROR)"))
        if reply is None or not reply.startswith("LAB_OK"):
            raise SystemExit(f"STOP: {text[:40]}...: {reply}")
        return reply

    def full(self, background, rects=()):
        return self.command(f"LAB:DRAW:FULL:{background}:" + ";".join(f"{x},{y},{w},{h},{c}" for x, y, w, h, c in rects))

    def frame(self, rows, codes, background=3):
        """One refresh with these four rows; codes[i] for band i + 1, the rest `background`."""
        lut = lut_hex(rows)
        if lut != self.lut:
            half = len(lut) // 2 // 2 * 2
            self.command("LAB:LUT:" + lut[:half])
            self.command("LAB:LUT+:" + lut[half:])
            self.lut = lut
        rects = [(0, (i + 1) * BAND, WIDTH, BAND, code) for i, code in enumerate(codes) if code != background]
        return self.command(f"LAB:DRAW:LUT:{background}:" + ";".join(f"{x},{y},{w},{h},{c}" for x, y, w, h, c in rects))


def bands():
    return [(hold, family) for hold in HOLDS for family in FAMILIES]


def pages(upto=None):
    total = max(HOLDS)
    for page in range(1, (upto or total) + 1):
        yield tuple(3 if page < total - hold + 1 else 0 if page == total - hold + 1 else CODE[family]
                    for hold, family in bands())
    if upto is None:
        yield tuple(1 for _ in bands())


def owed(upto=None):
    net = [0] * len(bands())
    for codes in pages(upto):
        for i, code in enumerate(codes):
            net[i] += drive(ROWS[code])
    return net


def camera(name):
    cam.shot("/dev/video0", name, cam.EXPOSURE)


def test11(picco):
    picco.full(3)
    started = time.time()
    all_pages = list(pages())
    for number, codes in enumerate(all_pages[:-1], start=1):
        print(f"{time.strftime('%T')} t11 page {number}: {picco.frame(ROWS, codes)}", flush=True)
    print(f"t11: {len(all_pages) - 1} pages in {time.time() - started:.0f} s", flush=True)
    camera("t11-black")
    print(f"{time.strftime('%T')} t11 to white: {picco.frame(ROWS, all_pages[-1])}", flush=True)
    whitened = time.time()
    for minutes in WATCH_MINUTES:
        while time.time() < whitened + minutes * 60:
            time.sleep(5)
        camera(f"t11-white-{minutes:02d}")
        print(f"{time.strftime('%T')} shot t11-white-{minutes:02d}", flush=True)


def cancel(picco, upto=None):
    left = owed(upto)
    print("net drive to pay back, band by band:", left, flush=True)
    refreshes = 0
    while any(left):
        steps = {}
        for i, value in enumerate(left):
            if value:
                step = (W if value > 0 else K, reachable(min(abs(value), CANCEL_STEP)))
                if step in steps or len(steps) < 3:
                    steps.setdefault(step, []).append(i)
        rows = [row(level, frames) for level, frames in steps] + [REST] * (3 - len(steps)) + [REST]
        codes = [3] * len(left)
        for code, ((level, frames), members) in enumerate(steps.items()):
            for i in members:
                codes[i] = code
                left[i] -= frames if level == W else -frames
        picco.frame(tuple(rows), codes)
        refreshes += 1
    for _ in range(CLEAN_CYCLES):
        picco.full(0)
        picco.full(3)
    print(f"cancel: {refreshes} refreshes, then {CLEAN_CYCLES} full black and white", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("white", "black", "marks", "t11", "t11-cancel", "check"))
    parser.add_argument("--pages", type=int)
    args = parser.parse_args()
    if args.step == "check":
        for name, r in (("black", BLACK), ("white", WHITE), ("stay", STAY), ("rest", REST)):
            print(f"{name}: {r[0]:#04x} {r[1]:#04x} drive {drive(r):+}")
        assert row(K, 24) == BLACK and row(W, 24) == WHITE, (row(K, 24), row(W, 24))
        print("reachable:", sorted({reachable(n) for n in range(25)}))
        print("owed after a full run:", owed())
        print("lut bytes:", len(lut_hex(ROWS)) // 2)
        return
    picco = Picco(os.environ["MURPHY_PORT"])
    if args.step in ("white", "black"):
        print(picco.full(3 if args.step == "white" else 0))
    elif args.step == "marks":
        size = cam.CALIB_SIZE
        print(picco.full(3, [(x, y, size, size, 0) for x, y in (*cam.CALIB_MARKS, cam.CALIB_ORIENT)]))
    elif args.step == "t11":
        test11(picco)
    else:
        cancel(picco, args.pages)


if __name__ == "__main__":
    main()
