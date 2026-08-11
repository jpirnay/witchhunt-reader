# Memory Allocation Strategy (ESP32-C3 reader path)

Status: **architecture note**. Written 2026-08-09 after a run of local fixes (table
streaming, per-line copy removal) stopped one crash and exposed the next one. The point of
this document is to stop treating each OOM as its own bug and state the rule that decides,
for every allocation on the reader path, *where it comes from and when it goes back*.

The **RULE** statements in §2, §3 and §6 are the normative part — five code comments cite
this file, and all of them mean rule 4 ([BuildArena.h](../lib/Memory/BuildArena.h),
[PngStreamDecoder.h](../lib/PngDecoder/PngStreamDecoder.h),
[ImageBlock.h](../lib/Epub/Epub/blocks/ImageBlock.h),
[JpegToFramebufferConverter.cpp](../lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp),
[Section.cpp](../lib/Epub/Epub/Section.cpp)). Sections marked **TODAY** describe what the code
did on 2026-08-09; **TARGET** entries are proposals, and several have since been implemented
or disproved.

> **Read §8 and §9 before acting on any TARGET.** §8 records measurements that contradict
> parts of §3–§5, and §9 records what changed in the code afterwards. Where they disagree
> with the earlier sections, the later section wins. The disproved hypotheses are kept
> deliberately: each one cost real device time, and re-proposing one costs it again.

---

## 1. The budget we actually have

Measured from X3 boot logs (2026-08-09 serial capture):

| Checkpoint | free | largest block |
|---|---|---|
| `after_hw_init` | 215300 | 114676 |
| `after_display_fonts` | 87432 | 61428 |
| `after_activity_route` | 68344 | 42996 |

Fonts + display init cost ~128 KB and never come back. **The reader lives in ~65 KB free /
~43 KB contiguous.**

Framebuffer geometry is per-panel, sized to the *runtime* panel (`allocFrameBufferStorage`,
FreeInkDisplay.cpp:368-380), not to the largest linked one:

| Panel | W×H | one framebuffer |
|---|---|---|
| X3 (UC8253) | 792×528 | **52,272 B** |
| X4 (SSD1677) | 800×480 | **48,000 B** |

There are **two** of them (`frameBuffer0` + `frameBuffer1`, FreeInkDisplay.cpp:212-214) —
~104 KB on X3 — and together they are the largest heap consumer in the system. The 52,272
figure throughout this document is the X3 secondary; on X4 substitute 48,000. Note
`SECONDARY_BUFFER_BYTES = 52*1024` in the restart heuristic (EpubReaderActivity.cpp:3460) is a
hard-coded approximation, not the true runtime size.

Two consequences drive everything below:

1. **Contiguity, not total free, is the binding constraint.** The failure that ends a session
   is almost never "out of memory" — it is "cannot get 52272 bytes *in one piece*". The
   recovery reboot exists solely as a defragmenter (`maybeRestartForFragmentedHeap`,
   EpubReaderActivity.cpp:3455), and it fires with ~111 KB free.
2. **There is no compaction and no exceptions.** A failed `malloc` returns null and
   `-fno-exceptions` turns a `throw` into `abort()`. Every allocation must be a
   `makeUniqueNoThrow` / `new (std::nothrow)` that is *checked*.

---

## 2. Classify every allocation by lifetime

The current code has good machinery (`BuildArena`, the borrow protocol) applied
inconsistently, because there is no stated rule for which allocations belong where. Proposed
taxonomy — every allocation on the reader path is exactly one of these:

| Class | Lifetime | Where it must come from |
|---|---|---|
| **A. Permanent** | whole session | heap, allocated once at boot, never freed |
| **B. Resident** | one page / one section, survives across loop() ticks | heap, but bounded and O(1) in book size |
| **C. Phase-scoped** | one phase of one build, strictly nested | **arena** |
| **D. Per-item transient** | one image / one line / one cell, freed immediately | **arena** (currently heap — this is the bug class) |

**RULE 1 — Class D never touches the heap.** A repeating short-lived allocation inside a
long-running pass is precisely what fragments a no-compaction heap. It does not matter that it
is small or that it is freed promptly; freeing it does not un-fragment anything.

**RULE 2 — Class C and D allocations are LIFO, so a bump arena is sufficient.** `BuildArena`
already enforces this (`reserveBlock`/`release`, rejecting out-of-order releases). Anything
that cannot be expressed as LIFO is misclassified and is really class B.

---

## 3. The two regions, and why they are not interchangeable

This distinction is currently implicit and is the source of the remaining crashes.

**TODAY** — `FreeInkDisplay` exposes two very different operations on the same 52272 bytes:

- `borrowSecondaryBuffer()` / `returnSecondaryBuffer()` — the block **stays owned by the
  display** (`_secondaryLent`; "the block this points at is still owned by frameBuffer0/1",
  FreeInkDisplay.h:389-392). It never enters the heap, so **giving it back cannot fail.**
- `releaseSecondaryBuffer()` / `reallocSecondaryBuffer()` — the block is **freed to the heap**.
  Getting it back is an ordinary 52 KB `malloc` that **can and does fail**
  (`reallocSecondary -> 0 (contig=51188)`, needing 52272 — short by ~1 KB).

**RULE 3 — Prefer borrow over release. Release only when the heap genuinely needs those bytes
*as heap*.** Borrow is failure-free by construction; release stakes the session on winning a
52 KB contiguous allocation later.

The distinction is already understood in the reader — Background-B and Background-C both
borrow, and `chooseSectionBuildMode` tests `secondaryBorrowed_` *before* `hasSecondaryBuffer()`
(EpubReaderActivity.cpp:2716) precisely because a lent buffer must not be mistaken for an
absent one. There is even a B→C **ownership transfer** (`:2923-2926`) that avoids a
release/re-borrow round trip. The SDK enforces single-lend structurally
(`borrowSecondaryBuffer` returns null when `_secondaryLent != nullptr`, FreeInkDisplay.cpp:509)
and refuses `releaseBuffers`/`reallocBuffers` while lent (`:386`, `:408`).

