# One-pass text AA (X4 Pro)

Status: analysed and measured on 2026-11-06 and 2026-11-07, not ported. Source: Free-Ink/freeink-sdk branch
`fix-display-updates`, tip `d73c852`, and crosspoint-reader branch `fix-display-updates` at `8739c86f6` ("Direct AA"),
which pins that same SDK tip.

**Decision so far.** Not started. Text passes the by-eye check on our panel, but it does not feel much faster, so the
case for a port is consistency, not speed. If we port, take a slice of the text-AA core as an opt-in. Do not merge the
branch: a trial merge into `witchhunt` conflicts widely, and most of its churn is Picco work.

## What it does

Today a page takes a B/W refresh and then an AA overlay refresh. One-pass composes both into one refresh, where each
pixel gets one of four waveforms: black, gray, hold, white. Hold is a one-frame nudge for a pixel that stays white,
which covers about 80% of the panel on a text page. A charge map (one byte per pixel) tracks each pixel's net drive, and
releases a held pixel into a real white drive once it has drifted too far.

- **Memory:** `X4ProTextAa` needs 576 KB of PSRAM (`SIZE*12`), so it is S3 only.
- **Tones:** all overlay grays collapse to one gray, so a page has three tones instead of four.
- **Waveforms:** fixed external LUTs for SSD1677, UC8179 and UC8279. The branch cites stock firmware 26110201 for them.
- **Who enables it:** nothing in the SDK calls `setReaderRefresh`. Upstream's firmware branch does, through
  `ReaderRefreshScope` in the EPUB and XTC page render.

## Measured on an X4 Pro with UC8179

Test port on our pin, one image with a runtime toggle, same book and start page. Literata 14, AA on, refresh every 30
pages. 74 one-pass pages: no panic, no compose failure, no stuck BUSY.

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

## Image tones swapped on UC8279 (upstream issue #4344)

**Open upstream, does not affect us:** our pin has no `kX4ProImage`. Our UC8279 X4 Pro path still uses `kQualityBank`.
Fix offered as Free-Ink/freeink-sdk#182, with the analysis posted on the issue.

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

**Fix, not run on hardware.** Free-Ink#182 uploads the packet through `kQualityLutReg`. `test_pro.cpp` asserted the
straight order, so it passed with the bug; the PR makes it check that `0x22` ends darker than `0x23`. Every use of the
packet is affected: sleep images, covers, and book pages with images, since upstream's reader sends those through
Absolute on UC8279.

**Not checked.** Stock's plane order: the rows match stock's, so stock must send dark gray as (1, 0). Our X4 Pro cannot
run this path: the boot probe reads the controller and picks `Uc8179Driver`, so the fix needs a tester with a UC8279
unit.

**If we ever take `kX4ProImage`:** bytes verified against stock (26102901, at `0xe4ff3` and `0xe5560`) say nothing about
the register order. Take the exchange with it.

## What a port has to fix

- **Page lost on compose failure.** `displayGray()` invalidates and returns without drawing. The test port fell back to
  B/W, and it never happened in 74 pages.
- **UC8279 voltages.** The text path (variants 2 and 3) and the image path write `0x01` and `0x82`. Nothing restores
  them, though the driver's own comment says PWR and VDCS stay panel-programmed. Later B/W refreshes may run on those
  voltages. Effect unproven: our unit is a UC8179.
- **History wiped by any B/W frame.** `invalidate()` drops the charge map although the panel keeps its charge. This is
  the 2632 ms page after a HALF.
- **Blocking waveform.** `waitRefreshComplete` holds the bus. Production needs `displayGrayFrameStart`, with finish and
  reseed (about 210 ms) off the critical path, which should lower the flip floor. That part is an estimate.
- **Book pages with images.** Our planes come from `beginGrayCapture`, and only the glyph renderer writes to it
  (`GfxRenderer.cpp`, `grayCaptureActive()`), so images never reach them. Measured on a full-page cover: 85,655 black
  pixels and `gray=0` under one-pass, where today's path runs a 413 ms gray pass on the same page. Such a page must leave
  one-pass, on every controller. Upstream does this on UC8279 only, through Absolute.
- **Leave `FREEINK_FB_PSRAM` out.** Default-on would move S3 framebuffers into PSRAM. It is unrelated and unmeasured.
- **Wiring.** Our `supportsGrayFrame` single-push path fits with two small helpers: stage the planes before the base is
  known, and keep them across a clean. The port must scope the mode to the EPUB page: `copyGrayscale*` stages instead of
  uploading, so a sleep image, the BMP viewer or XTC would otherwise reach the panel with stale planes.

Watch item: the branch's SSD2677 comment says re-driving static text builds ghosts, yet one-pass re-drives every black
pixel on every page.

## What carries over to other drivers

| Tweak | Where it could work |
|---|---|
| One-pass as written | X4 Pro only: PSRAM and stock LUTs for its three controllers |
| One-pass on Murphy M3 | Plausible: PSRAM, about 150 KB scaled. No vendor LUT, so one must be derived |
| One-pass on C3 (X4, X3) | Not as written. The charge map changes only 5-7% of white drives, so a 1-bit "black two pages running" plane (48 KB) may approximate it. Speculative, and reading heap is 15-53 KB |
| One-pass on T5S3 | No: no LUTs, already single push |
| Drive gray pixels to the new target on the next B/W frame | Only SSD1677 lacks this. Its 48 KB mask fits SSD1677 X4 Pro units, not a C3 |
| Separate reader and UI waveforms, ">1/8 of bytes changed" test | Only where we own two fast waveforms: X3, Murphy |
| Dark-mode black re-drive | Any differential driver, but expect a background pulse. Test first |
| AA on inverted pages | The facade change is generic. Each driver needs a LUT proven on dark pages |
| XTH4 tail fix in `Uc8279X3Luts.h` | Applies directly, our table lacks it. Unverified against stock, so test on a UC8279 X3 |
| Doses checked against the LUT (`testTextWaveforms`) | Copy for any external-LUT driver |
| Rest-frame LUT, Picco init and power policy | Panel-specific, skip |

## Next

1. Decide whether a port is worth it. It buys one panel update instead of two, and AA that survives fast flipping. It
   costs 576 KB of PSRAM, a slower flip floor (1131 against 1016 ms) and a separate route for image pages.
2. If so, port the core as an opt-in, with the fixes above.
3. Run `kUc8279X3_Xth4`'s tail fix on a UC8279 X3.
