# Murphy M3 gray lab

A standalone firmware and host script for finding out whether the Murphy M3 panel can show usable
gray. It replaces the reader on the device while a test runs and shares no code with it.

The Murphy firmware reports no gray today (`HalCapabilities::panelShowsGray()`). That rests on there
being no OEM gray table, not on a test of the panel. This lab is the test.

## Why it does not simply run the bench recipe

These points come from the UC8253c datasheet, the Good Display sample and the
[Murphy findings](https://github.com/crosspoint-reader/Murphy/blob/main/m3/findings/display_driver.md):

- A LUT sub-phase byte is `level[7:6] | frames[5:0]`. The bench read `0x88` and `0x48` as voltage
  codes, so its smallest step was 8 frames (160 ms at 50 Hz). The vendor's own LUT for this panel
  swings fully in 15 frames and uses 1-frame sub-phases.
- The recipe's light gray only ever pushes toward white, so it should do nothing to a white pixel.
- Its `W` recovery sent `0x00`, which selects LUT 24, and its B/W bank ends that LUT on black.
  White is `0xFF`, in production and in the vendor's clear screen.

## Rules for the one Murphy we have

The firmware refuses what breaks these (`src/LabRules.h`), and the host tests prove it:

- Nothing moves ink until `ARM`, and a reset disarms. `lab.py` sends `ARM` itself in driving steps.
- No raw command path. 0xA0, 0xA1 and 0xA3 program the OTP for good, so they do not exist here.
- Voltages stay within settings already seen on this panel family. VSH cannot be changed.
- A LUT nets at most 32 frames in one direction, and runs at most 96 frames.

Only `t1` and `t1-slots` run the recipe, which carries a net drive. `lab.py` journals those refreshes
and `undo` replays them with VSH and VSL swapped, which cancels the drive pixel by pixel. Run it
before anything else. Everything from Test 2 on nets to zero by construction.

Bench at 20-30 °C, and never leave a run unattended. **Stop** on a BUSY timeout, on an image still
visible after two `white` runs, or if white looks darker than it did before the session.

## Setup

```sh
export MURPHY_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_90:70:69:F8:0C:68-if00
ESPTOOL="$HOME/.platformio/penv/bin/python $HOME/.platformio/packages/tool-esptoolpy/esptool.py"

# Back up the whole flash first. It takes a few minutes.
$ESPTOOL --chip esp32s3 --port "$MURPHY_PORT" --baud 921600 read-flash 0 0x1000000 \
  ~/.local/state/witchhunt-device-backups/murphy-$(date +%Y%m%d-%H%M%S)-flash.bin

# Flash the lab. Same partition table as the reader, and the SD card is not touched.
cd tools/murphy-gray-lab
~/.platformio/penv/bin/pio run -t upload --upload-port "$MURPHY_PORT"
```

To put the reader back, run `pio run -e murphy -t upload` from the repo root, or write the backup
back with `$ESPTOOL ... write-flash 0 <file>`.

`bash test/run.sh` runs the host tests. They build this firmware for the PC with a simulated
controller behind the bus, so they prove what goes over the wire and nothing about the panel.

## Camera

`cam.py` drives a fixed USB webcam, with exposure, white balance and gain locked. `cam.py calibrate`
finds the panel from `lab.py marks`, so the panel can sit at any rotation. Levels are measured per
region against the same region in the `baseline-white` and `baseline-black` shots, so glare and uneven
light cancel: 0 % is black, 100 % is white, on a scale close to perceived lightness.

```sh
./cam.py sweep                  # choose an exposure, then MURPHY_EXPOSURE=<value> if not 1000
./lab.py white;  ./cam.py shot baseline-white
./lab.py black;  ./cam.py shot baseline-black
./lab.py marks;  ./cam.py shot marks;  ./cam.py calibrate
./cam.py bands <shot> --count 8 --width 200
./cam.py fine <shot>
```

A webcam at this distance gives about 2.25 camera pixels per panel pixel. That is plenty for levels
and for the mean of a fine pattern, but it cannot judge text: that needs eyes.

## The tests

`--dry-run` shows what a step would send without touching a port. Each test decides whether the next
is worth running.

**Test 0: what the panel says about itself. No refresh.**

```sh
./lab.py info
```

Reads the revision and dumps the OTP to `~/.local/state/murphy-gray-lab/`. `CHIP_REV` is fixed at
0x09, which proves the read timing before any OTP data is believed. The OTP holds the factory VCOM,
rails and waveforms for 17 temperature ranges. An SDK comment says UC8253 has no 0xA2 read while the
UC8253c datasheet lists one, so an empty dump is a possible answer, and the tests go on without it.

Result on our unit, 2026-11-04: the read works, in all three modes and with the rails on or off, and
`CHIP_REV` is 0x09. The OTP is blank apart from the chip ID at 0x14 and 0x1014 and UltraChip's trim at
0x1FF0, with no 0xA5 check code in either bank. So this module has no stored waveforms and no factory
VCOM, and the vendor's OTP-mode sample could not run on it. Every waveform on this panel comes from
the host.

**Baseline.** Before the first experimental waveform, photograph a clean `white` and a clean `black`.
They are what "white got darker" is measured against.

**Test 1: does a level depend on what was on the panel?** About 16 refreshes, 5 photos.

```sh
./lab.py t1 black            # photo
./lab.py t1 black --again    # photo
./lab.py undo
./lab.py t1 white            # photo
./lab.py t1 white --again    # photo
./lab.py undo
./lab.py t1-slots            # photo
./lab.py undo
```

Prediction: four bands from black, while the light band vanishes from white. If both starts match,
the prediction is wrong and the recipe goes straight to Test 3. In `t1-slots`, matching stripes mean
the bench's "slot asymmetry" came from each stripe's history.

**Test 2: frame-count wedge.** 4 refreshes and 1 photo per run.

```sh
./lab.py t2 white --cfg prod     # 1,2,3,4,6,8,12,16 frames of VSH on a white panel
./lab.py t2 black --cfg prod     # the same with VSL on a black panel
./lab.py t2 white --cfg oem-alt  # symmetric rails
./lab.py t2 black --cfg oem-alt
```

Pass: light lands at 55-80 % and dark at 20-45 % of the black-to-white range, within 5 points over
three runs. If the curve jumps inside one frame, the next levers are 100 and 200 Hz, then VDHR.

Result on our unit, 2026-11-04, production voltages and VCOM, all waveforms net zero:

| Frames | 1 | 2 | 3 | 4 | 6 | 8 | 12 | 16 |
|---|---|---|---|---|---|---|---|---|
| VSH from white | 81 | 54 | 31 | 15 | 4 | 1 | -1 | -1 |
| VSL from black | 0 | 13 | 35 | 58 | 80 | 88 | 94 | 98 |

Run to run within 0.7 points, and drift over 60 s at most 0.6. The recipe in Test 1 gave three levels,
not four: its light band is white, and its dark band sits at 16 %. Test 1's white start and `t1-slots`
were skipped, as they would only have added net drive. A single clean white from black reads 94-97 %
and settles at 96-99 % with or without the recipe, so that dip is normal for the production bank.

**Test 3: pixel scale and real text.** This is where the bench failed.

```sh
./lab.py fine --light 2 --dark 3
./lab.py text --font <file.ttf> --light 2 --dark 3 --mode overlay --split
./lab.py text --font <file.ttf> --light 2 --dark 3 --mode swapped --split
./lab.py text --font <file.ttf> --light 6 --dark 3 --mode onepass --split
```

`--light` and `--dark` are frame counts from the table above. In `overlay` and `swapped`, light is VSH
from white and dark is VSL from black. In `onepass` both are VSL from black. `--split` puts the same
text in both halves, four levels in one and 1-bit in the other, so they can be compared directly.

`overlay` is the X3 flow: a B/W page, then a short nudge. `onepass` resets every pixel to a rail
first, so it does not care what was there. `swapped` deliberately swaps the mid-tones, which shows
whether the bench's quilt was plane order. Use `--rotate` if the page comes out upside down.

Pass: better than `bw` at reading distance, with no quilt. If not, stop: AA stays off and the
reason goes in `docs/future_work`.

Result so far, 2026-11-04: fine patterns land where the table predicts. Measured means for 1, 2 and
4 px checkers were 79, 78, 78 for 2 frames on white (predicted 77) and 14, 16, 18 for 3 frames on
black (predicted 17.5). So gray holds at pixel scale, and the bench's quilt is not a panel limit.

**Test 9: one-pass slots from every start.** One refresh puts each of four starts (white, black, light, dark, set
up as the reader leaves them) under each one-pass slot (rest, to white, gray, to black), one band each.

```sh
./lab.py t9 --gray 3 --swing 8 --again                 # starts from a B/W page and the reader's nudge
./lab.py t9 --gray 3 --swing 8 --again --from-onepass  # starts from a one-pass frame, as in reading
./cam.py bands t9-onepass-g3-s8 --count 16              # start-major: 4 starts x 4 slots
```

Pass: the gray bands match each other whatever they started from. Run a control with every band starting white first:
the panel's own spread is not a start effect.

Result on our unit, 2026-11-07: with every band white, the same gray reads 48 at the top and 54 at the bottom, so
compare against that. Corrected for it, gray 3 with an 8-frame swing lands within about 1.3 points from white, light and
dark, and 7-10 points darker from black, once; the second frame on top agrees within about 3. A 16-frame swing halves
the black step but lands at 26 instead of 48 and takes 683 ms against 443. White from ink reads 84-90 against 89-95 for
white held, as FAST's white does. The panel's white also held through about 20 refreshes; a 5 % fall in both white and
black between runs was the room light. The reader uses gray 3, swing 8.

## The Xteink X3

The same lab drives the X3's UC8253 run (`-DLAB_BOARD_X3`, env `x3_gray_lab`, a C3). It mirrors
`Uc8253X3Driver`: its init, CDI 0x29 0x07, LUTs of 42 bytes in the X3's own layout (groups of 6, a level byte
for four phases), `0x11` after each plane, power on before the refresh and its two-phase BUSY. `clean()` uses
the X3's full bank with both planes alike, so only WW and BB fire. The read paths stay Murphy-only.

