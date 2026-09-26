# EPUB reader memory audit (September 2026)

An audit of every memory consumer on the EPUB reader path, made against
`docs/memory-allocation-strategy.md` (the classes A–D, rules 1–4 and invariants
in its §2, §3 and §6 still hold and are not restated here). The strategy doc grew
one discovery at a time; this document asks the question that sequence never
did: **taken as a whole, is the arena used where it helps, are the heap gates
measuring anything, and where does "hopefully big enough" still hide?**

Tree audited: `master` at `7b16746d0` (PR #310 merged). Line numbers refer to
that tree.

## 0. Method

Four sources, cross-checked against each other:

1. **Source inventory.** Every `BuildArena` instance and allocation site, every
   heap threshold (`getFreeHeap` / `getMaxAllocHeap` comparisons), every
   fixed-capacity container, and every heap block that outlives a build, read at
   source in `lib/Epub`, `lib/Memory`, `lib/ZipFile`, `lib/EpdFont`,
   `lib/ProgressiveJpeg`, `src/activities/reader`.
2. **Device traces** (X3, 2026-09-25, three runs of *Strange Pictures*, 78
   progressive JPEGs, 12 spines; run 3 from a wiped cache): the
   `Reader mem[...]`, `Background-C`, `SCT` and `FBUF` lines.
3. **Host census.** `test/epub_pipeline/epub_pipeline_dump --bench` with the
   `HeapTrack` per-site profiler, run twice per book: heap-only (the blocking
   path's allocation pattern: owned 10 KB arena, ZIP arena and SAX state on the
   heap) and `--arena=52272` (a lent region the size of the X3 secondary
   framebuffer, i.e. the Background-B/C pattern). Four book shapes: *Strange
   Pictures* (images), `moby-dick.epub` (long text), `test_large_css.epub`,
   `test_tables.epub`. Run with `WH_HOST_STDIO_UNBUFFERED=1`, which removes
   glibc's 4 KB-per-`FILE*` buffers (they have no device counterpart and had
   been showing up as ~25 KB of peak under a misleading symbol).
4. **No-inline attribution** (`test/build_gprof`, `-fno-inline`) to name the
   sites the release build folds into their callers.

The host census is a faithful proxy where it overlaps the device: the lent
arena's high-water on *Strange Pictures* is 45 872 on the host and 45 876 on
the X3.

## 1. The budget as it stands

### 1.1 Device (X3, Strange Pictures, 2026-09-25)

| Phase | free | contig | Source |
|---|---|---|---|
| Reader entry (comment baseline) | 53–55 KB | — | `EpubReaderActivity.cpp:128` |
| Background-C build start | 47.3 KB | — | run 5, `SCT build_start` |
| … after setup (CSS ruleset, SAX, chunk in the arena) | 38.1 KB | — | `SCT after_setup` |
| … after extraction | 38.0 KB | — | `SCT after_extract` |
| … minimum during the build | 11.3 KB | 8–12 KB (`ChapterHtmlSlimParser.cpp:736-739`) | run 5, C build; run 3's blocking build bottomed at 6.5 KB |
| Steady-state reading, after a C build | avg 35.6 KB, min 31.7 KB | avg 27.0 KB, min 22.5 KB | run 4, 206 `Reader mem` lines |
| Steady-state reading, after a **blocking released** build | — | **11.8 KB flat** | run 3, 155 lines |

The lent region (52 272 B) in the same C build: high-water **45 876** with the
extraction ring live (ring 32 KB + chunk 1 KB + extract grow 8 KB + ruleset +
SAX), **21.7 KB** on a rebuild from banked HTML (no ring) — i.e. during phase
(b), the phase in which the heap falls to 11 KB, at least **30 KB of the lent
region is idle**. The 21.7 KB is roughly 12 KB resident (SAX state 9 824 B,
chunk 1 024 B, CSS ruleset) plus ~9 KB of transient font-slot scope from
mid-build page draws.

The two reading-state rows are the audit's most important pair: the same book
on the same device reads at contig 27 KB after a borrowed build and at 11.8 KB
after a released one. The blocking path is still the one that shapes the heap
for the rest of the session.

### 1.2 Host census

Whole-process peak in bytes; the lent-arena column is net of the 52 272 B
backing block the harness allocates for the region.

| Book | heap-only peak | lent-arena peak (net) | moved off the heap | arena high-water |
|---|---|---|---|---|
| Strange Pictures | 102 130 | 79 401 | 22.7 KB | 45 872 |
| moby-dick | 64 507 | 54 964 | 9.5 KB | 43 520 |
| test_large_css | 36 834 | 26 701 | 10.1 KB | 10 848 |
| test_tables | 54 036 | 33 737 | 20.3 KB | 13 392 |

Harness gap: the dump does not lend the region to the image-header walk, so
*Strange Pictures* carries a 32 KB `walkEntry` ring on the heap in both modes
(176 walks). On the device that walk runs from the borrowed framebuffer (A7 in
§2) before a blocking build and from the build arena after a B/C build.

What is left on the heap in lent-arena mode, *Strange Pictures*, by peak-live
bytes (release build, symbolised with the no-inline build where needed):

| live | n | each | site |
|---|---|---|---|
| 33 240 | 176 | 32 768 | `EpubImageManifest::walkEntry` inflate ring (harness gap, see above) |
| 8 192 | 40 | 8 192 | `Section::runBuildSetup` — `externalPageBreakAnchors`, `vector<pair<string,string>>` grown by doubling (`Section.cpp:1007`) |
| 6 216 | 155 | 168 | `cssStyleCache_` unordered_map nodes (`ChapterHtmlSlimParser::startElement`) |
| 2 560 | 40 | 5 120 | page-break label vector, `vector<pair<uint16_t,string>>` |
| 2 340 | 71 | 8 192 | `Section::startBuild` — inlined setup allocations (`ChapterHtmlSlimParser` object 1 864 B, LUT and anchor vector growth) |
| 2 320 | 7 146 | 80 | `ParsedText::extractLine` — one per laid-out line |
| 2 226 | 7 145 | 181 | `TextBlock::TextBlock` — one per laid-out line |
| 2 208 | 221 | 48 | `CssParser::lookupRule` |
| 1 552 | 100 | 1 552 | `ZipFile::EntryReader::Impl` |
| 0 (transient) | 4 256 | 4 096 | `ParsedText` word vectors, `reserve` doubling from 16 to 128 words (`ParsedText.cpp:203-210`) — allocated and freed once per paragraph |
| 0 (transient) | 78 | 4 096 | `ZipFile::readBytesFromStat` image-header probe buffer |

Host sizes for `std::string`-bearing structures are ~1.3× the device's
(32-bit), but the counts are exact.

## 2. Inventory A — arena instances and what goes into them

`BuildArena` (`lib/Memory/BuildArena.h`): bump allocator; `reserveBlock` /
`release` are strictly LIFO (an out-of-order release is refused and counted in
`releaseFailures_`); `reset()` rewinds everything.

| # | Instance | Backing | Capacity | Lifetime | Where |
|---|---|---|---|---|---|
| A1 | Section build arena, owned | heap | `SCT_PARSE_ARENA_BYTES` 10 240 | one build | `Section.cpp:715` |
| A2 | Section ZIP-scope arena | heap | chunk 1 024 + ring ≤ 32 768 + slack | phase (a) only | `Section.cpp:1123` |
| A3 | Warm-pass scratch | heap, or the lent region when no build is live | `WARM_PASS_SCRATCH_BYTES` 41 216 | pre-reboot warm loop | `Section.cpp:1986` |
| A4 | `buildScratch_`, Background-B | **lent secondary framebuffer** | 52 272 (X3) / 48 000 (X4) | borrow → return | `EpubReaderActivity.cpp:1343` |
| A5 | `buildScratch_`, Background-C | lent framebuffer | same | build → next render | `EpubReaderActivity.cpp:3970` |
| A6 | `warmScratch`, per-render image warm | lent framebuffer | same | one `renderContents` | `EpubReaderActivity.cpp:4617` |
| A7 | Image-header walk (stack object) | lent framebuffer | same | one walk | `EpubReaderActivity.cpp:3731` |

A1 and A2 are what the **blocking** path gets: the framebuffer is *released*
(freed into the heap) rather than lent, so nothing large can be bump-allocated
and the 10 KB owned arena is too small for the SAX state, the CSS ruleset or
the footnote resolver — all three explicitly check `st.arena !=
st.ownedArena.get()` and go to the heap (`Section.cpp:866, 966, 1043`).

### 2.1 Build-time consumers, arena or heap

