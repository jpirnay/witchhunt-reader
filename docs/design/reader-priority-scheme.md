# The reader's priority scheme

The EPUB reader's render passes and background work follow one priority order. This record holds the order, the rules
the code is checked against (section 3), what each panel controller needs from the host's framebuffers and for how
long (section 4), and the changes that brought the code in line with it (#808-#823). The passes as they run are in
[../background-rendering.md](../background-rendering.md); the work still open is in
[../future_work/reader-priority-scheme.md](../future_work/reader-priority-scheme.md).

Code cites section 3 by rule (`InputFirst.h` for R0, `test/page_gray_during_build/BuildPageGrayTest.cpp` for R2) and
section 4 (`BuildPageGray.h`, `IndexingPopupGate.h`). The findings keep the numbers V1-V10 and the decisions D1-D10
that the PRs cite; there is no V6.

Short names: W = the write buffer (`frameBuffer`), S = the secondary buffer (`frameBufferActive`), RL = `RenderLock`.
The SDK names are in `freeink-sdk/libs/display/FreeInkDisplay/` (`FreeInkDisplay`, the drivers in `src/driver/`).
Measurements come from the 2.53 bench: *The Odyssey*, BOOK IV, the bench profile, n >= 3, AA off / two passes /
One-Pass, as quoted in each PR.

## 1. The scheme

Three classes of work, with input above all of them (the owner's order):

1. **The complete page.**
2. **What the page on screen still needs:** its pictures.
3. **Everything else:** the next page's pre-render, the look-ahead build, the glyph store, bookmarks, the final page
   count.

The page count may stay an estimate ("~") and never holds back the gray. One-Pass Anti-Aliasing is designed for first.

| Page kind | Complete when |
|---|---|
| Text, AA on | the B/W frame and its gray are on the panel, status bar included, count exact or "~" |
| Text, AA off | the B/W frame is on the panel |
| With pictures | as text, each picture drawn or a placeholder; pictures and their grays are class 2 |
| Under the live bars or a search strip | the B/W frame with the UI; its gray is owed from the moment the UI closes |

A page is **owed** from its B/W trigger until its gray refresh ends or a turn replaces it.

The order in `serviceBackgroundWork()` already followed the scheme in steady reading: the deferred gray before the
pre-render, the picture of the page on screen before more build, the image lane before Background-B. Class 1 was
delayed by who held a buffer, not by the order.

## 2. Findings that shipped

| Finding | What it was | Fixed by | Measured |
|---|---|---|---|
| V1 (S3s) | The page a chapter build draws got its gray only once the whole chapter was laid out | #808: the gray during the build, `BuildPageGray` `Pass::Restore` | Picco cold open, two passes: gray 3.27 -> 1.98 s |
| V1 (C3s) | The same, forced on the X3 and the X4's borrowed builds because the build holds S as its arena | #823: `Pass::Redraw` (section 5) | X3 cold open, two passes: gray 6.58 -> 4.08 s; before, +33-34 ms per page of the chapter |
| V2 (i) | A Background-B slice ran on every loop pass with no input test, so a press was read three to four slices late | #818, R0 (`InputFirst.h`) | X3, a turn 0.6 s after a cold open's first page: turn seen 374-446 -> 265-274 ms |
| V2 (iii) | An abandoned spine handed the look-ahead to the next one, so every turn at a fast pace paid a borrow | #817, D4 A (`LookAheadPause.h`) | X3, a page every 2.5 s, One-Pass: turns composed one-pass 16 -> 32 of 40, press to gray 1,253 -> 690 ms (median) |
| V5 | The inline two-pass gray waited a fixed 400 ms on every freshly drawn page | #816, D8 (`AaSettle.h`): it waits only while the reader is flipping | X4, pages back every 2.5-20 s: gray 1,316-1,326 -> 922-928 ms after the press |
| V7 (X4) | The build idled through the 1.7 s HALF of its first page: the wait held the SPI bus the card shares | #813 (fix 5), `BuildDuringRefresh.h` | X4 cold open: chapter laid out 6.57 -> 4.97 s |
| V7 (Picco, Murphy) | Their drivers blocked in `triggerDisplay` with RL held | #811 (fix 6): deferred `displayStart` / `displayFinish` in `Ssd2677Driver` and `Uc8253MurphyDriver` | Cold open, two passes, chapter laid out: Picco 3.44 -> 2.72 s, Murphy 6.20 -> 4.89 s |
| V8 | `CMD:IDLE:SCREEN` counted the chapter build as screen work | #815, R8 (`PageComplete.h`; [../serial-test-api.md](../serial-test-api.md)) | X3, AA off: the reply 6.84 -> 4.37 s after the open (page on the panel at 3.73 s) |

The Indexing popup's work moved under its own refresh in the same round (#819, `IndexingPopupGate.h`): the work starts
0.66 -> 0.26 s after a cold open on the X3, 0.67 -> 0.13 s on the X4. It rests on the lending rules of section 4.

The findings still open (the X4's resident build, V2 (ii), V3, V4, V7 on the X3 and the T5S3, V9, V10) are in
[../future_work/reader-priority-scheme.md](../future_work/reader-priority-scheme.md).

## 3. The rules

| Rule | Statement | In the code |
|---|---|---|
| **R0** | Input first. No class 2 or 3 unit starts while a press is down, a gesture is unresolved or a turn waits for RL. A running unit ends within one slice (40 ms plus one parse chunk) or aborts (decodes) | `InputFirst::mayRunSlice()`, asked by `serviceBackgroundWork()` (#818). The chapter on screen goes on through a press until the page asked for is drawn |
| **R1** | While a page is owed, neither task runs class 2 or 3 work, except work that shares no resource with the gray (other bus, other memory) and gives RL back within one slice | `serviceBackgroundWork()` returns while `pendingGrayscale_.active`; the progress save and an auto-sync push still run before the gray (V10) |
| **R2** | The gray starts when the panel is free. It does not wait for a build's end, a buffer's return, a count, or a settle timer with no press pending | `BuildPageGray.h` (#808, #823), `AaSettle.h` (#816). Not yet for the X4's resident build and build-through pages |
| **R3** | A class 3 holder of S, W, RL or the bus gives it up within one slice when class 1 or 2 asks, and takes the buffer whose loss costs class 1 least: never the only copy of the frame on screen where one-pass needs it | not yet: Background-B lends the frame on screen (V2 (ii)) |
| **R4** | Class 2 before class 3: the page on screen's pictures before the pre-render, the glyph store, bookmarks, look-ahead pictures and B | the lane before B (`LaneBeforeBuild.h`); the glyph-store and bookmark steps still run before the lane (V10) |
| **R5** | Class 2 never delays class 1: a page whose picture needs a decode is shown with a placeholder first | not yet: decision D2 |
| **R6** | The page count is class 3. The status bar shows the best estimate with "~"; the final count is drawn by the next refresh that happens anyway and never asks for one | not yet: decision D3 |
| **R7** | A complete page is not drawn again for class 3 reasons, except the clock minute | two exceptions in the code: a finished look-ahead build redraws the page on screen to firm up the chapter count (V9 (b)), and a bookmark settling onto the page on screen redraws it for the status bar's star (accepted, [../background-rendering.md](../background-rendering.md), *Starred pages*) |
| **R8** | The test API reports "page complete" apart from "chapter laid out" | `PageComplete::screenWorkPending()` (#815): `IDLE:SCREEN` ends with the gray, `IDLE` with the build |

## 4. Framebuffer lifetime per controller

What each controller reads from the host, and until when. An upload is a blocking SPI write: when the call returns the
buffer has been read (`EpdBus::sendPlaneFlipped`, no DMA). The facade swaps W and S after every swapping display and
passes the pushed frame to the finish (`FreeInkDisplay::syncPendingAsync()`). The bus has no RAM read-back: only BUSY
is read.

### The C3 controllers

| | X3: UC8253 (`Uc8253X3Driver`); the UC8279d X3 (`Uc8279Driver`) where it differs | X4: SSD1677 (`Ssd1677Driver`) |
|---|---|---|
| B/W FAST | Frame -> DTM2 before the waveform (`displayStart`); `prev` ignored. After the waveform nothing: the controller keeps the frame it drew as the old plane, DTM1 (#860). The UC8279d sends the frame to DTM1 again in `displayFinish` after every refresh | Frame -> BW RAM, `prev` -> RED, both before the waveform (`displayImpl`). The deferred finish only waits |
| HALF, FULL | As FAST; a HALF waits 200 ms after its waveform. A full sync starts from a white DTM1, and its finish sends the frame to DTM1 and runs its conditioning and settle passes from it | Frame -> BW and RED before the waveform; absolute |
| Pinned how long | The UC8253 reads the frame only before the trigger, except in a full sync's finish. The UC8279d reads it from the trigger to the end of the finish. The facade treats the frame as held to the finish on every X3 (`asyncRefreshHoldsFrameBuffer()`). Waveform 382 ms FAST, 459 ms HALF | Nothing after the start returns, 27-41 ms after the trigger |
| Baseline for the next FAST | DTM1, in the controller. Lost to a gray pass until its cleanup; `cleanupGrayscaleBuffers(nullptr)` leaves the driver unsynced and the next push is a full one | RED, in the controller, but a FAST writes it from `prev` at its **start**, so between two FASTs RED holds the frame before the one on the glass. The truth is in S until the next start, unless RED is seeded (`seedPreviousFrame`). A blocking single-buffer FAST reseeds both planes after its waveform; the deferred one does not |
| Two-pass gray | LSB -> DTM1, MSB -> DTM2 (`copyGrayscaleLsb/Msb`), or in strips (`writeGrayscalePlaneStrip`). Gray waveform 228 ms, no host buffer during it. Cleanup: **the B/W page -> DTM2 and DTM1** (`cleanupGrayscaleBuffers`) | LSB -> BW RAM, MSB -> RED, or in strips. Gray waveform 146 ms. Cleanup: **the B/W page -> RED**; with `nullptr` the next FAST becomes a HALF |
| Composed one-pass | Mask -> DTM1, page -> DTM2 before the waveform (`displayOnePassStart`); nothing after it. The UC8279d has no one-pass | Page -> BW, mask -> RED before (`displayOnePassStart`); after it the page -> both planes (`displayFinish`) |
| Popup | A B/W refresh of W after `syncWriteBufferFromDisplayed()`, which reads S. Shipped async it pins the painted frame only, as any B/W refresh | The same |
| Absolute gray (sleep cover, BMP viewer; no reader page) | Base: frame -> DTM2 and DTM1, or a full display when the RAM is stale; then the planes and the cleanup as above | No base push; the two planes go to BW and RED and one waveform draws all ([../secondary-buffer-management.md](../secondary-buffer-management.md)) |
| Upload, measured | 25 ms a 52 KB plane | 12-14 ms a 48 KB plane |

**What this allows.**

- **The gray overlay does not need S on either controller.** It needs a place to render each plane for the length of
  its upload, and afterwards the B/W page, from any buffer, for one upload (X4) or two (X3). The reader took that page
  from S (`cleanupGrayscaleWithPreviousBuffer()`) because it rendered the planes over W; `cleanupGrayscaleBuffers(bw)`
  takes any buffer. This is what `Pass::Redraw` uses (section 5).
- **The one-pass mask does need two host frames, for about 40-100 ms.** The mask is (frame on the glass AND new page)
  OR gray (`FreeInkDisplay::beginOnePassMask()`). The controller holds the old frame but cannot AND it and cannot be
  read back. Once uploaded the mask buffer is free at once; only the page stays pinned, until the finish.
- **X3:** between refreshes the controller needs no host buffer. During a waveform the facade keeps the painted one.
  So the other buffer can be lent at any time without cost to B/W paging: `borrowSecondaryBufferDuringRefresh()` lends
  it during a refresh, and the Indexing popup's work runs in it (#819).
- **X4:** during a waveform both buffers and the bus are free; #813 uses the bus, #819 lends the frame not being
  painted as on the X3. Between refreshes S is needed only as the carrier of the baseline, and a 14 ms seed moves that
  into RED. But every further deferred FAST while S is away leaves RED one frame behind (by the code, not seen on a
  panel).

**Lending, by the controllers' needs alone.**

| | The pushed frame | The other buffer | What must come back, and when |
|---|---|---|---|
| X3 | held by the facade to the finish: the waveform, 200 ms more after a HALF, and on the UC8279d its DTM1 upload | free at any time, before the trigger too | nothing for a B/W FAST. For a gray: the B/W page at the cleanup. For one-pass: both frames, the one on the glass intact, when the mask is composed |
| X4 | free once the start returns | free once the start returns | at the next FAST's start the frame on the glass as `prev`, or RED seeded from it before the lend. For a gray: the B/W page at the cleanup. For one-pass: as the X3 |

Held longer than the controllers need, as the study found it: S for a whole chapter build, though the page's gray needs
only plane scratch and the B/W page (V1, fixed on the borrowed builds by #823); the frame on screen, lent to look-ahead
work, though the other buffer holds only a pre-render that 60 ms rebuild (V2 (ii), open); the bus through the X4's
waveform (V7, fixed by #813). Cannot be shortened: two frames while a mask is composed.

### X4 units with an UltraChip panel (no unit at hand)

The UC8179 and the UC8279-X4 follow the X3's model: frame before the waveform, `prev` ignored, the same frame -> DTM1
in the finish (`Uc8179Driver`, `Uc8279X4Driver`). Neither composes a one-pass page on a C3 (the UC8179's planes are
PSRAM only). The UC8279-X4 driver takes a 48,000 B `_grayBase` from the internal heap when there is no PSRAM
(`Uc8279X4Driver::begin()`: PSRAM first, then `malloc`): on such an X4 that would be a framebuffer's worth of heap
for good. Not measured.

### The S3 boards

| Board, driver | B/W refresh reads | Gray | Baseline | Pinned |
|---|---|---|---|---|
| X4 Pro, `Uc8179Driver` | Frame before the waveform and -> DTM1 in the finish; `prev` ignored. Deferred for every mode | Two-pass: the driver keeps the B/W base in `_grayBase` (48 KB PSRAM) and restores both planes itself. One-pass: `RestTextAa`, 288,000 B PSRAM; the frame is copied before the waveform and not read again | DTM1 | W from trigger to finish (B/W); nothing after the start (one-pass). S: never. Upload 52-62 ms a plane |
| Murphy M3, `Uc8253MurphyDriver` | Deferred since #811. The controller is reset and initialised in every start, so its RAM is no baseline: a FAST sends the **host's `prev`** -> DTM1 and the frame -> DTM2, and keeps the frame as `_shown`; the finish waits, powers off and sends the nudge, with no host buffer | Planes copied to `_lsb`/`_msb`; one-pass composes from `_shown`, `_gray` and the frame. Six planes of 12,480 B, PSRAM first | The host's S for a B/W FAST; `_shown` for one-pass | Both buffers until the start returns. The only board whose FAST needs S at every refresh |
| BOOX Picco, `Ssd2677Driver` | Deferred since #811. `prev` ignored. Old and new go out together from the driver's `_old` (48 KB PSRAM) and the frame; `_old` <- frame in the start | Overlay masks to controller RAM; single push from `_textBase`, `_textGray`, `_textDark`, `_lsb` (5 x 48 KB PSRAM); the frame is copied first | `_old`, in the driver | W until the start returns. S: never |
| LilyGo T5S3, `LgfxEpdDriver` | The frame is copied into `g_canvas` (518,400 B PSRAM) before the push is queued; LovyanGFX's own task clocks the waveform out of its own buffers; `prev` ignored | Planes copied to `g_lsb`/`g_msb`; `cleanup(bw)` copies `bw` into the canvas | LovyanGFX's panel buffer | Nothing after the start returns |

On the X4 Pro, the Picco and the T5S3 the controller never reads S; the reader keeps it for overlays, the gray restore
and the pre-render's twin. Their builds run in PSRAM, so nothing a page needs is ever lent: the premise of
`Pass::Restore` (#808). An X4 Pro with an SSD1677 was not traced (no unit).

## 5. The gray during a chapter build on the X3 and X4

During a build there is one spare framebuffer-sized block. The build wants it as its arena; the gray restore wanted it
as its source; a one-pass mask wants it for 40-100 ms. Section 4 removes the second claim. Four options were weighed:

| | Idea | Why not, or why |
|---|---|---|
| (a) Build yields S, resumes | Save the arena, give S back for the pass, restore it | The live arena is the SAX state, the CSS rules, the feed chunk and the open page block, 12-24 KB with absolute pointers. It would have to go to the card and come back to the same address, and the facade lends either physical block. High effort, every arena pointer at risk |
| (b) First window, rest later | Lay out about 9 pages, end the build, build the rest as look-ahead (CrossPoint's model) | A partial section file needs its own finalize and a format version; a resume needs the parser alive, which is (a) again. At a fast pace B never finishes the rest, so the reader meets a popup every window. Percent, last-page and backward entries need the whole chapter. Decision D6: not now |
| **(c) Gray without S** | The build keeps S. The pass renders the planes over W, draws the B/W page again and hands W to the cleanup | No new memory, no format change, no SDK change. Shipped as `Pass::Redraw` (#823) |
| (d) Lend the other buffer | Idle borrowers (B, the lane, bookmarks) take W and leave the frame on screen in S | Removes V2 (ii): every turn outside a build stays one-pass. Open: it needs an SDK lend with no refresh pending and a return that keeps `_activeMatchesGlass` |

**`Pass::Redraw` (#823).** Between two build slices, once the page's refresh is over, the pass puts the page, its glyph
slots and the status bar's pixels into a block of the build's arena, renders the planes over W, draws the page there
again and hands W to the cleanup. Its risks and how they are handled:

- **The redrawn page must equal the pushed one,** or the controller's baseline differs from the glass. The status bar is
  not drawn again (battery, clock and "~" count may have moved): its bands are kept as pixels
  (`BuildPageGray::chromeBands()`, geometry in `FrameRegion.h`). A digest of the frame before and after checks every
  pass (`frame=same`); a mismatch drops the driver's baseline, so the next push is clean.
- **An aborted pass** (a press during the planes) draws the page again, and cleans up if a plane reached the controller.
- **X4:** the cleanup's write of the page to RED is also the reseed a deferred FAST lacks.
- **The X4's resident build** keeps `Pass::AtBuildEnd`: it runs on the heap its low-heap guard watches, which the page
  and its glyphs would come from.
- **One-Pass:** a C3 build page is never one-pass (the mask needs S, which the build holds); its gray comes as a second
  refresh about 0.65 s later (decision D1, option A).

X3, cold open of a 66-page chapter: gray on the panel 6.58 -> 4.08 s (two passes), 6.56 -> 4.10 s (One-Pass). The pass:
planes 119 ms, gray 228 ms, redraw and cleanup 78 ms. The chapter is laid out about 0.5 s later per page that gets a
pass during the build. Full figures in [../background-rendering.md](../background-rendering.md), *Its gray*.

A variant of (c) renders the planes in 80-row strips into an 8 KB scratch, as CrossPoint does, so W keeps the page and
an abort costs nothing. The SDK has the strip uploads for all three C3 controllers; the renderer's strip target was
removed. CrossPoint's strips cost more than our full-frame planes on the same panels: X3 245 ms for both planes against
119 ms; X4 111 ms for the MSB plane alone against about 100 ms for both of ours. It stays the fallback if the redraw's
abort cost shows.

**Fixes 5 and 6** shorten the build behind the page, no longer the wait for its gray: #813 lets the X4's slices run
through its build page's refresh, waiting for the waveform without the bus lock
(`HalDisplay::refreshLeavesBusToCard()`, `waitRefreshOffBus()`); #811 makes the Picco's and the Murphy's drivers
return once the waveform is fired.

## 6. A second core on the S3 boards

The loop task runs on core 1 (`CONFIG_ARDUINO_RUNNING_CORE 1`), the render task is pinned to core 1
(`ActivityManager::begin`; the comment above it, ported from upstream, still says the loop task is on CPU 0), and so is
the KOReader sync worker (`KOReaderSyncWorker`). Core 0 has the system and Wi-Fi tasks. The C3 has one core.

Render compute and build slices both hold RL, so two of them never compute at once on any number of cores. A second
core could overlap only the render's compute with a build or a decode waiting for RL. On the Picco's 2.53 cold open
that is the page draw (97 ms), the gray planes (214 ms) and the pre-render (42 ms); the rest is the panel and the card.
So a perfectly parallel build wins at most 0.15-0.35 s per cold open, for the build's end (class 3). The page (class 1)
waits for the build's first page and for the panel, never for a CPU. One real contention was measured and fixed without
a core: the loop task spinning beside a render on core 1 made the X4 Pro's page draw 205-280 ms against 41-43 ms.

| Candidate | Gain | What must become thread-safe | Failure modes |
|---|---|---|---|
| (a) The whole chapter build on core 0 | at most 0.15-0.35 s on the build's end; nothing on the page | `Section`, `Epub`'s metadata cache, `CssParser`, `FontCacheManager`, `FontDecompressor` and the glyph store, `ZipFile`, every user of the build's `BuildArena`, `image_scratch`, `SpinePageIndex` | two concurrency models (the C3 keeps slices under RL); the host tests run one thread on a frozen clock and cannot see a race |
| (b) Jobs with value in, value out: a picture decode for the page on screen | the build no longer stops for the decode; share not measured | the decoders draw through `GfxRenderer` in cache-only mode and use the process-wide `image_scratch`; a job needs its own ZIP handle, PSRAM block and row sink | card contention with the build; a cache file written while a page draw probes it; cancel on a turn |
| (c) Re-pin the sync worker to core 0 | its TLS work stops competing on core 1; not measured | nothing (jobs by value, results polled) | a long compute at priority 1 can starve core 0's idle task, the reason the render task is on core 1 |

The recommendation is no second-core work (decision D10, option A), with one measurement first; it is open in
[../future_work/reader-priority-scheme.md](../future_work/reader-priority-scheme.md).

## 7. Decisions

| # | Decision | Taken |
|---|---|---|
| D1 | Pages drawn while a build holds the buffer, One-Pass on (C3) | A: B/W, then the gray as a second refresh (#823). C, a strip-composed one-pass, stays a later step |
| D4 | Look-ahead at a fast pace | A: after an abandoned spine, wait for a pause (#817) |
| D7 | The speed comparison's rows | A: "page complete" and "chapter laid out" apart ([../feature-comparison.md](../feature-comparison.md), #798) |
| D8 | The 400 ms settle before an inline gray | measured, then kept only while the reader is flipping (#816) |
| D9 | Order of the work | #808 before (c); fixes 5 and 6 independent. Done in that order |

D2 (pictures), D3 (the final count), D5 (what an idle borrower takes), D6 (partial sections) and D10 (the second core)
are open, with their options, in the future-work file.
