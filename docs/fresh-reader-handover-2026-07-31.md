# Fresh Reader (content.bin) — Handover & Honest Retrospective (2026-07-31)

Written after many hours across several sessions produced a compiled-content pipeline that is proven in
isolation but **has never rendered a single page in the actual reader**. This document is the honest
account: what we tried, where we failed, what we believe is left, and — most important — a grounded
comparison against the three proven role models (master's background section build, FreeInk book,
microreader) with file:line citations, so the next attempt copies what works instead of re-deriving it.

---

## 0. TL;DR — the one thing that matters

**Master already solves the exact memory problem we kept failing on, and we diverged from its proven
sequence instead of copying it byte-for-byte.**

- The hard problem is: a giant single-spine book needs a **~32 KB contiguous inflate ring**
  (`INFLATE_DICT_SIZE = 32768`, `lib/InflateReader/InflateReader.cpp:8`), but the reader's resident
  heap is only ~38–52 KB free and fragmented.
- Master's answer (`chooseSectionBuildMode` → `IncrementalReleased`) is to **BORROW the secondary
  framebuffer as the build arena** (preferred) — `borrowSecondaryBuffer()` → `BuildArena` →
  `setExternalBuildScratch`, held for the *whole* build across all slices, returned only when the build
  finishes (`EpubReaderActivity.cpp:2808-2843`, `recoverSecondaryBufferIfNeeded` `:2274-2336`). Release
  is only a *fallback* when there is no buffer to lend.
- **We tried borrow and it failed** with `Failed to release extraction buffer block`
  (`Section.cpp:952-955`, the `shrinkChunkAfterExtract()` strict-LIFO release). We concluded "borrow is
  incompatible" and switched to release-always — but **master borrows successfully every day.** So our
  borrow was set up wrong (arena lifetime / LIFO ordering / per-slice churn), not fundamentally broken.
  We fixed the wrong thing.

**Recommendation for the next attempt: do NOT invent the producer's memory discipline. Make the
content.bin producer drive `Section::stepSectionBuild` through the *identical* borrow/release lifecycle
`buildSection()` + `stepCurrentSectionBuild()` use — same borrow-once-hold-across-slices-return-at-end,
same `chooseSectionBuildMode` gates — because it is already proven on both stress books.**

---

## 1. What was built (and what is genuinely proven)

### Proven in isolation (host tests + the bench, all green)
- **The pull core** `compiled::layoutPage` / `layoutPageBackward` (`PageLayout.cpp`): lays out ONE page
  from a `PagePosition` cursor, byte-identical to the golden across the whole corpus. ~55 ms/page
  device-measured.
- **`ContentBinCompiler`** (`lib/Epub/Epub/content/ContentBinCompiler.*`): incremental, resumable,
  content-only compile of content.bin. Reuses master's `Section::stepSectionBuild` per spine. Host
  byte-identical (sliced == whole-book), resume byte-identical.