| Consumer | Size | B/C (lent) | Blocking (A1/A2) | Scope block |
|---|---|---|---|---|
| CSS ruleset (resident) or offset index | 8 B/rule + style pool; ≤ 12 KB at `MAX_RULES` 1500 | arena, committed for the build | heap vector | committed (`CssParser.cpp:1969`) |
| SAX parser state | 9 824 B | arena (plain alloc) | heap `new` | **none — see F2a** |
| Feed chunk | 1 024 | arena | arena | `chunkBlock` |
| Extract grow buffer | 8 192 | arena | arena (if it fits) | `extractGrowBlock` |
| ZIP read buf + inflate ring | 1 024 + ≤ 32 768 | arena (shared scope) or A2 | A2 on the heap | `arenaBlock` in `EntryReader` |
| Footnote resolver `DocStream` ring | ≤ 32 KB | arena if it fits, else **heap ring across slices** | heap | `arenaBlock` |
| Mid-parse deferred header walk | 512 + 16/32 KB ring | **heap only** (`resolveDeferredNow` passes `arena=nullptr`, `EpubImageManifest.cpp:391`) | heap | — |
| Build-end header walk | same | arena | A7 before the build; heap after | scoped per stage |
| Font page slots during mid-build draws | ~9 KB | arena (`ScopedSlotArena`) | heap `malloc` | scoped per draw |
| `ParsedText` word vectors + strings | ~3 KB + words > 15 chars, per paragraph | **heap** | heap | — |
| `TextBlock` + line objects | 80 + 181 B per line, ~7 k lines/book | **heap** | heap | — |
| `cssStyleCache_` memo | 64 × ~120 B on device | **heap** | heap | — |
| CSS negative cache | ≤ 256 strings | **heap** | heap | — |
| Page-list anchors, page-break labels, LUT | up to ~6 KB + 4 KB + 2 KB | **heap** | heap | — |
| `Page` for a mid-build draw | ~10.5 KB | **heap** | heap | — |
| Table row layout (`BufferedTableRow`) | ≤ 12 KB attributed | **heap** | heap | — |

### 2.2 Decode-time consumers (`image_scratch`)

`image_scratch` is installed only inside A3's and A6's scopes; every decode
outside those (a page render with no lent region, the cover path) uses the
heap. Arena-eligible today: extract write buffer 4 KB and extract ring ≤ 33 KB
(`Epub.cpp:1636-1657`), PNG scanlines + ring (`PngStreamDecoder.cpp:170-270`),
TJpgDec work pool 12 KB and the progressive workspace (`JpegToFramebufferConverter.cpp:386-417, 1377-1385`).
Still heap by construction: the `PixelCache` band (≤ 24 KB, `PixelCache.h:139`),
the JPEG dither band (8 KB), the ditherer rows and `grayLine`.

## 3. Inventory B — heap gates

Thirty threshold comparisons on the reader path. Condensed; the columns that
matter for the conclusions are *what happens on refusal* and *whether a
refusal is latched* (recorded so that the cache built under it is rebuilt or
discarded, rather than kept silently).

### 3.1 Parser (`ChapterHtmlSlimParser.cpp`)

| Gate | Threshold (free / contig) | On refusal | Latched? |
|---|---|---|---|
| text layout, soft (`:654-691`) | 18 432 / 12 288 | log only | — |
| text layout, hard (`:679-697`) | 9 216 / 6 128 heap builds; **3 568 arena builds (5 104 with bionic)** since run 10 | stop parse, keep partial cache if pages > 0 | status bit; auto-rebuilt only when pageCount == 0 |
| word-vector growth (`ParsedText::addWord`, run 10) | largest block ≥ the four reserves | word dropped, parse stops at the next layout gate (partial cache) | — |
| table row layout (`:727-753`) | 18 432 / 6 128 | row emitted as paragraphs | **no — baked into the cache** |
| row buffer budget (`:180-188`) | 12 KB attributed bytes (not a heap sample) | row degrades | no — baked |
| image header read (`:755-764`), only without a manifest | 16 384 / 8 192 | alt text | status bit |
| `imageWalkBudget` (`:766-773`) | min(free − 16 384, contig − 16) vs stage need | alt text, image stays queued in `images.pending` | status bit → rebuild at build end |
| indexing popup (`:3253-3261`) | 32 768 / 12 288 | no popup, no progress ticks | — |

### 3.2 Section, CSS, metadata

| Gate | Threshold | On refusal | Latched? |
|---|---|---|---|
| `Section::heapAllowsEmbeddedStyle` (`Section.cpp:1685-1712`) | 45 056 / max(12 288, rules × 8 + 8 192); **bypassed when the arena is lent** | build without CSS | via cache key (`embeddedStyleFallback`) → blocking rebuild on entry |
| `CssParser::resolveStyle` low-heap (`CssParser.cpp:2007-2030`) | free < 24 576 (lean; every build is lean, `Section.cpp:983`) | rule lookups skipped, style silently missing | in-memory flag only; **a foreground blocking build never checks it** |
| `Epub::parseCssFiles` (`Epub.cpp:560-618`) | 24 KB + stylesheet size; sheet ≤ 128 KB | stylesheet skipped, no cache written | re-parses next open |
| `Epub::ensureSpineStats` (`Epub.cpp:1706-1722`) | spine × ~32 B + 32 KB | per-spine directory scans | one attempt per Epub |
| `BookMetadataCache::buildBookBin` | spine × 20 + 32 KB (≥ 16 spines) | per-item lookups | — |
| `EpubImageManifest::resolvePending` | contig − 64 vs stage need (arena first) | image stays queued | persistent queue |

### 3.3 Image decoders

| Gate | Threshold | On refusal | Latched? |
|---|---|---|---|
| JPEG decode entry (`JpegToFramebufferConverter.cpp:1287-1292`) | baseline 28 672 (16 384 with arena); progressive 16 384 | **blank image area, no .pxc**, retried next render | no |
| progressive workspace loop (`:1375-1393`) | free ≥ workspace + 20 480 unless the arena hosts it | coarser scale, then DC preview | **no — the .pxc has no quality key** |
| `shouldEnableJpegCache` (`:475-497`) | band + 12 288 + 8 192 | shown, not cached | no |
| companion cache (`:1542-1560`) | companion band + 8 192 | second decode later | no |
| `shouldForceBayerDither` (`:499-508`) | `minFreeHeapForJpeg()` + 8 192, evaluated **after** the pool it charges for is allocated | Bayer | inert in the reader (Bayer is the default) |
| PNG `decodeOpenFile` (`PngToFramebufferConverter.cpp:275-279`) | 36 864, or 16 384 with arena | blank, no .pxc | no |
| cover/thumbnail BMP (`JpegToBmpConverter.cpp:657-660, 604-612`) | 40 960; progressive workspace + 16 384 | no cover / coarser | caller |

### 3.4 Reader activity (`EpubReaderActivity.cpp`)

