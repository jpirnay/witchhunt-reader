# Plan for the remainder — reader integration

Written 2026-07-29, after P5 (the pull core does forward + backward live layout, byte-identical
to golden). Step back on objectives + a second role-model comparison, then the concrete plan for
the remaining work: arena memory, the device latency gate, and wiring the pull core into the
reader (keeping background compilation, our deliberate differentiator).

## 1. Objectives — still aligned

Unchanged from `stage1-single-source-live-pagination-2026-07-27.md §1`:
- content.bin is the ONLY persisted book cache; delete the per-settings section-file page store.
- The reader lays out one page at a time, live, from a `PagePosition` cursor. Nothing paginated
  is persisted. A settings change is free (re-lay-out, nothing to invalidate).
- **Background compilation stays** (decided 2026-07-29, re-confirmed): first open shows page 1
  immediately via a direct parse while content.bin compiles in the background; each spine switches
  to content.bin once committed. This is our deliberate deviation from the role models.
- Arena-backed memory for the per-turn working set: deterministic footprint, no fragmentation.

## 2. Second role-model comparison — what it confirmed and what it simplifies

Studied microreader's `ReaderScreen` and KOReader's model for the READER side (the remainder).

**Confirmed our compile differentiator is a real deviation, kept deliberately.** Both microreader
and KOReader do a SYNCHRONOUS upfront convert (a "Converting…/Preparing…" progress bar) the first
time a book is opened, then pure fast live reading forever after — NEITHER has background compile
or fast-first-page. We keep ours (user decision 2026-07-29): no first-open wait on a new book is
worth the extra complexity. **But we now treat it as an isolated, well-fenced subsystem**, not as
something woven through the read loop — because that weaving is exactly what killed Increment E/F.

**The read loop itself must be microreader-simple** (this transfers directly). microreader's loop
is: a button turns the page → update the `PagePosition` → `render_page_()` lays it out live →
refresh. No page cache, no section files, no invalidation, no scheduler in the turn path. Our read
loop must be the same: `next/prev` update the cursor, `layoutPage`/`layoutPageBackward` render it,
done. The compile-model complexity lives ENTIRELY in "how content.bin gets populated," strictly
separated from "how a page turn works." Keeping that separation is the core simplicity discipline
for the remainder.

**Prev-page is O(1) via a cursor stack** (transfers from microreader's cursor model). The reader
keeps a small stack of recent page-start cursors while reading forward; `prev` pops it. The
O(pages) standalone `layoutPageBackward` is only for after a JUMP (percent/anchor), where the
reader has no history — rare, and the walk is acceptable there.

## 3. The clean separation (the architecture for the remainder)

Two independent subsystems, meeting only at content.bin + the `spineAvailable` frontier:

```
  ┌─ READ PATH (microreader-simple, hot) ──────────────────────────────┐
  │  cursor (PagePosition) --next/prev/jump--> new cursor              │
  │  layoutPage / layoutPageBackward(cursor)  --> Page  --> render     │
  │  cursor stack for O(1) prev; save cursor as reading progress       │
  │  source: content.bin if spine committed, else the direct parse     │
  └────────────────────────────────────────────────────────────────────┘
                      ▲ reads                    ▲ frontier check
                      │                          │ (spineAvailable / refreshIndex)
  ┌─ COMPILE PATH (isolated, background, cold) ─────────────────────────┐
  │  first open: direct parse of the current spine -> page 1 NOW        │
  │  background: sliced ContentBinWriter pass, spine-by-spine, arena    │
  │  commits each spine's index slot when done (no tee, content-only)   │
  └────────────────────────────────────────────────────────────────────┘
```

The read path NEVER blocks on the compile path. A spine not yet committed is served by a direct
one-page parse (today's fast path, already in the scaffold); once committed, the read path switches
to content.bin for that spine (fast, and settings-changes become free). This is the v2 plan's
Option 2, kept minimal.

## 4. Sequenced plan for the remainder

- **P6 — Arena memory (do at reader integration, not before).** The per-turn working set — the
  windowed block reads + the laid-out lines (`TextBlock`s, historically THE fragmentation driver) —
  comes from a reader-owned `BuildArena`, reset per page turn. Host keeps heap (P1–P5 proved
  correctness there). Decision point: whether the pull core's `LaidOutBlock`/window allocations take
  an arena allocator, or the reader wraps calls in an arena scope. Lean: reader owns a per-turn arena
  scope; the pull core allocates its transient window/lines within it, frees on turn. Keep the pull
  core's interface unchanged (it takes a reader + params); the arena is a device-side memory home,
  invisible to the host tests.

- **G4 — DEVICE LATENCY GATE (hard stop; user flashes + measures).** Before deleting anything,
  measure on device:
  - ms/page FORWARD and BACKWARD (cursor-stack prev, the hot path) on King's Avatar + Small Gods at
    real settings, arena-backed.
  - the one-time background-compile throughput (does a spine commit before the reader reaches it?).
  - contiguous-heap headroom during a turn (the F failure was fragmentation, not exhaustion).
  GO/NO-GO on making the pull core the device read engine + deleting section files. Instrumentation:
  a per-turn `ms` + `free`/`contig` log; the user captures serial on the two heavy books.

- **G5 — Reader read-loop rewrite (the integration).** Keep it microreader-simple:
  - Replace the section-file blit path in the reader with `layoutPage`/`layoutPageBackward` from the
    live cursor. `next/prev/jump` update the cursor; a cursor stack gives O(1) prev.
  - Reading progress = the `PagePosition` (+ charOffset for re-sync), saved on each turn — no page
    numbers persisted.
  - Wire the background compile pass (Option-2 shape: fast first-page direct parse + sliced
    ContentBinWriter, NO tee, content-only `setStage1Sink`) + the frontier handoff
    (`spineAvailable`/`refreshIndex`). This is the ONE complex piece; fence it hard from the read
    loop. Behind `EPUB_STAGE1`.
  - Settings change: just re-`layoutPage` at the current cursor with new params. Free.

- **G6 — Delete section files + flip the flag.** Remove the per-settings section page store,
  `evictOldVariants`, `loadPageFromSectionFile`, once G5 is the sole read path and G4 passed. Flip
  `EPUB_STAGE1` default when corpus + device validated.

## 5. Complexity we are NOT carrying (confirmed against the role models)

- No per-settings paginated cache (section files) — deleted. Settings change re-lays-out live.
- No page-number LUT — progress is a cursor + char-offset ratio.
- No producer/consumer queue, no rolling-window frontier chase, no A/B/C scheduler (the E/F
  overreach). The compile path is one sliced background pass that commits spines in order.
- No tee (leaks settings-dependent data; content-only sink instead).
- Floats + tables stay on the scaffold fallback (P3/P4 decisions) — not worth pull-core parity for
  the corpus coverage; revisit only if profiling shows they're common.

## 6. Open questions for G5 (decide at integration, not now)

- Cursor-stack depth + what happens on overflow (a very long forward run) — cap + fall back to the
  O(pages) `layoutPageBackward` when the stack misses.
- Where the direct-parse-vs-content.bin switch lives (per-spine, checked on spine entry via
  `spineAvailable`).
- Whether the background compile runs on the loop task (sliced, like today's Background-C) or a
  separate task — lean sliced-on-loop, matching the proven pattern, to avoid cross-task heap races.
