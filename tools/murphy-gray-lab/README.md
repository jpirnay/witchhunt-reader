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

## Photographs

Phone on a stand, same spot and same light for the whole session, frontlight off (the firmware
holds it off). Turn off HDR and lock focus and exposure if the camera allows it. Test 1 and Test 2
frames contain full white and full black, so values can be normalised within each photo.

`lab.py` prints `PHOTO <name>` when a frame is ready. Name the photos after it.

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

**Test 3: pixel scale and real text.** This is where the bench failed.

```sh
./lab.py fine --light N --dark M
./lab.py text --font <file.ttf> --size 16 --light N --dark M --mode bw
./lab.py text --font <file.ttf> --size 16 --light N --dark M --mode overlay
./lab.py text --font <file.ttf> --size 16 --light N --dark M --mode onepass
./lab.py text --font <file.ttf> --size 16 --light N --dark M --mode swapped
```

`overlay` is the X3 flow: a B/W page, then a short nudge. `onepass` resets every pixel to a rail
first, so it does not care what was there. `swapped` deliberately swaps the mid-tones, which shows
whether the bench's quilt was plane order. Use `--rotate` if the page comes out upside down.

Pass: better than `bw` at reading distance, with no quilt. If not, stop: AA stays off and the
reason goes in `docs/future_work`.

**Test 4: stability and recovery**, and **Test 5: firmware behind a build flag**, come after that.
Neither is built yet, because both depend on which of `overlay` and `onepass` wins.

## Protocol

One line in, one `OK ...` or `ERR ...` line out: `HELLO`, `ARM`, `BUSY`, `REV`, `OTP <mode> <poweron>`,
`CFG <vsl vdhr vcom boost1 pll as hex>`, `LUT <slot> <84 hex digits>`, `FRAME` (then `READY`, then
both planes raw), `GO`, `W`, `K`. Planes are controller-native: 240 wide, 416 tall, MSB first.