```sh
export LAB_BOARD=x3 MURPHY_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_70:AF:09:5F:8A:84-if00
cd tools/murphy-gray-lab && ~/.platformio/penv/bin/pio run -e x3_gray_lab   # then flash it to the idle slot
./lab.py t0            # which plane code selects which LUT
./lab.py t2 white; ./lab.py t2 black
./lab.py t9 --gray 3 --swing 12 --again --from-onepass
```

`cam.py` keeps the X3's calibration and shots apart (`camera-x3.json`, `shots-x3/`), as it sits elsewhere on the stand.

Results on our X3, 2026-11-07:
- The plane codes select the LUTs as on the Murphy: (1,1) 21, (0,1) 22, (1,0) 23, (0,0) 24.
- The X3's panel is far slower to black. VSH from white reads 98.5, 94.7, 86.7, 71.9, 40.0, 32.6, 8.6 and 3.6
  at 1, 2, 3, 4, 6, 8, 12 and 16 frames. VSL from black reads 9.9, 19.4, 38.4, 56.9, 76.5, 90.2, 95.2 and 97.6,
  close to the Murphy's. So the gray needs a reset of 12-16 frames, not 8.
- The gray band spread from top to bottom is about 2 points, against 6 on the Murphy.
- Test 9, FAST's drives of 18 each way: gray 3 with a 12-frame reset lands at 46 from white, 48-49 from gray,
  and 39 from black, once. A 16-frame reset lands at 41, 42 and 34, and takes 549 ms against 446 ms.