| Gate | Threshold (free / contig) | On refusal | Notes |
|---|---|---|---|
| pre-render arm + execute (`:1438-1454`, `:3406-3411`) | 45 056 | skipped; the arm is consumed, no retry | comment at `:1431` still cites a 56 KB floor |
| B borrow start (`:1575-1584`) | 40 960 (+8 192 if a resolve is owed) / 12 288 | fall through to the heap-backed gates | the gate B actually uses |
| `bgB_waitheap` (`:1585-1596`) | max(49 152, 30 720 + ring) / max(24 576, ring + 8 192) | WaitHeap | **never admitted on any spine** (`:344-350`) |
| `bgB_cssResident` (`:1614-1617`) | 73 728 | WaitHeap | never admitted; derived from the 40 KB resolver floor no build uses |
| `bgB_residentAbort` (`:1675-1689`) | 30 720 / 16 384 | abort, discard | only heap-backed B |
| `heapAllowsInPlaceBuild` (`:3445-3499`) | CSS: max(67 584, 51 200 + ring) / max(32 768, ring + 8 192); else 61 440 / 28 672 | released build | X4 only; CSS floor derived from the 40 KB premise |
| C `residentAbort` (`:1865-1874`) | 30 720 / 16 384 | released rebuild | resident C only |
| B discard cap (`:1478`) | 3 discarded runs | B off for the book | — |
| `maybeRestartForFragmentedHeap` (`:4456-4507`) | free ≥ framebuffer + 32 768 (was a fixed 98 304 — run 10) and contig < the framebuffer size | silent restart | was a constant `52*1024` (976 B under the X3's 52 272) — **fixed in R0**; two of three callers deliberately pass contig = 0 because the heap may be corrupt after decode failures and walking the TLSF free list there has crashed the IWDT |
| warm-pass borrow, secondary realloc | no threshold: try, evict caches, retry | degraded (AA off), then restart heuristic | — |

## 4. Inventory C — fixed-capacity and unbounded structures

Roughly ninety fixed caps were found on the reading path. Most are harmless:
they bound a *time* optimisation (a memo, an LRU, a prewarm list) and past
the cap the output is unchanged. The ones that matter fall into three groups.
The firmware builds with `-fno-exceptions` (`platformio.ini:84`), so a plain
`std::vector` / `std::string` / `unordered_map` that cannot grow calls
`abort()` — that is the "heap-recovery restart".

### 4.1 Caps that change the output silently

The pending-image queue pattern (`kMaxPending = 16`, gone since PR #310): a
fixed array, no log above `LOG_DBG`, no status bit, the reader simply shows
less than the book contains.

| Structure | Cap | Past the cap | Where |
|---|---|---|---|
| `Page::footnotes` | 16 per page | the 17th footnote on a page is dropped | `Page.h:142-152` |
| SAX element stack | depth 64 | pushes are skipped but `startCb` still fires; every later `endCb` reports the **wrong element name** (stack shifted), and the last (depth − 64) closes get no `endCb` at all | `SaxParserYxml.cpp:184-191, 360-367` |
| SAX attributes | 12 per element, 384 B per value, 32 B names | attributes dropped / values truncated; one `LOG_DBG` at chapter end | `SaxParserYxml.cpp:47-59, 334-358` |
| Non-TOC anchors per chapter | 1 024 | later `id`s are not recorded; in-book links to them land nowhere | `ChapterHtmlSlimParser.cpp:77, 1384-1392` |
| `anchorsAwaitingLine_` | 16 | the anchor is recorded a page early | `ChapterHtmlSlimParser.cpp:90, 3135-3145` |
| `FootnoteEntry` number / href | 32 / 96 B | truncated; a truncated href is navigated as-is | `FootnoteEntry.h:5-10` |
| `partWordBuffer` | 200 B | a longer token is rendered as two words with a gap | `ChapterHtmlSlimParser.cpp:2705-2708, 2828-2846` |
| Float nesting / inset wrappers / float zones | 4 / 8 / 2 | deeper nesting is ignored; layout differs | `ChapterHtmlSlimParser.h:90-93, 322-323`, `BlockStyle.h:27-33` |
| `FootnotePreviews` targets per spine / resolved bitmap | 256 / 512 spines | later links keep a plain marker; spines ≥ 512 are re-scanned on every build | `FootnotePreviews.h:90`, `.cpp:33-38` |
| ZIP entry names | < 256 B | longer entries are never found | `ZipFile.cpp:103, 142, 419` |
| SD font families | 128 | the rest are dropped after the sort | `SdCardFontRegistry.cpp:188-205` |
| Nested footnote jumps | 3 | a 4th jump's return position is not saved | `EpubReaderActivity.cpp:5619-5636` |
| `MAX_RULES` (CSS, compile mode) | 1 500 | `LOG_ERR`, no cache written, the book re-parses its CSS and hits the cap again on **every** open | `CssParser.cpp:58, 843-859` |

### 4.2 Asymmetric caps — unbounded at build time, capped at load time

| Structure | Build side | Load side | Consequence |
|---|---|---|---|
| `Page::elements` | no cap (`push_back` per line/image/table fragment) | `MAX_PAGE_ELEMENTS` 1 024, `LOG_ERR`, page load fails | a page that builds with > 1 024 elements can never be displayed |
| `Section::lut` | no cap; `pageCount` is `uint16_t` | 10 000, `LOG_ERR`, `clearCache()` | a spine that builds to > 10 000 pages is rebuilt, fails to load, is rebuilt … |
| Page-break label count | `static_cast<uint16_t>` on write | `reserve(count)` from the file, one request of up to 65 535 × 28 B | wraps silently past 65 535 |

### 4.3 Caps with a defined, logged degradation (acceptable)

Table rows: 48 rows per fragment, 8 columns, 64 lines per cell, 12 KB row
budget — the row falls back to paragraphs (`LOG_DBG`; **not latched**, see
§3.1). Fonts: 512 prewarm glyphs, 128 groups, 4 page slots, 8 overflow
glyphs — per-glyph fallback, `LOG_DBG`. Progressive JPEG: 32 luma scans, 16
Huffman tables — `Unsupported`, DC preview, `LOG_ERR`. `PixelCache` band 24 KB
— no cache. Footnote preview store 2 048 entries — `LOG_ERR`, permanent for
the book. Memos and LRUs (`cssStyleCache_` 64, negative cache 256, hot rules
128, scaled-glyph cache 80) — time only.

### 4.4 Unbounded standard containers on the build path

All grown per chapter element, all on the heap, all `abort()` on OOM; the
9 KB hard text-layout gate is the only thing standing between them and the
restart:

`Page::elements` (per line), `pendingFootnotes` (132 B per link, drained per
line with `erase(begin)`), `pageBreakLabels` (per marker), `tocAnchors`,
`externalPageBreakAnchors` (per page-list entry; `Section::runBuildSetup`
also reads *every* `pagelist.bin` entry into three temporaries to filter by
href), `inlineStyleStack` (per inline element, bounded only by nesting),
the four `ParsedText` word vectors (doubling from 16, `shrink_to_fit` per
paragraph so the next one regrows), `lineBreakIndices` and three parallel
line vectors, `FontCacheManager::scanByFont_` (a `std::string` that `+=`s
every drawn string per (font, style) per page), `BookMetadataCache` spine
deques (`resize(spineCount)`, "no OOM fallback" per its own comment).

Session-lifetime: `BookmarkStore::bookmarks` (no RAM cap; the 1 000 cap is
applied on save and refuses the whole save), `Epub::cssFiles`,
`Section::pageBreakLabels` (see 4.2).

The nothrow pattern that the manifest (`records_`), the footnote resolver
(`NoThrowArray`), the OPF item index and the arena itself already use is the
model: growth fails as a logged number and the build degrades one feature,
not the session.

## 5. Inventory D — heap that outlives a build

Session-lifetime blocks the reader carries while a book is open (sizes are
device sizes where a comment measured them):

| # | Block | Size | Freed | Notes |
|---|---|---|---|---|
| S5 | `CssParser::cacheRuleOffsets_` (index vector) | 8 B/rule, ≤ 12 KB | `clear()` and `dropIndex()` **keep capacity**; only `clearCaches(true)` frees | bucket arrays of the rule/hot/negative maps survive a plain `clear()` |
| S6 | `EpubImageManifest` records | 12 B × capacity, grown nothrow by max(32, cap/2) | `releaseMemory()` in the realloc eviction tier | pin comment at `EpubReaderActivity.cpp:3120-3124` describes the pre-v5 string layout — stale |
| S7 | `spineStats_` deque | ~14 B/spine (24 KB at 1 732 spines) | book close | allocated inside the first build of the session |
| S8 | `Section`: LUT (4 B/page), TOC boundaries, page-break labels (28 B + string), `HalFile` Impl ~92 B | few KB | spine change | label and handle pins fixed 2026-09-25 (`Section.h:61-66`, `Section.cpp:1615-1622`) |
| S9 | Background-B's finished section kept for adoption | as S8 | adoption or reset | dropped in the second realloc tier |
| S13 | scaled-glyph cache | ~4.9 KB (9.5 KB with a synthesized SD size) | **never** — `releaseScaledGlyphCache()` has no callers | allocated at `onEnter` so it cannot land in the hole |
| S14 | SD-card font metadata | 40–50 KB (CJK `fullIntervals` up to 48 KB/style) | reader exit; dropped around blocking builds for pure-SD fonts | flash-mapped fonts alias it |
| S15 | `ReadingSessionTracker` docId/title/author | small | `clear()` keeps the blocks in the singleton | |
| P1 | `Page` (elements, per-line `TextBlock` + flat arena, image strings) | 10 504–10 636 B measured | end of render (X4) / deferred-AA pass (X3) | "the one reader path with no arena" (`EpubReaderActivity.h:434`) |
| P4 | font page slots | ~9 KB in six blocks | next prewarm | frequently inside the released hole; evicted first on realloc |

The secondary framebuffer state machine (RESIDENT / RELEASED / LENT,
`FreeInkDisplay.cpp:455-559`) is sound: `borrow` and `return` cannot fail and
never put the block on the heap; only `release` + `realloc` can. Every path
that still *releases* rather than lends is therefore a fragmentation risk by
construction: first-open indexing (`ReaderActivity.cpp:835-847`), the blocking
build (`compileSectionCache`, `:3598`), the warm pass when nothing can be lent
(`:4628`), Home cover loading, sleep, network trim, serial transfer.

## 6. Findings

**F1. The arena hosts the large blocks, not the churn — and the churn is what
collapses contig.** In a Background-C build the lent region peaks at 45.9 KB
only while the extraction ring is live; for the whole of phase (b) — the
phase in which the heap falls to 11 KB and contig to 8–12 KB — it carries
~12 KB resident plus ~9 KB of transient font slots, leaving ≥ 30 KB idle.
Meanwhile every per-paragraph and per-line object goes to the heap: four
`ParsedText` vectors doubling to 128 entries (one 3–4 KB block allocated and
freed per paragraph, 4 256 times in *Strange Pictures*), a word string per
word longer than 15 characters, one 80 B line record and one 181 B `TextBlock`
per laid-out line (7 146 each per book), the `cssStyleCache_` nodes, the
negative-cache strings, the page-list anchors (8 KB on the host, doubled
into place), the LUT and label vectors. The strategy doc's §8 objection — "89 % of
allocations are ≤ 128 B, a bump arena is the wrong tool" — is true of a
*build-lifetime* arena. It is not true of *scoped blocks*: a paragraph's
vectors and words die together at the end of the paragraph, and a page's
`TextBlock`s die together when the page is flushed to the section file. Those
are exactly the lifetimes `reserveBlock`/`release` express. (The reverted
`TextBlockLinePool` was a *heap-backed* pool that itself took contig below
the 52 272 cliff; a pool carved from the idle part of the lent region cannot.)

*Status: addressed by R2 (see there); 68 % of a build's allocations gone on
the host, device validation pending. The reading of the census in this
finding was right about the sites and wrong about the remedy: most of the
churn was regrowth, which reuse removed with no arena at all; the page
block took the rest of the bytes.*

**F2. Three defects in the current LIFO discipline.**

- *(a) SAX state rewound before its last use.* `chunkBlock` is reserved at
  the first parse call (`Section.cpp:750`); the SAX state is bump-allocated
  later, inside that scope, by `visitor->setup()` (`ChapterHtmlSlimParser.cpp:3197`).
  `dropChunk()` (`Section.cpp:1381`) rewinds the cursor below the state, and
  `visitor->finalize()` (`:1399`) then runs `saxParser_.finalize()` on the
  rewound memory. It works because nothing allocates from the arena in the 18
  lines between; `SaxParser::reset` even documents that the arena "may already
  have been rewound". A lifetime that is longer than the block it sits in is
  a bug waiting for one more allocation.
- *(b) Abort during the footnote resolve releases out of order.* `~BuildState`
  releases `chunkBlock` in its body (`Section.cpp:783-787`); `previewResolver`
  is a member declared later, so its `DocStream` ring block (reserved above
  `chunkBlock`, held across slices) is released after. The `chunkBlock`
  release is refused (`releaseFailures_++`) and the block stays reserved
  until the next `reset()`. Bounded, but it is the one path that exercises
  the refusal counter, and it is silent.
- *(c) `free()` on arena memory.* `FontDecompressor::prewarmCache` rolls a
  slot back on an OOM of the 128-byte `groupIdToPos` map with plain
  `free(slot.buffer); free(slot.glyphs);` (`FontDecompressor.cpp:670-671`)
  without checking `slot.arenaBacked`, unlike the other three free sites.
  When the slot came from the lent framebuffer this frees an interior pointer
  into the display buffer — and a 128-byte malloc fails precisely in the
  low-heap mid-build draw the slot arena exists for.

*Status: all three fixed in R0 (branch `memory/audit-2026-09`): the SAX
state is taken in `ChapterHtmlSlimParser::setBuildArena`, at wiring time and
below every scoped block; `~BuildState` resets the resolver before releasing
`chunkBlock`; the rollback frees only heap-backed slots.*

Two further fragilities with no failing path found: a lazy CSS index reload
during phase (b) would commit a block above `chunkBlock` and be rewound by
`dropChunk()` (`CssParser.cpp:1558`), and `CssParser::indexArena_` dangles
between a build's `clearCaches()` and the next `runBuildSetup`.

**F3. Gates are sized by derivation, several are unreachable, and refusals
are often silent.** Of the thirty gates, `bgB_waitheap` and `bgB_cssResident`
never admitted on any spine in the X4 trace that the code itself cites
(`EpubReaderActivity.cpp:344-350`); `bgB_cssResident`, `IN_PLACE_BUILD_CSS`
and the in-place CSS base all still derive from a 40 KB resolver floor that no
build has used since every build became lean (`Section.cpp:983`); the 44 KB
embedded-style floor is "derived, not measured" by its own comment
(`Section.cpp:163`); six contig floors are exact round numbers without the
16-byte slack the parser file says every content-dropping floor needs
(`ChapterHtmlSlimParser.cpp:138-141`); the restart heuristic hard-coded
`52*1024` (fixed in R0; the contig = 0 its post-decode callers pass is
deliberate — the heap may be corrupt there). Four refusals
bake a degraded result into a cache with nothing to trigger a rebuild: table
rows demoted to paragraphs, CSS lookups skipped during a foreground blocking
build, a progressive JPEG decoded at a coarser scale or as a DC preview into
a `.pxc` with no quality key, and a JPEG/PNG decode refused outright (blank
area). RULE 3 of the strategy doc — gate on the producing side, with the
number the producer will actually allocate — is followed by the newest gates
(`imageWalkBudget`, `resolvePending`, the progressive workspace loop) and by
none of the older ones.

**F4. The blocking path is still the session's worst event.** It releases
the framebuffer instead of lending it, so it has no arena worth the name
(A1 = 10 KB), puts the ZIP ring (A2, ≤ 33.8 KB), the SAX state and the CSS
index on the heap, and leaves the reader at contig 11.8 KB for the rest of
the session (§1.1). Its host allocation pattern peaks at 102 KB on *Strange
Pictures* against 79 KB for the lent pattern. The mechanism to do better
already exists on the same path: `compileSectionCache` borrows the buffer,
walks the pending image headers from it and returns it (`:3574`, `:3721-3740`)
*before* releasing it for the build. §9.2 of the strategy doc asked whether
the blocking build should borrow too; this audit's answer is yes, and it is
the single change with the largest measured payoff (27 KB vs 11.8 KB reading
contig).

**F5. The pending-image queue was not the only "hopefully big enough".**
Thirteen caps change what the reader shows with at most a `LOG_DBG` (§4.1);
the SAX depth cap corrupts end-tag names rather than failing; two caps exist
only on the load side of a cache the build side fills without limit (§4.2),
so a page or spine past them is built and then can never be loaded; and
`MAX_RULES` turns a 1 501-rule stylesheet into a book that re-parses its CSS
on every open. Against that, ~15 build-path containers have *no* cap and
abort on OOM (§4.4). The two extremes are the same mistake from opposite
sides: a size chosen once, with no defined behaviour for the input that
exceeds it.

**F6. Session-lifetime blocks are small but undeclared.** The scaled-glyph
cache is never freed; the session tracker's strings survive `end()`; the CSS
index keeps its capacity across builds; the manifest's pin comment describes
a layout that no longer exists. None is a leak in the sense of growing, all
are fine individually, and there is no place that lists them — which is how
the four pins fixed on 2026-09-25 (labels, section handle ×2, manifest
strings) went unnoticed until a `heap_caps_dump` showed them bounding the
hole.

**F7. The host census is now a usable regression tool.** With `--arena` and
unbuffered stdio the harness reproduces the device's arena high-water to
4 bytes and names every heap site with counts; the one gap is the header
walk, which the dump runs from the heap.

**F8. The reading-stats store grows without bound, and the point where it
stops fitting wipes it.** (Found 2026-09-26 while resolving R6.) No cap
anywhere: `recordSession` creates a book entry before it checks
`sessionSeconds > 0`, so every book ever opened gets a permanent entry;
each book keeps one `[dayIndex, seconds]` bucket per calendar day it was
read, for ever (read only by the web export); the global day buckets grow
one per reading day for ever (the UI needs the last 30 days and the
current streak). The load materialises three copies — the file as a
`String`, the ArduinoJson document, the vectors — so 18 books cost a
~27 KB transient today (startup minimum 39 224 from 66 416), about 1.5 KB
per book, and the same load runs inside the reader at session end with
the framebuffer resident and 35–45 KB free. At roughly twice today's file
it fails there first — and `loadFromFile` sets `loaded_ = true` *before*
parsing, so a `NoMemory` (or a corrupt file) leaves the store "loaded and
empty", the session end records one session, and `saveToFile` writes that
as the whole history (`ReadingStats.cpp:233`, `JsonSettingsIO.cpp:815`).
The "store not loaded, would erase history" guard cannot catch it.

## 7. Recommendations

Ordered by payoff per line of code; the first group is small enough to go in
one PR.

**R0 — fix the three F2 defects and the stale numbers.** *Done on
`memory/audit-2026-09`.* `prewarmCache`: the two `free()`s are guarded with
`!slot.arenaBacked`. SAX state: taken in `setBuildArena` (called from
`runBuildSetup`, next to the CSS ruleset and below `chunkBlock`), so no
rewind can reach it. `~BuildState`: resets `previewResolver` before
releasing `chunkBlock`. The restart heuristic compares against the panel's
real buffer size instead of `52*1024`. The three stale comments (manifest
pin, 55.9 KB, resolver-floor premise) are corrected or marked stale with a
pointer to R3. Withdrawn from the original R0: "pass the real contig" to the
restart heuristic — its post-decode callers pass 0 on purpose, because
walking the TLSF free list on a possibly corrupt heap has crashed the IWDT.

**R1 — lend the framebuffer to the blocking build (F4).** *Done on
`memory/audit-2026-09`, device validation pending.* `compileSectionCache`
now borrows the buffer as the build arena (the same borrow → build → return
its header walk already did) and hands it back at the end; the build runs
with A4/A5's allocation pattern instead of A1/A2's, and the post-build
header walk runs from the idle region. Two cases still release: the
Background-C failure latch (`forceBlockingBuildSpine_`), because C already
ran borrowed and failed on the heap and its escalation *is* the +52 KB of a
released build; and nothing to lend. A borrowed blocking build that fails
logs the arena's high-water and last refused size, then retries released.

A correction to F4's framing, found while implementing: on today's tree
the blocking build is reached mostly *as* that escalation (run 3's blocking
build followed a C build that died at page 67), plus the CSS-fallback and
image-header rebuilds. So R1's payoff is on those rebuilds — which now get
the arena-resident CSS ruleset the released path never had — and the run-3
outcome is really R2's to fix: once the phase (b) churn leaves the heap, C
completes and no escalation happens. Device runs 6 and 7 did not reach the
blocking path (C completed); run 8's second pass did, through the C-failure
latch, and took the *released* branch as intended (`Index start mem (before
fb release, C-failure latch)` → 98 KB free → 180 pages → realloc OK). The
borrowed blocking build itself is still unexercised on hardware; it is the
same code path as C's borrow, but its
`Index start mem (secondary buffer BORROWED …)` / `Index end mem (after fb
return)` pair should be seen once on a CSS-fallback or image-header rebuild
before this is called validated.

**R1b — lend, don't release, on the two remaining phase-scoped release
sites.** *Done on `memory/audit-2026-09` (2026-09-26), device validation
pending.* Prompted by the run-6/7/9 failures, which were all one mechanism:
release → a long-lived block lands in the freed hole → realloc fails →
degraded / recovery restart. A lent block never enters the heap, so the
hole cannot be pinned and the return cannot fail.

- *First-open indexing* (`ReaderActivity`): its own comment already said
  "lend"; the code released. Now `borrowSecondaryBuffer` → `Epub::load(…,
  scratch)` → `returnSecondaryBuffer`. The load's stream reads (container,
  OPF, NCX, nav, page-map, CSS) take their read buffer and inflate ring
  from the region (`readItemContentsToStream` decides before any byte is
  written, so a region without room falls back to the heap); the spine
  tables keep the heap, which without the ring has room for them. The
  "drop the ePub and realloc again" dance is gone.
- *Home cover loading* (`HomeActivity`): the buffer is lent for the whole
  cover pass as `coverScratch_`, threaded through `ensureCoverThumb`,
  `beginCoverExtractSession` (the extraction ring, held across slices),
  `beginPngThumbSession` (the PNG decoder's ring and scanlines, held across
  slices) and `Epub::loadForCover` (the OPF ring). `JpegToBmpConverter`
  takes an optional region for its TJpgDec pool or progressive workspace,
  with the free-heap gate reduced to the row pipeline when it does. Sessions
  are strictly sequential, so the blocks stay LIFO; `restoreSecondaryBuffer`
  resets any abandoned session before it gives the region back.

Still releasing, on purpose: the Background-C failure escalation (C fails
on heap contig; +52 KB of general heap is the cure until the parse's
per-page heap objects move into the page block — R2 step 3), the warm-pass
fallback when there is nothing to lend, and the session-ending paths
(sleep, network trim, serial transfer). **Revisit after R2 step 3 and R3**:
the same conversion may then apply to the escalation and to the pre-reboot
warm pass.

**R2 — take the phase (b) churn off the heap (F1).** *Done on
`memory/audit-2026-09`, device validation pending.* The count-sorted census
reshaped this recommendation before a line was written: the dominant churn
was *growth*, not objects. Every paragraph regrew `ParsedText`'s four word
vectors from 16 to 128 entries (up to 16 allocations per paragraph, 15 000
per book) and the layout pass allocated another ~10 locals per paragraph
(`wordWidths`, `lineBreakIndices`, the hyphenation triple, the plain
breaker's four DP tables, the paragraph's text for `ensureFontReady`). None
of that needed an arena — it needed to stop being re-created. So R2 became
two steps:

1. *Reuse.* The parser keeps one `ParsedText` across paragraphs
   (`reset()` keeps capacity; the `shrink_to_fit` after each flush is gone —
   a block splits at 96 words, so the vectors settle at 128 and never grow
   again), the layout scratch lives in reused members, the three breakers
   fill a caller vector, and a new `Page` reserves room for a typical page
   instead of doubling eight times. Cost: ~6 KB of scratch resident for the
   build instead of transient (class B, O(1)).
2. *Page block.* A `TextBlock`'s flat arena (the largest of the three
   per-line allocations) comes from a page-scoped block in the lent region:
   `ParsedText` calls the parser's page-fit hook *before* it materialises a
   line, so a line that overflows is allocated from the page it lands on,
   and the parser rewinds the block once the page is serialised. Table
   cells and the reader-side deserialize keep the heap. A full region falls
   back to the heap per line. The CSS parser no longer reloads its ruleset
   lazily in arena mode (that would have committed a block above an open
   page block, L3).

