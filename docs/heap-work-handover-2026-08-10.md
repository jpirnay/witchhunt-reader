# Heap / fragmentation work — handover, 2026-08-10

Device: ESP32-C3 (X3), ~380 KB heap, **no compaction**, `-fno-exceptions` (so `bad_alloc` = `abort()`).
Books used throughout: `alice-illustrated.epub` (15 small spines, images, tables) and
`small-gods.epub` (2 spines, one of which is the entire novel — 583991 bytes).

Everything below is device-measured unless it says otherwise. The eliminated list is the more
valuable half of this document: each entry cost real time to disprove.

---

## Landed

**PR #124 (merged)** — CSS arena gate exemption, table-cell size bound, zip ring sizing,
CSS parse churn −61%, heap block-count probe, font page-slot LRU, mid-build arena adoption,
and a `RenderLock` self-deadlock fix caught by cppcheck.

**PR #125 (open)** — footnote gather can no longer write a "no footnotes" cache it never earned.

Net effect on a fresh alice open: the spine-2 `abort()` is gone, 2822 ms is gone from every book
open, 189 words of silently-dropped table text are back, and contig holds 40948 → 32756 across two
spine builds instead of ratcheting to 15860.

---

## Worth doing, in order

Ranked by value ÷ risk. The first two are correctness bugs and do not depend on heap state, so they
reproduce on the host test suite.

### 1. `MAX_CELL_LINES` silently truncates table cell text

`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:3267`

```cpp
if (cell.lines.size() < MAX_CELL_LINES) {   // 64, from Page.h
  cell.lines.push_back(tb);
}
```

Every line past 64 is discarded — no log, no fallback, no degraded-mode marker. This is how
alice lost 189 words before the cell-word bound routed oversized cells away from the grid path.
It still fires for any cell under 96 words that lays out to more than 64 lines in a narrow column.

The bound exists because `Page.h` stores the count in a `uint8_t`. The fix is not to raise it but to
fall back to the paragraph path when a cell would overflow — the machinery already exists
(`emitTableAsParagraphs`). Note the over-tall-row fallback rebuilds from `cell.lines`, which are
already truncated, so it cannot be the recovery path.

**Verify with:** word-stream comparison, not golden diff. Extract with
`re.findall(r'\bW x=\S+ s=\S+ z=\S+ t=([^\r\n]*)', dump)` and compare counts. Redirect stderr
separately — an interleaved `BENCHMARK` line split a `W` record and faked a missing word once.

### 2. Nested-table character data is dropped

`ChapterHtmlSlimParser::characterData` early-returns when `currentTable->depth > 1`, so every word
inside a nested table is discarded. Pre-existing, known, parked earlier by the user. Same class as
(1): violates "never drop content", independent of heap.

### 3. Close the footnote gather gap

PR #125 makes the build-path trigger decline while the framebuffer is lent — which is correct, but
the trigger is *only* polled from inside a build slice, and a build always holds the borrow. So it
never fires. Correctness holds via the render-time backstop, but the giant-spine case that motivated
the build-path trigger is unserved.

**Fix:** poll `Section::sawFootnote()` after a build completes and the buffer is returned. The flag
is already latched into `Section::sawFootnote_` before `buildState_` teardown for exactly this.
A few lines in the background tick.

### 4. Shrink the font group size in `fontconvert.py`

