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

**Still open.** Aware adds net drive: a light pixel returning to white takes 15 frames toward white
that nothing pays back, which only a long test or a balanced cleanup can answer. Test 5, the firmware
behind a build flag, is not built.

## Protocol

One line in, one `OK ...` or `ERR ...` line out: `HELLO`, `ARM`, `BUSY`, `REV`, `OTP <mode> <poweron>`,
`CFG <vsl vdhr vcom boost1 pll as hex>`, `LUT <slot> <84 hex digits>`, `FRAME` (then `READY`, then
both planes raw), `GO`, `W`, `K`. Planes are controller-native: 240 wide, 416 tall, MSB first.
