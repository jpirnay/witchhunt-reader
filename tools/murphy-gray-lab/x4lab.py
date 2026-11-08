#!/usr/bin/env python3
"""The gray lab's long-hold test on the Xteink X4 (SSD1677), through the reader firmware's CMD:LAB.

The X4 has no lab firmware. A development build of the reader draws each frame (both RAM planes, from
rectangles) and sends it with an external LUT built here from the X4 Pro's text table, the one the X4's
one-pass runs. The row is RED * 2 + BW, so the plane codes are the UC8253 lab's: 0 (0,0), 1 (0,1),
2 (1,0), 3 (1,1). Rows 0 and 1 are the X4's one-pass black (VSH1 17) and white (VSL 24) as shipped.

  ./x4lab.py white | black | marks         a normal full refresh, for cam.py's baselines and calibration
  ./x4lab.py t11 [--stay nudge|rest]       Test 11 (see lab.py): bands held black 1-500 pages, watched an hour
  ./x4lab.py t11-cancel [--pages N]        pay back what it drove (give the same --stay)

Set MURPHY_PORT to the X4's port; cam.py keeps the X4's shots apart (LAB_BOARD=x4, set here).
"""

import argparse
import os
import re
import sys
import time
from pathlib import Path

os.environ["LAB_BOARD"] = "x4"
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # tools/, for devctl
import cam  # noqa: E402
import devctl  # noqa: E402

WIDTH, HEIGHT = 800, 480  # panel RAM, as cam.py's PANEL_W, PANEL_H for the X4
TABLE = Path(__file__).resolve().parents[2] / "freeink-sdk/libs/display/FreeInkDisplay/src/lut/X4ProTextAaLuts.h"
# The table's timing: group 0 runs phases of 3, 1, 2 and 2 frames, group 1 phases of 2, 8, 2 and 8.
PHASES = [(0, 0, 3), (0, 1, 1), (0, 2, 2), (0, 3, 2), (1, 0, 2), (1, 1, 8), (1, 2, 2), (1, 3, 8)]
REST, VSH1, VSL = 0, 1, 2  # SSD1677 voltage-select codes
BLACK = (0x15, 0x54)  # VSH1 17: kTextOnePass row 0, the X4's one-pass black
WHITE = (0x0A, 0xAA)  # VSL 24: kTextOnePass row 1, the X4's one-pass white
RESTROW = (0x00, 0x00)

HOLDS = (1, 50, 200, 500)
FAMILIES = ("redrive", "stay", "rest")
BANDS = 2 + len(HOLDS) * len(FAMILIES)
BAND = HEIGHT // BANDS
WATCH_MINUTES = (0, 10, 20, 30, 40, 50, 60)
CODE = {"redrive": 0, "stay": 2, "rest": 3}


def base_table():
    text = TABLE.read_text()
    body = text[text.index("kX4ProTextSsd1677[110]") :]
    body = body[body.index("{") + 1 : body.index("}")]
    data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    if len(data) != 110:
        raise SystemExit(f"{TABLE}: expected 110 bytes, read {len(data)}")
    return data


def drive(row):
    """Net frames of a row: VSH1 counts toward black, VSL toward white."""
    total = 0
    for group, phase, frames in PHASES:
        level = (row[group] >> (6 - 2 * phase)) & 3
        total += frames if level in (VSH1, 3) else -frames if level == VSL else 0
    return total


def row(level, frames):
    """Two VS bytes that drive `level` for exactly `frames` frames, from the table's phases."""
    for mask in range(1 << len(PHASES)):
        if sum(PHASES[i][2] for i in range(len(PHASES)) if mask >> i & 1) == frames:
            out = [0, 0]
            for i, (group, phase, _) in enumerate(PHASES):
                if mask >> i & 1:
                    out[group] |= level << (6 - 2 * phase)
            return tuple(out)
    raise ValueError(f"{frames} frames do not fit the phases")


def lut_hex(rows):
    """The table with its rows 0-3 (codes 0-3) replaced; group 0 and 1 bytes only, the rest of each row 0."""
    table = base_table()
    for code, (first, second) in enumerate(rows):
        table[10 * code : 10 * code + 10] = [first, second] + [0] * 8
    return bytes(table).hex()


