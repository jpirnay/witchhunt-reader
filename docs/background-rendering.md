# Background work in the EPUB reader (A / B / C)

How the reader hides latency behind three cooperative background mechanisms. This is the
**current-state reference**. All code is in `src/activities/reader/EpubReaderActivity.cpp`.

For the memory model these mechanisms operate under — the borrow-vs-release distinction in
particular, which is what shapes B's and C's gates — see
[memory-allocation-strategy.md](memory-allocation-strategy.md).

(The historical design notes `epubreader-control-flow-refactor.md` and `background-b-handoff.md`
were deleted as outdated in `3d79a05b` / `01cb8a1e`; a few source comments still cite them.)

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
| **A** — next-page pre-render | per-page-turn render compute (~90 ms) | render task (`PreRender` pass) | resident | next page is text-only & heap ok |
| **B** — next-section pre-build | the "Indexing…" parse when you cross a chapter | loop task | **borrowed** as a build arena (heap-backed fallback if not lendable) | idle, lookahead window, reader settled |
| **C** — current-section build | the freeze when you *land* on an uncached section | loop task (build) + render task (draw only) | **borrowed**; released only when there is none to lend | on entry to an uncached section |

A and B are *look-ahead* for a section that's already on screen. C is for the section you just
navigated to and have nothing to read yet — so it has the highest priority.

## Priority — `serviceBackgroundWork()`

