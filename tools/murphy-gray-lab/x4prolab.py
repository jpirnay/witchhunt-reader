#!/usr/bin/env python3
"""The gray lab on the Xteink X4 Pro (UC8179), through the reader firmware's CMD:LAB.

Like piccolab.py: a development build draws both planes from rectangles and refreshes them with tables built
here. The UC8179 takes five 42-byte tables, 0x20 (VCOM) and one per pixel code, each seven groups of a level
byte (four phases, two bits each, first phase in the top bits: 01 drives black, 10 white), four phase lengths
in frames and a repeat count. Every table here keeps the X4 Pro's shipped text table timing (kX4ProTextUc8179:
group 0 phases of 1, 3, 2, 1 frames, group 1 of 6, 8, 2, 0, once each) unless --timing replaces it.

The lab's pixel code is RED * 2 + BW, with RED in DTM1 and BW in DTM2. Which table a code selects is the
controller's business: `map` measures it, and CODE_TO_TABLE holds the result.

  ./x4prolab.py white | black | marks       a normal full refresh, for cam.py's baselines and calibration
  ./x4prolab.py grid                         squares for `cam.py mesh` (shot "grid"), after `cam.py calibrate`
  ./x4prolab.py map                          one refresh from white: table 0x21 black 23, 0x22 black 6,
                                             0x23 black 2, 0x24 nothing; bands for codes 0, 1, 2, 3
  ./x4prolab.py wedge [--start black] [--reverse]
  ./x4prolab.py gray --gray SEQ [--gray SEQ ...]   SEQ: one of K W . per drivable phase (7 by default)
  ./x4prolab.py t11 | t11-cancel [--pages N] | timing | check

Test 11 families, what a page that keeps a pixel black gives it:
  redrive  20 frames to black, as the shipped text table's black (X4ProTextAa codes black every page)
  stay     1 frame
  rest     nothing
Set MURPHY_PORT to the X4 Pro's port; cam.py keeps its shots apart (LAB_BOARD=x4pro, set here).
"""

import argparse
import os
import re
import sys
import time
from pathlib import Path

os.environ["LAB_BOARD"] = "x4pro"
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # tools/, for devctl
import cam  # noqa: E402
import devctl  # noqa: E402

WIDTH, HEIGHT = 800, 480
GROUPS = 7
TIMING = [[1, 3, 2, 1], [6, 8, 2, 0]]  # phase lengths per group, as kX4ProTextUc8179
K, W = "black", "white"
LEVEL = {K: 0b01, W: 0b10}
# Code (RED * 2 + BW) -> table register. Assumed from the one-pass frame (DTM1 low plane, DTM2 high plane:
# (1,1) white 0x21, (0,1) held white 0x22, (1,0) gray 0x23, (0,0) black 0x24); `map` checks it.
CODE_TO_TABLE = {0: 0x24, 1: 0x22, 2: 0x23, 3: 0x21}

HOLDS = (1, 50, 200, 500)
FAMILIES = ("redrive", "stay", "rest")
BANDS = 2 + len(HOLDS) * len(FAMILIES)
BAND = HEIGHT // BANDS
WATCH_MINUTES = (0, 10, 20)
CODE = {"redrive": 0, "stay": 2, "rest": 3}
CANCEL_STEP = 20
CLEAN_CYCLES = 5


def phases():
    """Drivable phases: (group, phase, frames)."""
    return [(g, p, n) for g, group in enumerate(TIMING) for p, n in enumerate(group) if n]


def seq_row(seq):
    """A row: one level per drivable phase, K black, W white, . nothing. Returns level bytes per group."""
    plan = phases()
    if len(seq) != len(plan):
        raise ValueError(f"{seq}: {len(plan)} drivable phases")
    out = [0] * GROUPS
    for (g, p, _), c in zip(plan, seq):
        if c != ".":
            out[g] |= LEVEL[K if c == "K" else W] << (6 - 2 * p)
    return tuple(out)


def row(level, frames):
    """Drive `level` for exactly `frames` frames, from the earliest phases that sum to it."""
    plan = phases()
    best = None
    for mask in range(1 << len(plan)):
        if sum(plan[i][2] for i in range(len(plan)) if mask >> i & 1) == frames:
            if best is None or mask < best:
                best = mask
    if best is None:
        raise ValueError(f"{frames} frames do not fit the phases")
    return seq_row("".join(("K" if level == K else "W") if best >> i & 1 else "." for i in range(len(plan))))


REST = (0,) * GROUPS
BLACK = seq_row(".KKKKK.")  # 3 + 2 + 1 + 6 + 8 = 20: the shipped black (0x15, 0x50)
WHITE = seq_row(".WWWWWW")  # 3 + 2 + 1 + 6 + 8 + 2 = 22: the shipped white (0x2A, 0xA8)
STAY = seq_row("K......")
ROWS = (BLACK, WHITE, STAY, REST)  # codes 0-3 in the Test 11 frames