Host census, lent-arena mode, allocations per build:

| Book | before | after step 1 | after step 2 | of which per-line |
|---|---|---|---|---|
| Strange Pictures | 84 728 | 34 323 | 27 177 | 14 292 (object + `PageLine`) |
| moby-dick | 204 437 | 94 595 | 67 342 | 54 536 |
| test_tables | ~1 900 | 1 841 | 1 834 | — |

Section-cache bytes are unchanged throughout (goldens byte-identical); the
arena high-water stays at the extraction peak (45 872) because the page
block lives in the ≥ 30 KB that was idle. What remains per line is the
`TextBlock` object (88 B) and the `PageLine` (24 B), both held through
`std::unique_ptr` with the default deleter; moving them into the page block
needs an arena-aware deleter on `Page::elements` (~30 sites) and is the
candidate step 3 once the device numbers say whether it is worth it.

On the device the acceptance test is the run-3 scenario: a Background-C
build of *Strange Pictures* from a wiped cache must complete (no hard
text-layout abort, no blocking escalation) with a higher minimum free heap
than run 5's, and the reader must not lose contig across the build.

*Device run 6 (X3, 2026-09-25 19:18, same chapter from a wiped cache,
heap pre-fragmented — the reader entered at contig 22 516 after a failed
first-open realloc):*

