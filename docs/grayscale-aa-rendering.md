# Grayscale Anti-Aliasing Rendering

How the reader renders anti-aliased text on the e-ink panels: the two-plane model, the current
algorithm and the paths the reader chooses between, and the two earlier approaches it replaced.
Where each controller keeps its previous frame, and what a grayscale pass leaves behind on each, is in
[secondary-buffer-management.md](secondary-buffer-management.md).

---

## Background: two planes, four levels

The X3, X4 and X4 Pro controllers (UC8253 or UC8279d on the X3; SSD1677, UC8179 or UC8279 on the X4
and X4 Pro) hold two independent 1-bit planes:

| Plane | SSD1677 | UltraChip (UC8253, UC8279, UC8179) |
|-------|---------|-------------------------------------|
| LSB | BW RAM (`0x24`) | DTM1 (`0x10`) |
| MSB | RED RAM (`0x26`) | DTM2 (`0x13`) |

A grayscale waveform reads both planes at once and maps each pixel's two bits to a level. The
firmware writes two encodings (see `GfxRenderer::setAbsoluteGrayPlanes()` for the bit patterns):

- **Overlay** (reader text AA): a plane bit is set only for a grey pixel, and the B/W base supplies
  black and white. (LSB, MSB) = 11 dark grey, 01 light grey, 00 "leave the pixel as the base drew it".
- **Absolute** (images and the sleep cover, where `supportsAbsoluteGrayPlanes()`): the planes carry
  every pixel. (LSB, MSB) = 00 black, 10 dark grey, 01 light grey, 11 white.

Each driver translates these into its controller's own selector pattern (the SSD1677 complements
absolute planes for its factory LUT; UC8179 combines them with its snapshot of the base).

The T5S3 is different. Its controller is LovyanGFX's `Panel_EPD`, which keeps a 4-bpp buffer of its
own. `LgfxEpdDriver` copies the planes into host-side buffers and darkens the matching pixels of its
8-bit canvas, and in normal reading the planes are captured during the B/W render and go out with the
page as one waveform (see "Which path the reader takes").

Each plane is one framebuffer: 48,000 B on the X4 and X4 Pro (800×480 ÷ 8), 52,272 B on the X3
(792×528 ÷ 8). For a B/W page turn the same two planes hold "new frame" and "previous frame", so after
a grayscale pass they have to be put back to the B/W page before the next differential refresh, or the
diff runs against plane data and ghosts. That is the cleanup step below.

The X3 and X4 run on an ESP32-C3 with no PSRAM. Both framebuffers are resident, and any grayscale
scheme has to fit in the heap that is left, shared with the EPUB parser, the font cache, the image
decoders and the task stacks.

---

## Current approach: reuse the B/W framebuffer as plane scratch

### Why it works

`copyGrayscaleLsbBuffers()` and `copyGrayscaleMsbBuffers()` stream whatever full-size buffer they are
given. Once the B/W page has been pushed with `displayBuffer()`, the swap has left that page in
`frameBufferActive`, and the write buffer is not needed until the next render. So the write buffer can
serve as the render target for each plane in turn, with each plane streamed to the controller as soon
as it is rendered. No extra allocation.

### How it works

`GfxRenderer::renderGrayscalePlanesSequential(renderFn, shouldAbort)`:

```
1. clearScreen(0x00)                  the write buffer becomes plane scratch
2. setRenderMode(GRAYSCALE_LSB); renderFn(GRAYSCALE_LSB)
3. shouldAbort()? -> abandonGrayscalePass()
4. copyGrayscaleLsbBuffers()          stream the plane to the controller
5. clearScreen(0x00); setRenderMode(GRAYSCALE_MSB); renderFn(GRAYSCALE_MSB)
6. shouldAbort()? -> abandonGrayscalePass()
7. copyGrayscaleMsbBuffers()
8. setRenderMode(BW); displayGrayBuffer()        the gray waveform
9. cleanupGrayscaleWithPreviousBuffer()
```