So the rule is not "nobody knows about borrow" — it is that **the image warm pass is the one
major consumer still on the release path**, and it is the one that fails. `fallbackToReleased-
Rebuild` (`:1244-1252`) documents the legitimate reason to prefer release: a borrow yields only
phase-b reading heap (~62 KB), while a CSS-heavy blocking rebuild needs the full freed block
(~90 KB). That reasoning does not apply to the warm pass once its working set is bounded
(TARGET 2).

**RULE 4 — While the secondary buffer is released, nothing may perform repeated
allocate/free cycles.** The released region *is* the contiguous hole that must be handed back
whole. Any churn during that window is carving up the very block you have promised to
reconstitute.

The current image warm pass violates Rule 4 directly, which is the observed failure:

```
[101099] releaseSecondary      contig=57332   <- hole opened
[101984]..[109301]             7.3 s, 8 decodes over 4 images (3 JPEG + 1 PNG),
                               each taking and returning a 32 KB inflate ring (PNG)
                               or a 12 KB TJpgDec pool (JPEG), plus per-decode
                               object/row/ditherer/band traffic. Nothing reused.
[109302] reallocSecondary -> 0 contig=51188   <- hole no longer whole; short by ~1084 B
[109303] Fragmented heap recovery ... restarting
```

The dominant term is the **32 KB PNG inflate ring** and the **12 KB TJpgDec pool**, each
allocated and freed once per decode — sizes in the same order as the 52272-byte hole they are
being carved out of. Adaptive tone mapping adds a third pass per image (see §4), which is why
it correlates with the failure without being a leak: it is an amplifier of a Rule 4 violation,
not the cause. Anti-aliasing, which doubles the decode count, is a larger amplifier still.

---

## 4. Where each consumer lands

### Class A — permanent (heap, boot-time)
- Framebuffers: primary + secondary, 52272 B each. `EInkDisplay::begin()`.
- Font system / display init: ~128 KB, never returned.
- Settings, theme, recent-books, reading stats: small, bounded.

### Class B — resident (heap, must stay O(1) in book size)
- The current `Page` and, when armed, one pre-rendered page (`preRenderedPage`).
  `TextBlock` already flattens each line into ONE arena allocation
  (TextBlock.h) — this is the model the rest of the path should follow.
- `Section` LUT / TOC boundaries / page-break labels. Pages themselves **stream to disk**,
  so they are correctly *not* resident.
- CSS resident index when in arena mode; hot/negative caches (evictable — and already
  evicted under pressure by `reallocSecondaryEvictingCaches`).

### Class C — phase-scoped (arena — mostly correct today)
`Section::runBuildParse` already splits the build into two phases with **disjoint peaks**
(Section.cpp; floors documented at EpubReaderActivity.cpp:129-136):
- *extract*: inflate ring (≤32 KB, sized to the entry) + ~2 KB scratch, no layout state
- *parse*: layout working set (~20 KB), no ZIP state

This is the pattern to generalise: name the phases, give each a budget, and never let two
peaks overlap. `SCT_PARSE_ARENA_BYTES` (10 KB heap-owned) vs the 52,272 B borrowed region is
the same arena at two very different budgets — the borrowed case is what makes the whole
extract phase touch the heap "not at all" (Section.cpp:801-806).

Arena consumers today, and what each takes:

| Consumer | Budget | Mechanism |
|---|---|---|
| `Section::BuildState` chunk feed | 1 KB, grown to 8 KB during extract | `reserveBlock` + `alloc`, released after extract |
| ZIP inflate ring (shared scope) | `min(32768, max(inflatedSize,512))` | `alloc` inside `EntryReader`'s block |
| ZIP arena (heap fallback) | 1 KB + ring + 16 ⇒ ~1–33.8 KB | separate owning `BuildArena`, freed at end of phase (a) |
| CSS resident index + style pool | `ruleCount*8` + deduped pool | `reserveBlock` → `commit()` on success |
| CSS offset index (fallback) | `ruleCount*8`, ≤12 KB at the 1500-rule cap | `alloc` |

Notably **CSS arena mode is enabled exactly when the build runs in the borrowed framebuffer**
(`externalArena = st.arena && st.arena != st.ownedArena.get()`, Section.cpp:662-664). That is
the existing precedent for TARGET 1: the same seam, extended to the parser.

Three invariants already hold this together and must not be broken by new consumers:
declaration order (`buildScratch_` before `section`; `ownedArena` before `zip`/`reader`, with
teardown releasing into the arena), `BuildArena::release` being newest-first only, and
`initArena()` calling `external->reset()` so a build never inherits a previous cursor.

### Class D — per-item transient (**arena — heap today; the gap**)
These are the allocations that repeat inside a long pass. All are currently heap:

| Allocation | Repeats per | Size | File |
|---|---|---|---|
| **PNG inflate ring** | **every** PNG decode *and* every PNG tone analysis | **32 KB** (`ringSizeFor`, capped at `INFLATE_DICT_SIZE`) | InflateReader.cpp:31 via PngStreamDecoder.cpp:221 |
| TJpgDec work pool | JPEG decode **and** each JPEG tone analysis | 12 KB (`TJPG_WORK_POOL_SIZE`) | JpegToFramebufferConverter.cpp:268,912 |
| ZIP extract ring | lazy extraction of each image | ≤32 KB + 2×1 KB | ZipFile.cpp:631,637,651 |
| PNG row buffers ×2 | PNG decode | `rawRowBytes_` each (~2×1.9 KB @482px) | PngStreamDecoder.cpp:152-153 |
| `PngStreamDecoder` object | PNG decode | ~3.1 KB + uzlib state | PngToFramebufferConverter.cpp:150,214 |
| `Atkinson1BitDitherer` | mono decode | `(dstWidth+4)*6` (~2.9 KB @482px) | BitmapHelpers.h:68-80 |
| `PixelCache` band buffer | image decode | ~2.0–2.6 KB in practice, raw `malloc` | converters/PixelCache.h:107 |
| tone histogram | analysed image | 1 KB (256 × uint32) | JpegToFramebufferConverter.cpp:913 |
| `renderFromCache` read buffer | **every replay of a cached image** | ≤4 KB | ImageBlock.cpp:180 |
| `Page` / `ParsedText` / per-word `std::string` | page / block / **word** | small, very many | ChapterHtmlSlimParser.cpp:556,876 |

