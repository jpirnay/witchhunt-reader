#!/usr/bin/env python3
"""Reader-level one-pass check on the BOOX Picco: page-turn times, then what reading leaves on white.

  ./piccoread.py <tag> --book /path.epub --at 3:1 --pages 150

Opens the book at a fixed position with text anti-aliasing and one-pass text on, turns `--pages` pages
(BTN:RIGHT, then IDLE:SCREEN), and logs each turn from the key to the finished frame. Then leaves the reader,
runs two full white refreshes through CMD:LAB and takes `<tag>-white` (cam.py, LAB_BOARD=picco), then
`<tag>-white-10` ten minutes later. The device log goes to runs/<tag>.log.
"""

import argparse
import os
import re
import statistics
import sys
import time
from pathlib import Path

os.environ.setdefault("LAB_BOARD", "picco")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))  # tools/, for devctl
import cam  # noqa: E402
import devctl  # noqa: E402

RUNS = Path(os.path.expanduser("~/.local/state/murphy-gray-lab/runs"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tag")
    parser.add_argument("--book", required=True)
    parser.add_argument("--at", default="3:1", help="spine:page to open at")
    parser.add_argument("--pages", type=int, default=150)
    parser.add_argument("--one-pass", type=int, default=1, choices=(0, 1))
    parser.add_argument("--wait-minutes", type=int, default=10)
    args = parser.parse_args()
    RUNS.mkdir(parents=True, exist_ok=True)
    device = devctl.Device(os.environ["MURPHY_PORT"], log_path=str(RUNS / f"{args.tag}.log"))

    def cmd(text):
        # devctl knows no LAB_ reply prefix, so a lab command names its own terminator.
        until = re.compile(r"^LAB_(OK|ERROR)") if text.startswith("LAB:") else None
        reply = device.command(text, until=until)
        print(f"{time.strftime('%T')} {text[:60]} -> {reply}", flush=True)
        if reply is None or "_ERROR" in reply:
            raise SystemExit(f"STOP: {text}: {reply}")
        return reply

    for setting in ("sleepTimeoutMinutes=0", "textAntiAliasing=1", f"onePassTextAa={args.one_pass}"):
        cmd("SET:" + setting)
    cmd(f"ACT:READER:{args.book}@{args.at}")
    cmd("IDLE:30000:SCREEN")
    print(cmd("STATE"), flush=True)
    times = []
    for page in range(args.pages):
        started = time.monotonic()
        cmd("BTN:RIGHT")
        cmd("IDLE:30000:SCREEN")
        times.append(round((time.monotonic() - started) * 1000))
    state = cmd("STATE")
    print(f"turns {len(times)}: median {statistics.median(times)} ms, p90 "
          f"{sorted(times)[int(len(times) * 0.9)]} ms, min {min(times)}, max {max(times)}", flush=True)
    print("times", times, flush=True)
    heap = re.search(r"heap=(\d+) maxblock=(\d+)", state)
    print("heap", heap.groups() if heap else state, flush=True)
    cmd("ACT:HOME")
    cmd("IDLE:30000:SCREEN")
    for _ in range(2):
        cmd("LAB:DRAW:FULL:3:")
    cam.shot("/dev/video0", f"{args.tag}-white", cam.EXPOSURE)
    print("shot", f"{args.tag}-white", flush=True)
    if args.wait_minutes:
        time.sleep(args.wait_minutes * 60)
        cam.shot("/dev/video0", f"{args.tag}-white-{args.wait_minutes:02d}", cam.EXPOSURE)
        print("shot", f"{args.tag}-white-{args.wait_minutes:02d}", flush=True)


if __name__ == "__main__":
    main()