Step 9 hands the driver `frameBufferActive`, the full B/W page (images included) that the last
`displayBuffer()` swapped there, as its baseline for the next fast refresh, and copies it back into the
write buffer. Each driver restores its own state from it (the per-controller table is in
[secondary-buffer-management.md](secondary-buffer-management.md)). With the secondary away (lent or
released) it hands the driver `nullptr` instead: the driver drops its synced claim and takes a clean
sync on its next push, and the write buffer keeps the last plane. The write buffer is never accepted
as a baseline, because by step 9 it holds a plane (SDK `4a320d6`). The reader only runs AA with the
secondary resident, so that branch is a safety net.

`abandonGrayscalePass()` runs the same cleanup without the gray flush, so an aborted pass leaves the
B/W page on screen and the baseline intact.

`renderGrayscalePlanesInterleaved()` is the same sequence entered while an async B/W refresh is still
running (`triggerDisplayAsync()`): the LSB plane renders during the waveform, and
`finishDisplayAsync()` waits out the rest before the first plane write.

```
ESP32 RAM during the pass:
  B/W framebuffer  [48-52 KB]  render scratch for both planes
  Extra:            0 KB
```

### Which path the reader takes

All three need text anti-aliasing on and a resident, non-degraded secondary
(`EpubReaderActivity::renderContents()` checks `hasSecondaryBuffer()`).

| Path | Where | What happens |
|---|---|---|
| Single push | `supportsGrayFrame()`: the T5S3, and the X4 Pro and the Murphy on text pages with One-Pass Anti-Aliasing on | `beginGrayCapture()` makes the B/W render write both planes as it goes; page and greys go out as one waveform (`triggerGrayscaleFrame()`). |
| Inline | `!usesDeferredAa()`: an async-capable panel that is not an X3 (X4, X4 Pro) | B/W page via `triggerDisplayAsync()`, then `renderGrayscalePlanesInterleaved()`. It waits `AA_SETTLE_MS` (400 ms) after the B/W waveform before committing, and a press in that time cancels it, so a reader flipping through pages is not held by a pass they will never see. A turn parked on the render lock counts as a press (`pageTurnWaitingForLock_`): by then it has been drained and released, and nothing else shows it. Skipped when a page turn is already on its way. |
| Deferred | the X3, and any panel that cannot overlap | The B/W page is shown; `runDeferredGrayscalePass()` runs `renderGrayscalePlanesSequential()` from idle time, and a page turn aborts it. |

### One-pass text (X4 Pro with UC8179, Murphy M3, X3, X4)

The One-Pass Anti-Aliasing setting puts this panel on the single push for text pages. Each pixel gets
one of four waveforms in one refresh: rest, to white, to black, or one gray. A pixel the glass already
shows at its target rests, so text that stays put is not driven again; the others are driven to the
page. The rule is one SDK helper, `RestTextCodes.h`, shared by the X4 Pro, the Murphy M3 and the Picco;
each driver maps the four drives onto its own controller's planes. The point is that nothing visibly
changes once the page is up. It is no faster: the finished page lands about when today's B/W page does.

`display.supportsOnePassText()` offers the setting. `ReaderUtils::OnePassTextScope` holds the mode for
one page's render, pre-render or display, because while it is on `copyGrayscale*Buffers()` stage planes
instead of uploading them. Everything outside those three calls, a menu, a sleep image, the BMP viewer,
the TXT, MD and XTC readers, finds it off and runs as before. A page with images stays off too: the
capture rides the glyph blit and records no image greys. The X4 Pro's block is 288 KB of PSRAM,
allocated at the first one-pass page. The Murphy M3 does the same with its own waveform and three planes of
12.5 KB, and gives gray the gray drive on every page, as its text bank was tuned.