Two corrections to the obvious reading of these numbers, both measured:

- **`MAX_BAND_BYTES` (24 KB) is effectively dead as a limiter.** It only binds when
  `bytesPerRow > 1365`, i.e. width > ~5460 px. Real band sizes are ~2.0–2.6 KB. The band is
  *not* the churn problem.
- **The PNG inflate ring is.** `ringSizeFor` returns `min(32768, max(uncompressed, 512))`, and
  any real page image exceeds 32 KB uncompressed, so it is **a flat 32 KB malloc/free per
  decode** — and PNG tone analysis is a *full second inflate of the entire image*
  (PngToFramebufferConverter.cpp:168-170; `ROW_STEP=4` saves only the histogram tally, not the
  decode). JPEG's tone pass is cheap by comparison (1/8 DCT scale) but still re-allocates the
  full 12 KB pool.

**Nothing is reused between consecutive decodes in one warm pass.** Every allocation above is
scoped to a single `decodeToFramebuffer`/`analyzeAdaptiveTone` call and freed before the next
image starts. For one PNG with AA + tone on, that is **three separate 32 KB ring cycles**
(BW decode, tone analysis, grayscale decode) plus the object/row/ditherer traffic around each.

Decode counts per image, cold (`Page::warmImageCaches`, Page.cpp:305-334):

| AA / tone | full decodes | extra tone passes |
|---|---|---|
| AA off | 1 (BW) | 0 |
| AA on, tone **off** | 2 (BW + grayscale) | 0 |
| AA on, tone **on** | 2 (BW + grayscale) | **+1** (grayscale only — BW is deliberately untoned) |

So AA is what doubles the decode count; the tone filter adds a *third* pass on top, and only
on the grayscale decode. This is why tone mapping correlates with the failure without being a
leak — it is the third 32 KB ring cycle per image, not extra retained memory.

Note `PixelCache` uses a raw `malloc`, not `makeUniqueNoThrow`, so it is also outside the
project's OOM-handling convention.

**`ChapterHtmlSlimParser` has zero arena awareness** — `grep -c arena` returns 0 — even though
it runs with a 52 KB arena borrowed right beside it. Every `Page`, every `ParsedText`, and
every per-word `std::string` it produces goes to the general heap. Measured on
`test_kerning_ligature`: 7676 words vs ~20124 allocations, i.e. the parse path is dominated by
per-word churn (see [[parsedtext-alloc-churn-measured]]).

---

## 5. Target architecture

> **Measurement update (2026-08-09).** TARGET 3 was implemented and **reverted** — see §8. A size
> histogram then showed that **89% of build-path allocations are ≤128 bytes**, which makes a bump
> arena the wrong instrument for TARGET 1 as originally written. Read §8 before acting on
> TARGET 1 or 3.

**TARGET 1 — One arena per build, threaded all the way down.**
`Section` already owns a `BuildArena` and hands it to `CssParser` (`setIndexArena`) and
`ZipFile::EntryReader`. Extend the same seam to `ChapterHtmlSlimParser` → `ParsedText` →
per-word storage. The parser is the largest consumer of class-D allocations and the only major
one still entirely on the heap.

Note the measured caveat: `ParsedText`'s *DP scratch* vectors are NOT worth arena-backing —
blocks are hard-capped at 97 words, so those are ~1300–1600 allocations of 20124, bounded and
LIFO-clean. The win is in **word storage**, not the layout scratch.

**TARGET 2 — Image decode gets a phase arena, allocated once per warm pass.**
Today each decode mallocs and frees its own inflate ring (32 KB, PNG), work pool (12 KB, JPEG),
row buffers, ditherer, band and histogram — and **nothing is reused between images**. Instead:
size one scratch block for the worst case in the pass, allocate it once, and let every decode
`reserveBlock`/`release` inside it. The allocations are already strictly nested per decode, so
they fit `BuildArena`'s LIFO model without restructuring.

The single highest-value piece is the **inflate ring**: it is the largest per-decode block, it
is a flat 32 KB for every real image, and `InflateReader` already supports an externally
supplied ring (`ownsRing_`, InflateReader.cpp:30-35) — the same seam `ZipFile::EntryReader`
uses to allocate from an arena. Extending that to `PngStreamDecoder` is a contained change
that removes the dominant term.

**TARGET 3 — Make the borrow/release choice explicit and prefer borrow.**
The warm pass currently runs on the *released* path specifically to get headroom
(EpubReaderActivity.cpp:2830-2845 — "it needs the freed headroom"). With TARGET 2 its working
set becomes one bounded block, which is exactly what a borrow provides. Borrowing instead of
releasing removes the failure mode entirely, because `returnSecondaryBuffer()` cannot fail.

