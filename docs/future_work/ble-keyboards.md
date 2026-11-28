# BLE keyboards and page turners

Status: **measured on devices (2026-11-28); S3 path chosen, C3 to decide, implementation not started.** A Bluetooth LE
page turner serves reading; typing into the existing text fields is marginal, and a text editor is out of scope
(`SCOPE.md`).

The measurements behind this page are on three branches that are never merged:

- `chore/ble-c3-spike`: the C3 build-time measurement of both Espressif controllers on the prebuilt core
  (`docs/ble-keyboards-handover-2026-11-27.md` there) and the `CMD:BLE` probe (`src/BleSpike.cpp`).
- `spike/ble-c3-rebuild-gate`: the probe on the rebuilt C3 core with the flash-only controller
  (`docs/ble-gate-on-rebuild-2026-11-28.md` there).
- `spike/ble-s3-probe`: the probe on the S3 boards, on top of the TinyUSB device-only build.

The C3 figures assume the core rebuild with the #4212 trim (`docs/design/c3-core-rebuild.md`, branch
`perf/c3-sdk-trim`), the S3 figures the TinyUSB device-only build (`docs/tinyusb-build.md`, branch
`perf/s3-tinyusb-device-only`): each gives back about what BLE costs while it is off.

## The S3 boards: the SDK host on NimBLE, host in PSRAM

**Decided by the measurements.** The SDK's `BleKeyboardHost` (`freeink-sdk/libs/network/BleKeyboardHost`) on
NimBLE-Arduino 2.5.1, the prebuilt core's controller, NimBLE's host in PSRAM through CrossPoint's forced include
(`s3_nimble_psram.h`, #3785). Internal RAM free / largest internal block from `CMD:STATE` at Home:

| | X4 Pro | T5S3 | Picco |
|---|---|---|---|
| master | 186,192 / 139,252 | 132,512 / 90,100 | 182,052 / 139,252 |
| TinyUSB device-only | 211,672 / 163,828 | 157,728 / 114,676 | 207,532 / 163,828 |
| + BLE linked, radio off | 180,920 / 139,252 | 126,968 / 81,908 | 176,780 / 131,060 |
| + NimBLE host up (`HOST`) | 134,312 / 94,196 | 80,320 / 38,900 | 130,212 / 90,100 |
| after `HOST_OFF` | 180,628 / 102,388 | 126,620 / 51,188 | 176,524 / 131,060 |

- Linking costs about 30.8 KB of internal RAM at every boot (the controller's IRAM and static data); the TinyUSB build
  pays all but 5.3-5.5 KB of it. The host up costs 46.6 KB more on all three (upstream measured 46,508 B on its X4
  Pro), and gives all but 300 B back when it stops; the largest internal block does not always come back.
- Flash: +219 KB (x4pro 5,963,849 → 6,183,093 B; 370 KB left in the slot).
- The controller alone (raw HCI, `CTRL`) costs 35.8 KB on the X4 Pro; it runs, scans and stops cleanly on all three.
- USB Drive works with BLE linked (X4 Pro). Not run yet: a linked remote, TLS or Wi-Fi beside a linked remote.

## The C3 boards (X4, X3): decide

**Open, the owner's decision.** Measured on an X3, flash-only controller (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` in the
rebuild, CrossPoint's `patch_arduino_ble_controller.py`), free / largest block:

| | Home | in a book (`alice-illustrated.epub`) |
|---|---|---|
| trim build, no BLE | 79,584 / 61,428 | 72,688 / 61,428 |
| BLE linked, radio off | 69,432 / 51,188 | 62,300 / 57,332 |
| controller alone up | 39,416 / 36,852 (30.4 KB) | 32,768 / 29,684 (29.9 KB) |
| controller alone up, cut down (2 activities, small scan caches, no advertising or test mode) | 42,536 / 38,900 (27.1 KB) | 35,692 / 32,756 (26.9 KB) |
| NimBLE host up | 18,424 / 15,860 (51.3 KB) | 11,396 / 8,692 |

- The flash-only controller runs on the X3 through the rebuild: `run_in_flash=1`, every HCI command answered,
  advertising reports seen, up in 37 ms. Flash +380 KB on the trim build (+235 KB against master), 157 KB left.
- **NimBLE does not fit** beside the two-framebuffer reader: 11 KB free and an 8.7 KB largest block in a book.
- **The controller alone does not fit either, on the soak.** Controller up at Home, then
  `Through the Brazilian Wilderness.epub` opened uncached and 100 page turns, a dictionary lookup and three chapter
  jumps: no panic, the secondary buffer stayed, but 8 chapter builds stopped at under 9 KB free ("Low heap … aborting
  parse before paragraph layout") and kept partial sections of 3 pages, the lowest free since boot was 7,252 B and the
  largest block at the end 21,492 B. The bar was a lowest free of 12 KB and a largest block of 28 KB. The cut-down
  controller saves 3 KB; the analysis put the room needed at about 8 KB (a controller under 22 KB).
- One start and stop in a book can halve the largest block for the rest of the boot (30,708 B after one cycle).
- With the radio left on, a hand-written keypress-only host (scan, connect, Just Works pairing on the controller's AES,
  minimal GATT for HID notifications) would add little, but its floor is the controller's.

The options:

1. **No BLE on the C3.** The S3 boards carry it.
2. **The reader goes single-buffer while the radio is up** (+52,272 B on the X3, +48,000 B on the X4): no
   anti-aliasing and no build arena while a remote is in use, and a chapter build then needs the radio stopped.
3. **A hand-written host, with the radio up only between chapter builds**: stopped for every build, restarted after the
   page shows, at most once per boot into room kept for it. Every start costs the 37 ms plus the reconnect of the remote,
   and fragmentation after a stop is what upstream's X3 work fought for months (#2527, #3785, #4074, #4280).

## The controller's memory at boot: dropped

`bleInUse()` from a setting (memory released at boot when BLE is off, a reboot to switch it on) gives back 1,176 to
1,264 B on the X3 and leaves the largest block unchanged (`KEEP=0` against `KEEP=1`). Not worth a reboot.

## Plan (S3 first)

One concern per PR; each on X4 Pro, T5S3 and Picco (Murphy, X4 Classic, EEGO A4 build only).

1. **Radio owner** (`lib/` or `src/`): starts and stops `BleHid`, polls it from the loop task, stops it for USB Drive,
   sleep and an idle time without a link, declines light sleep while up (`HalPowerManager`); a `HalCapabilities`
   predicate; no FreeRTOS task of an activity's. Host tests for its rules (CrossPoint's `RadioPolicy.h` is a model,
   with our numbers).
2. **Input**: remote keys enter through `HalGPIO::injectPress()` with the physical index the user's mapping gives Page
   Forward / Page Back, so remapping, press types, the reading statistics and the sleep timer all apply (CrossPoint's
   separate path is #4416's bug). Default key table for the common page turners; tap and hold.
3. **Settings row and pairing screen**: one `SettingsList.h` row with a `SettingRequires` value; scan, pair, forget;
   the bond store the SDK keeps.
4. **Wi-Fi beside a remote** on the S3: a background KOReader sync and a File Transfer session with a remote linked.
5. **Release envs** on, after a soak with a real remote (Free3, Free2, a BOOX Keypage, a volume-key shutter).

Credit for what is ported: Tuan Q. Nguyen (@martinqnguyen) and Justin Mitchell (@itsthisjustin), CrossPoint #3785 and
#4074.
