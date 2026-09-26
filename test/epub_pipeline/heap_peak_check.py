#!/usr/bin/env python3
"""Heap-peak regression gate for the EPUB pipeline (memory audit 2026-09, R5).

Runs epub_pipeline_dump --bench on each fixture in two modes -- heap-only (a released build,
everything on the heap) and --arena=52272 (a borrowed build: the X3 framebuffer lent as the
build arena) -- and compares the heap-side peak against the committed baseline. The arena
mode's figure has the arena's own allocation subtracted, so both columns are "what the build
put on the heap". Fails when a fixture rises by more than MARGIN bytes: the regression test the
pending-image queue never had.

Re-baseline deliberately, not to make the test pass: UPDATE_HEAP_BASELINE=1 rewrites the file.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

MARGIN = 8 * 1024
ARENA = 52272


def run(dump, epub, cache, arena):
    args = [dump, epub, cache, "--bench"]
    if arena:
        args.append(f"--arena={ARENA}")
    env = dict(os.environ, WH_HOST_STDIO_UNBUFFERED="1")
    res = subprocess.run(args, capture_output=True, text=True, env=env, errors="replace")
    m = re.search(r"BENCHMARK pipeline_(ok|FAILED) .*?heap_peak=(\d+)B", res.stderr)
    if not m or m.group(1) != "ok":
        sys.stderr.write(res.stderr[-4000:])
        raise SystemExit(f"dump failed for {epub} (arena={arena})")
    peak = int(m.group(2))
    return peak - ARENA if arena else peak


def main():
    dump, corpus, baseline_path = sys.argv[1], sys.argv[2], sys.argv[3]
    baseline = {}
    with open(baseline_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, mode, value = line.split()
            baseline[(name, mode)] = int(value)
    measured = {}
    tmp = tempfile.mkdtemp(prefix="heap_peak_")
    try:
        for (name, mode) in sorted(baseline):
            epub = os.path.join(corpus, name)
            cache = os.path.join(tmp, f"{name.replace('/', '_')}.{mode}")
            shutil.rmtree(cache, ignore_errors=True)
            measured[(name, mode)] = run(dump, epub, cache, mode == "arena")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if os.environ.get("UPDATE_HEAP_BASELINE"):
        with open(baseline_path, "w", encoding="utf-8") as f:
            f.write("# fixture mode heap-side-peak-bytes (see heap_peak_check.py; regenerate with UPDATE_HEAP_BASELINE=1)\n")
            for (name, mode) in sorted(measured):
                f.write(f"{name} {mode} {measured[(name, mode)]}\n")
        print(f"baseline rewritten: {baseline_path}")
        return 0
    failed = False
    for key in sorted(measured):
        base, now = baseline[key], measured[key]
        delta = now - base
        flag = "REGRESSION" if delta > MARGIN else ("lower" if delta < -MARGIN else "ok")
        print(f"{key[0]:<28} {key[1]:<9} baseline={base:>7} now={now:>7} delta={delta:+7}  {flag}")
        if delta > MARGIN:
            failed = True
    if failed:
        print(f"heap-side peak rose by more than {MARGIN} B on at least one fixture; "
              "find the allocation (epub_pipeline_dump --bench prints the sites) or re-baseline on purpose "
              "with UPDATE_HEAP_BASELINE=1")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