def stay_row(stay):
    return row(VSH1, 2) if stay == "nudge" else RESTROW


def bank(stay):
    return (BLACK, WHITE, stay_row(stay), RESTROW)


class X4:
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

    def frame(self, rows, codes):
        """One refresh with these four rows; codes[i] for band i + 1, controls rest."""
        lut = lut_hex(rows)
        if lut != self.lut:
            self.command("LAB:LUT:" + lut)
            self.lut = lut
        rects = [(0, (i + 1) * BAND, WIDTH, BAND, code) for i, code in enumerate(codes) if code != 3]
        return self.command("LAB:DRAW:LUT:3:" + ";".join(f"{x},{y},{w},{h},{c}" for x, y, w, h, c in rects))


def bands():
    return [(hold, family) for hold in HOLDS for family in FAMILIES]


def pages(upto=None):
    total = max(HOLDS)
    for page in range(1, (upto or total) + 1):
        yield tuple(3 if page < total - hold + 1 else 0 if page == total - hold + 1 else CODE[family]
                    for hold, family in bands())
    if upto is None:
        yield tuple(1 for _ in bands())


def owed(stay, upto=None):
    rows = bank(stay)
    net = [0] * len(bands())
    for codes in pages(upto):
        for i, code in enumerate(codes):
            net[i] += drive(rows[code])
    return net


def camera(name):
    cam.shot("/dev/video0", name, cam.EXPOSURE)


def test11(x4, stay):
    x4.full(3)
    rows = bank(stay)
    started = time.time()
    all_pages = list(pages())
    for number, codes in enumerate(all_pages[:-1], start=1):
        print(f"{time.strftime('%T')} t11 page {number}: {x4.frame(rows, codes)}", flush=True)
    print(f"t11: {len(all_pages) - 1} pages in {time.time() - started:.0f} s", flush=True)
    camera("t11-black")
    print(f"{time.strftime('%T')} t11 to white: {x4.frame(rows, all_pages[-1])}", flush=True)
    whitened = time.time()
    for minutes in WATCH_MINUTES:
        while time.time() < whitened + minutes * 60:
            time.sleep(5)
        camera(f"t11-white-{minutes:02d}")
        print(f"{time.strftime('%T')} shot t11-white-{minutes:02d}", flush=True)


def cancel(x4, stay, upto=None):
    left = owed(stay, upto)
    print("net drive to pay back, band by band:", left, flush=True)
    refreshes = 0
    while any(left):
        steps = {}
        for i, value in enumerate(left):
            if value:
                step = (VSL if value > 0 else VSH1, min(abs(value), 28))
                if step in steps or len(steps) < 3:
                    steps.setdefault(step, []).append(i)
        rows = [row(level, frames) for level, frames in steps] + [RESTROW] * (3 - len(steps)) + [RESTROW]
        codes = [3] * len(left)
        for code, ((level, frames), members) in enumerate(steps.items()):
            for i in members:
                codes[i] = code
                left[i] -= frames if level == VSL else -frames
        x4.frame(tuple(rows), codes)
        refreshes += 1
    x4.full(3)
    print(f"cancel: {refreshes} refreshes, then a full white", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("white", "black", "marks", "t11", "t11-cancel", "check"))
    parser.add_argument("--stay", choices=("nudge", "rest"), default="nudge")
    parser.add_argument("--pages", type=int)
    args = parser.parse_args()
    if args.step == "check":  # offline: the rows' drives and what a full run leaves
        for name, r in (("black", BLACK), ("white", WHITE), ("nudge", row(VSH1, 2)), ("rest", RESTROW)):
            print(f"{name}: {r[0]:#04x} {r[1]:#04x} drive {drive(r):+}")
        print("owed after a full run:", owed(args.stay))
        return
    x4 = X4(os.environ["MURPHY_PORT"])
    if args.step in ("white", "black"):
        print(x4.full(3 if args.step == "white" else 0))
    elif args.step == "marks":
        size = cam.CALIB_SIZE
        print(x4.full(3, [(x, y, size, size, 0) for x, y in (*cam.CALIB_MARKS, cam.CALIB_ORIENT)]))
    elif args.step == "t11":
        test11(x4, args.stay)
    else:
        cancel(x4, args.stay, args.pages)


if __name__ == "__main__":
    main()
