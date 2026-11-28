# BLE keyboards and page turners

Status: **measured on devices (2026-11-28); S3 path chosen, its step 1 in review (#953); C3 to decide, the single-buffer
session measured and failed.** A Bluetooth LE
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
- USB Drive works with BLE linked (X4 Pro). Not run yet: TLS or Wi-Fi beside a linked remote.

### A real remote: the Free2

Measured with the X4 Pro probe (`spike/ble-s3-probe`, `CMD:BLE:SCAN/CONNECT/RECONNECT/KEYS`) and the owner's Free2:

- **The firmware decides the radio.** As shipped (phone/tablet firmware) the Free2 is Bluetooth Classic only (keyboard
  class 0x002540, A2DP/AVRCP/Handsfree profiles) and the S3 hears nothing over LE. Reflashed over USB to the Kobo/PC
  firmware (Hanlinyue's updater) it advertises as `Free2-M` over LE with the HID service. The C3 and S3 have no Classic
  radio, so the user guide has to say so.
- Bonding (Just Works) 3.9-4.0 s, reconnect after a host restart 3.7 s. Keys arrive as an 8-byte keyboard report on
  report 1: Left Arrow (0x50) and Right Arrow (0x4F) in its left/right mode, release 60-90 ms after the press.
- **The SDK drops the link on idle.** The Free2 asks for an interval of 20-30 ms, peripheral latency 10 and a 6 s
  supervision timeout; `BleKeyboardHost` rejects every peripheral request (`onConnParamsUpdateRequest`) and keeps its own
  12-24 / 0 / 8 s. The remote naps anyway, and the link dies by supervision timeout (HCI 0x08) about 8 s into every idle;
  presses during the 3.7 s reconnect are lost. With the request accepted the link held 22 minutes and presses after
  2.5 minutes idle arrived about 1 s later. The fix (PR 2, and for Free-Ink): accept the interval and latency, keep the
  supervision timeout at 8 s or more (the SDK chose 8 s for long blocking renders, ea75c718). This is likely CrossPoint
  issue #4313 ("never reconnected inside a book").

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

### The single-buffer session, measured (scheme 2)

On the X3 (trimmed core, the controller cut down, NimBLE, the Free2 linked; `spike/ble-c3-rebuild-gate`,
`docs/ble-gate-on-rebuild-2026-11-28.md` there):

| In `alice-illustrated.epub` | free | largest |
|---|---|---|
| reader, two framebuffers, radio off | 59,692 | 55,284 |
| secondary released | 112,864 | 55,284 |
| + NimBLE host up and the Free2 linked | 58,304 | 55,284 |

At rest it fits: the radio's memory lands in the released buffer's hole. The soak (the uncached 16 MB book, the
secondary kept released, 30 turns and three chapter jumps) **fails**: the reader takes the secondary back on a book open
and retries the realloc on every render while it is gone (345 failed attempts); builds lose the secondary as their
scratch (19 parses aborted, 6 pictures dropped for want of the 32 KB inflate ring, chapters with no pages, lowest free
4,972 B); the link dropped 13 times during builds (the remote's 6 s timeout); and after the radio stopped the heap had
113 KB free in pieces (largest 40,948 B), so the secondary came back only after a reboot. To work, the scheme needs
builds and picture decodes inside the write buffer while the old page stays on the glass, a reader that knows the
session, the radio's memory in a pool of its own so the heap keeps its shape, and a supervision timeout above the
longest build stall.

## The controller's memory at boot: dropped

`bleInUse()` from a setting (memory released at boot when BLE is off, a reboot to switch it on) gives back 1,176 to
1,264 B on the X3 and leaves the largest block unchanged (`KEEP=0` against `KEEP=1`). Not worth a reboot.

## Plan (S3 first)

One concern per PR; each on X4 Pro, T5S3 and Picco (Murphy, X4 Classic, EEGO A4 build only).

1. **Radio owner** (in review: #953, `src/BleRadio`, dev envs only): starts and stops `BleHid`, polls it from the loop task, stops it for USB Drive,
   sleep and an idle time without a link, declines light sleep while up (`HalPowerManager`); a `HalCapabilities`
   predicate; no FreeRTOS task of an activity's. Host tests for its rules (CrossPoint's `RadioPolicy.h` is a model,
   with our numbers).
2. **Input** (in review): `src/BleRemoteKeys.h` turns the SDK's raw button edges into Page Forward / Page Back presses
   (arrows, Page Up/Down, Space, the consumer volume keys; the SDK's Free3 and BOOX Keypage profiles by raw code), a
   hold from the remote's own press and release times at the hardware buttons' 1 s; `BleRadio` injects each through
   `MappedInputManager::injectRawPress()` with the physical index the user's mapping gives the button, so remapping,
   press types, button actions, the reading statistics and the sleep timer all apply (CrossPoint's separate path is
   #4416's bug). With it the SDK's connection-parameter fix above (`acceptPeerConnParams`, Free-Ink branch
   `fix/ble-accept-peer-conn-params`). A per-remote swap of the two directions (the Free2's right key sends Left
   Arrow) and learned bindings come with step 3.
3. **Settings row and pairing screen**: one `SettingsList.h` row with a `SettingRequires` value; scan, pair, forget;
   the bond store the SDK keeps.
4. **Wi-Fi beside a remote** on the S3: a background KOReader sync and a File Transfer session with a remote linked.
5. **Release envs** on, after a soak with a real remote (Free3, Free2, a BOOX Keypage, a volume-key shutter).

Credit for what is ported: Tuan Q. Nguyen (@martinqnguyen) and Justin Mitchell (@itsthisjustin), CrossPoint #3785 and
#4074.