- A pixel leaving black reaches only about 86 against 94-98 for white held, 10-12 points short. That is
  FAST's own ghost: VSL 24 instead of 18 gains 1.5 points, and a second drive about 2 more.
- The room light fell during the session. Compare bands within one run only.

**Test 10: does a pixel kept black come back white?** The composed one-pass planes cannot tell a pixel that
stays black from one that turns black, so it takes the black drive (VSH 18) again on every page. `t10` holds
bands black for 1, 2, 5, 10, 20 and 50 pages in three families that differ only in what a page that keeps a
pixel black gives it: that drive again, a stay row, or rest. All go white in the same
refresh with the one-pass white (VSL 18). `undo` afterwards cancels the drive the re-driven bands took.

The stay row is `--stay fast`, the reader's FAST row for black that stays (VSH 2 on the X3, VSH 1 on the
Murphy, the default), or `--stay full`, the full refresh's flash (VSL then VSH, net zero). The X3 runs below
used `--stay full`: the "full flash" column is not what FAST does.

```sh
./lab.py t10; ./cam.py bands t10-white --count 20 --margin 6; ./cam.py bands t10-black --count 20 --margin 6
./lab.py undo
```

Results on our X3, 2026-11-07, read within minutes (bands: control, then re-drive, flash, rest for each hold):

