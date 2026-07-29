# Intra-spine read frontier — progressive reading of a spine still being compiled

Written 2026-07-29 (user requirement). The background-compile + fast-first-page strategy assumed
whole-spine granularity: a spine becomes readable when its index slot commits (`onSpineEnd`). That
is implausible for books like **Small Gods** with hundreds of pages in a SINGLE spine — the reader
would wait for the entire book-sized spine to compile before showing anything. We need to **read
the already-compiled prefix of a spine while the rest of that same spine is still being written**,
with a safe progress indicator so we never render past what is durably on disk.

## 1. Why a partial spine is currently unreadable

`BlockStreamReader::openSpine(i)` requires the spine's AUX region — per-spine **style pool**,
**block-offset table** (for `seekToBlock`), anchors, labels, chapters — which the writer emits only
at `onSpineEnd` (ContentBinWriter.cpp:227-242), then commits the index slot. So:

- A block's `styleId` indexes the per-spine style pool → **can't lay out a block without it**.
- `seekToBlock` needs the block-offset table → **can't random-access without it**.
- The index slot is 0 until spine end → `openSpine` refuses, `spineAvailable(i)` is false.

Net: **a spine is all-or-nothing today.** The block DATA for the prefix is on disk (written per
`onBlock`), but the metadata to read it isn't.

## 2. The addition: a periodic intra-spine CHECKPOINT + a frontier marker

The writer, while compiling a long spine, periodically writes a **checkpoint** advertising "blocks
[0..N) of this spine are safely readable," carrying the metadata the reader needs for that prefix:

- **styles-so-far** — the spine's style pool as of block N (styles only grow; a superset is fine).
- **block-offsets-so-far** — the block-offset table entries for blocks [0..N) (for `seekToBlock`).
- **frontier N** — the count of committed logical blocks + the char offset at N (for progress).
- anchors/labels/chapters for blocks [0..N) (bounded; they key on block index < N).

The block DATA is already durable (flushed per checkpoint). The reader reads up to block N and lays
out any page whose content lies entirely before the frontier; at the frontier it **stops and waits**
(shows "compiling…"/spinner or just holds) until the next checkpoint advances N.

### Where the checkpoint lives (format)

