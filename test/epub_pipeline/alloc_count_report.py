#!/usr/bin/env python3
"""Allocation counts of a section build, before and after a change (CI job alloc-counts).

Runs epub_pipeline_dump --bench, built from the change and from its base, on a few books in two modes
-- heap-only (a released build, everything on the heap) and --arena=52272 (a borrowed build: the X3
framebuffer lent as the build arena) -- and reports the number of heap allocations and the heap peak
of each. Block count, not bytes, is what fragments the C3's heap during a build, so a change that
moves per-line or per-cell objects into the arena (memory-and-parsing triage, sections 9b and 9c) is
judged by these counts: docs/superpowers/specs/2026-11-18-memory-and-parsing-triage.md.

Reports, never fails on a count: a change may add allocations on purpose. It warns when a count rises by
more than WARN_RISE, and fails only when the change's own dump tool cannot build a book. The table goes
to stdout and, in CI, to the job summary ($GITHUB_STEP_SUMMARY).

Usage: alloc_count_report.py <base dump or -> <head dump> <test dir>
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ARENA = 52272
WARN_RISE = 0.05
BOOKS = [
    "fixtures/moby-dick.epub",
    "epubs/test_table_streaming.epub",
    "epubs/test_nested_tables.epub",
    "epubs/test_br_in_blocks.epub",
]
MODES = ["heap", "arena"]


def run(dump, epub, cache, mode):
    """Returns (allocs, heap_peak) of one cold build, or None when the build failed."""
    args = [dump, epub, cache, "--bench"]
    if mode == "arena":
        args.append(f"--arena={ARENA}")
    env = dict(os.environ, WH_HOST_STDIO_UNBUFFERED="1")
    shutil.rmtree(cache, ignore_errors=True)
    res = subprocess.run(args, capture_output=True, text=True, env=env, errors="replace")
    m = re.search(r"BENCHMARK pipeline_(ok|FAILED) .*?heap_peak=(\d+)B .*?allocs=(\d+)", res.stderr)
    if not m or m.group(1) != "ok":
        print(f"build failed: {dump} {epub} {mode} (exit {res.returncode})", file=sys.stderr)
        print(res.stderr[-2000:], file=sys.stderr)
        return None
    peak = int(m.group(2))
    # As heap_peak_check.py does: the lent arena is itself one heap block, so its size comes off the peak and
    # both modes report what the build put on the heap.
    return int(m.group(3)), peak - ARENA if mode == "arena" else peak


def fmt_delta(before, after):
    if before is None or after is None:
        return ""
    delta = after - before
    pct = f" ({delta / before:+.1%})" if before else ""
    return f"{delta:+,}{pct}"


def main():
    base_dump, head_dump, test_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    has_base = base_dump != "-"
    rows = []
    warnings = []
    head_failed = False
    tmp = tempfile.mkdtemp(prefix="alloc_counts_")
    try:
        for book in BOOKS:
            epub = os.path.join(test_dir, book)
            for mode in MODES:
                cache = os.path.join(tmp, "cache")
                before = run(base_dump, epub, cache, mode) if has_base else None
                after = run(head_dump, epub, cache, mode)
                if after is None:
                    head_failed = True
                rows.append((book, mode, before, after))
                if before and after and after[0] > before[0] * (1 + WARN_RISE):
                    warnings.append(f"{book} ({mode}): allocations {before[0]:,} -> {after[0]:,}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    lines = ["### Allocation counts of a section build (epub_pipeline_dump --bench)", ""]
    if has_base:
        lines += ["| Book | Mode | Allocations before | after | change | Heap peak before | after | change |",
                  "|---|---|---:|---:|---:|---:|---:|---:|"]
    else:
        lines += ["| Book | Mode | Allocations | Heap peak |", "|---|---|---:|---:|"]
    for book, mode, before, after in rows:
        a = f"{after[0]:,}" if after else "failed"
        p = f"{after[1]:,} B" if after else "failed"
        if has_base:
            b = f"{before[0]:,}" if before else "failed"
            bp = f"{before[1]:,} B" if before else "failed"
            lines.append(f"| {book} | {mode} | {b} | {a} | "
                         f"{fmt_delta(before and before[0], after and after[0])} | {bp} | {p} | "
                         f"{fmt_delta(before and before[1], after and after[1])} |")
        else:
            lines.append(f"| {book} | {mode} | {a} | {p} |")
    lines += ["", f"Mode `arena` lends a {ARENA:,} B build arena (the X3's framebuffer); `heap` lends none. The heap "
                  "peak is what the build put on the heap (the arena's own block subtracted)."]
    report = "\n".join(lines) + "\n"
    print(report)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(report)
    for w in warnings:
        print(f"::warning::allocation count rose more than {WARN_RISE:.0%}: {w}")
    return 1 if head_failed else 0


if __name__ == "__main__":
    sys.exit(main())