- **The read primitive** `ContentBinCompiler::readPageAt` via `ContentBinWriter::withReadableFile`
  (commit `59942404`): renders a page from content.bin *through the producer's own open handle* (SdFat
  allows one handle per file). Host test `ReadPageMidCompileDoesNotDisturbWriter` proves a mid-compile
  read leaves content.bin byte-identical. **Device M1 bench (`benchOneProducer`, King's Avatar 60-spine):
  ok=1 pages=1067 stalls=0, min-free 48 KB, no hang.** This is the strongest result we have — the
  one-producer + read-from-frontier loop works, headless.
- **R1** (`522aca51`): the per-block offset table streams to a sidecar (O(1) resident), device-verified
  the ~48 KB drain is gone (min-ever contig 30 K → 59 K on the giant spine).

### Standalone merit for MASTER (independent of the content.bin direction — cherry-pick candidates)
- **`e44655f4` — ZipFile per-book spine-stat cache** + `EntryReader::open(FileStatSlim)`. UNCONDITIONAL
  (no `EPUB_STAGE1` gate). Kills the per-spine central-dir scan (huge on 1732-spine books); uses
  `std::deque` deliberately to avoid a big contiguous alloc on a fragmented heap. Pure perf.
- **`9f4ca076` — tjpgd `_WIN32`→`_MSC_VER<1600` guard.** Build fix: unblocks ALL Windows host builds.
- **`f1bb1db7` — FootnoteEntry memsets its whole fixed arrays.** Real correctness fix (uninitialized
  array tails were serialized as garbage → non-determinism).
- **`lib/Serialization/BufferedFileIO.h`** (`d40962f0`/`8c4bf1bf`): generic buffered SD write/read.
  The finding — tiny unbuffered SD I/O dominated the compile cost (~6 s / 4097 blocks) — applies to
  master's section-file serialize too.

### Reverted / superseded (dead ends, do not resurrect)
- The two-builder CBC-in-reader (reader's Background-C section build + a second CBC producer running
  concurrently) — reverted; caused the fragmentation crash class. See §3.
- Producer/consumer "Increment E/F", the Tee that leaks settings-dependent data — reverted.

---

## 2. The M-series migration plan (where we are)

Documented in `docs/fresh-reader-design-2026-07-30.md` §3c. Status:
- **M1 (read primitive de-risk in the bench): DONE + committed + device-proven.**
- **M2 (wire the full cooperative model into the reader behind `READER_FRESH_ENABLED`,
  `[env:default_fresh]`): CODE COMPLETE, COMPILES, BOOTS — but never renders a page.** This is where we
  are stuck. The producer steps, but the giant spine never commits and no page displays.
- M3 (nav + progress via charOffset), M4 (delete Background-C / section files), M5 (cache hygiene): not
  started.

### The three M2 bugs found on device (in order), and how each was "fixed"
1. **Preparing-deadlock (fixed, confirmed).** `renderFromContentBin` re-drew the "preparing" popup +
   `requestUpdate()` every tick; on X3 each `displayBuffer()` keeps `isRefreshPending()` asserted for the
   whole multi-second waveform, which gated `stepContentProducer` out → the frontier never advanced →
   deadlock. Fix: draw the popup ONCE, then idle so the producer runs. Device confirmed the producer
   then steps.
2. **Per-slice framebuffer borrow corrupts the build (mis-fixed).** We borrowed the framebuffer *per
   slice* — a spine build spans many slices and its `BuildState` expects a STABLE arena, so
   swap/return-per-slice broke it (`Failed to release extraction buffer block`). We "fixed" it by
   holding the borrow across the spine — but STILL got the LIFO release error. **Then we wrongly
   concluded borrow itself was incompatible and removed it.** (This is the core mistake — see §0/§4.)
3. **Heap-only can't fit the ring; release-always crashes elsewhere.** Running the producer from the
   heap: `Failed to allocate ZIP arena (33824 bytes, free=38616)` — the ring doesn't fit at the reader's
   ~38 KB resident free (the bench had ~48 KB, no UI). Switching to *release* the framebuffer got free
   to **115 KB** and the `Failed to allocate` errors vanished — **but the page still never displayed and
   the device rebooted**, i.e. a crash in the release→realloc→render transition we did not diagnose.

**Net: we have cycled through borrow / heap / release without matching master's exact working sequence,
and the last (release) variant traded the alloc failure for an undiagnosed crash-after-compile.**

---

## 3. Why we kept failing — the pattern

1. **We reinvented the producer's memory discipline instead of reusing master's.** Master's
   `IncrementalReleased` is a fully-worked borrow/release lifecycle with gates (`chooseSectionBuildMode`,
   `heapAllowsInPlaceBuild`) tuned on both stress books. We wrote a *new* lifecycle in
   `stepContentProducer` and it was subtly wrong each time.
2. **The bench (no UI) hid the reader-context constraints.** M1 passed with 48 KB free and no display;
   the reader has ~38 KB free and an X3 waveform that gates the producer. Every M2 bug was a
   reader-context interaction the headless bench could not surface (deadlock, tighter heap, arena LIFO).
3. **We chased the visible error, not the root sequence.** borrow→"fix"→heap→"fix"→release, each fixing
   the last symptom and exposing the next, rather than adopting master's proven whole.

---

## 4. Grounded comparison to the three role models

Ring size reference: inflate ring = 32 KB (`InflateReader.cpp:8`, sized `min(32768, max(size,512))`).
FreeInk's miniz path also uses a 32 KB window (`miniz.h:962`).

### 4.1 Master's `Section` build — COPE-WITH-MEMORY via TEMPORAL SEPARATION + framebuffer borrow

**COMPILE.** The decisive idea is **two-phase extract-then-parse so the ring and the parser working set
are NEVER co-resident** (`Section.cpp:576-582`, `:952-953`):
- Phase (a) extract: allocate `EntryReader` (readBuf + 32 KB ring) from a `zipArena`
  (`PARSE_CHUNK_BYTES + ringSizeFor(size) + align`, `:858-859`), inflate the WHOLE entry to a temp SD
  file, then **release ALL zip state** (`shrinkChunkAfterExtract; reader.reset; zip.reset; dropZipArena`,
  `:954-960`).
- Phase (b) parse: feed the temp file through an 8 KB `BufferedFileReader` in 1 KB chunks to the parser,
  whose working set is a 10 KB arena (`SCT_PARSE_ARENA_BYTES`, `:129-132`, `:990-1024`).

**Memory fit for the giant spine** = borrow the ~52 KB framebuffer as the arena
(`chooseSectionBuildMode`→`IncrementalReleased`, `EpubReaderActivity.cpp:2517-2548`): prefer
`borrowSecondaryBuffer()` → `BuildArena` → `setExternalBuildScratch` (`:2831-2835`); the ring is carved
from the borrowed region so the heap is untouched (`Section.cpp:863-866`). **Borrow is held for the
WHOLE build across all slices; returned only when `hasActiveBuild()` goes false** (`:2278-2280`,
`:2286-2295`). `heapAllowsInPlaceBuild` adds the ring to the floors so a giant spine routes straight to
released/borrowed (`:2481-2515`).

**RENDER.** Purely from the section file: `loadPageFromSectionFile()` seeks `lut[currentPage]` +
`Page::deserialize` — **no ZIP/epub/inflate open during reading** (`Section.cpp:1683-1704`). Per-settings
variant (a font change invalidates it).

### 4.2 FreeInk book — COPE-WITH-MEMORY via a TWO-ARENA SPLIT + a STORED-CHAPTER SHRINK

**COMPILE.** Slices via `ChapterLayoutSession::step(minNewPages)` (`ChapterLayout.cpp:1939-1967`).
Unlike master, FreeInk **stream-inflates while parsing** — inflate window + layout working set ARE
co-resident (`ZipCatalog.cpp:255-315`). It manages that peak two ways:
- **Two-arena split:** parse-side allocations (inflate window + decompressor + XML) come from a SEPARATE
  arena from the layout scratch, "so two ~50 KB blocks fit where one ~100 KB can't" on a fragmented heap
  (`ChapterLayout.h:125-134`).
- **Stored-chapter shrink:** if the chapter is a pre-extracted STORED (uncompressed) copy, resident
  parse state drops **~46 KB → ~8 KB** — this is what makes holding a session open *while rendering*
  viable (`ChapterLayout.h:157-185`). Images are pre-scanned once, probed sequentially, so only **one
  32 KB inflate window is ever alive** (`ChapterLayout.cpp:83-97, 1830-1911`).

**RENDER — the mid-build read, ONE handle.** `PageCacheWriter` serializes pages to a `.fibp` as laid
out; `readPage()` serves an already-written page mid-build via `storage_->readBackAt()` — a single
read-write open with **seek-read-seek-back on the SAME handle** (`PageCache.cpp:363-388`,
`BookStorage.h:54-65`). Page index is a 128-entry chunk list (`PageCache.h:86-91`), `kMaxPages=4096` so a
1174-page single spine is not special. **suspend()/isPartial()** persist a partial build + input-side
progress for instant reopen (`PageCache.h:74-79, 138-147`). Peak RAM is chapter-size-independent.

> Our `withReadableFile` (flush → save cursor → read → restore → rebase) is **exactly** FreeInk's
> `readBackAt` seek-read-seek-back on one handle. This part we got right.

### 4.3 microreader — COPE-WITH-MEMORY via a WINDOWED compiled source + per-paragraph cache (docs only)

Clean-room reference (`docs/pull-core-plan-microreader-guided-2026-07-27.md:7`). Compiles to a single
`.mrb` with a baked per-paragraph descriptor table (O(1) seek), splitting >8 KB paragraphs at WRITE time
to bound read-time memory (`docs/compiled-content-format.md:139-142`). Renders ONE page live from a
`PagePosition{paragraph, offset}` cursor with symmetric `layout()`/`layout_backward()` over a WINDOWED
on-demand `.mrb` reader — never a whole-chapter load (`pull-core-plan:29-53`). Key memory ideas: a
**page-independent per-paragraph cache** (a paragraph laid out once is reused by the page ending on it
AND the page starting on it), and a windowed source keeping only recent paragraphs resident. **The
compiled render path needs NO inflate at all** — same shape as master reading a section file.

### 4.4 Ours — the alignment table

| Aspect | master | FreeInk | microreader | OURS |
|---|---|---|---|---|
| Persisted artifact | paginated pages, per-settings section file | paginated pages, `.fibp` keyed by layout-gen hash | compiled `.mrb` (settings-independent) | compiled `content.bin` (settings-independent) ✓ |
| Ring vs parser | **temporally separated** (extract→release→parse) | co-resident, managed by 2-arena split + stored shrink | n/a at render (windowed) | **inherits master's extract-then-parse** (we reuse `stepSectionBuild`) ✓ |
| Giant-spine ring fit | **borrow framebuffer, held whole build** | 2 arenas + stored shrink | windowed source, no ring at render | **we FAILED to replicate the borrow** ✗ |
| Mid-build read | loadPageFromActiveBuild (growing section file) | readBackAt, ONE handle, seek-read-seek-back | windowed `.mrb` | withReadableFile, ONE handle ✓ (matches FreeInk) |
| Render source | section-file LUT, no ZIP | `.fibp` LUT, no ZIP | `.mrb` cursor, no inflate | content.bin cursor via layoutPage, no epub ✓ (matches microreader) |
| Settings change | recompile variant | new generation | relayout | **relayout, no recompile** ✓ (best of the three) |

**Reading of the table:** our *persisted-format* and *render* choices match the best of microreader
(settings-independent, cursor render, no recompile on settings change) and our *mid-build read* matches
FreeInk exactly. The ONE row we got wrong is the giant-spine ring fit — and it's the row master already
has a proven answer for that we failed to copy.

---

## 5. What we believe needs to be done (concrete next steps)

1. **Adopt master's `IncrementalReleased` lifecycle verbatim for the producer.** In
   `stepContentProducer`, replicate `buildSection`'s borrow sequence EXACTLY:
   `borrowSecondaryBuffer()` once when a spine build starts → wrap in `BuildArena` →
   `producer_->setExternalScratch(arena)` → hold across all slices → `returnSecondaryBuffer()` +
   `setExternalScratch(nullptr)` only when the spine finishes (`spineInFlight()` false). Do NOT release,
   do NOT re-borrow per slice. If master's borrow works and ours didn't, diff the two setups until they
   are identical (candidate divergences: we reset the arena between slices; we may not honor the LIFO
   release order the `EntryReader` extraction scope requires; the arena `BuildArena` may be constructed
   differently than `buildSection`'s).
2. **First reproduce the failure in the bench WITH a display**, or add a device probe that borrows the
   framebuffer for a producer spine build in isolation — so the borrow/LIFO bug is fixed without the
   full reader's noise (the headless bench cannot reproduce it).
3. **Diagnose the release-path crash (the reboot after free=115 K).** If we keep release as a fallback,
   the release→realloc→render transition crashed; capture the panic backtrace (the device writes
   `/crash_report.txt`; or catch the Guru Meditation over serial on COM6).
4. **Only after the giant spine COMMITS in-reader and a page renders**, proceed to M3/M4.
5. **Seriously weigh the alternative: compile in a PRE-READER indexing pass** (framebuffer free, heap
   ~110 K) so the reader only ever READS content.bin — no in-reader producer memory discipline at all.
   This sidesteps the entire borrow/release problem; the cost is a cold-open wait. This was raised
   before and deferred; given three failed in-reader memory attempts, it deserves a real comparison.

---

## 6. Immediate-merit-for-master cherry-picks (do these regardless of the fresh-reader outcome)

Onto a branch off `master`, in this order (all independent, all low-risk):
1. `e44655f4` ZipFile spine-stat cache (perf, unconditional).
2. `9f4ca076` tjpgd Windows build fix.
3. `f1bb1db7` FootnoteEntry memset (correctness).
4. `lib/Serialization/BufferedFileIO.h` + apply buffering to master's section-file serialize (perf).

These carry the concrete wins out of this effort even if the fresh reader is shelved.

---

## 7. Device/process gotchas learned (save the next session time)
- **C3 has two COM ports: COM6 is the real USB-CDC, COM3 is a fake Intel AMT SOL.** Always monitor COM6.
- **Native USB re-enumerates on reset/panic** — a monitor attached after boot misses setup() output.
  Reset+capture from boot, or persist a summary loop in benches.
- **A wedged USB-CDC needs a physical power-cycle** (unplug/replug); DTR/RTS soft reset won't recover it.
- **SdFat allows ONE handle per file** — the consumer must read THROUGH the producer's handle
  (`withReadableFile`), never open a second. Host FS allows two, so host tests miss this.
- **X3 keeps `isRefreshPending()` asserted for the whole waveform** — anything gated on it (the producer)
  starves if the reader re-renders in a tight loop. Draw popups once.
- `platformio.ini`: `[env:default_fresh]` = the fresh reader; `[env:default]` = untouched master reader.