The X3 and the X4 have no memory for either, so the planes are composed in the two frame buffers
(`renderer.composesOnePassText()`). `beginOnePassMask()` turns the buffer holding the frame on screen into
the old plane, white where a pixel was white and stays white, and points the renderer at it; the page's
gray is drawn into it, and `displayOnePassFrame()` sends both in one refresh. When the frame on screen is
not known, after a Background-B borrow say, `beginOnePassMask()` returns null and that page takes the two
passes. Each board's waveform and what was measured are under
[One-pass text AA, board by board](#one-pass-text-aa-board-by-board).

### Absolute planes: images and the sleep cover

`GfxRenderer::beginAbsoluteGrayPass()` opens an absolute pass on panels that support one (the sleep
cover in `SleepActivity`, and `BmpViewerActivity`). It consumes a pending refresh override as the base
mode, the SDK pushes the B/W base, and the planes then go out in the absolute encoding. On the UC8279d
this selects the long XTH4 waveform (the vertical-banding fix ported from crosspoint-reader PR #3469);
on the UC8253 the planes go out under the `gc` bank with both endpoints idle, after a real base push;
on the SSD1677 the factory waveform draws base and greys in one pass (`GrayscaleBase::Combined`). The
T5S3 has no absolute mode: its sleep cover paints 8-bit grey into the borrowed panel canvas
(`borrowGray8Canvas()`, `displayGray8Canvas()`).

### Greys left on the glass

The cleanup puts controller RAM back to the B/W page, but the glass still holds grey at every AA edge.
A later differential push that trusts the RAM gives those pixels only a gentle white-to-white drive.
`Uc8253X3Driver` therefore tracks `_grayOnGlass` (set by every gray waveform, cleared only by a
waveform that drives every pixel) and takes a clean base for the next grayscale pass while it is set;
UC8179 and UC8279 X4 use `_redriveAfterGray`. UC8279d and SSD1677 do not track it (open, see
[future_work/display-and-refresh.md](future_work/display-and-refresh.md)).

### Timing (X3 UC8253, device log 2026-09-23)

| Refresh | Duration |
|---|---|
| Page turn (`X3_DRF_fast`) | ~382 ms |
| AA gray waveform (`X3_DRF`) | ~228 ms |
| Periodic scrub (`X3_DRF_half`) | ~460 ms |

### Invariant that makes this safe

The write buffer holds plane data from step 1 to step 9. That is safe because:

- the B/W page is already on the panel and in `frameBufferActive` before the pass begins, and
- step 9 restores the driver's baseline and the write buffer from `frameBufferActive`, not from the
  write buffer, so images and all other B/W content come back too.

> **Note for maintainers:** this depends on `frameBufferActive` holding the B/W page that was pushed
> just before the pass. If another swapping refresh happens between the B/W push and the pass,
> `frameBufferActive` no longer holds the right frame and the next FAST differential ghosts.

Planes are sent row-reversed by `EpdBus::sendPlaneFlipped()` while streaming; the framebuffer itself
is never flipped in place.

### Trade-offs

- Zero extra allocation, so no fragmentation and no AA suspension under memory pressure; and also no
  "suspend and recover" safety valve.
- Two render passes (one per plane) on the staged paths; the single-push path walks the page once.
- The write buffer is not valid B/W content during the pass. Anything that reads it in that window
  (a screenshot, an overlay) sees a plane.

---

## One-pass text AA, board by board

How each board composes a one-pass page, its waveform, and what was measured. The setting is One-Pass
Anti-Aliasing, off by default: #646 for the X4 Pro with UC8179 and the Murphy M3, #648 for the X3, #650 for the X4.
Open work is in [future_work/display-and-refresh.md](future_work/display-and-refresh.md), "One-pass text AA".

**Why it is an option.** Text passes by eye and, more to the point, the screen no longer flickers slightly when the AA
pass finishes. It is not faster.

**Source.** Free-Ink/freeink-sdk branch `fix-display-updates`, tip `d73c852`, and crosspoint-reader branch
`fix-display-updates` at `8739c86f6` ("Direct AA"), which pins that same SDK tip. Only the slice for the X4 Pro's
controller came over: a full merge conflicts widely with `witchhunt`, and most of the branch's churn is Picco work.

### X4 Pro with UC8179: a pixel at its target rests

Since 2026-11-14 the X4 Pro composes like the Murphy and the Picco. `RestTextAa` (SDK `RestTextAa.h`) codes each
pixel from what the glass holds to the page with the shared rule (`RestTextCodes.h`), and one 18-frame table drives
all four codes (`kRestTextUc8179` in `Uc8179Driver.cpp`):

| Code | Drive |
|---|---|
| Rest | none |
| To white | white 16 frames |
| Gray | white 8, black 4, idle 4, white 2 |
| To black | black 12 frames |

- **Memory:** six planes, 288 KB of PSRAM (the page, two staging planes, its gray, the glass in B/W and its gray),
  allocated at the first one-pass page. S3 only.
- **What the glass holds:** after a clean refresh it is the page in B/W, so only the gray moves. After any other B/W
  frame, a menu say, it is not known, and every pixel of the next page is driven to its target once.
- `FREEINK_UC8179_TEXT_REST=0` builds the charge-map pass described below.

**Measured on one X4 Pro, gray lab (2026-11-14, `tools/murphy-gray-lab/x4prolab.py`).** Reader runs: 150 pages each,
key to finished frame over serial, one-pass on, a HALF every 30 pages. Levels: % of white from the webcam, 0 = black.

| | Charge map (until 2026-11-14) | Rest |
|---|---|---|
| Page turn, median / p90 | 1,390 / 1,459 ms | 1,262 / 1,333 ms |
| Gray, from white / from black | 31 % / 0 % | 65 % / 63 % |
| PSRAM | 576 KB | 288 KB |
| Waveform | 23 frames | 18 frames |

- **Gray:** the charge-map gray (white 4, black 3) depends on the start: 31 % from white, black from black. The rest
  gray drives white first and lands 2 points apart from either start.
- **Ghosting (Test 11):** bands held black for 50, 200 and 500 pages, then turned white, came back within 2.1 points
  of a band held for one page, for rest as for re-drive. Black held at rest did not fade within 8.5 minutes.
- **After reading:** 150 pages left no structure on a white frame that the camera resolves.

#### Before: the charge map (until 2026-11-14)

Without one-pass a page takes a B/W refresh and then an AA overlay refresh. The first one-pass port composed both into
one refresh, where each pixel gets one of four waveforms: black, gray, hold, white. Hold is a one-frame nudge for a pixel that stays white,
which covers about 80% of the panel on a text page. A charge map (one byte per pixel) tracks each pixel's net drive, and
releases a held pixel into a real white drive once it has drifted too far.

- **Memory:** `X4ProTextAa` needs 576 KB of PSRAM (`SIZE*12`), so it is S3 only.
- **Tones:** all overlay grays collapse to one gray, so a page has three tones instead of four.
- **Waveforms:** fixed external LUTs for SSD1677, UC8179 and UC8279. The branch cites stock firmware 26110201 for them.
- **Who enables it:** upstream's firmware, through `ReaderRefreshScope`. Ours switches it per page with
  `setOnePassText()` (`ReaderUtils::OnePassTextScope`).

**Measured on an X4 Pro with UC8179.** Test port on our pin, one image with a runtime toggle, same book and start page.
Literata 14, AA on, refresh every 30 pages. 74 one-pass pages: no panic, no compose failure, no stuck BUSY.

| | Today | One-pass |
|---|---|---|
| Key press to finished AA page, 3 s cadence (median) | 1620 ms | 1140 ms |
| Panel waveform per page | 667 + 296 ms | 641 ms |
| 1.2 s cadence: AA passes aborted | 5 of 15 | 0 |
| About 1 s per press: AA passes aborted | 17 of 20 | 0 |
| About 1 s per press: ms per page | 1016 | 1131 |

- **Where the time goes:** today's B/W page lands at about 1060 ms and the AA pass finishes it at 1620 ms. One-pass lands
  the finished page at 1140 ms. So a page turn reaches the panel no sooner. What goes away is the AA step after it.
- **Blocking cost per page (medians):** compose 49, upload 117, waveform 641, finish 95, reseed 115, total 1018 ms.
- **Pixels per page, of 384k:** hold about 308k, white-driven about 36k, black about 27k, gray about 13k.
- **Charge releases:** 1.7-2.6k pixels per page, 5-7% of white drives.
- **After a HALF:** 2632 ms and about 343k pixels driven white, because any B/W frame wipes the history. By code
  reading, a menu open and close has the same effect on the next page.
- **Memory during the run:** PSRAM free about 7.37 MB, internal free 183 KB (minimum 166 KB).

**By eye, 2026-11-07, on this X4 Pro:** text looks okay, with no artifacts or ghosting, and it does not feel a lot
faster, which fits the numbers above. Upstream issue #4344 reports text on a UC8279 unit as a large improvement.

### Murphy M3: one gray, and a pixel at its target rests

**Why one gray.** Before one-pass the gray landed about 330 ms after the page. Four levels cannot be one refresh
without a flash: a page turn needs rest, to white, to black, light and dark, and a refresh has four LUT slots. Merging
rest with to white would drive the whole background one way on every page. Three levels fit: rest, to white, to black,
and one gray that nets to zero and takes about as long as FAST (proposed as VSL 4, VSH 8, VSL 4, 16 frames). Text
darkness Lighter shows roughly how one gray looks.

Its own waveform, not the X4 Pro's: `MURPHY_LUT_2x_TEXT` in `Uc8253MurphyLuts.h`, chosen by
`Uc8253MurphyDriver::displayGrayFrame()` from the page and the record it keeps of the glass. No charge map: a pixel
that already shows its target rests, which adds no drive, so a page needs only the planes the driver already had plus
three of 12.5 KB. The rule is the shared `RestTextCodes.h` with `grayRests = false`: unlike the X4 Pro and the Picco,
gray that stays gray takes the gray drive again on every page.

| Slot | Waveform | Why |
|---|---|---|
| Rests | rest, 20 frames | The pixel already shows its target, white or black. Less drive than FAST, whose `KK` adds a frame of VSH |
| To white | VSL 20 | FAST's `R22` |
| Gray | VSL 5, VSH 8, VSL 3 | VSH 8 takes any start to black, 1% from white; VSL 3 then lands about 48%, between the nudge's 35% and 58% |
| To black | VSH 15 | FAST's `R23` |

**Measured, `tools/murphy-gray-lab` Test 9 (2026-11-07), one panel, room temperature:**
- Gray lands within the panel's own spread from white or gray. That spread is about 6 points from top to bottom, with
  every band starting white.
- A pixel that was black lands 7-10 points darker, the first time only. From a HALF's black it is the 10.
- White from ink falls 4-7 points short of white, as FAST's does. Black lands black, and rest leaves grays alone.
- A 16-frame reset halves the darker-from-black step, but it lands far darker and takes 683 ms against 443 ms.

**Measured in the reader:** a text page turn is one refresh of 442-452 ms, where today's is FAST at about 470 ms plus a
nudge of 205-275 ms. A HALF, the page after an image page or a menu, an image page itself and the first page after
boot keep the two passes. 12 of 12 fast presses landed, no fault.

A pixel that stays black rests every page, which matters on this panel: the gray lab's Test 11 (2026-11-08) shows
black re-driven every page fading by 14.5 after 500 pages and coming back 8 points lighter than the white around it,
where rest stays clean (`tools/murphy-gray-lab/README.md`).

### Xteink X3 (UC8253): composed, no extra memory

The X3 has no PSRAM, so its planes are composed in the two frame buffers it already has.
`FreeInkDisplay::beginOnePassMask()` turns the buffer that holds the frame on screen into the old plane
(white before and after), the reader draws the page's gray into it with one MSB pass, and
`displayOnePassFrame()` sends both planes and makes the page the frame on screen. `_activeMatchesGlass`
guards it: only a frame a display just put there counts, and after a lend or a window update the page
takes the two-pass path. `Uc8253X3Driver` runs one 24-frame bank: rest, FAST's 18 frames to white and
to black, and gray VSL 9, VSH 12, VSL 3 (lab Test 9 on an X3, see `tools/murphy-gray-lab/README.md`).

**Measured in the reader (X3, 2026-11-07):** 26 text pages one-pass, each one refresh of 445 ms (FAST
alone is 382 ms) after a gray render of about 55 ms; heap flat; the setting holds over a reboot. Those were
pre-rendered turns: until 2.41 a page rendered fresh (no pre-render to show) lent the secondary buffer to its font
prewarm, which loses the frame on screen, so it always took two passes. Now a page bound for one-pass keeps the
buffer and its prewarm uses the heap (`renderContents`). Two passes remain for image pages, a HALF (the periodic one,
and the one after an image page, a popup or a book open), the page after a menu, a page after a Background-B borrow,
a page redrawn because a glyph found no memory, and fast flipping (`aaPreempted`).

**A pixel that stays black** is driven 18 frames every page: the composed planes have no memory to tell it from
gray. Measured with the gray lab's Tests 10 and 11 (2026-11-07): held black for up to 500 pages, it comes back white
as far as one held for a page, and further than the full refresh's flash (1.8 points short at 500) or rest (1 point);
nothing creeps back over the hour after. Its black is 2-4 points deeper while held. The same test on the Murphy M3
(2026-11-08) does show the re-drive (above), so the X3's result is the X3 panel's, not a rule.

### Xteink X4 (SSD1677): composed, no extra memory

The X3's composed path, with the work in `Ssd1677Driver::displayOnePassStart()`: BW RAM takes the page,
RED RAM the old plane, one `0xCC` external-LUT activation, then `displayFinish()` puts the page in both
planes. The waveform is Free-Ink's `kX4ProTextSsd1677` with rows 1 and 3 exchanged: the row is
RED * 2 + BW, so ours runs black (VSH1 17), to white (VSL 24), gray (VSL 6, VSH1 4) and white that stays
white (the X4 Pro's VSL 2 hold nudge). Offered on the X4 board only, the SSD1677 it has run on.

**Measured on an X4 (2026-11-07):** 13 text pages one-pass, each one refresh of 596-600 ms, against FAST at
507 ms plus a 144 ms gray pass today; gray render 35-42 ms; heap flat; no fault. The camera showed a clean
page, but cannot judge the gray. Fast flipping skips AA, as today.

The X4 re-drives black too (VSH1 17 a page). Measured on 2026-11-08 through the reader's `CMD:LAB` (lab Test 11), it
behaves like the X3: within half a point after 500 pages, and resting black fades there.

### What carries over to other drivers

What upstream's branch holds beyond the slice we took, and where each piece could work:

| Tweak | Where it could work |
|---|---|
| One-pass as written | X4 Pro only: PSRAM and stock LUTs for its three controllers |
| One-pass on Murphy M3 | Done, its own waveform (above) |
| One-pass on C3 (X4, X3) | Done as the composed path, in the two frame buffers a C3 already has (above) |
| One-pass on T5S3 | No: no LUTs, already single push |
| Drive gray pixels to the new target on the next B/W frame | Only SSD1677 lacks this. Its 48 KB mask fits SSD1677 X4 Pro units, not a C3 |
| Separate reader and UI waveforms, ">1/8 of bytes changed" test | Only where we own two fast waveforms: X3, Murphy |
| Dark-mode black re-drive | Any differential driver, but expect a background pulse. Test first |
| AA on inverted pages | The facade change is generic. Each driver needs a LUT proven on dark pages |
| XTH4 tail fix in `Uc8279X3Luts.h` | Applies directly, our table lacks it. Unverified against stock, so test on a UC8279 X3 |
| Doses checked against the LUT (`testTextWaveforms`) | Copy for any external-LUT driver |
| Rest-frame LUT, Picco init and power policy | Panel-specific, skip |

### UC8279 image tones swapped upstream (#4344): not in our pin

Our pin has no `kX4ProImage`; our UC8279 X4 Pro path still uses `kQualityBank`. Upstream's bug is fixed by
Free-Ink/freeink-sdk#182, merged into `fix-display-updates` and confirmed by the issue's reporter on a UC8279 X4 Pro on
2026-11-07. It is kept here because it bites whoever takes `kX4ProImage`.

The report shows a sleep image on the Direct AA build where a near-black coat and rock come out lighter than the sky.
That is not lost contrast: dark gray and light gray have traded places.

**Cause.** `Uc8279X4Driver::displayGray()` uploads `kX4ProImage` with `bus.cmd(0x20 + i)`. The bank it replaced,
`kQualityBank`, goes through `kQualityLutReg = {0x20, 0x21, 0x23, 0x22, 0x24}`. Stock image tables run VCOM, black,
light, dark, white, and our absolute planes are inverted on the way out, so black and white land right while dark gray
selects `0x22`.

| Level | Planes (LSB, MSB) | On the wire (DTM1, DTM2) | Register | `kQualityBank` (2.0.0) ends | `kX4ProImage` ends |
|---|---|---|---|---|---|
| 1 dark gray | 1, 0 | 0, 1 | `0x22` | B16 W2 (dark) | B24 W6 (light) |
| 2 light gray | 0, 1 | 1, 0 | `0x23` | B16 W2 B1 W2 (light) | B24 W3 (dark) |

B and W are frames driven towards black (rail 1) and white (rail 2). `kX4ProTextUc8279` settles both the rails and the
selection: its black dose sits on rail 1 at `0x24`, and its hold row, wire (0, 1), sits at `0x22`. The `kQualityBank`
tails come from compiling `makeQualityBank()` at its default speed of 60. The planes come from `grayPlanePixel()` in
absolute mode, and `streamPlane(..., invert=true)` inverts them.

**Evidence from the report's photos.** The original BMP was run through the firmware's Atkinson quantizer with legacy
thresholds, which is what UC8279 uses. Each photo was then fitted against the four level densities, with nothing in the
fit assuming a swap:

| Photo | Black | Dark | Light | White | R² |
|---|---|---|---|---|---|
| 2.0.0 | 34 | 91 | 146 | 210 | 0.95 |
| Direct AA | 75 | **165** | **113** | 215 | 0.77 |

The black figure on the Direct AA row is weak: black is 3.7% of the pixels and the fit is looser. Do not read a black
level problem into it.

**The fix.** Free-Ink#182 uploads the packet through `kQualityLutReg`. `test_pro.cpp` asserted the straight order, so it
passed with the bug; the PR makes it check that `0x22` ends darker than `0x23`. Every use of the packet is affected:
sleep images, covers, and book pages with images, since upstream's reader sends those through Absolute on UC8279.

**Not checked.** Stock's plane order: the rows match stock's, so stock must send dark gray as (1, 0). Our X4 Pro cannot
run this path: the boot probe reads the controller and picks `Uc8179Driver`.

**If we ever take `kX4ProImage`:** bytes verified against stock (26102901, at `0xe4ff3` and `0xe5560`) say nothing about
the register order. Take the exchange with it.

---

## Murphy M3: gray and its B/W waveforms

Murphy builds have four-level gray, through `-DFREEINK_MURPHY_GRAY` in `[murphy_board]`; a build without it hides the
settings through `HalCapabilities::panelShowsGray()`. How the gray was found, and every table behind the decisions
below, are in [tools/murphy-gray-lab/README.md](../tools/murphy-gray-lab/README.md). All of it comes from one panel,
at room temperature. Open work is in [future_work/display-and-refresh.md](future_work/display-and-refresh.md),
"Murphy M3 gray".

A book with a stored `textAntiAliasingOverride` of 0 stays B/W whatever the setting says. Books opened on a Murphy
before gray may carry one, which is how the first reader test came out B/W.

**Image midtones stay dark.** BMP, JPG and PNG all show in one gray pass, and Confirm's gray and B/W toggle works.
Midtones come out dark: the 4-level dither assumes light gray at 67 %, and ours is 50-58 % (the lab's Test 5 has the
ramp; JPG and PNG also read 8-15 points below BMP on a soft test image). This was left as it is. Light gray at 5
frames instead of 4 would be about 70 %, but it would change text as well. Not tried.

**FAST gives black to white 20 frames instead of 15,** as VSL needs about twice as long as VSH. B/W UI screens ghost,
which is why their refresh counter came down; on a real Settings list the band a selection bar leaves went from 4.2
to 2.5 points (the lab's Test 6). 24 frames overshoots, and neither VSL at -15 V nor a VSL touch on `WW` helped.

**The refresh counter.** The driver no longer promotes FAST to a full refresh on its own, so the setting means what it
says, and "Never" means no periodic full refresh at all. It had capped every setting at 9 while the log still said
FAST. At 20 the UI and the reader looked fine by eye. Ghosting over runs longer than 9 has not been measured.

**HALF stays as the manufacturer set it.** HALF leaves more ghost than FAST, and more VSL on `WW` in
`MURPHY_LUT_21_DEFAULT` would clean better at no cost in time, but it whitens the background, and every FAST after it
then lands short of the new white, so the two have to be judged as a pair (the lab's Test 7 and 8). FAST runs far more
often than HALF, and on a real list 10 extra frames came out as a net loss, so HALF was left alone. If the ghost after
HALF ever matters more, 10 extra + 25 is the balanced choice, at 100 ms more per FAST.

**DC.** 20 frames against 15 nets 5 toward white per round trip in frames, though nominally 20 x 12.6 V about balances
15 x 16.6 V. The true centre is unknown with no factory VCOM.

**Upstream.** Offered to Free-Ink/freeink-sdk: gray as #174 and the FAST change and the limit removal as #176, both
merged into Free-Ink main on 2026-11-04. HALF was not offered, as it did not change. The firmware still pins our fork,
for other reasons (`.gitmodules`: the T5S3 support of Free-Ink #51 and our fixes on it).

---

## Earlier approaches (removed)

### Snapshot (legacy)

Allocate a second 48 KB buffer, copy the B/W page into it (`storeBwBuffer()`), render each plane into
the framebuffer, stream it, then copy the snapshot back (`restoreBwBuffer()`) and resync the
controller from it. Simple, but it allocated 48 KB on every page turn. On the C3 the heap fragmented
over a session until the allocation failed and AA was silently suspended
(`antiAliasingSuspendedLowMemory`). `storeBwBuffer()` and `restoreBwBuffer()` remain in `GfxRenderer`
for the screenshot path.

### Tiled strips (interim)

Render each plane in horizontal strips into a ~24 KB scratch held for the reader session, culling
glyphs outside the strip, and stream each strip with a windowed RAM write, leaving the B/W framebuffer
untouched. It saved the per-page allocation but needed 2-3 render passes per plane and a windowed
write API from the open-x4-sdk whose licence turned out to be incompatible with the project. The
firmware's strip path was removed; `GfxRenderer::isStripActive()` and `glyphIntersectsStrip()` remain
as no-op stubs. The SDK has its own `writeGrayscalePlaneStrip()` again, for absolute-plane uploads,
but the reader does not use it.

### Comparison

| Property | Snapshot | Tiled strips | B/W-buffer reuse (current) |
|---|---|---|---|
| Extra peak RAM | 48 KB per page | ~24 KB for the session | 0 KB |
| Allocation pattern | alloc/free every page | one alloc at session start | none |
| Fragmentation risk | high over long sessions | low | none |
| Render passes per plane | 1 | 2-3 (strips) | 1 |
| B/W framebuffer intact during the pass | no | yes | no |
| Controller resync source | the snapshot | the live framebuffer | `frameBufferActive` |
| AA suspension under pressure | yes | no | no |

---

## Key API reference

| Method | Layer | Purpose |
|--------|-------|---------|
| `renderGrayscalePlanesSequential(fn, abort)` | `GfxRenderer` | The staged pass, end to end |
| `renderGrayscalePlanesInterleaved(fn, abort)` | `GfxRenderer` | The same, overlapping an async B/W refresh |
| `beginGrayCapture()` / `displayGrayscaleFrame()` / `triggerGrayscaleFrame()` | `GfxRenderer` | Single-push AA where `supportsGrayFrame()` |
| `beginAbsoluteGrayPass()` | `GfxRenderer` → `HalDisplay` | Open an absolute pass and push its B/W base |
| `copyGrayscaleLsbBuffers()` / `copyGrayscaleMsbBuffers()` | `GfxRenderer` → `HalDisplay` → `FreeInkDisplay` | Stream a plane to the controller |
| `displayGrayBuffer()` | `GfxRenderer` → `HalDisplay` → `FreeInkDisplay` | Run the grayscale waveform (no swap) |
| `cleanupGrayscaleWithPreviousBuffer()` | `GfxRenderer` → `HalDisplay` → `FreeInkDisplay` | Restore the driver's baseline and the write buffer from `frameBufferActive`; with the secondary away, tell the driver there is no baseline |

## Scheduling: the deferred pass runs before the next-page pre-render

On the deferred path the AA pass runs before Background A re-arms, because the planes and a
pre-rendered page compete for the same heap. The cost (a quick turn inside the AA window pays a full
render) and the candidate reordering are in
[future_work/display-and-refresh.md](future_work/display-and-refresh.md) under "Pre-render before the
deferred AA pass".