```
runDeferredGrayscalePass();                 // 1. AA of the page just shown (visible quality)
if (pendingGrayscale_.active) return;        //    AA still owed → nothing else runs
if (section && section->hasActiveBuild())    // 2. Background C: build the section you're waiting on
  { stepCurrentSectionBuild(); return; }
stepBackgroundSectionBuild();                // 3. Background A re-arm, then Background B
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
- **Gate:** free heap ≥ `PRE_RENDER_MIN_FREE_HEAP_BYTES` (56 KB); **text-only** pages only (image
  pages are excluded — their decode is too heap-hungry and deep).
- **Note on X3:** a page turn is *waveform-bound* (~0.5 s), so A only saves the ~90 ms of
  prewarm+BW compute. Its benefit is modest on X3; the panel, not the CPU, sets page-turn speed.

### Ordering: deferred AA runs BEFORE the pre-render (open, 2026-08-17)

Re-arming A only *after* the deferred AA has freed its memory is deliberate — the AA planes and
the pre-rendered page compete for the same heap. But on a panel where the AA pass is slow, that
ordering makes A miss exactly when it is most wanted.

Measured on the T5S3 (540x960, deferred AA), per page:

```
display → Deferred AA (planes 80 + gray 473 + restore 52 ≈ 605 ms) → pre-render (≈53 ms)
```

So for roughly **600 ms after every page the reader has nothing pre-rendered**. A turn inside that
window logs `pendingPreRender=1 hit=0` and pays a full render (~670 ms) instead of a buffer swap
(~580 ms). From a device log, tapping at a normal reading-fast cadence:

| turn | idleSlackMs | result |
|---|---|---|
| 1 | 251 | `hit=1` |
| 2-6 | < 605 | `hit=0`, `pendingPreRender=1` each time |

Window ended `2/6`. An earlier session on the same build reached `7/7` purely because the taps were
slower — the hit rate is a function of tap cadence against the AA duration, not of the code changing.

**Candidate fix: run the pre-render first, then the deferred AA.** The pre-render is
latency-critical and cheap (~53 ms); the grayscale pass is a nicety that is already deferred and
would only start ~53 ms later. On X3/X4 this changes little (AA there is inline or fast, and page
turns are waveform-bound anyway); on the S3 panels it is the difference between hitting and missing
every quick turn.

**Not done.** It reorders reader work on every board, and the heap argument above is real — the
pre-render would hold its page while the AA then allocates its planes, which is the pairing the
current order exists to avoid. It needs a heap check on the C3 (where `PRE_RENDER_MIN_FREE_HEAP_BYTES`
is 56 KB of ~380 KB) and a device test, not an inference from one log.

## B — next-section pre-build (look-ahead)

Builds the **next consecutive** sections' caches during idle so crossing a chapter boundary is a
cache hit, not a blocking parse. `stepBackgroundSectionBuild()`, one bounded slice per idle tick.

- **Lookahead:** a *page budget*, not a spine count — `BG_BUILD_LOOKAHEAD_PAGES` (50) of runway
  ahead of the reading position, counting the current section's unread tail plus the sections
  built so far. Whole spines are built until the budget is covered, so many tiny front-matter
  spines get several built while one big chapter covers it alone. The cursor walks forward as each
  settles and re-anchors on any navigation.
- **State machine:** `Probe` (cache check) → `WaitHeap` (gates) → `Building` (`BG_BUILD_BUDGET_MS`
  = 40 ms slices) → `Settled`.
- **Borrows the secondary buffer as its arena — the preferred path.** `beginBackgroundBorrow()`
  is tried *first*, before any heap floor is consulted, and is what makes B viable on the reading
  heap at all: the parse working set, the inflate ring and the CSS index all bump-allocate inside
  the lent ~48 KB region, so the heap-backed floors below (which the ~57 KB reading steady state
  cannot reach) stop being the binding constraint. The block never enters the heap, so returning
  it cannot fail.
  - **Cost:** the borrow drops the current page's AA (`secondaryBufferDegraded_`), so it only
    starts once the reader has been settled for `BG_BUILD_BORROW_QUIET_MS` (**1.5 s**) — below
    that the reader is skimming and B should stay out of the way. It also declines while
    `CooperativeAbort` reports a queued button edge, so it never takes a buffer it is about to
    hand back. Gates: free ≥ 40 KB, contig ≥ 12 KB. The window is measured from the last page
    reaching the SCREEN (`lastPageOnScreenMs_`), not the last page TURN — `lastPageTurnTime`
    starts at 0 and is stamped only by turns, so before the reader's first turn the check was
    vacuously satisfied.
    1.5 s is sized against what B needs, not what feels safe: attempt 2 (extraction already
    banked) is setup 123 + parse 818 = ~950 ms measured on X3 alice spine 1.
  - Unlike C's borrow it deliberately does **not** seed RED RAM or opt in to single-buffer fast
    differential: no refresh happens during B's borrow, and a completed Background-A pre-render
    usually leaves the *next* page in `frameBuffer`, so seeding from it would be actively wrong.
  - **Preemption:** a page turn ends the borrow, and `endBackgroundBorrow()` tears the live build
    down *before* handing the region back (the build allocates inside it). The parse is discarded,
    but phase (a)'s inflated XHTML survives on SD, so the retry skips re-inflation and is the
    cheap one. `BG_BUILD_MAX_PREEMPTIONS` (2) then leaves the spine to C. (That banking was
    broken until 2026-08-11: `abortSectionBuild` deleted the cache whenever THIS build had
    produced it, complete or not, so every retry re-paid the extraction and B abandoned spines
    it should have finished. Keyed on `extractDone` now.)
- **Heap-backed fallback (no buffer lendable).** Then B builds resident out of the heap, and these
  are the gates that apply: free ≥ max(`BG_BUILD_PARSE_MIN_FREE_HEAP_BYTES` 48 KB,
  `BG_BUILD_EXTRACT_BASE_HEAP_BYTES` 30 KB + inflate-ring); contig ≥
  max(`BG_BUILD_MIN_CONTIG_HEAP_BYTES` 24 KB, ring + 8 KB).
- **CSS refuse gate — heap-backed path only.** A CSS section built resident *out of the heap*
  reliably drops below the runtime CSS-resolve floor (~40 KB free) mid-parse → styles silently
  skipped → a *css-degraded* cache the foreground must rebuild. So B refuses CSS sections unless
  free ≥ `BG_BUILD_CSS_MIN_FREE_HEAP_BYTES` (72 KB); below that it parks in `WaitHeap` and lets
  **C** build the section on arrival. A **borrowed** build never consults this gate — it resolves
  CSS out of the arena, which is the whole point of the borrow.
  Note the lean resolver (`setLeanResolve`) is no longer tied to the borrow: since 2026-08-11
  every build uses the disk-backed path, because the hot-rule LRU it gives up cannot hit during a
  build anyway (the parser memoises on `tag|class|id` upstream, so the resolver only ever sees
  first occurrences — measured `hotHits=0` on every spine of three books). That dropped the
  resolver floor 40 KB → 24 KB and `heapAllowsEmbeddedStyle`'s free floor 56 KB → 44 KB. B also
  **early-aborts** (`Section::activeBuildCssDegraded()`) the instant a slice starts skipping, rather
  than finishing a build it will discard.
- **Discards** truncated or css-degraded results (`clearCache()`); those rebuild clean in the
  foreground/C with the buffer released.
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
| `IncrementalReleased` | **X3** (any), or **X4** that misses the in-place floors, or the buffer is already lent | **borrowed** as the build arena; *released* only when there is none to lend. Restored via `recoverSecondaryBufferIfNeeded()` |
| `IncrementalResident` | **X4** that fits the in-place floors (`IN_PLACE_BUILD_MIN_FREE` 60 KB / `…_CONTIG` 28 KB; higher CSS variants) | kept resident |
| `Blocking` | `forceBlockingBuildSpine_` latch, no secondary buffer, or a CSS-fallback rebuild | released → rebuilt → realloc'd |

The mode name is historical: `IncrementalReleased` now *borrows* first and only releases as a
fallback, because the lent block never enters the heap and returning it cannot fail. The
`Blocking` row is the one that still genuinely releases — and it builds with the 10 KB heap
arena rather than the freed 52 KB, which
[memory-allocation-strategy.md §9.2](memory-allocation-strategy.md) identifies as the largest
measured fragmentation source left in the reader.

Device rationale (see [contributing/eink-controllers.md](contributing/eink-controllers.md)):

- **X3** keeps the differential baseline in the controller's **DTM1**, so giving up the RAM buffer
  costs no display benefit (fast refresh still works) — and building out of it keeps CSS parses
  above the resolve floor. So X3 **never builds resident**: it always takes the buffer, in practice
  by borrowing it. Mid-build draws are plain BW off the DTM1 baseline; AA returns once the buffer
  is back. Device-confirmed 2026-08-11: `Background-C: building spine 1 incrementally, secondary
  buffer BORROWED`, `CSS RESIDENT in arena`, `cap=52272 highWater=28656 failedAlloc=0`.
- **X4** re-seeds its fast-refresh baseline from the RAM buffer, so builds that clear the in-place
  floors keep it resident. The rest take it, at the cost of half-refresh mid-build draws until it
  is handed back.

Note the mode name says *released* but the buffer is normally **borrowed** — that path only
releases when there is nothing to lend. Both variants set `secondaryBufferDegraded_`;
`recoverSecondaryBufferIfNeeded()` (top of every `render()`, guarded to skip while a build is
active) returns or reallocates and (X4) reseeds on the first render after the build ends.
`onExit()` restores it too if the reader is left mid-build.

---

## Diagnostics

- **Per-page** (`DBG`): `Page summary: … refresh=<fast|half|full> mode=0x.. renderMs=… …`. The
  refresh mode/byte are captured at the page's own `triggerDisplay` (before the deferred-AA display
  call would overwrite the renderer's live last-mode), so they reflect the page, not the AA pass.
- **Background work** (`INF`, every ~5 s under `DEBUG_BACKGROUND_WORK`): `BG work: A runs/completes |
  B runs/completes state=<probe|waitheap|building|settled> spine=… css=… | preReady=… buildPct=…
  free=… contig=…`. A CSS book on a tight heap shows B parked in `waitheap` (the refuse gate) rather
  than building-and-discarding.
- **C lifecycle** (`INF`): `Background-C: building spine N … (buffer resident | secondary buffer
  BORROWED | … RELEASED …)`, `Background-C spine=N complete: M pages`, and on failure
  `Background-C spine=N … falling back to blocking rebuild` / `… declined … blocking build`.
  `BORROWED` is the healthy case; a `RELEASED` line means there was no buffer to lend.
- **B borrow** (`INF`): `Background-B: borrowed secondary buffer for spine N …` /
  `Background-B: returned secondary buffer (spine N, build discarded|no build live,
  preemptions=K)`. Repeated `build discarded` at `preemptions=2` means B keeps losing the race to
  page turns on that spine and is handing it to C.
- **Heap gates** (`INF`, off by default — build `-DHEAP_GATE_TRACE=1`): one
  `gate=<name> PASS|REJECT free=…(floor=…)
  contig=…(floor=…)` line per decision. Several floors reject *silently* otherwise, so this is the
  only way to see which one sent a build down the released path.
- **Render-task stack** (`ERR`, always on): `Render task stack LOW: N bytes free` if the high-water
  margin drops below 1536 B — the render task runs the deepest chains (build parse + image decode +
  dither) and its stack abuts the heap, so an overflow corrupts the heap.

## On X3 specifically (the common device)

- The borrow is what carries CSS books here. At steady reading heap (~57–65 KB free) B cannot
  clear the 72 KB heap-backed CSS gate, so before the borrow existed every forward chapter cross
  into a CSS section fell to **C** rather than a B cache hit. B now borrows the buffer instead and
  resolves CSS from the arena, so that gate is only reached when there is no buffer to lend.
- X3 has no resident build mode at all (`chooseSectionBuildMode` returns `IncrementalReleased`
  unconditionally), so on this device C is *always* the borrow-or-release path and the in-place
  floors never apply.
- Page turns are fast (`refresh=fast`) except the scheduled anti-ghost **half** every
  `getRefreshFrequency()` (15) turns. On X3 a "fast" request is never silently turned into a half —
  it's honoured, or escalated to a *full* only when the differential baseline isn't synced (which
  the clean build/restore paths avoid).