Option A (chosen — least disruptive): a **rolling checkpoint at the END of the file**, rewritten in
place each interval. The header gains a `checkpointOffset` (or a reserved slot) pointing at the
current in-progress spine's checkpoint: `{spineIndex, frontierBlockN, firstCharOffset, auxOffset',
committedCharOffset}` + the partial aux (styles/offsets/anchors/labels/chapters for [0..N)). Written
after the last durable block, flushed, then the header pointer updated + flushed (two-phase so a
crash never advertises a half-written checkpoint). On `onSpineEnd` the normal per-spine aux + slot
commit supersede the checkpoint (which is then stale/ignored).

Rejected Option B (interleave mini-aux between block runs): bloats the file and complicates the
sequential reader. Option A keeps the block stream contiguous (the fast path) and confines the
churn to one rewritten region.

### Reader side

- `BlockStreamReader` gains: `openPartialSpine(i)` (loads the checkpoint's partial aux if spine i is
  the in-progress one and has a checkpoint), `spineFrontier(i)` → N (committed block count, or the
  full `blockCount` once slot-committed), and `refreshFrontier()` (cheap re-read of the checkpoint
  pointer + frontier, like `refreshIndex` but intra-spine).
- The pull core's forward layout already stops when it runs out of blocks; it must additionally stop
  at `spineFrontier` (treat "no more readable blocks yet, but not spine end" as a soft stop → the
  reader waits, distinct from a true spine end).
- **Backward within a partial spine**: `layoutPageBackward` forward-renders from spine start; that
  works within [0..N) unchanged (start is always ≤ frontier). Prev never needs unread blocks.

### Writer side — ADAPTIVE checkpoint cadence (revised 2026-07-29, user)

`ContentBinWriter` gains `checkpoint()`, but the cadence is **adaptive to how far the frontier is
ahead of the reader**, not a fixed interval. The writer is told the reader's current position (block
index) and:

- **Frontier barely ahead of the reader** (gap small, e.g. < a page): checkpoint **every block**, so
  the reader can start rendering ASAP and never stalls waiting for a coarse interval. This is what
  makes page 1 appear quickly — no separate first-page path needed (see §4).
- **Frontier well ahead of the reader** (gap large): checkpoint **every X blocks** (coarse), to
  amortize the SD write latency of the rolling checkpoint once the reader is comfortably behind.

So the checkpoint write frequency scales inversely with the read/compile gap: aggressive when it
matters (cold start / reader catching up), cheap when it doesn't (reader far behind the compile).
The background compile pass drives `checkpoint(readerBlockIndex)`; the writer picks per-block vs
every-X based on `frontier - readerBlockIndex`. X and the gap threshold tune at G4/G5.

## 3. The progress indicator (safe-to-render signal)

Two levels, both cheap:

- **Per-spine availability** (exists): `spineAvailable(i)` — the whole spine is committed.
- **Intra-spine frontier** (new): `spineFrontier(i)` → committed block count N, and the char offset
  at N. The reader renders a page iff the page's end cursor ≤ frontier. `committedCharOffset /
  totalChars` also gives a real "compiling X%" for the current spine, for a progress UI.

The reader's rule: to render the page at cursor C, need `frontier > C.blockIndex` (the page's blocks
are all readable). If not, `refreshFrontier()`; if still short, the compile hasn't reached here —
show a brief "preparing…" and retry next loop tick. In practice the compile runs ahead of reading
(one-time upfront cost is front-loaded), so this stalls only at the very first pages of a cold book.

## 4. Fast first page — NO separate path (revised 2026-07-29, user)

The earlier plan had a SEPARATE "direct parse the current spine → page 1 now" path that ran in
parallel with content.bin, then a handoff once the spine committed. **We drop that entirely.** It
was extra machinery (a dual-mode reader + a handoff — the exact kind of parallel-path complexity
that made Increment E/F fragile) born from assuming the compiler is too slow to produce page 1
quickly.

Instead the ASSUMPTION/TARGET is: **the compiler is fast enough that, with per-block checkpoints
near the reading position (§ Writer side), the frontier reaches page 1 almost immediately.** So
there is ONE read path — content.bin via the frontier — for page 1 and every page after:

1. Open the book → kick off the background compile (writing content.bin + adaptive checkpoints).
2. The reader polls `spineFrontier`; as soon as it covers page 1's blocks, render page 1. With
   per-block checkpointing at the front, that is ~immediate for a normal book.
3. Every subsequent turn is the same: render if the frontier covers the page, else briefly wait.

No direct parse, no dual mode, no handoff. If G4 shows the compiler is NOT fast enough to hit page 1
acceptably even with per-block front checkpointing, we revisit — but the target is to make the
compiler fast, not to carry a parallel first-page engine. This is the single-conclusive-source
principle taken to its conclusion: the compiled source is the ONLY thing the reader ever reads.

## 5. Format-version impact

content.bin v7 → v8: adds the header `checkpointOffset` field (+ the rolling checkpoint record
format). content.bin is a rebuildable cache, so a v7 file just recompiles (auto). A COMPLETED v8
spine reads identically to v7 (checkpoint ignored once the slot commits) — so the offline host
oracle (whole-book compile then read) is unaffected; only the LIVE partial-read path is new.

## 6. Scope / sequencing note

This is a real addition to the format + writer + reader + the pull-core stop condition. It lands
with G5 (the reader integration / background compile wiring), because that is where partial reading
is actually exercised. For the **G4 latency gate** it does NOT block measurement: G4 compiles the
whole book first, then measures read latency on the completed content.bin — the frontier only
matters for the live-during-compile case, which G4 can measure separately (compile throughput
already tells us whether the frontier will keep ahead of reading).

## 7. Open questions for G5

- Checkpoint interval K/T — tune so the reader stalls only cold-start; measure at G4/G5.
- Does the frontier need char-offset granularity, or is block-count enough? (block-count for the
  stop condition; char-offset only for the % UI — start with block-count.)
- Crash-consistency of the two-phase header update on the device FS (SD/FAT) — verify a torn write
  leaves the last-committed slot readable (it does: the checkpoint is advisory; committed slots are
  the durable truth).
