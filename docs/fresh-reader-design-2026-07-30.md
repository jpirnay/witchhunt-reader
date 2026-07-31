# Fresh reader — design (2026-07-30)

A clean-slate reader built on `content.bin` as the single conclusive source, designed around the
memory + background-compile requirements from the first line rather than retrofitted. Written after
reverting the whole-book-blocking-compile approach, which repeatedly OOM'd because it ignored the
memory regime. This doc is the contract to agree BEFORE any code.

## 0. What we keep (already in master, verified)

- The **pull core** (`compiled::layoutPage` / `layoutPageBackward`, `PageLayout.*`): one page from a
  `PagePosition` cursor, byte-identical to the golden. Device-measured ~55 ms/page even on a
  907-page spine. This is the read engine.
- The **compile → content.bin** path (`ContentBinWriter`, `Section::compileBookToContentBin`,
  `buildSectionFromContentBin`) and perf fixes (ZIP-seek cache, buffered writes/reads).
- The streaming **`BlockStreamReader`** with per-spine commit slots + `spineAvailable()` +
  `refreshIndex()` — the producer/consumer frontier primitives.

## 1. The hard-won memory facts (the regime must obey these)

Every OOM this session was the same root: an **unbounded or large-contiguous allocation at the
reader's tight heap** (~40–52 KB free, fragmented, `-fno-exceptions` → a throwing `new` aborts).
Concretely, ranked by how much they hurt:

1. **The ~32 KB inflate ring.** Every ZIP entry read (spine XHTML, image header, image decode) wants
   a ~32 KB *contiguous* inflate ring. On a fragmented ~50 KB heap that block often does not exist.
2. **Per-book / per-spine unbounded accumulation.** The block-offset table grew one 12 B entry per
   block → ~48 KB for a 4097-block spine, held to spine end. Sibling per-spine vectors (anchors,
   labels, styles) have the same shape but are smaller.
3. **Whole-book peak.** Compiling all spines in one call holds cross-spine state and keeps the heap
   pressured the whole time; the shipping section build survives Small Gods precisely because it does
   ONE spine at a time with the heap relaxed between.

**Design rules that follow (non-negotiable):**

- **R1 — Fixed working set.** No structure grows with spine size or block count in RAM. Anything
  O(blocks)/O(pages) either streams to SD or lives in a fixed-cap arena buffer (FreeInk's model:
  fixed caps + guarded appends + flush-at-cap). Applies to the block-offset table, and to the
  parser's per-block word/text accumulation (cap → flush a continuation record).
- **R2 — One inflate ring at a time, from a known home.** The compile owns exactly one inflate ring,
  carved from a **borrowed framebuffer arena** (the ~48 KB region is contiguous by construction).
  No code path allocates a second ring from the fragmented heap while the first is live. Image
  header/decode reads that happen during compile use the SD-extracted copy (R3), not a fresh ring.
- **R3 — Unzip once, keep on SD, reuse.** Each ZIP entry we will need again (spine XHTML; images) is
  inflated to an SD sidecar exactly once, when a ring is available, and every later read is a plain
  bounded SD read (no ring). This is the user's core requirement and it kills the ring-OOM class:
  - Spine XHTML: already cached (book-keyed HTML temp file). Keep.
  - **Images: NEW.** Extract each referenced image's raw bytes to `<cache>/img/<entryhash>.<ext>`
    once. `getDimensions` and the render-time decode both read that file. (Today the dimension read
    re-inflates from ZIP and OOMs; `ImageBlock::ensureExtracted` already extracts at render time —
    unify on it.)
- **R4 — One spine at a time, always.** The background compiler advances spine-by-spine (and, for a
  giant single spine, block-run by block-run within it), never a whole-book peak. Between units the
  heap fully relaxes.
- **R5 — Everything memory-gated + nothrow.** Any allocation that could fail uses `makeUniqueNoThrow`
  / a size check with a graceful fallback; never a bare `new` that can abort. Verified by a device
  heap probe on the two stress books before shipping any path.