def drive(levels):
    total = 0
    for g, p, n in phases():
        v = levels[g] >> (6 - 2 * p) & 3
        total += n if v == 1 else -n if v == 2 else 0
    return total


def table(levels):
    out = []
    for g in range(GROUPS):
        group = TIMING[g] if g < len(TIMING) else [0, 0, 0, 0]
        out += [levels[g] if g < len(levels) else 0, *group, 1 if any(group) else 0]
    return out


def lut_hex(rows):
    """VCOM, then the four pixel tables, each placed where its code's register is."""
    by_register = {CODE_TO_TABLE[code]: r for code, r in enumerate(rows)}
    data = table(REST)  # 0x20: VCOM at DC through the same timing
    for register in (0x21, 0x22, 0x23, 0x24):
        data += table(by_register[register])
    assert len(data) == 210
    return bytes(data).hex()


class X4Pro:
    def __init__(self, port):
        self.device = devctl.Device(port)
        self.lut = None

    def command(self, text):
        reply = self.device.command(text, until=re.compile(r"^LAB_(OK|ERROR)"))
        if reply is None or not reply.startswith("LAB_OK"):
            raise SystemExit(f"STOP: {text[:40]}...: {reply}")
        return reply

    def full(self, background, rects=()):
        return self.command(f"LAB:DRAW:FULL:{background}:" + ";".join(f"{x},{y},{w},{h},{c}" for x, y, w, h, c in rects))

    def frame(self, rows, codes, background=3, lut=None):
        lut = lut or lut_hex(rows)
        if lut != self.lut:
            self.command("LAB:LUT:" + lut)
            self.lut = lut
        rects = [(0, (i + 1) * BAND, WIDTH, BAND, code) for i, code in enumerate(codes) if code != background]
        return self.command(f"LAB:DRAW:LUT:{background}:" + ";".join(f"{x},{y},{w},{h},{c}" for x, y, w, h, c in rects))


def camera(name):
    cam.shot("/dev/video0", name, cam.EXPOSURE)


def map_codes(x4):
    """From white: 0x21 black 23, 0x22 black 6, 0x23 black 2, 0x24 nothing. Bands 1-4 carry codes 0-3."""
    regs = {0x21: row(K, 23), 0x22: row(K, 6), 0x23: row(K, 2), 0x24: REST}
    data = table(REST)
    for register in (0x21, 0x22, 0x23, 0x24):
        data += table(regs[register])
    x4.full(3)
    codes = [0, 1, 2, 3] + [None] * (BANDS - 6)
    rects = [(0, (i + 1) * BAND, WIDTH, BAND, c) for i, c in enumerate(codes) if c is not None]
    x4.command("LAB:LUT:" + bytes(data).hex())
    x4.lut = None
    print("map:", x4.command("LAB:DRAW:LUT:0:" + ";".join(f"{x},{y},{w},{h},{c}" for x, y, w, h, c in rects)),
          flush=True)
    camera("map")
    print("bands 1-4 = codes 0-3; darkest = 0x21 (23), then 0x22 (6), 0x23 (2), white = 0x24", flush=True)


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


def test11(x4):
    x4.full(3)
    started = time.time()
    all_pages = list(pages())
    for number, codes in enumerate(all_pages[:-1], start=1):
        reply = x4.frame(ROWS, codes)
        if number % 25 == 0 or number < 3:
            print(f"{time.strftime('%T')} t11 page {number}: {reply}", flush=True)
    print(f"t11: {len(all_pages) - 1} pages in {time.time() - started:.0f} s", flush=True)
    camera("t11-black")
    print(f"{time.strftime('%T')} t11 to white: {x4.frame(ROWS, all_pages[-1])}", flush=True)
    whitened = time.time()
    for minutes in WATCH_MINUTES:
        while time.time() < whitened + minutes * 60:
            time.sleep(5)
        camera(f"t11-white-{minutes:02d}")
        print(f"{time.strftime('%T')} shot t11-white-{minutes:02d}", flush=True)


def reachable_set():
    plan = phases()
    return {sum(plan[i][2] for i in range(len(plan)) if m >> i & 1) for m in range(1 << len(plan))}


def wedge(x4, start, reverse=False):
    level = K if start == "white" else W
    counts = [n for n in (1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 23) if n in reachable_set()]
    if reverse:
        counts.reverse()
    x4.full(3 if start == "white" else 0)
    for first in range(0, len(counts), 3):
        group = counts[first : first + 3]
        rows = [row(level, n) for n in group] + [REST] * (3 - len(group)) + [REST]
        codes = [3] * (BANDS - 2)
        for code, n in enumerate(group):
            codes[counts.index(n)] = code
        print(f"wedge {start} {group}: {x4.frame(tuple(rows), codes)}", flush=True)
    print("bands 1..", len(counts), "=", counts, flush=True)
    camera(f"wedge-from-{start}{'-rev' if reverse else ''}")
    x4.full(3)