| Held black | Re-drive | Full flash | Rest |
|---|---|---|---|
| 1 page | 104.5 | 104.5 | 104.5 |
| 5 pages | 104.6 | 104.0 | 104.0 |
| 20 pages | 104.2 | 103.3 | 103.6 |
| 50 pages | 104.3 | 103.4 | 104.0 |

- Every band held black lands within 103.3-104.6 after the white drive, whatever its family and hold; the
  never-black controls read 105.4 and 106.1. Re-driving leaves nothing behind that the other two do not.
- Re-driving deepens the black while it is held: about -1 to 0 against about 4 for one drive or rest.
- Not measured: hours or days of reading, and what the DC does to the panel over that time. Whether the
  reader's periodic HALF cancels any of it is not checked.
- The `t10-rest` shot read 6 points high at the top and 13 low at the bottom, controls included, while three
  later shots of the unchanged panel agreed within 0.2: a camera artifact, not the panel.

**Test 11: the same over 500 pages, and the hour after.** Holds of 1, 50, 200 and 500 pages, then the white
drive and a shot every 10 minutes for an hour. Not journalled (500 pages of planes make a 70 MB journal):
`t11-cancel` pays back the 18 frames of VSH each re-driven band took per page held past the first, in steps of
the firmware's 32-frame net limit.

```sh
./lab.py t11; ./cam.py bands t11-white-60 --count 14 --margin 8; ./lab.py t11-cancel
```

Results on our X3, 2026-11-07, an hour after the white drive (never-black controls 103.3 and 103.6):

| Held black | Re-drive | Full flash | Rest |
|---|---|---|---|
| 1 page | 102.0 | 101.9 | 102.1 |
| 50 pages | 102.2 | 100.8 | 101.2 |
| 200 pages | 102.2 | 100.4 | 101.1 |
| 500 pages | 102.2 | 100.2 | 101.0 |

- Re-driven black comes back as white as black held for one page, at every hold. The full refresh's flash
  leaves the most behind, 1.8 points at 500 pages, rest about 1.
- Nothing creeps back: every band, controls included, rose about a point in the first 10 minutes, then held
  within 0.2 for the hour.
- While held, re-driven and flashed black sit 2-4 points deeper than one drive or rest.
- Still not measured: what the DC does to the panel over weeks.