| | run 4 | run 5 | run 6 |
|---|---|---|---|
| reader entry free / contig | 48 204 / 45 044 | 47 480 / 38 900 | 43 188 / 22 516 |
| min free during the C build | 16 524 | 16 068 | 13 392 |
| … relative to entry | −31.7 KB | −31.4 KB | −29.8 KB |
| contig while reading afterwards | 27 636 | 18 420 | 22 516 |
| contig lost across the build | −17.4 KB | −20.5 KB | **0** |
| arena high-water / failedAlloc / releaseFails | 45 884 / 0 / 0 | 45 884 / 0 / 0 | 45 884 / 0 / 0 |

The build completed (157 pages → header walk from the region → 180, run
5's sequence), with no hard-gate abort, no escalation and no page-block
refusal. The build **no longer costs contig**: identical before and after,
where runs 4 and 5 each lost 17–20 KB to it. Min free improved only ~1.6 KB
relative to entry: the ~6 KB of layout scratch now resident for the build
(step 1) offsets most of what the page block moved out (step 2) at the
low-water point — a wash on bytes, a large gain on allocation count.
`lowHeapSkips` rose to 215 (from 82) because free spends longer under the
24 KB lean floor; harmless with the arena-resident ruleset.

*Device run 7 (X3, 2026-09-25 19:29, same chapter after a heap-recovery
restart, reader entry free 43 200 / contig 32 756):* build sequence and
contig behaviour as in run 6, but the boot-wide watermark told the rest of
the story: **minimum free during the C build was 6 408 B** (run 5 on the
old firmware: 11 272). The "Low heap" samples are taken at layout time; the
true minimum is a *mid-build page draw* — its ~10.5 KB `Page` is heap (§5
P1) — landing on whatever the parse holds at a slice boundary. Step 1 made
~8 KB of that resident (the 128-entry word vectors and the layout scratch),
where the old code held nothing between paragraphs; the page block's gain
did not cover it at that moment. So R2 as first landed traded 68 % of the
churn for a 5 KB lower floor at the one moment that matters.

*Fix (step 1c):* `ParsedText::releaseLayoutScratch()`, called from the
parser's `onSliceYield()` when `Section::runBuildParse` yields a slice —
the only point a draw can interleave. Pure scratch is dropped every yield
and the word vectors whenever the block is empty; within a slice nothing
regrows. Cost: a handful of allocations per slice instead of ~16 per
paragraph. Blocking builds never yield and are unaffected. Device
re-measurement pending.

*Device run 8 (X3, 2026-09-25 21:30, step 1c flashed, clean boot, no
capture gap):* reader entry free 47 420 / contig 38 900 — run 5's baseline
exactly. First pass: the C build completed as before (157 → walk → 180,
`lowHeapSkips` 123, down from 215), but **contig after the build was
18 420 — run 5's number**. Runs 6 and 7 had started at 22 516, below what
the build touches, so their "0 contig lost" was an artefact of the starting
point, not a property of the build; that claim is withdrawn. The post-build
contig is the reading state's own live objects (the deferred-AA `Page`,
the font page slots — §5 P1/P2/P4), not build churn. The boot-wide
watermark was **7 340 B** (run 7: 6 408; run 5: 11 272): step 1c recovered
some, not all.

