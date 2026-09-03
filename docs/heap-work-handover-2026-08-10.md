# Heap / fragmentation work — handover, 2026-08-10

*Last reviewed 2026-08-11, against `master` at `1d8eaf87` (PR #126 merged).*

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

**PR #125 (merged)** — footnote gather can no longer write a "no footnotes" cache it never earned.

Net effect on a fresh alice open, measured after #124/#125 and before #126: the spine-2 `abort()` is
gone, 2822 ms is gone from every book open, 189 words of silently-dropped table text are back, and
contig holds 40948 → 32756 across two spine builds instead of ratcheting to 15860.

**PR #126 (merged)** — the streaming table renderer, which closed the `MAX_CELL_LINES` item that
used to head the list below. Rows now lay out at `</tr>` instead of the whole table buffering to
`</table>`, so the live set is one row of `ParsedText` plus one fragment of `TextBlock`s, and a
fragment is bounded by the viewport by construction. The per-table 12 KB byte budget became a
per-**row** budget and the 96-word cell bound was deleted, since a per-row budget is charged per
word and can act with a cell open (`degradeRowAtOpenCell`). `SECTION_FILE_VERSION` 65 → 66.

It closed the truncation twice, which is the part worth remembering. `15be06af` replaced the silent
truncation with a fallback to `emitTableAsParagraphs` and passed 418/418 — because no fixture
reached it. The fallback rested on `layoutAndExtractLines` preserving its source, and
[ParsedText.cpp:430](../lib/Epub/Epub/ParsedText.cpp#L430) erases every word it lays out, so the call
that *detected* the overflow was the call that erased the evidence: the fallback emitted not the tail
of the cell but the whole cell, plus every cell laid out before it. Strictly worse than the
truncation. `c44f6f2d` fixed it with an opt-in `preserveSource`. The bail is now row-scoped, so one
oversized cell in row 40 no longer flattens all 40 rows.

Host-measured on the new `test_table_grid_edges.epub` fixtures: a cell over the 64-line cap keeps its
70 words and the other row stays a grid (was: no grid, 70 words deleted); a row over the viewport
keeps its 40 words the same way; a 60-row table renders as a grid across 3 fragments on 4 pages (was:
5 pages of flattened paragraphs). Word stream identical and in the same order across every table
book. **Not yet measured on device** — see worth-doing #3.

---

## Worth doing, in order

Ranked by value ÷ risk. The first is a correctness bug that does not depend on heap state, so it
reproduces on the host test suite.

### 1. Nested-table character data is dropped

[ChapterHtmlSlimParser.cpp:2110](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L2110) —
`characterData` early-returns when `currentTable->depth > 1`, so every word inside a nested table is
discarded. Pre-existing, known, parked earlier by the user. Violates "never drop content",
independent of heap.

A nested table already calls `degradeTable("nested table")`, so the inner text has a natural
destination — the outer table's paragraph stream. But every `depth > 1` element handler needs
auditing, not just this one `return`. This was scoped as C5 of the streaming table work and
deliberately kept out of it; it is still its own commit.

### 2. Close the footnote gather gap

PR #125 makes the build-path trigger decline while the framebuffer is lent — which is correct, but
the trigger is *only* polled from inside a build slice, and a build always holds the borrow. So it
never fires. Correctness holds via the render-time backstop, but the giant-spine case that motivated
the build-path trigger is unserved.

**Fix:** poll `Section::sawFootnote()` after a build completes and the buffer is returned. The flag
is already latched into `Section::sawFootnote_` before `buildState_` teardown for exactly this
([Section.cpp:1477](../lib/Epub/Epub/Section.cpp#L1477)). A few lines in the background tick.

Still true as of `1d8eaf87`: the only two call sites
([EpubReaderActivity.cpp:1222](../src/activities/reader/EpubReaderActivity.cpp#L1222) and
[:1365](../src/activities/reader/EpubReaderActivity.cpp#L1365)) both sit immediately after a build
slice, and the borrow check at
[:2528](../src/activities/reader/EpubReaderActivity.cpp#L2528) declines there every time.

### 3. Device-measure the streaming table renderer

PR #126 is entirely host-verified. The claim to check on the X3 with `alice-illustrated.epub` is that
contig across the two spine builds is **no worse than 40948 → 32756**. The mechanism predicts better:
the peak used to be the whole table's `ParsedText` held simultaneously with every row's laid-out
`TextBlock` lines, and both terms are now bounded by the display rather than the document.

Watch for the second-order effect too — tables now span pages as grids instead of being flattened at
48 rows, so a long table allocates fragments across several pages where it previously allocated none.

### 4. Anchors have no dump coverage

`runAndDump` emits no anchor records, and `Section` only exposes `getPageForAnchor(id)` — a lookup,
not an enumeration. So an anchor regression breaks TOC navigation silently, with a green suite.

This is the same blind-spot class as the table-content one that `0d093a72` fixed, and it is not
hypothetical: `flushTableFragment()` can call `emitPage` mid-table, so `completedPageCount` advances
*during* the table parse. Inside a grid table there is no text block to flush `pendingAnchorId`
([:906](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L906)), so an `id` on a `<tr>` stays
pending until the first paragraph after `</table>` — before and after #126 alike. The mechanism is
unchanged and the page number moves only because the table is now denser. **That is reasoned, not
measured.** `test_table_grid_edges.epub` carries `id="row-thirty"` and `id="row-fiftyfive"` for
whoever checks it.

Adding `ANCHOR id= page=` to the dump touches every golden in the corpus, so it wants its own commit,
the way `0d093a72` was its own commit.

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

Both still present as of `1d8eaf87`.

- `"Build for spine %d hit a footnote"`
  ([EpubReaderActivity.cpp:2535](../src/activities/reader/EpubReaderActivity.cpp#L2535)) prints
  `currentSpineIndex`, not the spine being built. During a Background-B build those differ.
- `[COF] Couldn't open temp items file … probably going to be a fatal error` fires three times on
  every normal first open (the book's cache dir does not exist yet). It is not fatal. Logging it as
  `ERR` on a healthy path buries real errors.

---

## Known and unsolved

**The per-group font inflate temp is the largest measured fragmentation source, and no viable fix
has been found.** It is malloc'd and freed per group, per prewarm call, per page, at a different
size each time. It cannot use an arena (the render path has none — eliminated #13), a permanent
reservation makes things worse (#8), and shrinking the groups costs flash we do not have (#15).

Worse than previously recorded: the transient **scales with reading font size**, which earlier
measurements missed because they sampled one setting.

| font | largest group |
|---|---|
| `bookerly_10` | 13809 |
| `bookerly_12` | 19377 |
| `bookerly_14` | 25609 |
| `bookerly_16` | 31875 |

The device measurement this work was based on showed `peakTemp=10555`. A reader on a 16 pt font sees
a **~32 KB** transient instead. If this is revisited, the angle left unexplored is reducing the
*number* of allocations rather than their size — e.g. reusing one buffer across the groups of a
single prewarm call, which is a runtime change with no flash cost and no permanent reservation.

---

### Cross-reference: the AA / pre-render ordering has a heap dimension (2026-08-17)

Raised by S3 bring-up, but the constraint is a C3 one, so it is flagged here.

Background-A re-arms the next-page pre-render only *after* the deferred AA frees
its planes, because the two compete for heap. On a panel with a slow AA pass that
ordering costs every quick page turn (measurements in
[background-rendering.md](background-rendering.md) → "A — next-page pre-render"),
and the obvious fix is to pre-render first and run the AA after.

**That reordering is a heap question before it is a scheduling one.** It would
have a pre-rendered page resident while the AA then allocates its planes — the
exact pairing the current order avoids. On the C3 the pre-render gate is
`PRE_RENDER_MIN_FREE_HEAP_BYTES` = 56 KB of ~380 KB, so whoever picks this up
should measure the combined high-water on an X3 with a large section before
changing the order. Not attempted.

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
| 15 | ~~**Shrink the font group size in `fontconvert.py`**~~ **— CORRECTED 2026-08-18, this row was wrong** | The original entry read: *"Flash. DEFLATE ratio is strongly size-dependent — <2K groups compress at 0.505, >16K at 0.260. Capping at 8192 costs +478957 bytes (+37.7%) of font data against 228685 bytes of flash headroom. A 16 KB cap is the only one that builds (+175951) and changes nothing, since the transient is already 8–10.5 KB."* The +37.7% came from applying a size→ratio *model* to hypothetical chunks, not from compressing them. Re-measured by actually re-deflating every chunk with zopfli (pipeline validated by reproducing the shipped headers byte-for-byte), an 8192 cap costs **+108706 bytes (+8.6%)**, and the real converter at cap 8192 confirms it. The claim that the transient was "already 8–10.5 KB" was also wrong — that was the prewarm page buffer, not the group inflate, which was 33–49 KB and is what failed on device (`cannot allocate 39746 bytes for group 5`, contig 28660). Shipped at cap 8192; see `fontconvert.py` for the full cost curve |
| 16 | Delete the table byte budget once rows stream | True at *table* scope, false at *row* scope: one row of 8 cells each holding the 9200-byte cell that caused the original X3 `abort()` is ~72 KB, and streaming bounds none of it. The 12 KB number was retargeted to the row, not dropped |

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

**A guard with no fixture is a guess.** `15be06af` shipped a fallback that was strictly worse than
the bug it replaced, with a green 418/418 behind it, because nothing reached the guard — its own
commit message says it was "exercised only by inspection". Write the fixture that enters the branch
before trusting the branch.

**A green suite only covers what `dumpPage` prints.** Table cells were invisible to every golden
until `0d093a72`, which is why the truncation survived as long as it did. Before verifying a change
in some subsystem, check that the dump can *see* that subsystem — anchors still cannot
(worth-doing #4).

---

## Reproducing the measurements

- Host suite: `cd test/build && cmake --build . -j 8 && ctest -j 8` → 424/424.
- Pipeline dump: `epub_pipeline_dump_noheap.exe <book.epub> <cachedir>` (use the `_noheap` variant on
  Windows; the malloc-interposing one deadlocks under MinGW).
- **Content changes: compare word streams, not goldens.** Extract with
  `re.findall(r'\bW x=\S+ s=\S+ z=\S+ t=([^\r\n]*)', dump)` and compare the *sequence* pre/post for
  every corpus book × both `fontSizeNormalization` variants. Redirect stderr separately — an
  interleaved `BENCHMARK` line split a `W` record and faked a missing word once. Since `0d093a72`
  this covers table cells too; grid and paragraph layout both emit cells in row-major order with
  hyphenation off, so the stream is invariant even where the rendering mode changes.
- Device: `pio run -e default`, then read `[HEAP]`, `[FBUF]`, and `Reader mem[...]` lines.
- The X3's real serial port is **COM6** — COM3 is a fake AMT SOL device; native USB re-enumerates on
  reset.