The largest *measured* fragmentation source is the per-group inflate temp in `prewarmCache` —
3–10.5 KB, several per page, malloc'd and freed in a tight loop, and different sizes each time.
It cannot use an arena because the render path has none (see eliminated #13).

Groups are currently capped at `GROUP_MAX_UNCOMPRESSED_BYTES = 65536`, whose comment calls 64 KB
"a comfortable transient malloc on the ESP32-C3" — untrue once contig sits near 34 KB. Making groups
small and *uniform* (say ≤ 4 KB) turns variable-size churn into a repeatable hole the allocator can
actually reuse. Offline change, zero runtime memory, measurable on the host.

Cost to weigh: more groups means more inflate calls per page. Measure decode time before committing.

### 5. Re-test the warm-pass borrow, one narrow question

Reverted, but on confounded evidence — see eliminated #14. The single question to answer:

> Does the JPEG cache writer's `free heap 44108 < 53248` requirement survive borrowing?

If not, **that requirement is the thing to fix**, not the borrow. The decoders themselves are already
arena-capable (`PngStreamDecoder::setScratchArena`, and the JPEG converter's 12 KB block via
`image_scratch`), and `Section::warmAllImageCaches` already borrows for the whole-section pass. Only
the reader's per-page path is unwired.

### 6. Build-scoped arena candidates — measure before writing

Two survive scrutiny; one did not (eliminated #12).

| candidate | why plausible |
|---|---|
| parser `cssStyleCache_` / `inlineStyleCache_` | `unordered_map` nodes, one per distinct (tag\|class), strictly build-scoped |
| `ParsedText::words` | `vector<string>`, measured 9200 B for one cell |

Both need a custom allocator threaded through container *and* strings — materially more work than
"put it in the arena". And 92.3% of words fit SSO, so the win is smaller than the element count
suggests. **Instrument first** with the `EpubCssPerformanceTest` allocation-tracking harness.

### 7. Contig decay — still unexplained

Much milder now (40948 → 32756 over two builds, vs → 15860 before), but not understood. The
`allocBlk` / `freeBlk` / `allocBytes` probe added in PR #124 is the tool: block counts oscillating
while contig ratchets down means fragmentation, not retention.

### 8. Cosmetic, but misleading in exactly the wrong moment

- `"Build for spine %d hit a footnote"` prints `currentSpineIndex`, not the spine being built.
  During a Background-B build those differ.
- `[COF] Couldn't open temp items file … probably going to be a fatal error` fires three times on
  every normal first open (the book's cache dir does not exist yet). It is not fatal. Logging it as
  `ERR` on a healthy path buries real errors.

---

## Eliminated — do not re-attempt

Each was investigated and disproved. Re-proposing one costs the same time again.

| # | Hypothesis | What killed it |
|---|---|---|
| 1 | Tone mapping leaks memory | Free heap flat across the pass |
| 2 | The warm pass fragments the heap | contig 30708 identical before and after |
| 3 | Per-line `shared_ptr` churn fragments the parse | Parse does not progressively fragment |
| 4 | Arena-back the `ParsedText` DP scratch | Bounded at ~97 words; nothing to win |
| 5 | Per-word `std::string` churn is the cost | 92.3% fit SSO; never reach the heap |
| 6 | Reorder warm pass vs realloc | Reverted on device evidence — moved the failure earlier |
| 7 | Re-derive the Phase-1 heap gates for X3 | No gate fires on X3; the `Epub` object pinning the framebuffer was the real lead |
| 8 | **A fixed 16 KB font scratch removes the churn** | **Halved contig, 34804 → 13300.** Allocated lazily mid-session, it pinned the largest free region. Total heap also fell 3712 B from the `.bss` growth of 8 fallback slots |
| 9 | Prune footnote Pass B by fragment existence | **Measured 0 pruned.** alice's 24 "markers" are page-number links (`#Page_13`, text `13`) whose targets *do* exist |
| 10 | Tighten `isMarkerText` to cut false positives | `test_inline_footnotes.epub` deliberately exercises a bare `<a href="notes.xhtml#n3">[3]</a>` with no `epub:type` |
| 11 | Piggyback footnote Pass A on the section build | No whole-book parser pass exists — the `spineCount` loops are metadata only. The build *consumes* previews, so it is circular |
| 12 | Move the section LUT into the arena | `Section.cpp:1289` moves it into the Section; it outlives the arena |
| 13 | Give the render pass its own arena | Font path needs ~21 KB; the 52272 B framebuffer realloc must keep succeeding, and there is nothing to lend during prewarm (the buffer is being rendered into) |
| 14 | Borrow the framebuffer for the image warm pass | Reverted — but **on confounded evidence**, see worth-doing #5. Not settled |

---

## Principles this session actually earned

**Judge a permanent allocation by placement and timing, not size stability.** "Allocated once" is not
"harmless". #8 checked that the size was constant and never checked where the address would land.

**`.bss` growth costs the heap ceiling, not just heap.** Changing a static array size moves `Total:`
in the `[MEM]` line. That is the cheapest possible regression signal — check it.

**A cache that records a negative result must prove it looked.** PR #125 exists because a gather that
opened zero spines wrote "this book has no footnotes" as a permanent, authoritative answer.

**Verify the premise with a measurement before building on it.** Three changes in this session were
made confidently and each needed reverting or repair (#8, #9, #14). The two that survived —
the CSS arena gate and the table cell bound — were both preceded by a measurement that disproved the
first theory.

---

## Reproducing the measurements

- Host suite: `cd test/build && cmake --build . -j 8 && ctest -j 8` → 418/418.
- Pipeline dump: `epub_pipeline_dump_noheap.exe <book.epub> <cachedir>` (use the `_noheap` variant on
  Windows; the malloc-interposing one deadlocks under MinGW).
- Device: `pio run -e default`, then read `[HEAP]`, `[FBUF]`, and `Reader mem[...]` lines.
- The X3's real serial port is **COM6** — COM3 is a fake AMT SOL device; native USB re-enumerates on
  reset.