Second pass from the reading state (entry free 41 788 / contig 18 420,
which had *survived* Home → Reader, so something allocated while reading
pins it — R3): the C build **aborted at page 146 on the contig gate**
(`17028 free, 5620 max alloc`), the latch sent it to the released blocking
build (R1's escalation rule, working as designed: 98 KB free, 180 pages,
zero CSS skips), and the buffer came back. So C's margin on this chapter is
about 5 KB of entry heap: it completes from 47 KB and dies from 42 KB.

Two consequences landed as *step 2b*:

1. The mid-build draw's `Page` now takes its `TextBlock` bytes from a block
   on the build arena (`loadPageFromActiveBuild(idx, scratch)` →
   `Page::deserialize(file, scratch)`), opened by the caller and closed by
   `displayBuildPage` before it releases the lock: ~7 KB of the draw's
   ~10.5 KB leave the heap at exactly the watermark moment.
2. A pre-existing ordering hazard that step 2 had turned live: the
   font-slot arena scope in `displayBuildPage` closed at function exit,
   *after* the unlock that lets a build slice run during the waveform wait.
   With per-line arena allocation in the parser, a slice's lines could land
   above that open block and be rewound with it. Every arena scope in that
   function now closes before the unlock. Device re-measurement pending.

Run 7 also caught a pin outside the reader's scope: at Home exit the
framebuffer realloc failed at contig 40 948 with 101 KB free, and the pin
forensics showed two **task stacks** (`a5a5a5a5` fill: 10 752 B and 2 176 B)
bounding the hole — the lazily created `KOSyncWorker` (`WORKER_STACK_BYTES`
10 240, `ensureTask()` on the first sync job) and the button sampler (2 048).
A task created while the buffer is released pins the hole for the session;
that is what held reading-time contig at ~22.5 KB in runs 6 and 7 and made
the KOReader sync fail its 26 624 B TLS gate. Belongs with F6/R3: create
long-lived tasks before the first release (or give them static stacks).

*Device run 9 (X3, 2026-09-26 morning, step 2b flashed):* the drill from a
wiped cache completed in Background-C; then the reader got stuck on one
image page, recovered to Home, and the next open rendered it — with the
serial capture dead for that window the stall is unattributed. Afterwards
the buffer was gone for good: realloc failing at contig 36 852–40 948 with
96 KB free, under the restart gate's 98 304 floor. That session is what
prompted R1b.

*Device run 10 (X3, 2026-09-26 11:00, R1b flashed):* R1b works — Home
covers and first-open indexing log `Lent … / Returned …`, no realloc, no
"drop the ePub and retry". Chapter 3 then came up **empty**, and the log
shows a chain of four defects, all fixed in `1e9c2b4d6`:

