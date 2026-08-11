# Streaming table renderer — implementation plan

> **Status: C1–C4 landed** on `refactor/streaming-table-renderer` (`0d093a72`, `f8c29d79`,
> `c44f6f2d`, `54d32f1a`, `48ffd6ea`). Host suite 424/424, word-stream equality verified across
> every table book. Outstanding: the device measurement, and C5 (nested tables), which was always
> separable. See "What actually happened" at the end.

Derived 2026-08-10 from the `streaming-table-renderer-design` note, then re-grounded against the
current source. Two of the note's premises have moved since it was written; those corrections are
first, because they change what the work is.

---

## Corrections to the design note

**1. The `MAX_CELL_LINES` truncation was not fixed — it was made worse, and is now fixed for
real.** *(Revised after the fixtures in `f8c29d79` were built; the original reading of `15be06af`
is below, and it was wrong.)*

`15be06af` replaced the truncation with a fallback to `emitTableAsParagraphs`, which looked right
and passed 418/418 — because no fixture reached it. Its own commit message says the guard was
"exercised only by inspection". The fallback rested on `layoutAndExtractLines` preserving its
source, which that commit also documented at the declaration. **It does not.**
[ParsedText.cpp:430](../lib/Epub/Epub/ParsedText.cpp#L430) erases every word it lays out.

So the call that *detected* the overflow was the call that erased the evidence, and the fallback
emitted nothing at all: not the tail of the cell, the whole cell — plus every cell in the table laid
out before it. Strictly worse than the truncation it replaced. Fixed in `c44f6f2d` with an opt-in
`preserveSource` that snapshots and restores the word vectors, verified by the new ch3/ch4 fixtures
recovering all 70 and all 40 words unsplit.

What remains for this rewrite is the **quality** half: the bail is *table-scoped*. One oversized
cell in row 40 flattens all 40 rows. Making it row-scoped is the main user-visible payoff.

The general lesson, and the reason step one below is what it is: a guard with no fixture is a guess.
Both the truncation and its broken replacement survived because the goldens could not see inside a
table.

**2. The pipeline dump cannot see table content.**
[PipelineRunner.cpp:55-59](../test/epub_pipeline/PipelineRunner.cpp#L55-L59) emits one line per
table fragment:

```cpp
out << "  TABLE y=" << tbl.yPos << " x=" << tbl.xPos << " h=" << tbl.getTotalHeight() << "\n";
```

No cells, no lines, no words. Every golden under `test/epub_pipeline/goldens/` is therefore blind to
grid-path cell text, and the word-stream verification the note prescribes
(`re.findall(r'\bW x=... t=([^\r\n]*)')`) silently skips everything inside a table. **This is why
the truncation survived undetected for as long as it did**, and it makes the note's proposed
verification method unsound until fixed. It has to be step one.

---

## Target design

Residency drops from *whole table* to *one row plus one fragment*. The crux from the note holds:
columns are already `viewportWidth / N`
([ChapterHtmlSlimParser.cpp:3208](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L3208)), so
streaming costs no layout quality. Option 3 (flush-and-restart) is adopted; no pre-scan, no
paragraph fallback on a column-count change.

### State

`BufferedTable` ([ChapterHtmlSlimParser.h:191-209](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h#L191-L209))
loses `rows`, `maxCols`, `streaming` and `bufferedBytes`, and gains:

| field | role |
|---|---|
| `pendingRow` (one `BufferedTableRow`) | cells of the row being filled; reused per `<tr>` |
| `pendingRowBytes` | per-**row** byte budget (replaces the per-table one) |
| `fragmentRows` / `fragmentHeight` / `fragmentCols` | the fragment being packed — today's `emitFragment` locals, promoted to state |
| `degraded` | table can never be a grid (nested table, rowspan, low heap) → paragraphs from here on |
| `rowDegraded` | *this row* already emitted a paragraph, so the rest of the row must follow it (document order) |

### Flow

| site | today | after |
|---|---|---|
| `<table>` [:1109](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L1109) | create buffer | unchanged |
| `<tr>` [:1139](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L1139) | push a row | clear `pendingRow`, clear `rowDegraded` — **the recovery point**: a row degraded for size or height does not poison the next row |
| `<td>` [:1158](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L1158) | push cell, run 3 gates | push cell into `pendingRow`; heap gate → `degradeTable`; rowspan → `degradeTable`; unsupported colspan → `degradeRow` |
| word [:522](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L522) | charge table budget, 96-word cell bound | charge `pendingRowBytes`; over budget → `degradeRowAtOpenCell()` |
| `</td>` [:2484](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L2484) | compute `isHeaderRow`, or stream | if degraded: `streamClosedCell` (unchanged); else nothing |
| `</tr>` [:2514](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L2514) | nothing | **`layoutPendingRow()`** — the new heart |
| `</table>` [:2518](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L2518) | `emitBufferedTable()` → grid or paragraphs | `flushFragment()` |

### `layoutPendingRow()`

Body lifted almost verbatim from the two loops inside `emitTableAsFragments`
([:3233-3409](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L3233-L3409)), one row at a time:

1. `isHeaderRow` = all cells are `<th>`; `renderCols` = 1 for a full-width single-cell span, else
   `pendingRow.effectiveCols`.
2. **Flush-and-restart:** `renderCols != fragmentCols && !fragmentRows.empty()` → `flushFragment()`.
   A 3-col table with a 2-col header becomes two aligned fragments instead of a flattened list.
3. Heap gate before layout (`ensureHeapForTextLayout`) — CrossInk's "cell layout failed" bail, now
   costing one row instead of the book.
4. Lay each cell out. Row-scoped bail (flush fragment → emit this row's cells as paragraphs →
   `emitCellImagesAsBlocks` → return) on any of: `> MAX_CELL_LINES` lines, row taller than the
   viewport, `innerColumnWidth < MIN_COL_INNER_WIDTH`, heap gate refused.
5. Flush if `fragmentRows.size() >= MAX_TABLE_ROWS` **or** the row would overflow the viewport.
6. Append the `TableRow`; **free `pendingRow`'s `ParsedText`** — the point of the exercise.

`emitFragment` ([:3342](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L3342)) becomes a member
`flushFragment()`, otherwise unchanged.

### Why residency improves — the mechanism

Today's peak is the whole table's `ParsedText` (4 parallel vectors + a `std::string` per word, per
cell) held *simultaneously* with `layoutRows`, which holds every row's `TextBlock` lines, because
`emitTableAsFragments` lays out all rows before emitting any fragment
([:3230](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L3230)). After the change the two live
sets are one row of `ParsedText` and one fragment of `TextBlock`s — and a fragment is bounded by
`viewportHeight` by construction (step 5), so both terms are bounded by the display rather than by
the document. No new allocation is introduced; the fragment vector already existed.

### The per-fragment row cap is load-bearing

`PageTableFragment::deserialize` rejects `rowCount > MAX_TABLE_ROWS`
([Page.cpp:230](../lib/Epub/Epub/Page.cpp#L230)). Today that invariant is enforced upstream by the
48-row *table* limit ([:1150](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L1150)), which this
rewrite removes — long tables become grids across many fragments instead of being flattened. Step 5
must therefore carry the cap explicitly. It is unreachable in practice (480 px ÷ a ~30 px minimum
row = 16 rows), but the invariant should be local to the code that has to hold it, not a consequence
of font metrics.

---

## Commit sequence

Each is independently reviewable and independently revertable; goldens are regenerated only where
the commit message explains the diff.

**C1 — `test: dump table cell contents in the pipeline dump`**
Extend `dumpPage`'s `TAG_PageTable` case to walk rows → cells → lines → words, reusing
`dumpTextLine`'s `W x= s= z= t=` record shape so the existing word-stream extraction picks table
words up. Regenerate all 30 goldens. **Production code untouched** — the diff is purely additive
lines in goldens, which is exactly what makes every later golden diff meaningful.

**C2 — `test: add table fixtures for the streaming rewrite`**
Five new fixtures (below). Goldens committed here record *current* behaviour, including the
table-wide fallbacks the rewrite is about to narrow. Deliberate: it makes C4's improvement visible
as a diff rather than as a claim. `EpubPipelineTest` auto-discovers `.epub` files
([EpubPipelineTest.cpp:110](../test/epub_pipeline/EpubPipelineTest.cpp#L110)), so no test wiring is
needed.

**C3 — `refactor: promote the table fragment packer to members`**
`emitFragment` → `flushFragment()`; extract the per-row body of the first loop into `layoutRow()`.
Pure motion. **Goldens must not change** — that is the commit's own test.

**C4 — `feat(table): lay out table rows as they close instead of buffering the table`**
The rewrite. Deletes `MAX_TABLE_BUFFER_BYTES`, `MAX_TABLE_CELL_WORDS`, `maxCols`, the `streaming`
flag's paragraph meaning, `beginTableStreamingAtOpenCell`; adds `pendingRowBytes`, the per-fragment
row cap, and row-scoped fallbacks. Bump `SECTION_FILE_VERSION`
([Section.cpp:34](../lib/Epub/Epub/Section.cpp#L34), currently 65) — the layout changes, so cached
sections must be rebuilt or a book will render tables two different ways depending on cache age.
(The earlier streaming commit `55883bd2` correctly did *not* bump it: it was byte-identical.)

**Landed out of sequence, because the C2 fixtures caught it:** `c44f6f2d`, the `preserveSource` fix
above. It had to go in before C4 — the row-scoped fallback C4 introduces sits on exactly the same
"lay it out, then decide" pattern and would have inherited the same content loss.

**C5 — `fix(table): don't drop character data inside nested tables`** *(optional, separable)*
[:2131](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L2131) early-returns for `depth > 1`, so
every word inside a nested table is discarded — handover item #2, "never drop content". With a
degraded outer table the inner text has a natural destination (the paragraph stream), but every
`depth > 1` element handler needs auditing, not just this return. **Size this separately; do not
fold it into C4.**

---

## Fixtures

None of these exist. `test_tables.epub` covers only 2×2 and 3×2 grids;
`test_table_streaming.epub` covers nesting, rowspan, column overflow and a grid-compatible colspan.

| fixture | exercises | note |
|---|---|---|
| column count changes mid-table | flush-and-restart (step 2) | the headline case |
| cell > 64 lines, under any word bound | row-scoped line bail | needs narrow columns **and** long words — a first attempt at this tripped the word bound instead, so confirm it reaches the grid path before trusting it |
| row taller than the viewport | step 4 height bail | |
| full-width single-cell row in a multi-column table | `renderCols == 1` fragment | |
| table with > 48 rows | previously flattened, now a grid across fragments | also guards the per-fragment cap |

## Verification

**Word-stream equality, not golden diff.** With C1 in place the goldens finally carry table words,
so extract with `re.findall(r'\bW x=\S+ s=\S+ z=\S+ t=([^\r\n]*)', dump)` and compare pre/post for
every corpus book × both `fontSizeNormalization` variants. **Redirect stderr separately** — an
interleaved `BENCHMARK` line once split a `W` record and faked a missing word.

The invariant is exact equality of the word *sequence*, and it holds even where the rendering mode
changes: paragraph flattening and grid layout both emit cells in row-major order, and cells are
constructed with hyphenation off, so no word is ever split or reordered by the mode switch. Two
deliberate exceptions, each of which must be asserted as a specific delta rather than waved through:

- fixtures whose content was previously dropped (nested tables, if C5 lands) — the stream *grows*
- nothing else

Also check: `TABLE h=` heights and `PAGE` counts will shift where a table now breaks across
fragments differently. Expected; explain per fixture in the commit message.

**Host:** `cd test/build && cmake --build . -j 8 && ctest -j 8` → currently 418/418, plus the new
fixtures × 2 variants.
**Device:** `alice-illustrated.epub` on the X3 (tables + images, the book that motivated all of
this). Watch `[MEM]`, `[HEAP]` and `Reader mem[...]`; the claim to check is that contig across the
two spine builds is no worse than the current 40948 → 32756. Real port is **COM6**.

---

## Deviations from the design note, with reasons

**The note says delete `MAX_TABLE_BUFFER_BYTES` and `MAX_TABLE_CELL_WORDS` outright. This plan
keeps the byte accounting, retargeted from the table to the row.** The note's premise — streaming
bounds residency — is true at *table* scope but not at *row* scope: one row of 8 cells each holding
the 189-word / 9200-byte cell that caused the original X3 `abort()`
([:108-119](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L108-L119)) is ~72 KB, and nothing in
the streaming design bounds it. Keeping the 12 KB number as a *per-row* budget costs one `size_t`
and fires far more rarely than today.

The 96-word cell bound *is* deleted, and this is why: it exists solely because the per-table budget
could only act at the next `<td>`, which a one-cell table never has. A per-row budget is charged
per word and can act with a cell open (`degradeRowAtOpenCell`), so it subsumes the cell bound
completely.

**Rowspan degrades the whole table, not the row.** A rowspan mis-renders every row it covers, and
tracking spans across rows is real work for a rare case. Content is preserved either way.

**The `MIN_FREE_HEAP_FOR_TABLE` gate stays table-scoped.** A heap dip at `<td>` will not have
recovered by the next row; degrading once is more honest than re-testing per row.

## What actually happened

Written after the fact. The plan held, with three deviations worth recording.

**The fixtures found a live bug before any production code was touched.** C2 was written as
scaffolding — "commit the current behaviour so the rewrite shows as a diff" — and the current
behaviour turned out to be total deletion of an oversized cell, and of every cell laid out before it
in the same table. That is what forced `c44f6f2d` in ahead of the rewrite. The order in this plan
(dump → fixtures → refactor → rewrite) was chosen to make the rewrite reviewable and it paid for
itself two commits early instead.

**Two behaviour changes were added during C4 that the plan did not anticipate**, both consequences
of tables now spanning pages as grids — a state that was previously unreachable:

- A row that cannot fit in what is left of the page breaks the page *before* opening its fragment.
  Without it the table arrived on the next page as a one-row box followed by the rest.
- The column count is tracked per *table*, not per fragment. `packer.cols` resets on every flush,
  so a ragged row landing just after a page break would have narrowed the table's second half.

Neither is visible in a single-page table, which is why no existing fixture would have caught them;
both were found by reading the fragment structure the new `ch5` golden produced.

**The row budget is kept, against the design note's advice.** Reasoning in "Deviations" above. The
note's claim that streaming makes the bounds unnecessary is true at table scope and false at row
scope. `ch6` exists because after deleting the 96-word cell bound, nothing exercised the mechanism
that replaced it.

### Measured

| | before | after |
|---|---|---|
| `ch3` cell over the 64-line cap | no grid at all, 70 words deleted | 70 words kept, other row still a grid |
| `ch4` row over the viewport | no grid at all, 40 words deleted | 40 words kept, other row still a grid |
| `ch5` 60-row table | 5 pages of flattened paragraphs | 4 pages, grid in 3 fragments |
| `ch1` 2-col header on a 3-col table | one padded 3-col fragment | 2-col + 3-col fragments |
| word stream, all table books | — | identical, same order |

## Still open

- **Device measurement.** Everything above is host-side. The claim to check on the X3 is that contig
  across alice's two spine builds is no worse than the current 40948 → 32756.
- **C5, nested tables.** Untouched, as planned.
- **Anchors inside table rows** — see below; reasoned, not measured.

## Open question, partially settled

`flushTableFragment()` can call `emitPage` mid-table, so `completedPageCount` advances *during* the
table parse rather than after it. Anchors are recorded against `completedPageCount` when
`pendingAnchorId` is flushed ([:906](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp#L906)), and
inside a grid table there is no text block to trigger that flush — so an `id` on a `<tr>` stays
pending until the first paragraph *after* `</table>`, both before and after this change. The
mechanism is unchanged; the resulting page number moves only because the table is now denser.
`lastBodyChildByteOffset` is unaffected (the `<table>` is the body child, so it does not move during
the table).

**This is reasoned, not measured.** `ch5` carries `id="row-thirty"` and `id="row-fiftyfive"` for
whoever wants to check it, but the pipeline dump has no anchor coverage at all — `runAndDump` emits
no anchor records and `Section` only exposes `getPageForAnchor(id)`, a lookup, not an enumeration.
Adding `ANCHOR id= page=` to the dump would touch every golden in the corpus, so it was left out of
this branch. It is the same blind spot class as the table-content one C1 fixed, and worth its own
commit: an anchor regression breaks TOC navigation silently.
