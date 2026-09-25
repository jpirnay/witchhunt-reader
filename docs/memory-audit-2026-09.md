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
| text layout, hard (`:679-697`) | 9 216 / 6 128 | stop parse, keep partial cache if pages > 0 | status bit; auto-rebuilt only when pageCount == 0 |
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
| `maybeRestartForFragmentedHeap` (`:4456-4507`) | free ≥ 98 304 and contig < the framebuffer size | silent restart | was a constant `52*1024` (976 B under the X3's 52 272) — **fixed in R0**; two of three callers deliberately pass contig = 0 because the heap may be corrupt after decode failures and walking the TLSF free list there has crashed the IWDT |
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
completes and no escalation happens. Device run 6 did not reach the
blocking path at all (C completed), so the borrowed blocking build is still
unexercised on hardware; it is the same code path as C's borrow, but its
`Index start mem (secondary buffer BORROWED …)` / `Index end mem (after fb
return)` pair should be seen once on a CSS-fallback or image-header rebuild
before this is called validated.

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
24 KB lean floor; harmless with the arena-resident ruleset. The candidate
next step, if the min-free gain is wanted too, is to take that resident
scratch from the lent region in B/C builds — for the R3 re-measurement.

**R3 — one declared budget per build, not thirty gates.** Once R1 and R2
land, the lent region has a known layout: resident lane (ruleset + SAX +
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

**R4 — give every cap a defined behaviour past the cap (F5).** The rule to
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

**R5 — keep the census honest.** Commit the `--arena` mode and
`WH_HOST_STDIO_UNBUFFERED` (in the working tree at the time of writing:
`test/epub_pipeline/{DumpMain,PipelineRunner}.*`,
`test/zip_entry_reader/HalStorage.h`), lend the arena to
the header walk in the harness, and add a host test that fails when the
heap-side peak of the four fixture books rises by more than a set margin —
the test the pending-image queue never had.

## 8. Appendix — where the numbers come from

- Device runs (X3, firmware at PR #310's tip): run 3 = `device_run3.log` (14:19, wiped cache, blocking build), run 4 = `device_run4.log` (15:13), run 5 = `device.log` (15:19, after the SAX-in-arena fix).
- Host census: `epub_pipeline_dump <book> <cacheDir> --bench [--arena=52272]`, stderr `BENCHMARK` lines; `WH_HOST_STDIO_UNBUFFERED=1`; symbolised with `addr2line -e test/build_test/epub_pipeline/epub_pipeline_dump -f -C -i <off>` and, for inlined sites, the `test/build_gprof` binary.
- Source inventories: file:line references above are against `7b16746d0`.
