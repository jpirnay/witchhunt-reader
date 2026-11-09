# One-pass text AA (X4 Pro, Murphy M3, X3, X4)

Status: shipped as an opt-in setting, One-Pass Anti-Aliasing, off by default, on the X4 Pro with UC8179, the Murphy
M3, the X3 and the X4 with SSD1677. How it works is in [grayscale-aa-rendering.md](../grayscale-aa-rendering.md).
What stays open is below.

Source: Free-Ink/freeink-sdk branch `fix-display-updates`, tip `d73c852`, and crosspoint-reader branch
`fix-display-updates` at `8739c86f6` ("Direct AA"), which pins that same SDK tip.

Why it is an option: text passes by eye and, more to the point, the screen no longer flickers slightly when the AA pass
finishes. It is not faster. Only the slice for this one controller came over: a full merge conflicts widely with
`witchhunt`, and most of the branch's churn is Picco work.

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

**Fixed upstream, does not affect us:** our pin has no `kX4ProImage`. Our UC8279 X4 Pro path still uses `kQualityBank`.
Free-Ink/freeink-sdk#182 was merged into `fix-display-updates`, and the issue's reporter confirmed it on a UC8279 X4 Pro
on 2026-11-07.

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

**Fix, confirmed on a UC8279 by the reporter.** Free-Ink#182 uploads the packet through `kQualityLutReg`. `test_pro.cpp` asserted the
straight order, so it passed with the bug; the PR makes it check that `0x22` ends darker than `0x23`. Every use of the
packet is affected: sleep images, covers, and book pages with images, since upstream's reader sends those through
Absolute on UC8279.

**Not checked.** Stock's plane order: the rows match stock's, so stock must send dark gray as (1, 0). Our X4 Pro cannot
run this path: the boot probe reads the controller and picks `Uc8179Driver`, so the fix needs a tester with a UC8279
unit.

**If we ever take `kX4ProImage`:** bytes verified against stock (26102901, at `0xe4ff3` and `0xe5560`) say nothing about
the register order. Take the exchange with it.

## Open on the X4 Pro

- **Other controllers.** The option shows on UC8179 units only, the one we have tested. The branch has tables for
  SSD1677 and UC8279, and UC8279 also writes `0x01` and `0x82` without restoring them, which may affect later B/W
  refreshes. Effect unproven: our unit is a UC8179.
- **History wiped by any B/W frame.** The charge map goes although the panel keeps its charge, so the page after a HALF
  takes 2632 ms and drives nearly the whole panel white. A menu has the same effect on the next page, by code reading.
- **Temperature.** The LUT is fixed, so the waveform does not follow the panel's temperature.
- **Static black.** One-pass re-drives every black pixel on every page, and the branch's SSD2677 comment says re-driving
  static text builds ghosts. None seen so far.

## What carries over to other drivers

| Tweak | Where it could work |
|---|---|
| One-pass as written | X4 Pro only: PSRAM and stock LUTs for its three controllers |
| One-pass on Murphy M3 | Done, see below |
| One-pass on C3 (X4, X3) | Done as the composed path, in the two frame buffers a C3 already has: the X3 in #648, the X4 in #650 (2026-11-07), see below |
| One-pass on T5S3 | No: no LUTs, already single push |
| Drive gray pixels to the new target on the next B/W frame | Only SSD1677 lacks this. Its 48 KB mask fits SSD1677 X4 Pro units, not a C3 |
| Separate reader and UI waveforms, ">1/8 of bytes changed" test | Only where we own two fast waveforms: X3, Murphy |
| Dark-mode black re-drive | Any differential driver, but expect a background pulse. Test first |
| AA on inverted pages | The facade change is generic. Each driver needs a LUT proven on dark pages |
| XTH4 tail fix in `Uc8279X3Luts.h` | Applies directly, our table lacks it. Unverified against stock, so test on a UC8279 X3 |
| Doses checked against the LUT (`testTextWaveforms`) | Copy for any external-LUT driver |
| Rest-frame LUT, Picco init and power policy | Panel-specific, skip |

## Murphy M3

Its own waveform, not the X4 Pro's: `MURPHY_LUT_2x_TEXT` in `Uc8253MurphyLuts.h`, chosen by
`Uc8253MurphyDriver::displayGrayFrame()` from the page and the record it keeps of the glass. No charge map: a pixel
that already shows its target rests, which adds no drive, so a page needs only the planes the driver already had plus
three of 12.5 KB.

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