Results on our Murphy M3, 2026-11-08, `--stay fast` (the reader's FAST row, VSH 1), front light on
(`lab.py light on`, camera exposure 80: the glossy front mirrors the room otherwise), an hour after the
white drive (never-black controls 98.9 and 99.6):

| Held black | Re-drive (VSH 15) | FAST row (VSH 1) | Rest |
|---|---|---|---|
| 1 page | 101.1 | 101.0 | 101.1 |
| 50 pages | 102.8 | 101.2 | 100.4 |
| 200 pages | 103.0 | 101.3 | 100.6 |
| 500 pages | 109.5 | 102.4 | 100.2 |

- Unlike the X3, this panel shows the re-drive. Held black fades while it is re-driven: 6.4 after 200 pages
  and 14.5 after 500, against about -1 for rest and the FAST row. Driven white, it comes back lighter than
  the white around it: 8.4 points above black held for one page at 500 pages. The Murphy's one-pass rests
  such pixels, and should keep doing so.
- The FAST row's one frame a page leaves 1.3 points at 500 pages; rest leaves nothing.
- Nothing moves over the hour (within 0.2 between 10 and 60 minutes).
- A first run was cut off by a reboot after about 273 pages; `t11-cancel --pages 273` paid it back before
  this run. After the reboot the camera ignored its exposure until `cam.py` toggled the mode.


**Test 11 on the X4 (SSD1677)**, which has no lab firmware: `x4lab.py` drives a development build of the reader
through `CMD:LAB` (both RAM planes from rectangles, an external LUT built from the X4 Pro text table the X4's
one-pass runs). Rows: black VSH1 17 and white VSL 24 as shipped, a VSH1 2 nudge or rest as the stay row.

```sh
LAB_BOARD=x4 MURPHY_PORT=<X4 port> ./x4lab.py white|black|marks    # cam.py baselines and calibration
./x4lab.py t11 --stay nudge; ./x4lab.py t11-cancel --stay nudge
```

Results on our X4, 2026-11-08, an hour after the white drive (never-black controls 116.0 and 116.8; the
daylight rose about 8 points over the hour, so compare within a shot):

| Held black | Re-drive (VSH1 17) | Nudge (VSH1 2) | Rest |
|---|---|---|---|
| 1 page | 115.5 | 115.4 | 115.5 |
| 50 pages | 115.7 | 116.4 | 115.9 |
| 200 pages | 116.3 | 117.0 | 116.5 |
| 500 pages | 115.0 | 116.1 | 116.0 |

- Like the X3, not the Murphy: re-driven black comes back within half a point of black held for a page, and
  nothing moves over the hour.
- While held, re-driving keeps black deepest: after 500 pages 13.1 against 15.2 nudged and 27.9 resting, from
  about 7 for one drive. Black left resting fades within minutes on this panel (that band sits beside a
  white control, so a little of it is camera bleed).
**Test 4: does gray survive a real page turn?** The reader turns pages with FAST, and FAST rests any
pixel the planes call unchanged, so gray left around letters would stay. The planes are only
selector bits, so `--aware` marks each gray pixel as changed, which gives it the full drive to the
new page's rail at no cost in time.

```sh
./lab.py t4; ./cam.py shot t4-fast-plain; ./cam.py patches t4-fast-plain
./lab.py t4 --aware; ./cam.py shot t4-fast-gray-aware; ./cam.py patches t4-fast-gray-aware
./lab.py t4-pages aware --font <file.ttf> --pages 20; ./cam.py shot t4-pages; ./cam.py ghost t4-pages
```

`t4-pages` turns text pages through FAST and ends on a blank page, so anything visible is ghost. The
modes are `bw` (today), `plain` and `aware`.

Result on our unit, 2026-11-04, light 2 and dark 3:

| Gray leaving through FAST | Plain ghost | Aware ghost |
|---|---|---|
| Light to white | -38.3 | +2.4 |
| Light to black | -1.0 | +0.2 |
| Dark to white | +0.9 | 0.0 |
| Dark to black | +15.1 | -1.4 |

Blank page after 5 text pages, line-scale stddev with slow gradients removed: 0.38 noise floor, 0.56
1-bit today, 0.54 aware, 1.86 plain. Plain leaves readable text. A FAST page took 1.0 s and the nudge
0.8 s after it, so the page is readable as fast as today.

Compare shots taken close together. Room light moved these numbers by several points over minutes,
always as a smooth gradient, and `cam.py ghost` reports that as spread. Moving the camera at all means
new baselines, marks and `calibrate`.

Held for 10 minutes, gray moved at most 1.4 points against the clean strip beside it. Absolute levels
sagged by up to 5, and the clean white strip sagged with them.

**Test 5: the reader**, with `-DFREEINK_MURPHY_GRAY`.

The lab differs from the reader in one way that matters. `overlay` here keeps light pixels white and
nudges them down. GfxRenderer's B/W pass draws every antialiased pixel black, so the reader lifts both
grays from black: light is 4 frames (58 %) and dark 3 (35 %), on the VSL curve above. That also removes
most of the DC concern, because a fringe pixel goes white to black, takes a net-zero nudge, and goes
black to white on the next page, which is the ordinary cycle. Only gray that stays black takes unpaid
drive, through the gray-aware FAST.

Result on our unit, 2026-11-04, a real book at the production refresh cycle of one HALF in five:
20 pages took 20 nudges, about 0.55 s each after a 0.72 s FAST. The last page differed from a clean
redraw of the same page by 0.9 of a 105 range, with no structure. The reader menu and a cover image
work, and an idle minute refreshes nothing. No errors, and heap ended where it started.

The user judged the text better than 1-bit, and sleep covers show gray.

`./lab.py t5 cold`, `t5 warm` and `t5 warm --swap-planes` test skipping the reset before the nudge,
with the reader's slots and bytes. Rows are black, light, dark through LUT 21, dark through LUT 23:

| Handover | Light | Dark | Dark |
|---|---|---|---|
| Cold | 49-52 | 30-33 | 33-35 |
| Warm | 27-28 | 28-29 | 54-56 |
| Warm, planes swapped | 48-49 | 28-29 | 31-32 |

So a refresh leaves DTM1 and DTM2 trading roles until the next reset. The datasheet does not say this,
and it may be why the manufacturer asks for a reset before every refresh and why the bench saw its
mid-tones inverted. With the swap in the driver, the gray landed 330 ms after the page instead of
590 ms, and real pages matched a cold build within a point on row means. Levels also carry history:
black driven repeatedly lifts several points less than the Test 2 table says.

**Test 6: how much does FAST ghost, and what helps?** `./lab.py t6 <variant>` takes a patch white, black,
white through FAST beside a strip that never changes, and `./cam.py strip` reports the difference.
`--settled` draws the black with a full refresh first, which is what UI black is, and `--dwell` leaves
it up. Check `./cam.py still baseline-white <shot>` after each run: a bumped camera reads as a result.
`./ui_ghost.py <tag> --port <port>` measures the same thing on a real Settings list with the reader on
the device. The numbers are in `docs/future_work/murphy-gray.md`.

**Test 7 and 8: HALF.** `./lab.py t7 <h0|h5|h10|h15>` measures how well a HALF cleans a patch with a
history, and `./lab.py t8 <half> <fast> <name>` runs a HALF and a FAST together. Use t8 to judge any
waveform change: a better HALF can make every FAST after it worse, and t7 alone will not show that. Do
this on a blank panel, not in the reader: text sets off the webcam's autofocus, which shifts the scene.

**Still open.** Long-term DC behaviour. A book with a stored
`textAntiAliasingOverride` of 0 stays B/W, whatever the setting says.

## Protocol

One line in, one `OK ...` or `ERR ...` line out: `HELLO`, `ARM`, `BUSY`, `REV`, `OTP <mode> <poweron>`,
`CFG <vsl vdhr vcom boost1 pll as hex>`, `LUT <slot> <84 hex digits>`, `FRAME` (then `READY`, then
both planes raw), `GO`, `W`, `K`. Planes are controller-native: 240 wide, 416 tall, MSB first.