## 2. The two subsystems (meet ONLY at content.bin + the frontier)

Kept strictly separate — the weaving of these is what sank the earlier attempts.

### A. The background compiler (producer)
- **RETARGET master's proven driver, do not invent one.** Master already runs memory-safe BACKGROUND
  section indexing WHILE rendering (Background-B/-C: `Section::stepSectionBuild` sliced with a
  `budgetMs`, the framebuffer release/borrow discipline, heap-gated refusal). That machinery already
  handles both book shapes in production. The background CONTENT.BIN compiler is the SAME sliced,
  budgeted, memory-managed driver, just emitting content.bin (`setStage1Sink` + `ContentBinWriter`)
  instead of a per-settings section cache. The whole-book BLOCKING compile that kept OOMing was the
  mistake — not background compile being hard.
- Drives an **incremental** compile, one spine (or one block-run of a giant spine) per idle slice,
  from the **last known good state**: reopen content.bin, read the committed slot index, skip
  committed spines, continue from the first uncommitted one until EOF (all spines committed). This is
  the resume machinery (per-spine two-phase commit — a committed slot is durable truth) but driven a
  slice at a time, exactly like Background-C drives stepSectionBuild.
- Runs in the reader's idle time (like the existing Background-B/-C), gated on heap headroom (R4/R5),
  borrowing the framebuffer arena for its one ring (R2) while it holds a slice.
- Advertises progress via the committed-slot index (whole-spine) and — for a giant single spine — a
  **rolling intra-spine checkpoint** at EOF (the frontier: "blocks [0..N) of this spine are readable"),
  written crash-safe (two-phase header pointer). Adaptive cadence: per-block near the reader, every-X
  far ahead.

### B. The reader (consumer) — microreader-simple
- Turn a page = move a cursor → lay out one page → refresh. NO page cache, no invalidation, no
  scheduler in the turn path. Prev = O(1) via a bounded cursor stack; standalone
  `layoutPageBackward` only for post-jump.
- **Render waits for the frontier, then renders immediately.** To show the page at cursor C: if the
  compiler has committed past C (spineAvailable / intra-spine frontier ≥ C), lay it out now and
  render. If not, show a brief "preparing…" and poll `refreshIndex`/`refreshFrontier` each loop tick;
  the instant the frontier covers C, render. In steady state the compiler runs ahead, so this only
  waits at a cold book's first pages.
- **Prepare the next page after the first lands.** As soon as the current page is on screen, lay out
  page+1 into an off-screen buffer (the existing Background-A pre-render), so the next turn is
  instant. Symmetric with the current pre-render, but sourced from content.bin via the cursor.

## 3. The page-count problem (the reason the OLD reader can't just be reused)

The existing `EpubReaderActivity` is page-INDEX navigation (`section->currentPage`,
`section->pageCount`) woven through 8 subsystems, and an exact `pageCount` per spine requires fully
paginating the spine — which a giant spine (or a not-yet-fully-compiled one) does not have cheaply.
The fresh reader is **cursor-native**: position is a `PagePosition`, not a page index. Progress %
comes from `charOffset / totalChars` (char offsets are settings-independent and already in the
format), not `page / pageCount`. This is why it is a fresh reader, not a patch of the old one.

Chrome that needs a total (status bar "N of M") shows an **estimate** (chars-based) that firms up as
the compiler finishes the spine — the mature reader already has this exact "unknown while building"
pattern (`estimatedTotalPages`, `pageCount==0` sentinel) to borrow from.

## 3c. One-producer collapse — the concrete migration (2026-07-31, after the CBC-in-reader failure)

