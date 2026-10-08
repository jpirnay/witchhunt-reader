# Background work in the EPUB reader (A / B / C)

How the reader hides latency behind three cooperative background mechanisms. This is the
**current-state reference**. All code is in `src/activities/reader/EpubReaderActivity.cpp`.

For the memory model these mechanisms operate under — the borrow-vs-release distinction in
particular, which is what shapes B's and C's gates — see
[memory-allocation-strategy.md](memory-allocation-strategy.md). For what the borrowed buffer holds
and what lending it does to the display, see
[secondary-buffer-management.md](secondary-buffer-management.md).

## Task model

Two FreeRTOS tasks, serialised by one `RenderLock` (the `renderingMutex`):

- **Render task** (`ActivityManager::renderTaskLoop`) — the *only* task that draws. It is
  notification-driven: it takes the `RenderLock`, calls `Activity::render()`, releases the lock,
  and waits for the next `requestUpdate()`. `render()` dispatches to one pass per call via
  `classifyRenderPass()` (`FinishedBook`, `SectionBuilding`, `PreRender`, `BufferDisplay`,
  `BuildSection`, `Normal`).
- **Loop task** (the main Arduino `loopTask`) — input handling, plus idle-time background work
  via `serviceBackgroundWork()` when no input/page-turn is pending.

`renderContents()` releases the lock *before* the (blocking) e-ink waveform wait, so the loop
task gets the CPU during the ~0.5 s refresh — that window is when background work actually runs.

## The three mechanisms at a glance

| | What it hides | Runs on | Secondary buffer | When |
|---|---|---|---|---|
| **A** — next-page pre-render | per-page-turn render compute (~90 ms) | render task (`PreRender` pass) | resident | next page needs no image decode & heap ok |
| **B** — next-section pre-build | the "Indexing…" parse when you cross a chapter | loop task | **borrowed** as a build arena; B waits when there is none to lend | idle, lookahead window, reader settled |
| **C** — current-section build | the freeze when you *land* on an uncached section | loop task (build) + render task (draw only) | **borrowed**; released only when there is none to lend | on entry to an uncached section |

A and B are *look-ahead* for a section that's already on screen. C is for the section you just
navigated to and have nothing to read yet — so it has the highest priority.

## Priority — `serviceBackgroundWork()`

```
runDeferredGrayscalePass();                 // 1. AA of the page just shown (visible quality)
if (pendingGrayscale_.active) return;        //    AA still owed → nothing else runs
if (section && section->hasActiveBuild())    // 2. Background C: build the section you're waiting on
  { stepCurrentSectionBuild(); return; }
stepBackgroundSectionBuild();                // 3. Background A re-arm, the image lane, then Background B
```