1. **The C build aborted at page 146 on the hard contig floor** — `14092
   free, 3956 max alloc` against 6 128. Same page as run 8's second pass,
   so it is where this chapter's heap bottoms out, not a random dip. The
   6 KB floor was sized when every line's `TextBlock` bytes came from the
   heap; with them in the page block, the largest phase-(b) heap request
   is the 128-entry word vector of a 97-word block (`128 × 24 = 3 072 B`).
   Arena builds now use 3 584 (5 120 with bionic, which builds a
   transformed copy of the block's tail). To make that safe, `addWord`
   checks the largest free block before its four `reserve`s — unchecked
   `std::vector` growth is an `abort()` under `-fno-exceptions` — and a
   refusal ends the parse on the partial-cache path.
2. **Why contig was 3 956 with 14 KB free:** step 1c's
   `releaseLayoutScratch` `shrink_to_fit` the word vectors at every slice
   yield, and a yield lands mid-paragraph almost always (the 1 KB feed
   chunk ends inside a `<p>`). Each slice therefore reallocated the vectors
   to an odd exact size and the next words regrew them by doubling from
   there — 40 → 80 → 160, a 3 840 B request *above* the 128-entry steady
   state — a free/alloc pair per slice. Mid-paragraph the vectors now keep
   their capacity (at most ~3.6 KB, reached once); an empty block still
   gives them back, so the run-7 watermark fix stands.
3. **The escalation could not place its inflate ring.** After the release
   the hole was 53 236 contiguous; by the time `runBuildParse` asked for
   the 33 824 B ring, the 10 KB owned arena and ~8.5 KB of setup had gone
   into it (`Failed to allocate ZIP arena (33824 bytes, free=75432)`),
   twice (the no-CSS retry too) → 0 pages → "showing empty chapter". The
   forensics afterwards: `free 41116` + a 92 B path string + a 24 B block +
   `free 10012`, i.e. two session-lifetime allocations made during that
   failed build pin the hole, and the framebuffer never came back (contig
   40 948, every refresh a half refresh, for every later chapter — spines
   4 and 5 also failed the ring). A build on the owned arena now claims
   the ring *first*, before the arena and before setup (skipped when the
   inflated XHTML is already cached).
4. **The recovery restart never fired:** 96 268–97 188 free against a
   fixed 98 304 floor, 1.1–2 KB short, with 1.8× the buffer free. The
   floor is now the buffer plus 32 KB (85 040 on the X3).

Also from this run: `estimatePagesForSpine` assumed 1 024 B of XHTML per
page; this chapter is 634 B/page (114 201 B → 180 pages), so both page
LUTs doubled at page 112 — now 512 B/page. And the same 10 752 B task
stack at `0x3fcbc778` bounds the hole again (run 7's `KOSyncWorker`
finding, still R3). Run 8's "C's margin is about 5 KB of entry heap"
should now read as: the floor, not the heap, ended those builds — the
device had 14 KB free when it gave up. Device re-measurement pending:
the drill from the reading state must complete in C (no `aborting parse`),
and if it does escalate, no `Failed to allocate ZIP arena` and a resident
buffer afterwards.

*Device run 11 (X3, 2026-09-26 15:28, `1e9c2b4d6` flashed, clean boot):*
**both passes complete in Background-C.** First pass from Home (entry free
46 312, contig 38 900): 157 → walk → 180 pages, arena high-water 45 876,
`failedAlloc=0 releaseFails=0`. Second pass from the reading state after a
cache wipe (entry free 43 936 / contig 23 540 — the state that aborted at
page 146 in runs 8 and 10): 157 → 180 pages, high-water 45 884, no
`aborting parse`, no `Word vector growth refused`, no escalation, no
`Page arena block` / draw-block refusal, the buffer never left the reader.
The parser's gate logged 26 soft-zone passes and the **lowest contiguous
block it ever saw was 12 788 B** (lowest free 17 260) — against run 10's
3 956 B on the same chapter from a *better* entry state. So the
fragmentation was the yield-time `shrink_to_fit` churn (fix 2), and the
lowered floor (fix 1) was not needed for this chapter; it stays as the
correct sizing for arena builds. Boot-wide watermark **8 452 B** (run 8:
7 340; run 5: 11 272), reading contig after the build **26 612** (run 8:
18 420). The escalation path (fix 3) and the relative restart gate (fix 4)
were therefore not exercised in this run.

**R3 — one declared budget per build, not thirty gates.** *Steps 1-5 done
on `memory/audit-2026-09` (2026-09-26), device validation pending.* Once R1
and R2 land, the lent region has a known layout: resident lane (ruleset + SAX +
chunk, ~12 KB), phase-a lane (ring + grow, ≤ 41 KB, freed before phase b),
phase-b lanes (paragraph + page + font slots). `BuildArena` should report
high-water *per lane* in the existing `SCT` summary line, and the gates that
were derived from heap guesses should be re-derived from those measurements
or deleted: delete `bgB_waitheap` and `bgB_cssResident` (never admitted; the
borrow gate is what B uses); re-derive `IN_PLACE_BUILD_CSS_*` and the 44 KB
embedded-style floor from a trace; add the 16-byte slack to the six exact
contig floors. Every remaining refusal that changes output must latch: a
status bit for a demoted table row and for CSS skips on a blocking build, and
a quality byte in the `.pxc` header for a coarser progressive decode.

*What landed for R3 (2026-09-26):*

1. *Per-lane telemetry* (`646887768`): `BuildArena::beginLane()` /
   `laneHighWater()`, and the build's summary line now reads
   `arena: cap=… highWater=… lanes(setup=… extract=… resident=… parse=…)
   failedAlloc=… zipHW=…` — setup = resident after setup (ruleset),
   extract = the extraction phase's peak above it (ring + grow), resident =
   what phase (b) starts from (setup + chunk + SAX), parse = the parse's
   peak above that (page blocks, font slots, draw block). The next device
   run gives the per-lane numbers the declared budget is derived from.
2. *Dead gates deleted* (same commit): `bgB_waitheap`, `bgB_cssResident`,
   `bgB_embeddedCss`, the `bgB_residentAbort` mirror, their four floors and
   the central-directory scan per target spine. Background-B builds only
   in the borrowed buffer; with nothing to lend it waits and Background-C
   builds on navigation — which is what happened anyway.
3. *The 16-byte slack* on every contig floor whose refusal changes the
   build path or the output: the image-header read, the embedded-style
   gate, B's borrow gate, `heapAllowsInPlaceBuild`, C's `residentAbort`.
4. *Latches* (`404e000bc`): a demoted table row and CSS skips now persist
   in the section status byte (`kStatusTableRowDegraded`,
   `kStatusCssDegraded`) and earn the spine one rebuild per session on
   entry, B discards such a build, and a blocking build that still came out
   degraded says so at ERR. A progressive JPEG decoded coarser than asked
   (or as the DC preview) is stamped `PXC_MAGIC_COARSE`; readers replay it,
   the pre-reboot warm pass — the one pass with every framebuffer released
   — drops it and decodes again. That closes all four of F3's silent bakes
   except the outright decode refusal, which was never cached.
5. *Re-derived floors*: `SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES` stays at
   44 KB, now as 24 KB lean resolver floor + ~10 KB SAX on the heap + ~4 KB
   index + ~6 KB page/paragraph heap (host census, heap-only mode: 37 KB
   whole-process peak on the CSS fixture) instead of "56 minus the hot
   cache". `IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES` 66 → 56 KB: 28 KB
   working set + 24 KB lean floor + 4 KB margin, replacing the 40 KB floor
   in the old sum; X4 confirmation of the new admission band pending.

*Device run 12 (X3, 2026-09-26 16:13, R3/R4/R5 tip flashed):* the Chapter 3
drill again, clean open and from the reading state after a wipe. Both
complete in Background-C (157 → 180 each), no abort, no growth refusal, no
escalation, no realloc failure, no cap or latch line; boot-wide watermark
8 420 (run 11: 8 452), reading contig after the build 25 588 / 23 540, the
lowest block the parser gate saw 13 300. **The lane figures**, four builds:

| build | setup | extract | resident | parse | highWater |
|---|---|---|---|---|---|
| first open, ring in the arena | 2 861 | 41 984 | 13 596 | 11 753 | 45 876 |
| rebuild pass, cached HTML | 2 861 | 0 | 13 604 | 11 161 | 24 765 |
| reading state, ring in the arena | 2 861 | 41 984 | 13 604 | 11 991 | 45 884 |
| its rebuild pass | 2 861 | 0 | 13 596 | 8 256 | 21 852 |

The arena's high water is the extraction phase alone: ruleset 2.9 KB, then
ring (32 KB) + grow block (8 KB) + read buffer and alignment = 42 KB. The
layout starts from 13.6 KB resident (ruleset + chunk + SAX) and peaks 8 to
12 KB above it, so during phase (b) — the phase where the heap is scarce —
**27 KB of the lent region sits idle**. That is the declared budget's shape:
a 42 KB phase-(a) lane that the layout never touches, and a phase-(b) lane
with room for roughly twice what it holds. The obvious tenant is the
parse's remaining heap objects (the word vectors, the per-page LUTs and
labels, the `Page` object and its lines — R2 step 3), which would take the
reading-state build's heap need down by their size and make the C-failure
escalation rarer still.

*Run 12, second book (Roosevelt, appendix-b — the table chapter):* **a
regression, found and fixed the same afternoon.** The borrowed build of
the appendix read 11–18 KB free through the tables; the row gate wants
18 432, so every row was demoted to paragraphs and, because the R2 work
made the build survive, nothing escalated and that layout was cached:

```
lanes(setup=756 extract=41984 resident=11484 parse=1383)
```

1.4 KB of the arena used while the rows starved for heap. Before R2 the
same build ran out of heap and the released rebuild (95 KB free) laid the
tables out — the escalation was rescuing tables by accident. Two fixes:
(1) `07251eedb`: a Background-C build with a heap-demoted row escalates on
purpose, like a css-degraded one (restores the output at the cost of the
release cycle); (2) the lasting one: table rows lay their cell lines out in
the arena. Each row is laid out in a transient block above the page block,
committed when it stays on the page, released and re-laid out from its
preserved source when it has to open the next page — so a fragment's bytes
always live in the block of the page it lands on, as the arena's LIFO
discipline requires. The closing border pixel now counts in the fit test.
With the bytes off the heap the row gate on an arena build drops to the
hard floor + 3 KB. Host: goldens unchanged, heap and arena dumps
byte-identical for the three table fixtures. This is the first piece of
R2 step 3 (the parse's remaining heap objects into the phase-(b) lane);
device validation pending on appendix-b.

*Run 12, a second item the tables pointed at:* every image chapter was
being laid out **twice** on a borrowed build. The log for Chapter 3 shows
each image's header "beyond the 4 KB window", the first walk stage wanting
16 896 B of heap against a budget of ~14 KB, and every image deferred to
the build's end — where the walk resolved them all from the idle arena and
the chapter was rebuilt (157 → 180 pages, 6.5 s + 5.2 s, on every open).
`resolveDeferredNow` now hands the build arena to `walkEntry` (which
already knew how to stage a walk from one; the entry reader scopes its
block above the page block), so headers resolve inline and the second
pass disappears. Device validation pending (`3b6f28dbc`).

*Device run 13 (16:42, arena row layout flashed), appendix-b from a wiped
cache:* the rows are in the arena — `lanes(setup=756 extract=41984
resident=11492 parse=8896)` against run 12's `parse=1383`, and the chapter
came out **68 pages of grids** instead of 75 of paragraphs. But the very
first row was still refused: `12096 free` against the new 12 KB bar, 192
bytes short. That single refusal escalated the chapter to the released
rebuild (67 pages), whose realloc then failed on a pinned hole (53 236
free after the release, 34 804 / 40 948 largest after the build) and the
relative restart gate took it straight back into the reader — correct
output after an ugly detour. Two small changes: the arena row bar is now
the hard floor + 1 KB (a grid row takes ~2–3 KB of heap now), and the
blocking path's realloc failure runs the pin forensics too.

The open question is the heap itself: entry 40.3 KB, 38.0 KB after
extraction, 24 KB by page 25 (twenty long-block splits, no mid-build draw
in between), 12 KB at the first row — and 44.9 KB free again once the
build state was torn down. Some 26 KB of *transient* heap is held across
the text pages of this chapter. The host does not reproduce it (a
synthetic 40 × 150-word chapter peaks within 1 KB of its 100 × 60-word
twin), there is no word-width memo any more, and the fonts are
flash-resident. The parser's compiled-out per-page heap trace
(`SCT_HEAP_TRACE`) is the instrument for it: the next device run carries
it.

*Device run 14 (16:53, memo cap + arena header walk + per-page heap
trace):* **both regressions closed on the device.** Appendix-b from a wiped
cache: no row refused, no escalation, 67 pages of grids in one pass; and
the trace answers the run-13 question — free heap is flat at ~27 KB
through the 29 text pages (run 13: 24 KB by page 25, 12 KB at the first
row), so the style memos were the 26 KB. The table pages themselves still
cost heap: at pages 30–38 free sits at 12–15 KB and the block count jumps
from 387 to ~700–750, which is the fragment's *object* graph (a TextBlock
object, a cell and a lines vector per cell, ~100 B each) — the bytes are
in the arena, the objects are not, the next candidate for the phase-(b)
lane. Chapter 3 from a wiped cache: **one pass, 180 pages in 7.6 s**
(before: 157 → walk → 180, 6.5 s + 5.2 s), heap flat at 20–25 KB across
all 180 pages, the walk's ring stages visible in the parse lane (34 264,
peak 47 860 of 52 272). Lowest contiguous block seen by a gate: 5 108
during a long-block split on a table page — above the arena floor, not by
much.