The device proved that running the content.bin compiler as a SECOND builder alongside the reader's own
section builds (Background-C) is fatal: two builders fighting the one framebuffer + one loop task →
6.7 s build starvation, a Store-access-fault crash on spine entry, half-refresh every page, freeze.
FreeInkBook (freeink-sdk/libs/book/FreeInkBook) settles the model: NO tasks, ONE cooperative loop; the
producer is a `step()` pumped from the loop (`while(!s.done()){ s.step(4); render; input; }`), and the
reader reads that ONE producer's PARTIAL output mid-build (`PageCacheWriter::readPage`/`pageForChar`).

**Reader-map finding (2026-07-31):** the pull core (`compiled::layoutPage`/`PagePosition`) is NOT wired
into the reader at all — zero refs in `src/`. The reader is 100% "build a per-settings section file, read
pages by LUT index" (`section->pageCount`/`currentPage`/`loadPageFromSectionFile`). So the reader MUST
build section files today because that is the only thing it can read. Therefore the one-producer collapse
and the cursor-native reader (§3, old "Step 4") are THE SAME WORK — you cannot remove the section builder
without giving the reader a content.bin read path, and vice-versa.

**Target end state (exactly two roles):**
1. ONE PRODUCER — `ContentBinCompiler::step(budgetMs)` pumped from the reader loop; the ONLY code that
   opens the epub/ZIP + inflates + parses. Resumes from committed slots; exposes the frontier
   (`committedSpines()`, `spineInFlight()`), and — for the current spine — a partial/mid-build read.