Rationale: deferred AA finishes the current page's quality; **C** unblocks reading (you can't
read until it produces pages); **A**/**B** are speculative look-ahead that only matter once the
current page/section is settled.

---

## A — next-page pre-render

Renders the *next* logical page into the inactive framebuffer so a forward turn is a near-instant
`BufferDisplay` instead of a fresh render.

- **Scheduled** in `renderContents()` (`pendingPreRender = true` + `requestUpdate()`) and **re-armed**
  once per `(spine, page)` by `stepBackgroundSectionBuild()` after the deferred-AA frees its memory.
- **Runs** as the `PreRender` pass (`renderPreRenderPass`) on the render task.
- **Gate:** free heap ≥ `PRE_RENDER_MIN_FREE_HEAP_BYTES` (44 KB, derived from what the pass
  consumes; see the comment at its definition); an image page only once **every image replays from
  its pixel cache** (`.1bit.pxc`, plus `.bayer.pxc` when AA is on), which the image lane has usually
  written pages ahead. The pass never decodes — that is seconds, unabortable, under the render lock.
  The draw runs under `ImageBlock::PlaceholderOnlyScope`, so a cache that fails to replay marks the
  page incomplete (never shown) instead of falling into a decode. Not on a single-push panel
  (`supportsGrayFrame()`, T5S3), whose capture records no image greys. A refusal logs
  `PreRender skipped: ...` at DBG.
  No pre-render while B is building through page turns (see B).
- **Note on X3:** a page turn is *waveform-bound* (~0.5 s), so A only saves the ~90 ms of
  prewarm+BW compute. Its benefit is modest on X3; the panel, not the CPU, sets page-turn speed.
  At the time of writing the X3 reads below the floor and A does not run there (open, see
  [future_work/display-and-refresh.md](future_work/display-and-refresh.md)).

### Ordering: the deferred AA pass runs before the pre-render

On the deferred-AA path, A is re-armed only *after* the AA pass has freed its memory, because the
planes and the pre-rendered page compete for the same heap. A quick turn inside the AA window
therefore pays a full render. The measured cost and the candidate reordering are in
[future_work/display-and-refresh.md](future_work/display-and-refresh.md) under "Pre-render before the
deferred AA pass".

## B — next-section pre-build (look-ahead)

Builds the **next consecutive** sections' caches during idle so crossing a chapter boundary is a
cache hit, not a blocking parse. `stepBackgroundSectionBuild()`, one bounded slice per idle tick.

- **Lookahead:** a *page budget*, not a spine count — `BG_BUILD_LOOKAHEAD_PAGES` (50) of runway
  ahead of the reading position, counting the current section's unread tail plus the sections
  built so far. Whole spines are built until the budget is covered, so many tiny front-matter
  spines get several built while one big chapter covers it alone. The cursor walks forward as each
  settles and re-anchors on any navigation.
- **State machine:** `Probe` (cache check; a cached spine settles at once) → `WaitHeap` (gates,
  re-checked at most once a second) → `Building` (`BG_BUILD_BUDGET_MS` = 40 ms slices) → `Settled`.
- **Builds only inside the borrowed secondary buffer.** `beginBackgroundBorrow()` lends the block as
  the build's arena: the parse working set, the inflate ring and the CSS index bump-allocate inside
  it. The block never enters the heap, so returning it cannot fail. There is no heap-backed B: with
  no buffer to lend (a C build holds it, or a release never came back) B waits, and C builds the
  section when the reader arrives.
  - **Gates:** the reader has been settled for `BG_BUILD_BORROW_QUIET_MS` (**1.5 s**), measured from
    the last page reaching the screen (`lastPageOnScreenMs_`), not the last turn; no button edge is
    queued (`CooperativeAbort::shouldAbortLongTask()`); free ≥ `BG_BUILD_BORROW_MIN_FREE_HEAP_BYTES`
    (14 KB, plus `BG_BUILD_RESOLVE_EXTRA_HEAP_BYTES`, currently 0, for a spine that still owes the
    footnote resolve); contig ≥ `BG_BUILD_BORROW_MIN_CONTIG_HEAP_BYTES` (12 KB). The floor only has to
    get a build started: once running, a build protects itself (the parser stops below 9 KB and
    leaves a truncated section, CSS lookups degrade, an image header that does not fit is refused),
    and B discards every such result. 1.5 s is sized against a cheap retry (setup + parse ~950 ms on
    an X3).
  - **Cost:** while B holds the block the reader has no AA (`secondaryBufferDegraded_`). B seeds
    nothing and opts into no single-buffer fast differential: every render takes the block back
    first (`recoverSecondaryBufferIfNeeded()`), so no refresh happens while B holds it.
  - **Build-through (X3 only):** on the last `BG_BUILD_THROUGH_PAGES` (3) pages of a chapter, B's
    build of the next chapter starts without the quiet period and keeps the block across page turns.
    Those pages draw in B/W with the normal refresh, without AA or a pre-render. If the reader gets
    to the next chapter first, C adopts the live build. X3 only, because its controller keeps the
    displayed frame as its own baseline; elsewhere the SSD1677's RED would first need seeding from a
    frame the lent buffer may no longer hold.
  - **Preemption:** otherwise a page turn ends the borrow, and `endBackgroundBorrow()` tears the live
    build down *before* handing the block back (the build allocates inside it). The parse is lost,
    but the inflated XHTML from phase (a) stays on the card (`Section::abortSectionBuild()` keys on
    `extractDone`), so the retry is the cheap one. After `BG_BUILD_MAX_PREEMPTIONS` (2) the spine is
    left to C; a next-chapter spine abandoned mid-chapter is retried on the chapter's last pages.
- **Lean CSS resolver:** every build uses the disk-backed resolver (`setLeanResolve`). The hot-rule
  LRU it gives up cannot hit during a build anyway: the parser memoises on `tag|class|id`, so the
  resolver only sees first occurrences.
- **Discards and pauses:** B aborts as soon as a slice starts skipping CSS lookups
  (`Section::activeBuildCssDegraded()`), and discards a finished build that came out truncated,
  CSS-degraded, with footnote previews unresolved, with an image header refused or with a table row
  demoted (`clearCache()`). After such a discard, or a failed build, it pauses until the reader
  enters the next chapter (`backgroundPausedForChapter_`); the foreground or C rebuilds that section
  with more headroom.
- **Adoption:** when you cross into a B section, `buildSection()` adopts `backgroundSection_` — a
  completed build is a cache hit; a still-partial build is finished by C.

## C — current-section build (build-while-you-read)

For the section you just entered that has **no cache** (first open, a jump / TOC / percent target,
a settings/orientation change that invalidated caches, or one B was heap-gated off). Closes the
gap B doesn't cover. The render task **only draws**; the build runs on the loop task.

- **Start:** `buildSection()` detects the cache miss, picks a mode (below), draws the "Indexing"
  popup, kicks off the build so `hasActiveBuild()` is true, and returns — handing off.
- **Build:** `stepCurrentSectionBuild()` on the loop task, 40 ms slices, highest reader-build
  priority. On a newly-written target page it `requestUpdate()`s so the render task draws it.
- **Draw:** the `SectionBuilding` pass (`renderSectionBuildingPass`) shows the requested page from
  the in-progress LUT (`Section::loadPageFromActiveBuild`, text-only) or the "Indexing" popup until
  it's built. Page reads flush the writer first so a second handle sees committed bytes.
- **Navigation during the build:** only for an explicit `Page` target — forward advances
  optimistically (shows the page once C reaches it, popup until then), back works, back past page 0
  leaves the chapter. Paragraph/anchor/percent/last-page targets resolve only at completion.
- **Finalize (`Done`):** the on-disk LUT is written, the nav target is resolved (a Page target's
  running position is kept; past-the-end crosses to the next spine), then `READING` resumes and the
  `Normal` pass renders the page with AA.
- **Failure (failed / truncated / css-degraded):** discard, latch `forceBlockingBuildSpine_`, and
  fall back to the **blocking** path (which builds with the buffer released for ~52 KB headroom).

### Build mode — `chooseSectionBuildMode()`

| Mode | When | Buffer |
|---|---|---|
| `IncrementalReleased` | **X3** (always), another board that misses the in-place floors, a spine whose resident build already aborted on low heap, or the buffer is already lent | **borrowed** as the build arena; *released* only when there is none to lend. Restored via `recoverSecondaryBufferIfNeeded()` |
| `IncrementalResident` | a non-X3 board that fits the in-place floors (`IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES` 60 KB / `IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES` 28 KB; higher CSS variants) | kept resident |
| `Blocking` | `forceBlockingBuildSpine_` latch, no secondary buffer, or a CSS-fallback rebuild | resident if the in-place floors fit (non-X3), else borrowed; released only for the C-failure latch (to gain the ~52 KB a failed borrowed build lacked) or when nothing can be lent; realloc'd at the end |

The mode name is historical: `IncrementalReleased` *borrows* first and only releases as a fallback,
because the lent block never enters the heap and returning it cannot fail.

Device rationale (see [secondary-buffer-management.md](secondary-buffer-management.md)):

- **X3** keeps the differential baseline in the controller's **DTM1**, so giving up the RAM buffer
  costs no display benefit (fast refresh still works), and building out of it keeps CSS parses
  above the resolve floor. So X3 **never builds resident**: it always takes the buffer, in practice
  by borrowing it. Mid-build draws are plain BW off the DTM1 baseline; AA returns once the buffer
  is back. Device-confirmed on X3, 2026-08-11: `Background-C: building spine 1 incrementally,
  secondary buffer BORROWED`, `CSS RESIDENT in arena`, `cap=52272 highWater=28656 failedAlloc=0`.
- **Other boards** decide by heap: builds that clear the in-place floors keep the buffer resident
  (AA stays live, and on the SSD1677 the fast-refresh baseline is still the host copy). The rest
  take it; C opts into single-buffer fast differential so mid-build draws stay FAST.

Both incremental variants set `secondaryBufferDegraded_`; `recoverSecondaryBufferIfNeeded()` (top
of every `render()`, guarded to skip while a build is active) returns or reallocates the buffer on
the first render after the build ends. `onExit()` restores it too if the reader is left mid-build.

---

## Diagnostics

- **Per-page** (`DBG`): `Page summary: … refresh=<fast|half|full> mode=0x.. renderMs=… …`. The
  refresh mode/byte are captured at the page's own `triggerDisplay` (before the deferred-AA display
  call would overwrite the renderer's live last-mode), so they reflect the page, not the AA pass.
- **Background work** (`INF`, every ~5 s under `DEBUG_BACKGROUND_WORK`): `BG work: A runs=…
  completes=… | B runs=… completes=… | C runs=… completes=… state=<probe|waitheap|building|settled>
  spine=… css=… borrow=… preempt=… | preReady=… buildPct=… free=… contig=…`. A `preempt=` count
  climbing toward `BG_BUILD_MAX_PREEMPTIONS` means B is losing races to page turns.
- **C lifecycle** (`INF`): `Background-C: building spine N incrementally, …` (buffer resident,
  already BORROWED, or the borrow/release outcome), `Background-C spine=N complete: M pages`, and
  `Background-C declined for spine N (…); blocking build` when it cannot start. A borrowed buffer is
  the healthy case; a release means there was none to lend.
- **B borrow** (`INF`): `Background-B: borrowed secondary buffer for spine N …` /
  `Background-B: returned secondary buffer (spine N, build discarded|no build live,
  preemptions=K; heap … at start, … lowest)`. Repeated `build discarded` at `preemptions=2` means B
  keeps losing the race to page turns on that spine and is handing it to C.
- **Heap gates** (`INF`, off by default — build `-DHEAP_GATE_TRACE=1`): one
  `gate=<name> PASS|REJECT free=…(floor=…) contig=…(floor=…)` line per decision. Several floors
  reject *silently* otherwise, so this is the only way to see which one decided a build's path.
- **Render-task stack** (`ERR`, always on): `Render task stack LOW: N bytes free` if the high-water
  margin drops below 1536 B — the render task runs the deepest chains (build parse + image decode +
  dither) and its stack abuts the heap, so an overflow corrupts the heap.

## On X3 specifically (the common device)

- The borrow is what carries look-ahead here: B builds only inside the lent buffer, so on the X3's
  reading heap every next chapter is either a B cache hit or a C build on arrival. Near the end of a
  chapter, build-through lets B finish the next chapter even while the reader turns pages quickly.
- X3 has no resident build mode at all (`chooseSectionBuildMode` returns `IncrementalReleased`
  unconditionally), so on this device C is *always* the borrow-or-release path and the in-place
  floors never apply.
- Page turns are fast (`refresh=fast`) except the scheduled anti-ghost **half** every
  `refreshFrequencyPages` (default 15) turns. On X3 a "fast" request is never silently turned into a
  half — it's honoured, or escalated to a *full* only when the driver's baseline isn't synced (which
  the clean build/restore paths avoid).