**TARGET 4 — Budgets are declared, not discovered.**
`BuildArena`'s header already states the intent: "either the arena fits the budget or the build
refuses to start ... mid-build OOM/fragmentation surprises cannot occur." The reader path
currently has ~8 different free-heap floors (`BG_BUILD_PARSE_MIN_FREE_HEAP_BYTES`,
`IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES`, `MIN_FREE_HEAP_FOR_CSS`, `MIN_FREE_HEAP_FOR_TABLE`,
`EHP_TEXT_LAYOUT_*`, `PRE_RENDER_MIN_FREE_HEAP_BYTES` …). Each is a symptom of allocations
whose size is not known up front. Every allocation moved into a named arena budget lets one of
these gates be deleted.

---

## 6. Invariants to hold the line

1. No `malloc`/`free` pair inside a loop that runs while the secondary buffer is released.
2. Every class-D allocation names the arena it comes from, or has a comment saying why it
   cannot.
3. Heap gates go on the **producing** side, not the consuming side. (The table fix moved the
   check from `emitBufferedTable` — after the whole table was resident — to `<td>` as cells
   accumulate. Same bug shape as the warm pass: check before you spend, not after.)
4. Any new resident (class B) state must be O(1) in book size.
5. Prefer borrow to release; document the reason whenever release is chosen.

---

## 7. Open questions

- Should the primary framebuffer also be lendable during builds? It is another 52 KB, and the
  blocking path already releases both before the pre-reboot warm pass.
- Can the two `.pxc` variants (mono + tone-mapped) be produced in a **single** decode pass?
  That halves warm-pass work and is worth doing independently of the arena change.
- Is `SCT_PARSE_ARENA_BYTES = 10 KB` the right heap-owned fallback, given the borrowed path
  gets 52 KB? A build that succeeds when borrowing and fails when not is a latent
  inconsistency.

---

## 8. What the device and the allocator actually showed (2026-08-09)

This section records measurements that **contradict** parts of §3–§5 above. They were taken
after implementing TARGET 2 and TARGET 3; the earlier sections are left intact so the reasoning
trail is visible, but where they disagree with this section, this section wins.

### 8.1 TARGET 3 (reorder) was implemented and reverted

Reclaiming the framebuffer before the warm pass was premised on the warm pass being the
fragmenter. On device, with the reorder in place:

```
[52876] releaseSecondary       contig=53236   <- hole opened
[52890]..[53746] createSectionFile             (no image decode at all)
[53747] reallocSecondary -> 0  contig=25588   <- failed before the warm pass ever ran
```

Compared with the pre-reorder firmware, which reached `contig=51188` *after* the warm pass. So
the build alone takes contig from 53236 to 25588. Mid-build the parser logs
`Low heap (50380 free, 11764 max alloc)` — ample free heap, largest block already down to 11 KB.

**Conclusion: `createSectionFile` is the fragmenter, not the image warm pass.** The reorder was
reverted; it only moved the failure earlier and cost the images their cache. TARGET 2 (the
decode scratch arena) was kept — it is correct and tested, just aimed at the wrong hotspot.

Relevant asymmetry: the blocking build logs `Loaded CSS index: ... hot cache size=128` (the heap
path), not `CSS RESIDENT in arena`, because a blocking build has no borrowed arena to lend.

### 8.2 The allocation size histogram

`epub_pipeline_dump --bench` now emits `BENCHMARK alloc_sizes`. On test_kerning_ligature
(1140 lines, 45 pages, 19863 allocations):

| size | count | share |
|---|---|---|
| ≤16 B | 4404 | 22% |
| ≤32 B | 5849 | 29% |
| ≤64 B | 2869 | 14% |
| ≤128 B | 4479 | 23% |
| **≤128 B total** | **17601** | **89%** |
| ≤1 KB | 253 | 1.3% |
| ≥8 KB | 57 | 0.3% |

Two hypotheses from §4 are now **measured false**:

- *"per-word `std::string` dominates"* — no. **92.3% of words are ≤15 bytes** and live in SSO,
  never touching the heap. Only 588 of 7676 words allocate.
- *"the big decode blocks dominate"* — no. Everything ≥8 KB is 57 of 19863 allocations.

What the ≤128 B mass actually is: per line the build does `make_shared<TextBlock>` (control
block + object + its arena) and `make_shared<PageLine>` (control block + object), plus an
`elements` entry. The reader's `TextBlock::deserialize` pays the same shape on read-back.

### 8.3 What this changes

**A bump arena is the wrong instrument for 128-byte objects.** `BuildArena` earns its keep on
large phase-scoped blocks (inflate ring, CSS index, decode scratch) and those are already
covered. TARGET 1 as written — threading the arena into `ParsedText`'s layout scratch — would
address a few hundred allocations out of ~20000.

The shapes that would actually move the ≤128 B mass:

1. **Drop `shared_ptr` for `unique_ptr` on the line path.** `TextBlock` is never shared —
   `getBlock()` is only ever dereferenced, never copied into a longer-lived owner. That removes
   one control block per line on both build and read-back. Touches `Page`/`PageLine`/`TextBlock`
   ownership and the `processLine` callback signature.
2. **A fixed-size pool** for `PageLine`/`TextBlock`, page-scoped (they all die together at
   `emitPage`, which is a genuine LIFO boundary — `onPageComplete` takes the page by value and
   destroys it on return).

Both are wider than anything attempted so far. Neither should start before §8.4.

### 8.4 The link that is still unproven

**No measurement yet shows that allocation *count* is what collapses the contiguous block.**
`50380 free / 11764 max alloc` is consistent with small-object churn, but consistent is not
demonstrated. Before committing to either change above, log `contig` at several points inside
`createSectionFile` on device and find where the block actually collapses. That is a few log
lines and it decides whether the target is the small objects or something else entirely.

### 8.5 Landed from this round

- **TARGET 2** — `image_scratch` pass arena; PNG ring + scanline buffers and the JPEG work pool
  drawn from it. Correct and covered by `test/png_decoder` (5 tests), but aimed at a hotspot
  that turned out not to be the bottleneck.