2. CONSUMER (reader) — renders pages PURELY from content.bin via `compiled::layoutPage(reader, params,
   cursor)`. Position is a `PagePosition` cursor (block/offset/**charOffset**), NOT a page index. NEVER
   opens the epub. NO `Section` builds, NO section files.

**What each `Section`/reader dependency maps to (from the map):**
- `section->loadPageFromSectionFile()` → `compiled::layoutPage(BlockStreamReader&, params, cursor)`.
- `section->currentPage`/`pageCount` (page-index nav) → a `PagePosition` cursor + forward/back page
  turns via `layoutPage`/`layoutPageBackward`; "N of M" is an ESTIMATE from `charOffset/totalChars`.
- `resolveInto` (TOC/anchor/paragraph/percent jumps) → `BlockStreamReader::spineAnchors/Labels/Chapters`
  + `charOffset` (the anchors/labels/chapters are already baked per-spine in content.bin).
- `progress.bin` (spine+page+pageCount) → char-offset / the full `PagePosition` cursor (settings-independent).

**Migration order (each host-tested then device-verified, one concern per commit):**
- M1. Prove the one-producer + read-from-partial-frontier loop in `bench_pagelayout` (no reader UI) on
  King's Avatar + Small Gods: step the producer, render the current page via `layoutPage` from the
  committed/partial frontier, page forward/back by cursor, memory + latency green. (De-risk before the
  reader surgery — this is the whole model in isolation.)
- M2. Give the reader a cursor + a content.bin read path ALONGSIDE the section path (feature-flagged), so
  it can render a page via `layoutPage` when content.bin covers the cursor. No removal yet.
- M3. Route navigation (page turn, TOC, anchor, percent, resume) through the cursor; progress → charOffset.
- M4. Make the producer the reader's single builder (pumped from the loop); the reader waits on the
  frontier for the page it needs, renders it, pre-renders next. DELETE Background-C, `createSectionFile`,
  section files, the framebuffer-borrow-in-reader, Background-B remnants, the CBC-specific borrow churn.
- M5. Cache hygiene: only content.bin (+ sidecars) remain; delete section-file machinery + variant cache.

## 4. What lands first vs later

- **First (must work minute one):** open → background compile from last-good-state → cursor read loop
  → render-waits-for-frontier → pre-render-next → the memory regime (R1–R5) proven on BOTH King's
  Avatar (thousands of spines) and Small Gods (one multi-MB spine).
- **Later (explicitly deferred):** context menu, TOC, bookmarks, KOReader sync, footnote popups,
  per-book font overrides, images beyond the cover (extract-to-SD generalizes to all).

## 4b. The retarget is ~80% pre-built (2026-07-30 map)

Master's background section build ALREADY tees content.bin through the exact sliced, memory-safe,
RenderLock-guarded driver we need:
- `Section::setContentBinTee(writer, spineIndex)` / `setStage1TeeSink` — the section build emits
  content.bin AS IT PARSES, driven by the same `stepSectionBuild(budgetMs)` slices.
- `commitSpine()` fires ONLY on a clean `Done` (`!cssLowHeapDegraded_ && !truncatedCache`) — a bad
  parse never publishes a bad content.bin spine (two-phase commit safety, already wired).
- The driver: `serviceBackgroundWork()` on the loop task, `BG_BUILD_BUDGET_MS=40` slices, yields at
  the 1 KB feed boundary, gates every tick on `isRefreshPending() || RenderLock::peek() ||
  imageProcessingActive_` (skip, don't block), takes a RenderLock for the slice, PREFERS BORROWING
  the secondary framebuffer as the build arena (`borrowSecondaryBuffer` + `BuildArena` +
  `setExternalBuildScratch`) over freeing it, gates start on per-spine heap floors that ADD the
  inflate ring, and aborts-to-released mid-build below `RESIDENT_BUILD_ABORT_*`.
- Two-temporal-phase memory: extract (inflate ring live) → release ALL zip state → parse (parser
  working set live). Ring and parser set never coresident. THIS is why builds fit in ~68 KB.

So the background CONTENT.BIN compiler is: adopt this driver, run it across ALL spines (not just the
read-ahead runway), resume from the committed-slot index to EOF. The whole-book BLOCKING compile I
built ignored ALL of this — that was the error.

## 4c. Cache hygiene (user requirement 2026-07-30)

Temporary per-spine artifacts must NOT clutter the book's cache dir — King's Avatar has thousands of
spines. Once a spine's content is folded into content.bin (its slot committed), its transient inputs
(the book-keyed unzipped-HTML temp, any per-spine scratch) are no longer needed and must be removed.
Master already does some of this (`~Section` deletes a partial cache file). The fresh reader keeps the
cache dir to ~O(1) files: content.bin + its progress sidecar + images (extracted once, R3) + book.bin
— NOT thousands of per-settings section files (which the cursor-native reader does not produce at
all — see G6). Any temp (HTML extract, block-offset sidecar) is deleted as soon as its data lands in
content.bin.

## 5. Additional things noticed (worth deciding)

- **G6 tension:** with content.bin the single source + cursor-native reader, the per-settings section
  files are no longer needed at all — deleting them (and the variant cache) is the endpoint. The old
  reader wrote section files; the fresh one should NOT.
- **Latency:** the 35 s "read-back" for 864 pages in the reverted approach was LayoutSink replaying
  the whole spine to disk. The cursor-native reader does NOT pre-paginate a spine — it lays out one
  page per turn (~55 ms), so that latency does not exist here. Confirm this holds on device.
- **Cover / image-only spine:** an image-only titlepage must lay out to a real 1-image page (today it
  can produce 0 pages if the image dims fail to resolve). R3 (extract-to-SD, ring-free dims) fixes the
  resolution; the pull core already handles atomic images.
- **First-open:** land on the text reference (skip front matter) on a truly-first open, but never
  override a saved cursor.
- **Crash-consistency:** the two-phase commit is the safety net — a committed slot's data is always
  durable, so a crash/exit mid-compile resumes from the last good slot. The reader only ever reads
  committed spines / below-frontier blocks.

## 6. Build discipline (the process fix)

Each piece host-tested first, then device-verified on BOTH stress books, BEFORE the next. A device
heap probe (free / largest-contig) is part of every verify — the memory regime is proven, not hoped.
No whole-book blocking anything. Commit in small single-concern units.