Still open under R3: the reading-time pins (S13 scaled-glyph cache, P1
deferred-AA `Page`, P4 font slots), the lazily created `KOSyncWorker`
stack pinning the hole at Home (F6), and the lend-vs-release revisit for
the C-failure escalation once the parse's remaining heap objects move into
the page block.

**R4 — give every cap a defined behaviour past the cap (F5).** *Done on
`memory/audit-2026-09` (2026-09-26).* The rule to
apply to §4: a cap is acceptable when (i) it is provably above any input and
the bound is written next to it, or (ii) exceeding it is logged at `LOG_ERR`
once and latched in the section status byte so the reader can say "chapter
simplified" and rebuild when memory allows, or (iii) it is replaced by a
nothrow-growing or card-backed structure, as `images.pending` replaced
`kMaxPending`. In that order of preference for the §4.1 list: the SAX depth
overflow becomes a parse error (the `YXML_ESTACK` path already exists) instead
of a shifted stack; `Page::footnotes` grows nothrow (a `FootnoteEntry` is
128 B, the vector is per page); the anchor cap latches the status byte; the
`FootnoteEntry` truncations refuse the entry rather than navigate a broken
href; `MAX_RULES` overflow writes a cache marked "rules truncated" so the
book is parsed once, not on every open. For §4.2, apply the same cap on the
build side (split the page, or fail the build with the truncated status) so
nothing is ever built that cannot be loaded. For §4.4, the R2 lanes give the
per-paragraph and per-page containers a fixed capacity with a logged
fallback; `scanByFont_` gets a bounded scan buffer.

*What landed for R4 (2026-09-26):* the SAX depth cap now flattens the tree
past 64 levels instead of shifting it (the excess elements are unreported,
their text goes to the deepest reported ancestor; host test); footnotes per
page 16 → 64 with the refusal reported (the `test_spine_toc_edges` goldens
change by exactly the four links the old cap dropped); a footnote href too
long to navigate is refused rather than stored truncated;
`ChapterHtmlSlimParser::noteCapOverflow` logs each cap once at ERR and the
union lands in the section status byte as `kStatusSimplified`, which the
reader logs on a cache hit and never rebuilds on (deterministic);
`Page::MAX_ELEMENTS` and `Page::MAX_PAGES_PER_SECTION` now bind on the
build side too (§4.2: serialize clamps and reports, the parse stops
truncated at 10 000 pages), the printed-page label count stops at the
`uint16`; `MAX_RULES` overflow writes a cache stamped truncated in the
existing flags byte (`CssParser::rulesTruncated()`, logged on every load)
instead of no cache and a re-parse per open; the font prewarm scan buffer
is bounded at 4 KB per (font, style). Left as they are, documented as
bounded or time-only: SAX attributes (names/values the layout does not
use), `partWordBuffer` 200 B, float nesting, `FootnotePreviews` 256/512,
ZIP entry names, SD font families, nested footnote jumps, `anchorsAwaitingLine_`.
Not done: a UI hint for `kStatusSimplified` (a product decision; the
truncated-chapter hint is the model if wanted).

**R5 — keep the census honest.** *Done (2026-09-26).* Commit the `--arena` mode and
`WH_HOST_STDIO_UNBUFFERED` (in the working tree at the time of writing:
`test/epub_pipeline/{DumpMain,PipelineRunner}.*`,
`test/zip_entry_reader/HalStorage.h`), lend the arena to
the header walk in the harness, and add a host test that fails when the
heap-side peak of the four fixture books rises by more than a set margin —
the test the pending-image queue never had.

*What landed for R5:* the census mode and unbuffered stdio were already
committed (`c0a28a1e1`); the harness now runs the deferred image-header
walk after each build from the lent region (as Background-C does), and
`HeapPeakRegression` (ctest, `test/epub_pipeline/heap_peak_check.py`)
compares each fixture's heap-side peak in both modes against
`heap_peak_baseline.txt` with an 8 KB margin. Baseline at landing (heap-side
bytes, heap-only / arena): moby-dick 62 933 / 54 697, test_large_css
36 306 / 26 506, jpeg_images 47 489 / 28 187, jpeg_metadata_heavy
61 555 / 41 388, table_streaming 56 809 / 36 164, inline_footnotes
42 805 / 29 121. `UPDATE_HEAP_BASELINE=1` re-baselines on purpose.

**R6 — audit the warm-boot footprint.** *Resolved 2026-09-26: a
measurement artefact, no warm-boot-specific cost.* The post-sync silent
restart (`RTC_SW_CPU_RST`) read 52 908 free / 34 804 largest at
`setup_complete` against the cold boot's 62 280 / 53 236 — but on the warm
boot Home enters *before* the mark (no boot splash), on the cold boot
*after* it. Home's entry loads the reading-stats store for its history line
(`ScopedLoad`, 18 books), and that is the whole difference: cold boot,
right after Home enters, 54 348 free / 32 756 largest; warm boot 57 936 /
34 804. Ten seconds in, both sit at 30.6 KB free at Home. What the
comparison does show is the cost of that load itself: a ~17 KB transient
(startup minimum 39 224) that takes the boot heap's largest block from
~55 KB to ~34 KB before the first Home draw, and the store is released
afterwards, so the contiguity loss is fragmentation from the transient.
Home's later needs — the cover lend/return leaves it at 11–15 KB largest,
and the KOReader sync's 26 624 B TLS gate has failed at Home before (F6) —
make that worth a smaller projection for the history line (per-book totals
only) rather than the whole store. Filed under R3's Home-time pins.

**R7 — schedule image work ahead of far look-ahead.** *User direction,
2026-09-26.* The background lanes today parse the next section (B) or the
current one (C) and leave image decode to the page turn that reaches the
image. A reader five pages from an undecoded image should not be spending
its idle time laying out a section fifty pages away. Add a background lane
that decodes and scales the not-yet-processed images nearest to the
reading position (the `images.pending` queue already knows which ones),
and rank it above B's look-ahead when a pending image lies within a few
pages. The lane borrows the same region the builds do, so it is exclusive
with them by construction; the scheduler decision is the new part.

**R8 — bound the reading-stats store (F8).** *Follow-up, agreed
2026-09-26; not started.* (1) Mark the store loaded only when the parse
succeeded or the file is genuinely absent, and refuse to save otherwise —
this is the data-loss fix and comes first. (2) No entry for a zero-second
session. (3) Drop per-book day buckets from RAM, or keep them for the web
export only; trim the global buckets to the last ~400 days at save time.
(4) Cap the book list by last-read date (~100). (5) Deserialize from the
file stream instead of a `String`, which removes one of the three copies
from the load peak. The Home-entry cost (R6's residual) falls out of (3)
and (4).

## 8. Appendix — where the numbers come from

- Device runs (X3, firmware at PR #310's tip): run 3 = `device_run3.log` (14:19, wiped cache, blocking build), run 4 = `device_run4.log` (15:13), run 5 = `device.log` (15:19, after the SAX-in-arena fix).
- Host census: `epub_pipeline_dump <book> <cacheDir> --bench [--arena=52272]`, stderr `BENCHMARK` lines; `WH_HOST_STDIO_UNBUFFERED=1`; symbolised with `addr2line -e test/build_test/epub_pipeline/epub_pipeline_dump -f -C -i <off>` and, for inlined sites, the `test/build_gprof` binary.
- Source inventories: file:line references above are against `7b16746d0`.
