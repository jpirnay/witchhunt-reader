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

### Writer side

- `ContentBinWriter` gains `checkpoint()` — call every K blocks or every ~T ms of compile: flush the
  block data, write the rolling checkpoint (partial aux + frontier), two-phase-update the header
  pointer. K/T tuned so the reader rarely stalls (a page is ~a few dozen blocks; checkpoint every
  ~64 blocks or ~250 ms is ample). The background compile pass drives it.

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

## 4. Interaction with fast-first-page

Fast-first-page (the direct parse of the current spine → page 1 now) is UNCHANGED and independent:
it serves the first page while content.bin (and its checkpoints) are still being written. Once the
frontier passes the reading position, the reader switches that spine to the content.bin path
(fast + settings-changes-free). For a giant single spine, the frontier makes this switch happen
*within* the spine rather than only at its (distant) end — which is the whole point.

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