- **`Page::elements` reserve** — each page reserves from the previous page's element count
  instead of growing 1→2→4→…→N. Measured 19992 → 19863 allocations (−129); small, as 45 pages
  implies.
- **Instrumentation** — `heapTrackAllocCount()` and `heapTrackSizeHistogram()`; the histogram is
  what corrected §4.

---

## 9. Current state (2026-08-11, master `1d8eaf87`)

Where §3–§5 disagree with this section, this section wins. §8 remains the authority on what was
*measured*; this one records what the code does now.

### 9.1 Rule 3 is satisfied on both background build paths

Both background builds now **borrow** rather than release, which is what §3 asked for:

- **Background B** — `beginBackgroundBorrow()`
  ([EpubReaderActivity.cpp:918](../src/activities/reader/EpubReaderActivity.cpp#L918)) is B's
  *preferred* path, tried before any heap-backed floor is consulted. The borrow spans slices, and
  `endBackgroundBorrow()` tears the build down **before** handing the region back, because the
  build allocates inside it.
- **Background C** — prefers the borrow and falls back to `releaseSecondaryBuffer()` only when
  there is no buffer to lend
  ([:3188-3201](../src/activities/reader/EpubReaderActivity.cpp#L3188)).

The failure-free property matters more than the headroom: `returnSecondaryBuffer()` cannot fail,
so the realloc-failure / heap-recovery-restart class of bug is structurally impossible on these
paths.

### 9.2 Two release windows remain, and they are not equal

| Window | Site | Measured contig cost |
|---|---|---|
| **Blocking build** | [EpubReaderActivity.cpp:2932](../src/activities/reader/EpubReaderActivity.cpp#L2932) | 53236 → 25588 (§8.1, **no image decode at all**) |
| Reader per-page image warm | [:3760](../src/activities/reader/EpubReaderActivity.cpp#L3760) | 57332 → 51188 (§3 trace, 8 decodes) |

The build is roughly **5× the warm pass**. §8.1 concluded "`createSectionFile` is the fragmenter,
not the image warm pass" — and the trace it was taken from is the blocking path specifically.

There is a structural reason, and it is the sharpest inconsistency left in the system:
`setExternalBuildScratch()` is never called on the blocking path, so `externalArena` is false
([Section.cpp:746](../lib/Epub/Epub/Section.cpp#L746)). That single flag decides three things —
CSS resolves from the heap instead of the arena, the ZIP scope allocates its own entry-sized
`zipArena` (~33 KB) instead of sharing the main one
([Section.cpp:889](../lib/Epub/Epub/Section.cpp#L889)), and the arena is
`SCT_PARSE_ARENA_BYTES` = **10 KB**.

**The path that just freed 52 KB is the path that builds with the smallest arena**, and it carves
its own allocations out of the very hole it must hand back. On X3 this is not an edge case:
`chooseSectionBuildMode()` returns `IncrementalReleased` unconditionally
([:2865](../src/activities/reader/EpubReaderActivity.cpp#L2865)), so every blocking fallback —
after a failed C attempt, after a CSS-degraded rebuild — lands here.

**Do not "fix" this by re-allocating the freed bytes back as a heap arena.** That is the borrow
with two extra heap round-trips bolted on: a borrowed block never leaves the display's ownership,
so reclaiming it is a flag flip that cannot fail, whereas free-then-malloc goes through TLSF twice
and only *hopes* to get the same region back. Strictly worse, no benefit. (Rule 3 already says
this; it is restated because the mistake is an easy one to make while looking at §9.2.)

The real question is why this path releases at all, and the answer is one recorded claim
([:1319-1322](../src/activities/reader/EpubReaderActivity.cpp#L1319)): *"borrow only gives phase-b
reading-heap (~62 KB), so CSS-heavy books that need the full freed block (~90 KB) fall back
here."* That claim is circular as written — a released build has no arena, so `externalArena` is
false, so CSS resolves **from the heap**, and *that* is what needs ~90 KB. A borrowed build
resolves CSS out of the arena with the hot cache off. The 90 KB is a requirement of the
configuration the release itself creates.

It is not imaginary, though: the fallback is only reached *after* a borrowed build already failed.
So the open question is narrow and decisive — **when a borrowed CSS build fails, is the ARENA out
of room or the HEAP?** `BuildArena` exposes the pair that answers it, and
`fallbackToReleasedRebuild` now logs `highWater` / `capacity` / `failedAlloc` / free / contig
before the teardown destroys the arena.

- `failedAlloc != 0` → the arena genuinely ran out; the release is justified, size the arena.
- `failedAlloc == 0` with `highWater` well under capacity → the arena was never the constraint,
  the failure is heap-side, and the release is treating the wrong cause. The blocking release
  path can then be deleted rather than protected — and the largest measured fragmenter in the
  reader goes with it.

**Device measurement, X4, 2026-08-11.** No build failed in the trace, so the fallback log did not
fire — but the ordinary done-telemetry already makes the case, on one book, two spines:

| spine | arena | capacity | highWater | failedAlloc | CSS |
|---|---|---|---|---|---|
| 0 | heap-backed (`ownedArena`) | 10240 | **9224 (90%)** | 0 | `Loaded CSS index … hot cache size=128` (heap) |
| 1 | borrowed framebuffer | 48000 | 28652 | 0 | `Loaded CSS RESIDENT in arena` |

**The borrowed build's working set is 2.8× what the heap-backed arena can hold**, on a spine of
only 16340 bytes. A build with no external arena therefore spills ~18 KB into the heap and runs
with its 10 KB arena already 90% full — while a borrowed build of the same shape sits at 60% with
room to spare. That is §9.2's mechanism, measured, and it is why `SCT_PARSE_ARENA_BYTES` being
10 KB against the borrowed path's 48 KB is a latent inconsistency rather than a tuning detail.

**But the arena is not the whole story, and §8.4's open question now has an answer.** §8.4 asked
for `contig` logged at several points inside `createSectionFile` to decide whether small-object
churn is what collapses the contiguous block. The X3 trace does exactly that, on a build where
the arena was working perfectly (`cap=52272 highWater=28656 failedAlloc=0`, `CSS RESIDENT in
arena`, ring and chunk both inside):

```
spine_page=2   free=39920  contig=36852
spine_page=3   free=29788  contig=22516
spine_page=9   free=24532  contig=21492
spine_page=13  free=23168  contig=19444
spine_page=16  free=22088  contig=15348      <- 40948 -> 15348 across one parse
```

Free oscillates between 20 and 40 KB — bytes are coming back — while contig ratchets down and
never recovers until the build ends (it returns to 31732 afterwards). That is fragmentation, not
retention, and **everything §9.2 proposes moving into an arena was already in one.**

There are two distinct effects in that trace, and only one of them is churn.

**The gradual half is churn**, as §8.2 predicted: `freeBlk` climbs 16 → 35 across the parse while
`allocBlk` oscillates 357–516 with no trend. Same bytes, more pieces. That is the per-line
`make_shared<TextBlock>` / `make_shared<PageLine>` traffic, and it is what §8.3's two candidate
shapes address.

**The cliff is not.** Contig halves in a single page, which churn cannot do.

### 9.2.1 The cliff is a render interleaving with a build (measured 2026-08-11, X3)

The first suspect was `EpubImageManifest::ensureResolved`, which constructs a `ZipFile` on the
first image miss and deliberately leaves it open for the whole build — a large block taken
mid-parse, the eliminated-#8 shape. A probe was added to attribute it. **It is not the cause:**

```
retain[ZipFile ctor] allocBytes+72  blk+2 free-104 contig 30708->30708
retain[zip open    ] allocBytes+80  blk+1 free-96  contig 30708->30708
retain[resolve net ] allocBytes+220 blk+5 free-300 contig 30708->30708
retain[zip close   ] allocBytes+0   blk+0 free+0   contig 12276->12276
```

220 bytes, and contig does not move. The manifest is exonerated; the ZipFile is cheap because it
caches offsets, not content.

What the same trace *does* show, at the page boundary where contig collapses:

```
spine_page=2  free=48888 contig=30708  allocBytes=207292
  Prewarm: 43 glyphs in 3525 bytes from 2 groups
  Prewarm: 31 glyphs in 3764 bytes from 2 groups
  Prewarm:  5 glyphs in  346 bytes from 1 groups     <- pageBuf=7635 peakTemp=10555
  triggerWithRefreshCycle: fast -> waveform wait -> display
  Low heap (35312 free, 8692 max alloc) before paragraph layout
spine_page=3  free=32532 contig=8692   allocBytes=223136   (+15844)
```

A **foreground page render interleaved with the live build** — the `SectionBuilding` pass drawing
a page out of the partial build, with its font prewarm. Those allocations, made while the build's
working set is resident, split the largest free block 30708 → **8692**. It never recovers: every
later page in that build logs `contig=8692`, and the parser spends the rest of the parse in
`continuing in degraded mode`. The same step appears at the same boundary in the previous run, so
it reproduces.

This is the handover's "known and unsolved" font-prewarm transient with the missing condition
attached: **alone it is churn; against a build's resident working set it is the allocation that
splits the region.** §9.4's verdict (the per-call buffer is low value) was argued on transient
*size*; this is an argument about *placement*, which is a different question and the one
eliminated #8 says actually decides.

### 9.2.2 Which is also why Background-B never runs

```
gate=bgB_waitheap REJECT free=58572(floor=49152) contig=18420(floor=24576)
```

B now fails on **contig**, not free. Contig recovers only to 18420 after the build, against a
24576 floor. So §9.7 and this section are one story, not two:

> render interleaves with build → contig 30708 → 8692 → never recovers past 18420 →
> B's contig floor is unreachable → B never pre-builds.

### 9.2.3 Releasing the page slots: measured, partial (X3, 2026-08-11)

Three directions were on the table. (1) *Skip prewarm for mid-build pages* is **dead**: prewarm
costs ~50 ms and the fallback it avoids is measured at **5642 µs/glyph** against 1 µs prewarmed,
so a mid-build page would render slower than the popup it replaces. (2) *Stop drawing mid-build
pages* deletes the feature. (3) *An arena for the prewarm* was deferred in favour of the cheaper
test of the same hypothesis, since if placement is the problem, giving the bytes back before the
build resumes is enough — and if it is not, the arena would not have helped either.

`040b2c1b` releases the font page slots at the end of `displayBuildPage`. Result:

| | before | after |
|---|---|---|
| `allocBytes` step across the draw | **+12276** | **+3244** |
| `spine_page=3` | free 32532, contig 8692 | free 41928, contig 18420 |
| `spine_page=7` | free 28016, contig 8692 | free 38096, contig 18420 |
| `after_parse` | free 32896, contig 8692 | free 41868, contig 14836 |
| `continuing in degraded mode` | throughout the parse | **gone** |

The retention step fell by ~9 KB, matching the page-slot size (pageBuf 7635 + pageGlyphs 1264)
almost exactly, so the mechanism is confirmed. Free heap is ~10 KB higher at every comparable
page and the parser no longer runs degraded — a real quality win independent of contig.

**But it is not the whole cliff.** With the slots released, contig *still* steps 26612 → 16372
across the draw, and B still fails its floor (`contig=20468(floor=24576)`).

**Confound, stated plainly:** this run entered the build at `after_setup contig=38900`, the
comparison run at `47092`. Run-to-run variance in the starting block exceeds the effect being
measured, so the free-heap and degraded-mode results are trustworthy and **the net contig
comparison is not**. Any future attempt here needs runs that start from the same contig, or a
delta measured strictly across the draw rather than end-to-end.

### 9.2.4 The draw, fully attributed (X3, 2026-08-11)

`07bcc22a` bracketed each step of the draw. One trace settles it:

| step | Δcontig | ΔallocBlk | ΔallocBytes | what |
|---|---|---|---|---|
| page load | −6144 | +155 | +6428 | `Page::deserialize` |
| prewarm | **−9216** | +6 | +9028 | 3 × (slot buffer + glyph table) |
| render | −5120 | +2 | +5032 | scaled-glyph cache |
| slot release | **0** | −6 | **−9028** | `clearCache()` |

**The last row is the finding.** The release returns every byte the prewarm took (−9028 against
+9028), every block (−6 against +6) and 9124 of free heap — and `contig` does not move at all,
22516 → 22516. Bytes come back; contiguity does not. **The damage is placement**, so releasing
sooner cannot fix it: by the time the slots go, the render's allocations sit above them and the
9 KB hole they leave is not the largest block. This is the evidence that justifies an arena, as
opposed to the earlier assumption that it would help.

Two corrections to §9.2.3's reasoning fall out of the same trace:

- The transient peak theory was wrong about *which* transients. `Page::deserialize` is 155 small
  allocations as predicted, but it still costs 6144 of contig — small allocations fragment too.
  And the prewarm's damage is the six *persistent* slot blocks, not the group buffer.
- The render step's 5032 bytes are `SCALED_GLYPH_MAX_ENTRIES 80 × 16` + `SCALED_GLYPH_ARENA_BYTES
  3584` = 4864 plus headers: the **scaled-glyph cache**. It is allocated *lazily on first scaled
  glyph* and never freed — a permanent block taken mid-session, which eliminated #8 proved is the
  worst possible shape. Nobody chose that; it is what "allocate on first use" does when first use
  happens to fall inside a build.

Ranked by cost, with the cheapest fix first:

1. **Scaled-glyph cache, −5120.** Allocate it at a stable point (reader entry / renderer init)
   instead of on first use. It is permanent either way, so fixing its placement fixes it for the
   whole session rather than per draw. Smallest change here by a wide margin.
2. **Font page slots, −9216.** The arena case, now evidenced rather than assumed. Six clean
   allocations, strictly scoped to one draw, and the borrowed region they would come from is
   sitting mostly unused (`highWater` 12864–28656 of 52272).
3. **Page load, −6144.** §8.3's `PageLine`/`TextBlock` pool territory; the largest change and the
   one to attempt last.

Caveat on the whole section: the trough is mid-build and recovers, and no trace yet has a
pre-change baseline beside it, so treat 40948 → 15348 as the shape of the problem rather than a
regression signal.

### 9.3 `image_scratch` is installed in exactly one place

[Section.cpp:1647](../lib/Epub/Epub/Section.cpp#L1647), inside `warmAllImageCaches`. The reader's
per-page warm at [:3765](../src/activities/reader/EpubReaderActivity.cpp#L3765) never installs it,
so `image_scratch::get()` returns null and every decoder falls back to the heap — inside the
released window. TARGET 2 is built and tested but only half-wired.

Both gates that would refuse a decode under a borrow **double-count blocks the arena already
serves**: `MIN_FREE_HEAP_FOR_JPEG` is literally `TJPG_WORK_POOL_SIZE + 16 KB`, and
`PNG_DECODE_HEAP_FLOOR` is 36 KB against a 32 KB ring the arena supplies. Making them
arena-aware is a precondition for borrowing here, not an optional extra — otherwise the borrow
silently stops `.pxc` caching and every page re-decodes.

**Both are now arena-aware** (`image_scratch::canServe`), and the per-page warm borrows instead of
releasing. Device-measured on both panels, 2026-08-11:

| | X4 (48000 B) | X3 (52272 B) |
|---|---|---|
| image pages | 4 | 3 |
| borrow/return pairs | 4 clean | 3 clean |
| release fallback fired | never | never |
| `contig` render_start → after_bw_render | **flat 40948** | **flat 31732** |
| `.pxc` written | every decode | every decode |

Against the §3 trace, where the warm pass took contig 57332 → 51188 and the following
`reallocSecondary -> 0` was the restart. `reallocSecondaryEvictingCaches()` no longer runs on
image pages at all, so the font-cache eviction and the fragmented-heap restart branch behind it
are unreachable from this path.

**The gate fix is load-bearing, not tidying.** It is what pays for the borrow: releasing handed
the decoder ~52 KB of extra *free heap*, borrowing does not, so the floors had to stop charging
for blocks the arena serves. X3 decoded at `free=48432-49584` against the old 53248 cache floor —
without the discount every image would have skipped its `.pxc` and re-decoded on every visit,
forever. The two changes only make sense together.

### 9.4 The font group scratch is ELIMINATED, not pending

`09b72990` implemented a shared group scratch; `bea07a3d` reverted it on device numbers
(contig 34804 → **13300**, CSS `lowHeapSkips` 11 → 317, pre-render permanently rejected). A
permanent 16 KB block taken mid-session pins the largest free region, and moving it to `init()`
starves the section builds instead.

**The per-call variant is also low-value**, which is worth recording so the "unexplored angle"
note is not mistaken for a recommendation: `09b72990` measured 5 transient allocations over 3
prewarm calls per page, so per-call reuse gives 5→3 — at `max(size)` each, since one buffer must
fit the largest group in the call. And `decompressGroup` allocates nothing between the `malloc`
and the `free` (`InflateReader::init(false)` is non-streaming), so the pairs have nothing
interleaved and coalesce cleanly. Treat the per-group churn as **accepted** unless a measurement
says otherwise.

### 9.5 Still class D, still heap

| Allocation | Site | Note |
|---|---|---|
| `PixelCache` band | [PixelCache.h:107](../lib/Epub/Epub/converters/PixelCache.h#L107) | raw `malloc`, outside the nothrow convention |
| Atkinson ditherer rows | [BitmapHelpers.h:72](../lib/GfxRenderer/BitmapHelpers.h#L72) | `new[]`, ~2.9 KB/decode |
| `PngStreamDecoder` object | PngToFramebufferConverter | ~3.1 KB/decode |
| Cached-image replay buffer | [ImageBlock.cpp:188](../lib/Epub/Epub/blocks/ImageBlock.cpp#L188) | ≤4 KB, per image **per render** |

All four are strictly nested inside a decode, so they fit `BuildArena`'s LIFO model with no
restructuring — they are simply not wired to it yet.

### 9.6 Three build-scoped vectors grow without `reserve()`

`st.lut` (4 B/page), `paragraphLutPerPage`
([ChapterHtmlSlimParser.h:302](../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h#L302), 8 B/page)
and `anchorData` (~28 B + optional heap string, capped at 1024) all `push_back` with no
reservation. The sizes are modest; the *pattern* is a doubling ladder of allocate-copy-free
interleaved through the whole parse, which is the fragmentation shape this document exists to
stop. `st.inflatedSize` is known at setup, so a conservative estimate is available. CLAUDE.md's
Resource Protocol rule 7 already requires this.

### 9.7 Background-B does nothing during active reading, on both devices

The gate trace was added to find floors no reachable heap state can satisfy. The 2026-08-11
captures have one on each panel — *different* floors, same root cause:

| | rejecting gate | floor | free while reading | B outcome |
|---|---|---|---|---|
| X4 | `bgB_cssResident` | 73728 | 57-60 KB (peaks 71080 at boot) | `runs=16 completes=2`, parked in `waitheap` |
| X3 | `bgB_embeddedCss` (`heapAllowsEmbeddedStyle`, 56 KB) | 57344 | 50-54 KB | `runs=16 completes=1`, parked in `waitheap` |

X4's floor is *unreachable* — free heap never touches 73728 on that device even at boot. X3's is
merely never met while reading. Either way, on a CSS book Background-B stops pre-building after
the first section or two.

The borrow is supposed to make this moot, and does when it is reachable — X3 spine 1 built
borrowed with `CSS RESIDENT in arena` and `failedAlloc=0`. But the borrow is tried only after
`BG_BUILD_BORROW_QUIET_MS` (4 s) with no page turn, which a reader turning every 1-2 s never
clears, so B falls through to heap floors it cannot meet. X3 caught the one time it did fire:

```
Background-B: borrowed secondary buffer for spine 2 build (52272 bytes, free=54144)
Background-B: returned secondary buffer (spine 2, build discarded, preemptions=1)
```

Borrowed, got two pages in, lost the race to a page turn, discarded. Then nothing for the rest of
the session.

**Two of the three causes are now fixed and device-confirmed (X3, 2026-08-11).**

*A preempted build no longer loses its work* (`67a2ee4f`). `abortSectionBuild` kept the banked
XHTML only when `reusedHtml`, so every preempted attempt discarded a **finished** extraction and
the retry re-inflated from scratch — the exact asymmetry that made preemption expensive. Now keyed
on `extractDone`. Confirmed: a `build discarded, preemptions=1` immediately followed by
`createSectionFile spine=1 reusing cached HTML (16340 bytes)`.

*B no longer takes a buffer it is about to lose* (`4a302744`). The 4 s quiet window measured from
`lastPageTurnTime`, which is stamped only by actual turns and initialises to 0 — so before the
reader's first turn it was **vacuously satisfied**. B took the buffer 731 ms after a cold open and
lost it 513 ms later. Now measured from `lastPageOnScreenMs_` (any page reaching the screen, seeded
at `onEnter`), and B also declines while `CooperativeAbort` reports queued input.

Result on the same book and panel: **`preempt=0` for the whole session and no `build discarded`
lines at all**, with B still borrowing during genuine pauses — four 1 Hz `waitheap` rejects after
the last page settled, then a clean borrow and build of spine 2.

**What remains is throughput, and it is downstream of §9.2.1.** While the reader is active B
correctly declines the borrow and falls through to the heap-backed floors:

```
gate=bgB_waitheap REJECT free=60560(floor=49152) contig=23540(floor=24576)
```

Free passes with 11 KB to spare; **contig misses by 1036 bytes** — because the render/build
interleave has already taken it 40948 → 23540 and it never recovers. So B's remaining idleness is
the contig cliff, not its own gating, and fixing the cliff should let the heap-backed path pass
instead of B depending entirely on 4-second pauses.

The third original candidate — re-deriving the floors — is deliberately **not** done. They are
sized for a heap-backed build whose ~28 KB working set the 10 KB `ownedArena` cannot hold, so
lowering them would admit builds that then run degraded. Fix the arena or the cliff, not the
thermometer.

### 9.8 Landed since 2026-08-09

- **PR #124** — CSS arena gate exemption, table-cell size bound, zip ring sizing, CSS parse churn
  −61%, heap block-count probe, font page-slot LRU, mid-build arena adoption.
- **PR #125** — the footnote gather can no longer write a "no footnotes" cache it never earned.
- **PR #126** — streaming table renderer. Rows lay out at `</tr>`, so the live set is one row of
  `ParsedText` plus one viewport-bounded fragment of `TextBlock`s. This retired the largest
  unbounded class-D consumer in the parser; the 12 KB byte budget was retargeted from the table to
  the **row** (eliminated #16 — it is not redundant at row scope).
- **`bea07a3d`** — the font group scratch revert, §9.4.

See [heap-work-handover-2026-08-10.md](heap-work-handover-2026-08-10.md) for the full
eliminated list. Sixteen hypotheses have been disproved on device; check it before proposing
anything here.
