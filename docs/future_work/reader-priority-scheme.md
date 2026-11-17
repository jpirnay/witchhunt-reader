# The priority scheme of the EPUB reader's render passes and background work

Design study, 2026-11-17. Analysis only: no code changed, no device touched.

**Status (2026-11-17).** The scheme is the owner's: 1) a complete page, 2) what the page on screen still needs,
3) everything else, with input above all three; the page count may stay an estimate and never holds back the gray;
One-Pass Anti-Aliasing is designed for first. The order of work in section 5 is accepted. In work: steps 1-3 and an
adaptive settle for V5 (the wait depends on whether the reader is flipping). Open: steps 4-7, and the decisions of
section 8, which stand as recommendations until taken. Delete each item here as it lands.

The logs it quotes are bench logs kept outside the repository (the maintainer's `witchhunt-work` folder); the figures
are also in the PRs that came out of the 2.53 bench (#789, #792, #802, #805, #808).

**Read at:** firmware `origin/master` 41c3c7de9 (incl. #789, #792, #802, #805), SDK submodule 00a6747b, branches
`origin/perf/psram-early-aa` (PR #808) and `origin/perf/build-through-first-page-refresh` (fix 5, not device-checked).
Fix 6 (deferred refresh in the SSD2677/UC8253-Murphy drivers) has no branch yet.

**Short names.** `ERA` = `src/activities/reader/EpubReaderActivity.cpp`; `FID` = `freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp`;
drivers = files in `freeink-sdk/libs/display/FreeInkDisplay/src/driver/`. W = write buffer (`frameBuffer`), S = secondary buffer
(`frameBufferActive`). RL = `RenderLock`.

**Logs** (line stamp = host seconds, restarting at each bench step; device clock in brackets where a stamp is quoted):

| Name | File | Build |
|---|---|---|
| X3a | `triage-2026-11-15/bench-x3/logs/x3i3-A.log` | master a4078b57e, 2026-11-16 |
| X3f | `triage-2026-11-15/bench-x3/logs/fin-x3.log` | cold-open final, 2026-11-15 |
| X4c | `bench-2.53/x4fix/logs/combined.log` (+ `combined.out`) | fixes 1+2 = #802 + #805, 2026-11-16 |
| X4m | `bench-2.53/x4fix/logs/master.log` | master before fixes 1+2 |
| Pw | `bench-2.53/logs/picco-wr.log` | release 2.53 |
| CPx4, CPx3 | `bench-2.53/logs/x4-cp.log`, `triage-2026-11-15/bench-x3/logs/x3-cpdev.log` | CrossPoint 5ea6927e |

**In short.**

- The order in `serviceBackgroundWork()` is right in the main (V10 lists small slips). Class 1 is delayed by who holds a
  buffer, not by the order.
- On the X3 and X4 a page drawn during a chapter build waits for the whole chapter for its gray: +3.0 s and +2.8 s on
  the bench chapter, 33-34 ms more per page (V1). The controllers do not require this: the gray needs plane scratch
  and the B/W page once more, not the secondary buffer (section 4).
- The look-ahead build costs page turns more than documented: on the X4 at 2.5 s a page, 43 of 71 FAST turns lost
  their one-pass and took 1.1-1.3 s instead of 0.70 s (V2). Pages turned during a build-through never get their gray (V3).
- Recommended: gray during the build from the write buffer alone, look-ahead borrowing the other buffer, no background
  slice while a press is in flight. No partial sections, no suspendable arena, no second core (sections 5 and 6).

**Limits.** The SDK drivers of the S3 boards (UC8179, UC8279-X4, Murphy, SSD2677, LGFX) were read by two helper passes
and spot-checked at the lines quoted in section 4; the C3 drivers and the reader were read directly. The bench book has
no pictures and no KOReader account, so picture pages and auto-sync have no measurement here. The T5S3 and the Murphy
have no bench log in this set: "not measured" throughout.

---

## 1. Inventory

Two tasks, one mutex (docs/background-rendering.md:15-34). The render task draws one pass per `render()`
(`classifyRenderPass()`, ERA:4665-4690); the loop task runs input and, on an input-free pass, `serviceBackgroundWork()`
(ERA:1270-1276, 1356-1391). Class: 1 = the complete page, 2 = the current page's pictures, 3 = the rest; U = user input.

### Render task

| Pass | Does | Holds | Measured | Abort | Class |
|---|---|---|---|---|---|
| `recoverSecondaryBufferIfNeeded()` ERA:4512-4596, top of every `render()` | Ends Background-B's borrow (discards its live build), returns C's borrow after a build | RL | X4 teardown 48 ms (X4c 187.138 -> 187.186) | - | serves 1 |
| BuildSection: `buildSection()` ERA:5305-5793; runs when `section` is null (open, crossing, jump, relayout) | Cache probe, build mode, Indexing popup, borrow of S, first slices | RL, bus (popup) | X3: 3-4 slices, 386-462 ms while the popup paints (X3a 3.516, 3.442) | - | 1 (no page exists yet) |
| SectionBuilding: `displayBuildPage()` ERA:5979-6049, 7017-7122; asked for by a C slice once the wanted page exists (ERA:2853-2858) | Page from the live build, **B/W only** (ERA:7022), pictures as placeholders (ERA:7058); trigger, RL released, wait | RL to the trigger; bus through the waveform (`HalDisplay::completeDisplay()`, lib/hal/HalDisplay.cpp:503-506) | HALF: X3 459 ms + 0.24 s finish (X3a 3.674 -> 4.398), X4 1,727 ms (X4c 4.105 -> 5.858), Picco 421 + 152 ms (Pw 1.256 -> 1.910) | no | 1, incomplete |
| Normal: `renderContents()` ERA:6356-7015; a turn without a pre-render, a status tick, a build's end | Decodes the page's own pictures first (ERA:6426-6480), prewarm, B/W render, status bar; then single push (S3 one-pass, T5S3), composed one-pass (X3/X4, FAST only, ERA:6614-6616), inline two-pass (X4, X4 Pro) or arms the deferred pass (X3, Murphy, Picco two-pass) | RL to the trigger (inline AA: through the gray); S lent to decode and prewarm | fresh render: X3 prewarm 23 + B/W 49 ms (X3a 5.498), X4 20 + 43 ms (X4c 46.532) | inline AA aborts on a press (ERA:7190-7220) | 1 (its decode is 2) |
| BufferDisplay: `displayPreRenderedPage()` ERA:7222-7355; a forward turn onto a ready pre-render (ERA:4304-4339) | Status bar over the pre-rendered page, display, gray | RL, bus, both buffers | X4: FAST 507 + replay 103/146/14 ms (X4c 9.920-10.226); one-pass gray 37-42 + upload 28-29 ms (X4c 11.995). X3: FAST 382 + replay 141/228/53 (X3a 8.241-8.710); one-pass 445-449 ms (X3f turns) | replay per element | 1 |
| PreRender: `renderPreRenderPass()` ERA:4765-4919; armed after every page display (ERA:6962-6984, 4754-4758), re-armed once by B's step and by the lane | Next page into W | RL, W | 54-64 ms (X4c 8.697, X3a 7.312), Picco 42 ms (Pw 3.731) | no | 3 |

### Loop task, in the order `loop()` runs it

| Step | Does | Holds | Measured | Abort | Class |
|---|---|---|---|---|---|
| Progress save, ERA:1066-1071 | `progress.bin` write | RL (Try), card | about 30 ms on the X3 (X3a 5.498 -> 5.529) | - | 3 |
| `serviceAutoSync()` ERA:1083-1091, EpubReaderAutoSync.cpp:372-398 | State file; on a push interval a chapter parse for the XPath (ProgressMapper.cpp:146) | RL (Try), card, heap | not measured | no | 3 |
| Input, `pageTurn()` ERA:4244-4366 | Waits for RL, drops the page's owed gray and pre-render | RL | - | - | U |
| `serviceOwedPeriodicRefresh()` ERA:1336-1354; `shouldSkipPeriodicUpdate()` ERA:7756-7808 | Clock/battery/sync-icon redraw of the page | - | not measured | waits while a pass or a gray is owed | 3 |
| `runDeferredGrayscalePass()` ERA:1503-1579 | Two plane renders into W, gray refresh, restore from S (lib/GfxRenderer/GfxRenderer.h:952-988) | RL, bus, W, S | planes/gray/restore: X3 119/228/50 ms (X3a 7.241), X4 87/147/12 (X4c 8.632), Picco 214/269/2 (Pw 3.684) | per element | 1 |
| `stepImageLaneDuringBuild()` ERA:2318-2328 | Picture of the page on screen, beside the build | RL, build arena's free tail (C3) or PSRAM | X3 4.5 s to the picture (docs/background-rendering.md:217-222) | input | 2 |
| `stepCurrentSectionBuild()` ERA:2715-2956 (C) | 40 ms slices of the chapter on screen | RL, **S as arena on the C3** (ERA:5609-5614), PSRAM on the S3 | X3 33.6 ms/page (X3a 4.740-6.422), X4 32.6 (X4c 6.166-8.119), Picco 17.1 (Pw 2.052-3.079) | never (ERA:2103-2105) | 1 until the page asked for exists, then 3 |
| `stepGlyphStoreRoom()` ERA:1399-1409 | Reclaims a glyph-store segment | RL, flash | not measured | no | 3 |
| `stepStarredPages()` ERA:1411-1466 | Bookmark anchors: a chapter parse in the lent S | RL, S | not measured here | no | 3 |
| `stepBackgroundSectionBuild()` ERA:1909-2288: pre-render re-arm, image lane (`stepImageWarmLocked()` ERA:2330-2441), Background-B | Look-ahead: next page, pictures 5 pages ahead, next chapters | RL; lane and B hold S | B: X4 42 pages, parse 1,329 ms (X4c 185.389); X3 38 pages in 1.75 s (X3f 47.648 -> 49.399) | lane: input; B: only by a render | lane 2 (page on screen) / 3; B 3 |

Also: the first open's Indexing popup (ReaderActivity.cpp:969-977), `persistImageManifest()` after a build
(ERA:2893, 2282), the body-size relayout (ERA:2948-2951). Covers and thumbnails are Home's, not the reader's.

---

## 2. Where the code follows the scheme, and where not

The documented order (AA first) is what `serviceBackgroundWork()` does, and it holds in steady reading. It breaks
where a resource rule or a gate sits in front of it. Ordered by cost.

| # | What happens | Code | Evidence | Cost |
|---|---|---|---|---|
| **V1** | **C3: the page a build draws gets its gray only when the whole chapter is laid out.** C holds S as its arena; the gray pass restores from S, so the reader switches AA off while S is away and arms the gray only at the build's end | B/W draw ERA:7022; AA gate ERA:6402-6406; gray armed at `Done` ERA:2952 -> `keepBuildPageOnScreen()` ERA:2958-3012 | X3a [09:13:47-51]: page 4.161, build done 6.751, gray 7.189 (open sent 0.313). X4c [18:36:47-52]: page 5.858, done 8.312, gray 8.617 (open 1.670). Crossing into an unbuilt chapter, X4 2.53: page 1.18 s, gray 3.34 s (RESULTS-x4.md) | X3 +3.0 s (66 pages), X4 +2.8 s (74 pages), growing 33-34 ms per page; both AA modes (one-pass gets the two-pass pass). Picco 2.53 +1.77 s (Pw: page 1.757, gray 3.529), **removed by #808** (gray at 1.98 s). X4 Pro about +1.45 s, derived from RESULTS-coldopen.md (first page 3.64 s, idle 5.43 s); #808 not device-checked there |
| **V2** | **A page turned while Background-B holds S is recognised late and loses its one-pass.** (i) B's `Building` branch runs a slice on every loop pass with no input test (ERA:2147-2205): pages 0-10 of the next spine were laid out between the press and the turn (X4c 186.843-187.134). (ii) B borrows the buffer that holds the frame on screen (FID:626-638), so `beginOnePassMask()` refuses (FID:988-992) and the page takes two passes. (iii) An abandoned spine hands the walk to the next one (ERA:2076-2084), so every turn pays | as left | X4c two-pass: press 186.775 -> turn seen 187.138 -> gray done 188.027 [18:39:50]; without B 9.275 -> 9.386 -> 10.228 [18:36:52]. One-pass: 69.269 -> 70.528, `AA replay` [18:43] against 59.271 -> 59.973, `One-pass AA (composed)`. X3f [21:26:07-10]: borrow 47.648, press 49.368, `AA replay` 50.438, turn 1,430 ms against 1,070-1,120 | X4 at 2.5 s a page: 42 of 73 turns 1.10-1.41 s against 0.93-0.96 s (`combined.out`, round c25); with One-Pass on, 43 of 71 FAST turns took the two-pass replay, 1.1-1.3 s against 0.70 s (five turns at X4c 69.269-80.491), and 45 look-ahead builds were thrown away (X4c lines 5152-8158: 25 `One-pass AA (composed)`, 47 `AA replay` of which 4 on HALF turns, 45 `build discarded`). X3 at 3.6 s a page: one turn per look-ahead build, +0.33 s and a second refresh. S3: lends PSRAM, the frame survives; not measured. The pace matters: B borrows 1.5 s after a page lands (ERA:264-265), so a press 2-4 s after the last meets a build that has just started; at a slow pace B is done first |
| **V3** | **Build-through pages go out B/W and never get their gray.** On a chapter's last 3 pages B keeps S through the turns | ERA:4524-4526, 6402-6406; nothing re-arms the gray in `endBackgroundBorrow()` ERA:1841-1898 | X4c [18:39:46-50]: `building spine 8 through page turns (page 71 of 74)` 183.339; page 71 shown 185.142, no AA line; build done 185.469; next press 186.775 | up to 3 pages per chapter without gray for their whole time on screen (1 here). X3 the same by code, not measured |
| **V4** | **A turn onto a page whose picture is not cached decodes first, draws after.** | ERA:6426-6480, before the prewarm and the B/W render | not in this bench. Documented: 1-4 s a decode (docs/background-rendering.md:148-149), 6.3 s once (ERA:2372-2373) | class 2 before class 1 by the decode's length, all boards. During a build and in the pre-render the order is already page first (ERA:7058, 4846) |
| **V5** | **Inline two-pass waits `AA_SETTLE_MS` = 400 ms before the gray.** | ERA:319, 6899-6904 | X4m [18:45:52-54]: HALF ends 198.871, gray refresh ends 199.454 (146 ms long): 0.44 s gap | X4 and X4 Pro in two-pass mode: every freshly rendered page. With One-Pass on it stays on the X4's HALF pages (open, every 15th turn, after a popup or picture page), which the composed path does not take (ERA:6614-6616) |
| **V6** | **The first open's Indexing popup blocks on the C3.** Async only where the PSRAM arena is lent | ReaderActivity.cpp:969-977 | X3a [09:13:44]: open 0.313, indexing starts 0.971. X4c [18:36:45]: 1.670 -> 2.346 | 0.63-0.66 s before any open work starts |
| **V7** | **The build idles through its page's refresh.** | ERA:2725-2727 | X4c: page 0 at 3.974, page 10 at 6.166 around a 1,727 ms HALF. X3a: 3.541 / 4.740. Pw: 1.159 / 2.052 | X4 1.7 s, X3 0.7 s, Picco 0.65 s of build time. Class 3 loses it; it reaches class 1 only through V1. Fix 5 (X4), fix 6 (Picco, Murphy) |
| **V8** | **`CMD:IDLE:SCREEN` counts the chapter build as screen work.** | ERA:7394-7401 | X3a [09:14:25]: the "turn during the build" case got its idle reply at 7.681, after build (6.850) and gray, and only then pressed (7.706) | bench only: "settled" is not comparable with CrossPoint's, and no log exists of a turn during a C3 build |
| **V9** | **Page count.** (a) The gray waited for the build's end, whose only product for the page on screen is the final count: all boards until #808, the C3 still (V1). (b) A finished look-ahead build redraws the page on screen to firm up the "~" (ERA:2253-2259 -> 1894-1897): a refresh plus a two-pass gray for one number. (c) The first estimate is coarse: `1/~7` for 66 pages (RESULTS-coldopen.md), from an integer percent (lib/Epub/Epub/Section.cpp:2501-2503) | as left | (b) not in these logs (the bench chapters are one file each) | (b) one extra refresh pair per chapter build in books whose chapters span files |
| **V10** | **Small order slips on the loop task.** The progress save, and an auto-sync push's chapter parse, run before the deferred gray (ERA:1066-1091 before 1275). The glyph-store step and the bookmark parse run before the image lane (ERA:1388-1390). The lane waits 300 ms and for the pre-render (ERA:2290-2294, 1393-1397) | as left | save: X3a 5.498 -> 5.529 [09:14:44]. The rest not measured | 30 ms on the X3; the rest unknown |

**Where it holds.**

- Deferred gray before the pre-render: X3a [09:14:44-45] `Deferred AA` 5.933, `Pre-rendered page` 6.000; X4c 8.632 / 8.697. Guard ERA:6140-6147, re-request ERA:1575-1577.
- Warm open: the gray starts as the refresh's finish ends (X3a 5.498 -> gray on the panel 5.879). No gate.
- A pre-rendered FAST turn in One-Pass is complete in one refresh: X3 445-449 ms (X3f), X4 0.70 s press to done (X4c 59.271 -> 59.973), single push on the S3.
- A status-bar tick never costs a page its gray (ERA:7770-7779). A turn aborts a gray pass per element (ERA:7190-7220).
- During a C build: the picture of the page on screen before more build (ERA:1383); jump targets resolve mid-build (ERA:2836-2851).
- The image lane before Background-B (`LaneBeforeBuild.h`); nothing runs while a deferred gray is owed (ERA:1367-1372).
- Under the live bars the page is B/W by design and no look-ahead runs (ERA:6400-6406, 1393-1397).

**By situation.**

| Situation | What happens |
|---|---|
| Warm open | X3: holds. X4: the gray waits the 400 ms settle (V5) |
| Cold open | V1, V6, V7 |
| Turn within a chapter | holds, one refresh in One-Pass, unless Background-B holds S (V2) |
| Turn into a built chapter | rendered fresh (no pre-render across chapters). With One-Pass on, after a look-ahead borrow it took the inline two passes and the settle: 1.38 s (X4c [18:45:29-30]: buffer returned 6.612, press 6.763, `Inline AA` 8.138) |
| Turn into an unbuilt chapter | V1 |
| Jump (chapter list, percent, bookmark, link) | into a built chapter: as a warm open, not measured. Into an unbuilt one: V1; the target resolves mid-build (ERA:2836-2851), except percent, paragraph and last-page targets, which wait for the build's end (docs/background-rendering.md:306-309) |
| Layout change in the reader bars | B/W under the bars by design, gray when they close (ERA:6400-6406); not measured |
| Picture page | V4; during a build the page comes first (holds) |
| Turn during a build | the page is B/W (V1); not measured on the C3 (V8) |
| Turn during a gray pass | holds: the pass aborts per element (ERA:7190-7220) |
| Turn during a pre-render | waits for it, 54-64 ms; the pass cannot abort (ERA:4765-4919) |

**The six suspects.** Gray waits for the whole chapter on the X3/X4: confirmed (V1). Page count finalisation: it is the
build's end that gates, confirmed in that sense (V9a). Pre-render before the gray: refuted, the gray is first. A
Background-B borrow forcing two passes: confirmed and far larger than documented at a fast pace (V2). Build-through
pages B/W: confirmed, and they stay B/W (V3). Idle gate: confirmed (V8).

---

## 3. The scheme as rules

**Complete page.**

| Page kind | Complete when |
|---|---|
| Text, AA on | B/W frame and its gray are on the panel, status bar included, count exact or "~" |
| Text, AA off | B/W frame is on the panel |
| With pictures | as text, each picture drawn or a placeholder. Pictures and their grays are class 2 |
| Under the live bars or a search strip | B/W frame with the UI. Its gray is owed from the moment the UI closes |

A page is **owed** from its B/W trigger until its gray refresh ends or a turn replaces it.

| Rule | Statement | Checkable as |
|---|---|---|
| **R0** | Input first. No class 2/3 unit starts while a press is down, a gesture is unresolved or a turn waits for RL. A running unit ends within one slice (40 ms plus one parse chunk) or aborts (decodes) | a pure gate function; press-to-`PageTurn stats` time with and without B |
| **R1** | While a page is owed, neither task runs class 2 or 3 work, except work that shares no resource with the gray: other bus, other memory, and it gives RL back within one slice | the order function of `serviceBackgroundWork()` |
| **R2** | The gray starts when the panel is free. It does not wait for a build's end, a buffer's return, a count, or a settle timer with no press pending | stamp `page complete`; constant in chapter length |
| **R3** | A class 3 holder of S, W, RL or the bus gives it up within one slice when class 1 or 2 asks, and takes the buffer whose loss costs class 1 least: never the only copy of the frame on screen where one-pass needs it | after any lend, `secondaryHoldsFrameOnScreen()` still true |
| **R4** | Class 2 before class 3: the page on screen's pictures before the pre-render, the glyph store, bookmarks, look-ahead pictures and B | order function |
| **R5** | Class 2 never delays class 1: a page whose picture needs a decode is shown with a placeholder first (decision D2) | page complete before decode start in the log |
| **R6** | The page count is class 3. The status bar shows the best estimate with "~". The final count is drawn by the next refresh that happens anyway and never asks for one (decision D3) | no `requestUpdate()` whose only change is the count |
| **R7** | A complete page is not drawn again for class 3 reasons, except the clock minute | - |
| **R8** | The test API reports "page complete" apart from "chapter laid out" | `IDLE:SCREEN` ends with the gray |

---

## 4. Framebuffer lifetime per controller

What each controller reads from the host, and until when. "Upload" is a blocking SPI write: when the call returns the
buffer has been read (`EpdBus::sendPlaneFlipped`, bus/EpdBus.cpp:477-483; Arduino `SPI.writeBytes`, no DMA). The facade
swaps W and S after every swapping display (FID:411-425) and passes the pushed frame to the finish again (FID:690).
The bus has no RAM read-back (bus/EpdBus.h: only BUSY is read).

### The C3 controllers

| | X3: UC8253 (`Uc8253X3Driver.cpp`), UC8279d (`Uc8279Driver.cpp`) | X4: SSD1677 (`Ssd1677Driver.cpp`) |
|---|---|---|
| B/W FAST | Frame -> DTM2 before the waveform (:232-250). `prev` ignored (:198). **After the waveform the same frame -> DTM1** (:358). UC8279d the same (:174, :218, :131) | Frame -> BW RAM (:957), `prev` -> RED (:976), both before the waveform. The deferred start reads nothing afterwards: the finish only waits (:850-864) |
| HALF, FULL | As FAST. FULL re-reads the frame for its conditioning and settle passes (:351, :371-373) | Frame -> BW and RED before the waveform (:953-955); absolute |
| Pinned how long | One buffer, the pushed frame, from the trigger to the end of the finish: waveform 382 ms FAST, 459 ms HALF, plus 200 ms when not FAST (:334), plus the sync. The other buffer is never read | Nothing after the start returns, about 27-41 ms after the trigger (X4c 9.947: 548 ms for a 507 ms waveform) |
| Baseline for the next FAST | DTM1, in the controller, written by every finish. Lost to a gray pass until its cleanup; `cleanup(nullptr)` makes the next push a FULL (:620-623) | RED, in the controller, but a FAST writes it from `prev` at its **start**, so between two FASTs RED holds the frame before the one on the glass. The truth is in the host's S until the next start, unless RED is seeded (`seedPreviousFrame`, :1058-1065). A blocking single-buffer FAST reseeds both planes after its waveform (:988-993); the deferred one does not |
| Two-pass gray | LSB -> DTM1 (:508), MSB -> DTM2 (:520), or in strips (:524-553). Gray waveform 228 ms, no host buffer (:558). Cleanup: **the B/W page -> DTM2 and DTM1** (:627-629) | LSB -> BW RAM (:1102), MSB -> RED (:1110), or in strips (:1115-1128). Gray waveform 146 ms. Cleanup: **the B/W page -> RED** (:1340); with `nullptr` the next FAST becomes a HALF (:928-933) |
| Composed one-pass | Mask -> DTM1 (:288), page -> DTM2 (:290) before the waveform; after it the page -> DTM1 (:358). UC8279d has no one-pass | Page -> BW (:840), mask -> RED (:841) before; after it the page -> both planes (:857-858) |
| Popup | A B/W refresh of W after `syncWriteBufferFromDisplayed()`, which reads S (lib/GfxRenderer/GfxRenderer.h:512). Shipped async it pins the painted frame only, as any B/W refresh | The same; nothing pinned after the start |
| Absolute gray (sleep cover, BMP viewer; no reader page) | Base: frame -> DTM2 and DTM1 (:450-455), or a full `display()` when the RAM is stale (:431-434); then the planes and the cleanup as above | No base push; the two planes go to BW and RED and one waveform draws all (docs/secondary-buffer-management.md:160-163). Not traced further |
| Upload, measured | 25 ms a 52 KB plane (X3a 7.241: `restore=50ms` for two) | 12-14 ms a 48 KB plane (X4c 8.632, 10.226: `restore=12-14ms`; 11.995: two planes 28 ms) |

**What this allows.**

- **The gray overlay does not need the secondary buffer on either controller.** It needs a place to render each plane
  for the length of its upload, and afterwards the B/W page, from any buffer, for one upload (X4) or two (X3). Today
  that page is taken from S (`cleanupGrayscaleWithPreviousBuffer()`, FID:1349-1390) because the planes were rendered
  over W. `cleanupGrayscaleBuffers(bw)` takes any buffer (FID:1332-1347, lib/hal/HalDisplay.cpp:567-570).
- **The one-pass mask does need two host frames, for about 40-100 ms.** The mask is (frame on the glass AND new page)
  OR gray (FID:993; the gray render then sets bits, ERA:3841-3852). The controller holds the old frame but cannot AND it
  and cannot be read back. Once uploaded, the mask buffer is free at once (FID:1021-1026); only the page stays pinned
  (X3) until the finish.
- **X3:** between refreshes the controller needs no host buffer. During a waveform it needs one. So one buffer can be
  lent at any time without cost to B/W paging, which `borrowSecondaryBufferDuringRefresh()` already uses for the
  popup (FID:640-654).
- **X4:** during a waveform both buffers and the bus are free; fix 5 uses the bus. Between refreshes S is needed only
  as the carrier of the baseline, and a 14 ms seed moves that into RED. But every further deferred FAST while S is away
  leaves RED one frame behind (noted as an open point in fix 5's draft; by code, not seen on a panel).

**Lending, by the controller's needs alone.**

| | The pushed frame | The other buffer | What must come back, and when |
|---|---|---|---|
| X3 | free after the finish (waveform + 25 ms sync; + 200 ms after a HALF) | free at any time, before the trigger too | nothing for a B/W FAST. For a gray: the B/W page at the cleanup. For one-pass: both frames, the one on the glass intact, when the mask is composed |
| X4 | free once the start returns | free once the start returns | at the next FAST's start the frame on the glass as `prev`, or RED seeded from it before the lend. For a gray: the B/W page at the cleanup. For one-pass: as the X3 |

**Held longer than the controller needs today:** S for a whole chapter build, though the page's gray needs only plane
scratch and the B/W page (V1). The frame on screen, lent to look-ahead work, though the other buffer holds only a
pre-render that 60 ms rebuild (V2). The bus through the X4's waveform (V7). The popup's refresh before the first-open
indexing (V6). **Cannot be shortened:** the X3's pushed frame until its DTM1 sync; two frames while a mask is composed.

### X4 units with an UltraChip panel (C3, no unit at hand)

UC8179 and UC8279-X4 follow the X3's model: frame before the waveform, `prev` ignored, the same frame -> DTM1 in the
finish (Uc8179Driver.cpp:394, 434, 521; Uc8279X4Driver.cpp:406, 439, 564). No one-pass on a C3 (neither composes; the
UC8179's planes are PSRAM only, Uc8179Driver.cpp:247-251). The UC8279-X4 driver takes a 48,000 B `_grayBase` from the
internal heap when there is no PSRAM (Uc8279X4Driver.cpp:305-308): on such an X4 that is a framebuffer's worth of heap
for good. Not measured.

### The S3 boards

| Board, driver | B/W refresh reads | Gray | Baseline | Pinned |
|---|---|---|---|---|
| X4 Pro, `Uc8179Driver.cpp` | Frame before the waveform (:434) and -> DTM1 in the finish (:521); `prev` ignored (:394). Deferred for every mode (:449-453) | Two-pass: the driver keeps the B/W base in `_grayBase` (48 KB PSRAM) and restores both planes itself (:814-819); cleanup reads the host only when not synced (:952-956). One-pass: `RestTextAa`, 288,000 B PSRAM (RestTextAa.h:46, 127); the frame is copied before the waveform (:865) and not read again (:911-929) | DTM1 | W from trigger to finish (B/W); nothing after the start (one-pass). S: never. Upload 52-62 ms a plane (RESULTS-coldopen.md, diag3/diag4) |
| Murphy M3, `Uc8253MurphyDriver.cpp` | Blocking: no deferred start (PanelDriver.h:97-100). The controller is reset before every refresh (:178-180), so its RAM is no baseline: a FAST sends the **host's `prev`** -> DTM1 (:187, :205) and the frame -> DTM2 (:211), then copies the frame to `_shown` after the waveform (:216-222) | Planes copied to `_lsb`/`_msb` (:271-280); one-pass composes from `_shown`, `_gray` and the frame (:240-246). Six planes of 12,480 B, PSRAM first (:262-269) | The host's S for B/W FAST; `_shown` for one-pass | Both buffers until the call returns. The only board whose FAST needs S at every refresh. Uploads not measured |
| BOOX Picco, `Ssd2677Driver.cpp` | Blocking. `prev` ignored (:196). Old and new go out together from the driver's `_old` (48 KB PSRAM, :191-192) and the frame; `_old` <- frame after the waveform (:256) | Overlay masks to controller RAM (:404); single push from `_textBase`, `_textGray`, `_textDark`, `_lsb` (5 x 48 KB PSRAM, :436-465); the frame is copied first (:450) | `_old`, in the driver | W until the call returns. S: never. Uploads not measured |
| LilyGo T5S3, `LgfxEpdDriver.cpp` | The frame is copied into `g_canvas` (518,400 B PSRAM, :230-235) before the push is queued (:772); LovyanGFX's own task clocks the waveform out of its own buffers; `prev` ignored (:770) | Planes copied to `g_lsb`/`g_msb` (:877-909); `cleanup(bw)` copies `bw` into the canvas (:944) | LovyanGFX's panel buffer | Nothing after the start returns. Not measured |

**What this allows.** On the X4 Pro, the Picco and the T5S3 the controller never reads S; the reader keeps it for
overlays, the gray restore and the pre-render's twin. Their builds run in PSRAM, so nothing a page needs is ever lent:
#808's premise. Two leftovers: `fastRefreshNeedsHostFrame()` is true for every board that is not an X3 (FreeInkDisplay.h:215)
although only the SSD1677 and the Murphy read `prev`; and `HalDisplay::hasSecondaryBuffer()` reports false while the
PSRAM arena is lent (lib/hal/HalDisplay.cpp:410-414), so a page turn on an S3 board still discards Background-B's live
build (ERA:4535), though that build holds nothing the page needs. An X4 Pro with an SSD1677 was not traced (its
PSRAM text planes may engage under `setReaderRefresh`, Ssd1677Driver.h:136-137; no unit).

---

## 5. Design options for the X3 and X4

The conflict: during a build there is one spare framebuffer-sized block. The build wants it as its arena; today's gray
wants it as the restore source; a one-pass mask wants it for 40-100 ms. Section 4 removes the second claim.

Time to the complete page on a cold open, N pages in the chapter, from the logs: **today X3 = 4.66 + 0.034 N s** (66
pages: 6.90; measured 6.88), **X4 = 4.56 + 0.033 N s** (74 pages: 6.97; measured 6.95). At 300 pages: about 15 s.

| | (a) Build yields S, resumes | (b) First window, rest later | **(c) Gray without S** | (d) Lend the other buffer |
|---|---|---|---|---|
| Idea | Save the arena, give S back for the pass, restore | Lay out about 9 pages, end the build, show the page the normal way, build the rest as look-ahead | The build keeps S. The pass renders the planes over W, renders the B/W page again and hands W to the cleanup | Idle borrowers (B, lane, bookmarks) take W and leave the frame on screen |
| What it needs | The live arena is the SAX state (9,712 B), the CSS rules, the feed chunk and the open page block, 12-24 KB with absolute pointers (X3a 3.386; lanes at 6.656). It must go to the card and back to the same address, and the facade must lend the same physical block again though every display swaps the two | A partial section file with its own finalize (LUTs and anchors are written only at the end, Section.cpp:1901-2109): format version bump. A resume needs the parser alive across the hand-back, which is (a) again; CrossPoint keeps it on a heap of 103-112 KB (RESULTS.md, RESULTS-x4.md). A restart redoes the window: 9 x 33 ms plus setup 57-90 ms | Reader and one renderer forwarder: extend #808's `BuildPageGray` to the C3, with a pass that ends in `cleanupGrayscaleBuffers(W)`. Page and font slots from the build arena, as `displayBuildPage()` takes them (ERA:6008-6012, 7050). No SDK change | SDK: the lend of `borrowSecondaryBufferDuringRefresh()` without a pending refresh, and a return that keeps `_activeMatchesGlass` when nothing drew meanwhile. Reader: drop the pre-render at the borrow, re-arm it after |
| X3 memory | none extra if to the card; no heap for it (58-68 KB free at rest, X3a 7.596; build low 36-38 KB, X3f 49.424, RESULTS-coldopen.md) | none extra | none extra: heap at the pass is the build's (61-65 KB free, X3a 3.384, 6.593) | none |
| Complete page | constant; card time not measured | constant, about today's page time + 0.3 s window + a render: not measured | **constant: page + pass.** X3 about 4.5 s, X4 about 4.5 s: estimated, not measured, from today's page time (3.85 s, 4.19 s), the X3's finish (0.24 s), today's pass (0.40 s, 0.25 s) and one more B/W render (49 ms, 43 ms) | removes V2: every turn outside a build stays one-pass |
| Risks | every arena pointer; block identity; a card error loses the build | watermark UX: at 2.5 s a page B never finishes the rest (X4c: 20 spines abandoned), so the reader meets a popup every window; CrossPoint shows it (`72/72` then `73/74`, build "200198 ms", RESULTS-x4.md). Percent, last-page and backward entries need the whole chapter. Estimates reach progress, sync, bookmarks | the re-rendered page must equal the pushed one (status bar from the values it was drawn with), or the baseline differs from the glass in those pixels. An aborted pass must re-render before its cleanup: about 50 ms more on a turn. X4: the cleanup's RED write is also the reseed the deferred FAST lacks. An X4 build that keeps S (small chapters, `IncrementalResident`) runs on the heap its guard watches (ERA:2818-2828): there the pass's page and font slots must not come from that heap, or the gray stays at the build's end | a draw outside `render()` while W is lent; ghosting if the "nothing drew" claim is wrong: needs the panel |
| Effort | high | high | low to medium | low to medium |
| S3 | nothing | could adopt, but #808 gives the same without it | nothing | nothing (PSRAM arena) |

A variant of (c) renders the planes in 80-row strips into an 8 KB scratch, as CrossPoint does, so W keeps the page and
an abort costs nothing. The SDK has the strip uploads for all three C3 controllers (Uc8253X3Driver.cpp:524,
Uc8279Driver.cpp:284, Ssd1677Driver.cpp:1115); the renderer's strip target was removed (stubs at
lib/GfxRenderer/GfxRenderer.h:1108-1109). CrossPoint's strips cost more than our full-frame planes on the same panels:
X3 245 ms for both planes against 119 ms (CPx3 66.144); X4 111 ms for the MSB plane alone against about 100 ms for both
of ours (CPx4 1.431; its LSB figure includes the waveform wait). Keep it as the fallback if the re-render's abort cost
shows.

**One-pass during a build.** With (c) a page drawn while a build holds S is B/W plus overlay: two refreshes. On a cold
open or a jump that is what the page gets anyway, since its HALF is never one-pass on the C3 (ERA:6614-6616). It is new
only for FAST pages turned during a build and for build-through pages, which have no gray at all today. A true
one-pass there needs the mask composed in strips against the old frame still in W, plus an SDK start that takes a
staged mask: possible by section 4, not measured, a later step.

### Recommendation: (c) + (d), with R0. Not (a). Not (b) now.

(c) makes the complete page independent of the chapter's length with no new memory and no format change. (d) and R0
remove the larger daily cost, V2, which the One-Pass default would otherwise carry on every fast turn. (b) changes what
a section is for the whole firmware to reach the same first page.

Order of work, each step one reviewable change:

1. **Measure first.** `IDLE:SCREEN` ends with the gray, the build reports as background (V8); one `page complete` stamp.
   Bench cases: a turn 0.6 s after a cold open's first page, and a 2.5 s pace through a chapter's second half.
2. **R0.** No B slice, lane start, glyph-store or bookmark step while a press is in flight; C slices skip while a turn
   waits for RL. One gate header, host-tested.
3. **B stops after an abandon** until the reader pauses or changes chapter (V2 iii).
4. **(d)** SDK lend that keeps the frame on screen, on the fork's `witchhunt`; then B, the lane and the bookmark parse
   use it. Check on the X3 and X4: one-pass on every turn at 2.5 s and at 15 s a page.
5. **Land #808**, then **(c)**: the build page's gray during the build on the C3; build-through pages the same (V3).
6. **X4:** reseed RED after a deferred single-buffer FAST (section 4), needed with AA off.
7. **Count:** byte-precise estimate; no refresh for the count alone (V9).
8. Later, by decision: placeholder-first pictures (V4), the C3's async first-open popup (V6), strip-composed one-pass,
   B building through every turn on the S3.

Fix 5 and fix 6 stay as they are: after step 5 they shorten the build, no longer the wait for the gray.

---

## 6. A second core on the S3 boards

**Facts.** The loop task runs on core 1 (`CONFIG_ARDUINO_RUNNING_CORE 1`, the framework's esp32s3 `sdkconfig.h:483`). The
render task is pinned to core 1 (src/activities/ActivityManager.cpp:93-103; the comment above it, ported from upstream,
still says the loop task is on CPU 0). The KOReader sync worker is pinned to core 1
(src/activities/reader/KOReaderSyncWorker.cpp:186-192). Core 0 has the system and Wi-Fi tasks. The PSRAM boards run at a
fixed clock (lib/hal/HalPowerManager.cpp:51). The C3 has one core.

**Contention today: almost none, by construction.** Render compute and build slices both hold RL, so two of them never
compute at once on any number of cores. What a second core could overlap is the render's compute while a build or a
decode waits for RL:

| Situation | Render compute that keeps other work out | Waiting a second core cannot shorten | Source |
|---|---|---|---|
| Cold open, Picco 2.53 | page draw 97 ms, gray planes 214 ms, pre-render 42 ms | HALF 421 ms and power-off 152 ms under a blocking driver; gray refresh 269 ms with its power cycle; the card | Pw [18:26:58-27:00]: 1.159 -> 1.256 -> 1.910; 3.684; 3.731 |
| Cold open, X4 Pro | page draw 41-43 ms, gray planes 167 ms | HALF 1,492 ms, which the build runs through already; two plane uploads of 58-62 ms; gray refresh 401 ms | RESULTS-coldopen.md (diag0, diag4, AA-fix table) |
| Turn into an unbuilt chapter, Picco with #808 | the build ends 0.53 s later for the page's gray (1.73 -> 2.26 s); about 0.21 s of that is the planes | the rest is the gray refresh and the power cycle with the lock held: fix 6 | #808's table (docs/background-rendering.md on the branch) |
| Picture on the page during a build, X4 Pro | decode and build alternate on the loop task: picture at 5.2 s, chapter at 10 s | the split into decode CPU, card and panel: **not measured** | docs/background-rendering.md:217-222 |
| TrueType glyph-store install, live font change | **not measured** (4-15 s a step on the X4 Pro; CPU share unknown) | flash erase and write | lib/hal/HalCapabilities.h:263-267 |

One real contention was measured, and fixed without a core: the loop task spinning beside a render on core 1 made the X4
Pro's page draw 205-280 ms against 41-43 ms (RESULTS-coldopen.md, diag3 against diag4, 2026-11-16; ERA:7746-7754).

So a perfectly parallel build wins at most the render's compute, 0.15-0.35 s per cold open, and wins it for the build's
end (class 3). The page (class 1) wins nothing: it waits for the build's first page, then for the panel.

| Candidate | Gain and its basis | What must become thread-safe | Memory, power | Failure modes |
|---|---|---|---|---|
| (a) The whole chapter build on core 0 | at most 0.15-0.35 s on the build's end (table above); nothing on the page | `Section`, which RL owns and the render task replaces (docs/background-rendering.md:28-34); `Epub`'s metadata cache (one file position); `CssParser` and its caches; `FontCacheManager`, `FontDecompressor` and the glyph store, which the layout measures and the page draw reads (`clearFontAccumulation()` per slice, ERA:2797); `ZipFile`; every user of the build's `BuildArena` (the lane's free tail, the page draw's slots and block); `glyph_miss`; `image_scratch`; `SpinePageIndex` | a task stack of 10-16 KB of internal RAM; the second core busy for the build | **two concurrency models**: the C3 keeps slices under RL, the S3 would need fine-grained locks. The host tests run one thread on a frozen clock and cannot see a race. Every reader invariant is written for one model |
| (b) Jobs with value in, value out: a picture decode for the page on screen, the chapter's inflate, glyph rasterisation | decode: the build is no longer stopped for the decode's length; share of the 10 s not measured. Inflate: 69 ms on the Picco (Pw 1.040 -> 1.109), nothing to win. Glyphs: not measured | the decoders draw through `GfxRenderer` in cache-only mode (ERA:2482) and take memory from the process-wide `image_scratch` (lib/Epub/Epub/blocks/ImageBlock.h:50-55); `ImageBlock`'s placeholder counters and `CooperativeAbort` are global; the ZIP is read through the shared `Epub`. A job needs its own ZIP handle, its own PSRAM block and a row sink that is not the renderer. `Storage` locks already (lib/hal/HalStorage.cpp:121-123) | a 10 KB stack (the sync worker's size); decode memory in PSRAM; core 0 busy 1-4 s a picture | card contention with the build (one SDMMC host; SPI on the T5S3); a cache file written while a page draw probes it; cancel on a turn |
| (c) Re-pin an existing task: the sync worker to core 0 | its TLS work stops competing with render and loop on core 1; not measured | nothing (jobs by value, results polled) | none | a long compute at priority 1 can starve core 0's idle task, the reason the render task was pinned to core 1 (ActivityManager.cpp:88-92) |

**Recommendation: no second-core work in this round, one measurement.** The scheme is about who waits for whom, and on
the S3 boards class 1 waits for the panel and for the build's first page, never for a CPU. What a second core can give
is tenths of a second of class 3, against a second concurrency model the C3 cannot share. The one candidate that fits
the scheme is (b) for class 2: the picture of the page on screen as a job in the sync worker's shape, beside class 1 and
class 3. Before it: measure on the X4 Pro how the 5.2 s and 10 s of a picture page during a build split into decode
CPU, card and panel (`Image lane: ... warmed in X ms` against the build's `parse done`), and what a sync costs a turn.
If the decode is a small part, drop the idea.

---

## 7. What to measure, and a host test per rule

Protocol: the 2.53 bench (`bench.py`, *The Odyssey*, BOOK IV, n >= 3, AA off / two-pass / one-pass), X3 and X4 first.

| Step | Proves it | Log lines |
|---|---|---|
| 1 | idle reply at the gray's end, before `Background-C ... complete` | `IDLE`, `Deferred AA`, the new stamp |
| 2 | press to `PageTurn stats` with B live equals without (X4 today 363 against 111 ms) | `CMD:BTN`, `PageTurn stats`, `[SCT] Page N processed` between them: none |
| 3 | one `abandoned` line per chapter, not twenty | `Background build spine=N abandoned` |
| 4 | One-Pass round at 2.5 s: `One-pass AA (composed)` on every FAST turn (today 25, against 43 replays); turn 0.70 s; no stale frame by screenshot and by eye | `One-pass AA`, `AA replay`, `FBUF` |
| 5 | cold open: gray on the panel within about 0.5 s of the page, for a 66-page and a 300-page chapter; build done later by the pass; heap low not below today's; a turn during the pass | `Wait complete`, `Deferred AA`, `[MEM] Min Free`, arena `failedAlloc=0` |
| 6 | three FAST turns during a build with AA off: no ghost (eye, camera) | `FBUF syncRedRam`, refresh modes |
| 7 | first-page count within 20 % of the final one; no refresh between build end and the next turn | `STATE chapter_page`, `DISP` |

| Rule | Host test |
|---|---|
| R0 | `mayRunSlice(pressDown, gestureOpen, turnWaiting)` truth table; a sliced fake build under `host_clock::Ticking` makes no step while a press is open (assert steps > 1 otherwise) |
| R1, R4 | lift the order of `serviceBackgroundWork()` into a pure `next(State)` and test every combination: gray owed, build live, picture on screen, look-ahead due |
| R2 | `BuildPageGray::whileBuilding()` true on a board whose build holds S; `asksPassNow()` unchanged |
| R3 | SDK host runner: lend, return untouched, then `secondaryHoldsFrameOnScreen()` and `beginOnePassMask()` succeed; lend, draw, return: they do not |
| (c) | the frame handed to the cleanup equals the pushed frame, byte for byte, with a fixed status bar; after an aborted pass the driver's baseline is the page (the check of `testGrayCleanupBaseline`) |
| R6 | `estimatedTotalPages()` from bytes: never below the built count, exact at the end; a finished build asks for no update when only the count changed |
| R8 | `hasPendingScreenWork()` false with a build live and no gray owed |

---

## 8. Decisions for the owner

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Pages drawn while a build holds the buffer, One-Pass on (C3) | A: B/W, then the gray as a second refresh about 0.5 s later. B: stay B/W until the build ends (today). C: wait for the strip-composed one-pass | A now, C as a later step |
| D2 | A turn onto a page whose picture is not cached | A: text and placeholder first, picture after (two refreshes). B: decode first (today). C: A only above a size threshold | C, threshold from a measurement |
| D3 | The final page count | A: stays "~" until the next refresh that happens anyway. B: redraw when known (today after a look-ahead build) | A |
| D4 | Look-ahead at a fast pace | A: after one abandoned spine wait for a pause. B: keep trying the next spines (today) | A |
| D5 | What an idle borrower takes | A: the write buffer; the pre-render is redone (60 ms), one-pass survives. B: the frame on screen (today) | A |
| D6 | First window and partial sections (CrossPoint's model) | A: not now. B: design it for chapters above some length | A; look again if 300-page chapters still feel slow after step 5 |
| D7 | The comparison's rows | A: "first page complete" and "chapter laid out" as two rows. B: one "settled" row | A |
| D8 | The 400 ms settle before an inline gray, which One-Pass keeps on HALF pages | A: keep. B: drop it where no press is pending at the HALF's end. C: measure the abort rate first | C |
| D9 | Order with the open work | #808 before step 5; fixes 5 and 6 independent | as stated |
| D10 | The S3's second core | A: nothing now, one measurement of a picture page during a build. B: a decode worker for the page on screen. C: the build on core 0 | A; B only if the measurement shows seconds; not C |