def gray(x4, variants):
    """Each gray row from a white and from a black start, side by side (bands 1-2, 3-4, 5-6)."""
    variants = variants[:3]
    rects = [(0, (1 + 2 * i + 1) * BAND, WIDTH, BAND, 0) for i in range(len(variants))]  # odd bands start black
    rects.append((0, 13 * BAND, WIDTH, BAND, 0))  # the bottom control is black
    x4.full(3, rects)
    rows = [seq_row(v) for v in variants] + [REST] * (3 - len(variants)) + [REST]
    codes = [3] * (BANDS - 2)
    for i in range(len(variants)):
        codes[2 * i] = codes[2 * i + 1] = i
    print(f"gray {variants} drives {[drive(seq_row(v)) for v in variants]}: {x4.frame(tuple(rows), codes)}",
          flush=True)
    name = "gray-" + "-".join(v.replace(".", "o") for v in variants)
    camera(name)
    print("shot", name, flush=True)
    x4.full(3)


def timing_run(x4, repeats):
    times = []
    for _ in range(repeats):
        reply = x4.frame(ROWS, [3] * (BANDS - 2))
        times.append(int(reply.split(":")[1]))
    print(f"timing {TIMING} = {sum(map(sum, TIMING))} frames: LAB_OK ms {times}", flush=True)


def cancel(x4, upto=None):
    left = owed(upto)
    print("net drive to pay back, band by band:", left, flush=True)
    sums = sorted(reachable_set())
    refreshes = 0
    while any(left):
        steps = {}
        for i, value in enumerate(left):
            if value:
                step = (W if value > 0 else K, max(s for s in sums if s <= min(abs(value), CANCEL_STEP)))
                if step in steps or len(steps) < 3:
                    steps.setdefault(step, []).append(i)
        rows = [row(level, frames) for level, frames in steps] + [REST] * (3 - len(steps)) + [REST]
        codes = [3] * len(left)
        for code, ((level, frames), members) in enumerate(steps.items()):
            for i in members:
                codes[i] = code
                left[i] -= frames if level == W else -frames
        x4.frame(tuple(rows), codes)
        refreshes += 1
    for _ in range(CLEAN_CYCLES):
        x4.full(0)
        x4.full(3)
    print(f"cancel: {refreshes} refreshes, then {CLEAN_CYCLES} full black and white", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("white", "black", "marks", "grid", "map", "t11", "t11-cancel", "check", "wedge",
                                         "gray", "timing"))
    parser.add_argument("--pages", type=int)
    parser.add_argument("--timing", help="phase lengths per group, groups split by '/': e.g. 1,3,2,1/6,8,2,0")
    parser.add_argument("--start", choices=("white", "black"), default="white")
    parser.add_argument("--gray", action="append", default=[])
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--reverse", action="store_true")
    parser.add_argument("--map", help="code->register as four hex registers for codes 0-3, e.g. 24,22,23,21")
    args = parser.parse_args()
    if args.timing:
        TIMING[:] = [[int(v) for v in g.split(",")] for g in args.timing.split("/")]
        assert len(TIMING) <= GROUPS and all(len(g) == 4 and all(0 <= v < 256 for v in g) for g in TIMING)
    if args.map:
        CODE_TO_TABLE.update({i: int(v, 16) for i, v in enumerate(args.map.split(","))})
    if args.step == "check":
        for name, r in (("black", BLACK), ("white", WHITE), ("stay", STAY), ("rest", REST)):
            print(f"{name}: {[f'{b:#04x}' for b in r[:2]]} drive {drive(r):+}")
        assert BLACK[:2] == (0x15, 0x50) and WHITE[:2] == (0x2A, 0xA8), (BLACK[:2], WHITE[:2])
        print("reachable:", sorted(reachable_set()))
        print("owed after a full run:", owed())
        print("lut bytes:", len(lut_hex(ROWS)) // 2)
        return
    x4 = X4Pro(os.environ["MURPHY_PORT"])
    if args.step in ("white", "black"):
        print(x4.full(3 if args.step == "white" else 0))
    elif args.step == "marks":
        size = cam.CALIB_SIZE
        print(x4.full(3, [(x, y, size, size, 0) for x, y in (*cam.CALIB_MARKS, cam.CALIB_ORIENT)]))
    elif args.step == "grid":
        half, size = cam.GRID_SIZE // 2, cam.GRID_SIZE
        print(x4.full(3, [(x - half, y - half, size, size, 0) for y in cam.GRID_YS for x in cam.GRID_XS]))
    elif args.step == "map":
        map_codes(x4)
    elif args.step == "t11":
        test11(x4)
    elif args.step == "wedge":
        wedge(x4, args.start, args.reverse)
    elif args.step == "gray":
        gray(x4, args.gray)
    elif args.step == "timing":
        timing_run(x4, args.repeats)
    else:
        cancel(x4, args.pages)


if __name__ == "__main__":
    main()