**Open:** how it reads, which needs eyes; a long read; temperature. A pixel that stays black now rests every page,
so the DC item in [murphy-gray.md](murphy-gray.md) gets smaller on text pages, not larger.

## Xteink X3 (UC8253): composed, no extra memory

The X3 has no PSRAM, so its planes are composed in the two frame buffers it already has.
`FreeInkDisplay::beginOnePassMask()` turns the buffer that holds the frame on screen into the old plane
(white before and after), the reader draws the page's gray into it with one MSB pass, and
`displayOnePassFrame()` sends both planes and makes the page the frame on screen. `_activeMatchesGlass`
guards it: only a frame a display just put there counts, and after a lend or a window update the page
takes the two-pass path. `Uc8253X3Driver` runs one 24-frame bank: rest, FAST's 18 frames to white and
to black, and gray VSL 9, VSH 12, VSL 3 (lab Test 9 on an X3, see `tools/murphy-gray-lab/README.md`).

**Measured in the reader (X3, 2026-11-07):** 26 text pages one-pass, each one refresh of 445 ms (FAST
alone is 382 ms) after a gray render of about 55 ms; heap flat; the setting holds over a reboot. Image
pages, the periodic HALF and the page after a menu keep two passes, and so does fast flipping
(`aaPreempted`), as today.

**Open on the X3:**
- When a background build finishes, the reader redraws the page on screen to update its page count.
  Background-B borrowed the second buffer for that build, so the frame on screen is unknown and the
  redraw takes two passes: one second refresh per chapter build. Fixing it needs the frame on screen
  kept somewhere a borrower does not write.
- A gray pixel that a menu turns black gets only FAST's 2-frame nudge, as no plane says it was gray.
- A forced refresh override arriving after the mask was built shows that page in B/W only.
- A pixel staying black is driven 18 frames every page (no memory to tell it from gray). Measured with the gray
  lab's Tests 10 and 11 (2026-11-07): held black for up to 500 pages, it comes back white as far as one held for
  a page, and further than the full refresh's flash (1.8 points short at 500) or rest (1 point); nothing creeps back over
  the hour after. Its black is 2-4 points deeper while held. Not measured: what the DC that adds does to the
  panel over weeks. Raised upstream on Free-Ink#184.
- The same test on the Murphy M3 (2026-11-08) does show the re-drive: held black fades to 14.5 after 500 pages
  and comes back 8 points lighter than the white around it, where rest stays clean. So the X3's result is
  the X3 panel's, not a rule. The X4's SSD1677 re-drives too (VSH1 17 a page); measured on 2026-11-08 through the
  reader's `CMD:LAB`, it behaves like the X3: within half a point after 500 pages, and resting black fades there.

## Xteink X4 (SSD1677): composed, no extra memory

The X3's composed path, with the work in `Ssd1677Driver::displayOnePassStart()`: BW RAM takes the page,
RED RAM the old plane, one `0xCC` external-LUT activation, then `displayFinish()` puts the page in both
planes. The waveform is Free-Ink's `kX4ProTextSsd1677` with rows 1 and 3 exchanged: the row is
RED * 2 + BW, so ours runs black (VSH1 17), to white (VSL 24), gray (VSL 6, VSH1 4) and white that stays
white (the X4 Pro's VSL 2 hold nudge). Offered on the X4 board only, the SSD1677 it has run on.

**Measured on an X4 (2026-11-07):** 13 text pages one-pass, each one refresh of 596-600 ms, against FAST at
507 ms plus a 144 ms gray pass today; gray render 35-42 ms; heap flat; no fault. The camera showed a clean
page, but cannot judge the gray.

**Open on the X4:** as on the X3, a Background-B borrow between two turns sends the next turn down the
two-pass path (once in 10 slow turns here); fast flipping skips AA as today. The hold row nudges the
background 2 frames of VSL every page; whether rest looks better is untried, but testable: since #655 (2026-11-08)
`x4lab.py` drives frames through the reader's `CMD:LAB` (`SerialTestApi.cpp:1789`), and Test 11 ran on the X4.

## Next

1. Run a long read on the X4 Pro with the option on: ghosting over many pages, and in the cold.
2. The same on the Murphy M3 and the X3, by eye first.
3. The X4 (SSD1677) by eye, and whether its hold row should rest.
4. Run `kUc8279X3_Xth4`'s tail fix on a UC8279 X3.
