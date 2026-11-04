#!/usr/bin/env python3
"""The ghost a real list leaves, measured on the reader rather than on lab patches.

Run with a settings list on screen and a reader built with the serial test API. Waits for a HALF to
draw the selection bar, lets it sit, and moves it away with one FAST. The ghost is the band the bar
left, against a row that never held it, in the same photo, taking the bright end of each band's
columns so that its text does not count.

Do not compare with a later redraw of the screen instead: the HALF that gives you one leaves its own
ghost in the band, and the band of the bar that just arrived differs only because FAST black and HALF
black are not the same black. Both were tried, and both gave a wrong answer.
"""
import argparse
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import cam

ROOT = Path(__file__).resolve().parents[2]
DEVCTL = ROOT / "tools/devctl.py"
COLUMNS = range(16, 224, 4)  # panel x, across the list
REFERENCE_OFFSET = (-94, -54)  # rows, from the lower bar position: two rows that were never selected


def press(port, key):
    """One key press. Returns the refresh mode the firmware logged for it."""
    with tempfile.NamedTemporaryFile(suffix=".log") as log:
        subprocess.run(
            [sys.executable, str(DEVCTL), "--port", port, "--log", log.name, f"cmd:BTN:{key}", "idle:6000"],
            check=True, stdout=subprocess.DEVNULL, timeout=30,
        )  # fmt: skip
        modes = [line.split("mode=")[1].split()[0] for line in Path(log.name).read_text(errors="replace").splitlines()
                 if "displayBuffer mode=" in line]  # fmt: skip
    return modes[-1] if modes else "none"


def bar_rows(image, white, black):
    """The tallest run of dark rows: the selection bar. None if there is no bar in the shot."""
    dark = [y for y in range(4, cam.PANEL_H - 4, 2) if cam.level(image, white, black, (20, y, 200, 2)) < 35]
    runs, run = [], []
    for y in dark:
        if run and y - run[-1] > 2:
            runs.append(run)
            run = []
        run.append(y)
    if run:
        runs.append(run)
    tall = max(runs, key=len, default=[])
    return (tall[0], tall[-1]) if len(tall) >= 16 else None


def band_level(image, white, black, band):
    """How white a band is: the bright end of its columns, so its text does not count."""
    top, bottom = band
    levels = sorted(cam.level(image, white, black, (x, top + 6, 4, bottom - top - 12)) for x in COLUMNS)
    return levels[len(levels) * 3 // 4]


def ghost_in(image, old_bar, reference, white, black):
    return band_level(image, white, black, old_bar) - band_level(image, white, black, reference)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tag")
    parser.add_argument("--port", required=True)
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--dwell", type=int, default=20, help="seconds the bar stays up before it moves")
    args = parser.parse_args()

    camera = cam.load_camera()
    white, black = cam.load_rectified("baseline-white", camera), cam.load_rectified("baseline-black", camera)
    forward, back = "DOWN", "UP"
    for _ in range(12):  # start from a bar drawn by a HALF
        if press(args.port, forward) == "HALF":
            break
        forward, back = back, forward
    else:
        sys.exit("no HALF in 12 presses")

    fast, half = [], []
    for pair in range(args.pairs):
        # The bar was drawn by a HALF after pressing `forward`: `back` moves it away with a FAST.
        time.sleep(args.dwell)
        names = [f"ui-{args.tag}-{pair}-{step}" for step in ("before", "after", "clean")]
        cam.shot("/dev/video0", names[0], cam.EXPOSURE)
        modes = [press(args.port, back)]
        cam.shot("/dev/video0", names[1], cam.EXPOSURE)
        modes += [press(args.port, key) for key in (forward, back, forward, back)]
        cam.shot("/dev/video0", names[2], cam.EXPOSURE)
        before, after, clean = (cam.load_rectified(name, camera) for name in names)
        old_bar, new_bar = bar_rows(before, white, black), bar_rows(after, white, black)
        if modes[0] != "FAST" or modes[-1] != "HALF" or not old_bar or not new_bar or old_bar == new_bar:
            print(f"pair {pair}: DISCARDED, modes {modes}, bars {old_bar} {new_bar}")
        else:
            low = min(old_bar[0], new_bar[0])
            reference = (low + REFERENCE_OFFSET[0], low + REFERENCE_OFFSET[1])
            fast.append(ghost_in(after, old_bar, reference, white, black))
            half.append(ghost_in(clean, old_bar, reference, white, black))
            print(f"pair {pair}: bar left rows {old_bar}: after FAST {fast[-1]:+.2f}, after the next HALF {half[-1]:+.2f}")
        forward, back = back, forward  # the HALF landed on `back`, so the next FAST goes the other way
    if fast:
        print(f"{args.tag}: after FAST {statistics.mean(fast):+.2f}, after HALF {statistics.mean(half):+.2f}, {len(fast)} pairs")


if __name__ == "__main__":
    main()
