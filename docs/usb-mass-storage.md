# USB Drive (USB Mass Storage)

Presents the SD card to a computer as an ordinary removable disk, so books go on
and come off with the host's file manager instead of a transfer protocol. On the
boards that have it, this replaces the **USB Transfer** row in
*File Transfer* — the serial protocol has nothing left to offer once the host can
mount the card directly.

Reached from **File Transfer → USB Drive**.

## Provenance

Ported from CrossPoint Reader, where the feature is Julia's
(`julia@uxj.io`, GitHub `uxjulia`):

| Piece | Origin |
| --- | --- |
| Firmware activity, HAL seam, PHY handoff, loop suspension | crosspoint-reader **#3203** — *feat: add usb mass storage mode for x4 pro* (Julia, with Justin Mitchell and Uri Tauber) |
| `UsbMassStorage` SDK library | freeink-sdk **#36** `feat/x4-pro-usb-support`, **#53**, **#57** (Julia Nguyen) |
| `FREEINK_CAP_USB_MSC` capability flag | freeink-sdk `79a82d5` (Justin Mitchell) |
| `STR_USB_DRIVE_CONNECT_DELAY` + translations | crosspoint-reader **#3271** |

Fork-local additions are called out under [What is new here](#what-is-new-here).

## Which boards, and why

USB-MSC needs an MCU with a USB-OTG peripheral **whose native D+/D- pins reach
the physical connector**, plus storage that exposes a 512-byte-sector block
device. The second requirement is the one that catches people out.

| Board | MCU | USB at the connector | SD transport | USB Drive |
| --- | --- | --- | --- | --- |
| Xteink X3 / X4 | ESP32-C3 | USB Serial/JTAG only — **the C3 has no OTG peripheral** | SPI | ✗ |
| Xteink X4 Pro | ESP32-S3 | native OTG | SDMMC 1-bit | ✓ |
| LilyGo T5 S3 | ESP32-S3 | native OTG (GPIO19/20 → connector) | SPI | ✓ |
| Seeed reTerminal Sticky | ESP32-S3 | **CH343P USB-UART bridge** | SPI | ✗ |

The C3 is a hard no: `SOC_USB_OTG_SUPPORTED` is 0, so `USBMSC` does not even
compile there. (This repo carries a separate, currently commented-out answer for
that case — `SwitchToUsbDriveActivity`, which reboots into a dedicated MSC
firmware in the `app1` OTA partition. Unrelated to this feature and untouched.)

The **Sticky** is a hard no for a different and more interesting reason: it has
the right MCU, and now that SPI-attached cards can serve MSC its storage would
qualify too — but its USB-C never reaches the S3. Per the vendor schematic
(sheet `USB&POGONPIN&MicroSD`), the connector's only D+/D- pair goes to a
**CH343P USB-to-UART bridge**, whose TXD/RXD land on UART0; and GPIO19/20, which
would have been the native pair, are spent on the **PDM microphone**
(`PDM_CLK` / `PDM_DATA`). There is one USB connector and no pogo-pin USB path.
So the host enumerates the bridge — a fixed-function serial converter — and the
ESP32 is never a USB device at all. No firmware change can reach around that.
The SDK already knew the half of this that mattered to it: *"Sticky's on-board
WCH bridge is wired to UART0 instead of native USB CDC"* (`BoardConfig.h`).

This fork builds no Sticky env in any case, so nothing here is gated on it —
but `FREEINK_CAP_USB_MSC` carries the rule now, so enabling it there would be a
mistake someone could otherwise make. Rule of thumb: **if the board needs a
driver on the host to show up as a COM port, it cannot be a USB drive.** For
such a board the USB Transfer (serial protocol) row remains the right answer,
which is exactly what it still gets.

### The LilyGo answer

Yes, the T5 S3 supports it. The vendor's own pin map settles both questions —
[`docs/pinmap.md`](https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO/blob/main/docs/pinmap.md)
in `Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO`:

```
| USB D-       | PIN_USB_DM  | 19 | Schematic Page 1/2 | Net `DM` |
| USB D+       | PIN_USB_DP  | 20 | Schematic Page 1/2 | Net `DP` |
...
| SD SPI MISO  | PIN_SD_MISO | 21 | Schematic Page 2   | Shared SPI bus |
| SD SPI SCK   | PIN_SD_SCK  | 14 | Schematic Page 2   | Shared SPI bus |
| SD SPI MOSI  | PIN_SD_MOSI | 13 | Schematic Page 2   | Shared SPI bus |
| SD SPI CS    | PIN_SD_CS   | 12 | Schematic Page 2   | Dedicated chip select |
```

USB D-/D+ land on GPIO19/20 straight off the ESP32-S3 with no CH340/CP2102
bridge in between, so the port is real native USB — consistent with the board
already being monitored over its own USB-C (`FREEINK_LOG_TRANSPORT_USB_CDC_WRITE`).

The SD, though, is on the shared SPI bus rather than SDMMC, and that was the
actual blocker: `SDCardManager::detachFilesystemForRawAccess()` lived behind
`#if FREEINK_SD_SDMMC`, so the whole capability was SDMMC-only. See below.

## How it works

```
File Transfer ──▶ UsbDriveActivity::onEnter()
                    │
                    │  paint the instructions FIRST (requestUpdateAndWait)
                    │  mute LOG_* on the wire
                    ▼
                  HalStorage::beginUsbDrive()
                    │  SDCardManager::detachFilesystemForRawAccess()
                    │     └─ FsVolume::end()   ← volume dropped, card kept alive
                    │  UsbMassStorage::begin(blockDevice)
                    │     └─ USBMSC + USB.begin()  ← shared PHY: Serial/JTAG ▶ OTG
                    ▼
            ┌───────────────────────────────┐
            │  host owns every sector       │   main.cpp + ActivityManager
            │  firmware runs NO filesystem  │   stand down entirely
            └───────────────────────────────┘
                    │  eject / unplug / I/O error / 5-min timeout
                    ▼
                  restartToHomeAfterStorageHandoff()
                    │  handoffUsbOtgToSerialJtag()  ← PHY: OTG ▶ Serial/JTAG
                    ▼
                  ESP.restart()  → Home
```

Two properties are load-bearing:

**It always leaves by rebooting, never by transitioning.** A USB host may have
rewritten anything on the card, so every cache the firmware holds — FAT state,
covers, section caches, the open book's progress — is suspect. There is no honest
way to resume, and constructing the next activity would read a filesystem that is
not mounted. `Activity::requiresExclusiveStorageLoop()` is what enforces this:
while it is true, `ActivityManager::loop()` skips pending-action processing and
the clock tick, and `loop()` in `main.cpp` skips sleep, screenshots, shortcuts and
navigation.

**The USB personality is switched at runtime, not at build time.** The envs keep
`ARDUINO_USB_MODE=1`, so USB Serial/JTAG stays what PlatformIO monitors and
flashes; the OTG stack borrows the shared PHY only for the duration of a session
and `handoffUsbOtgToSerialJtag()` walks it back before the reboot. Without that
handoff the host is left enumerating a device the reboot destroys, and the port
comes back dead until the cable is replugged.

## Build requirements

A board opts in with `-DFREEINK_CAP_USB_MSC=1` in its own env. Two constraints
come with it:

- **The env must extend `base`, not `firmware_tuned`.** Anything that sets
  `custom_sdkconfig` / `custom_component_remove` makes pioarduino rebuild the
  Arduino core from source, and that rebuild omits the prebuilt TinyUSB component
  graph `USBMSC` links against. Both S3 envs already extend `base` for this
  reason.
- **`USE_BLOCK_DEVICE_INTERFACE` must be on**, which is what makes SdFat's
  `SdSpiCard` derive from `FsBlockDeviceInterface`. The SDK's
  `SDCardManager/inject_build_flags.py` turns it on automatically whenever
  `FREEINK_CAP_USB_MSC` is set, so no env has to remember — and boards that never
  asked for USB Drive keep the non-virtual `SdSpiCard` and pay nothing.

## What is new here

Relative to upstream #3203:

- **SPI-attached SD cards can serve USB-MSC** (freeink-sdk, this fork). Upstream
  gates raw access on `FREEINK_SD_SDMMC`, which excludes the LilyGo. No second
  driver was needed: with `USE_BLOCK_DEVICE_INTERFACE` set, SdFat's own
  `SdSpiCard` *is* an `FsBlockDeviceInterface`, so the SPI path only has to drop
  the volume while keeping the card session alive — `FsVolume::end()` rather than
  `SdFat::end()`, which would also end the card. Remounting goes back through
  `begin()`, whose `sd.begin()` re-runs `SdCard::begin()` on the same
  factory-owned card object.
- **The log wire is muted for the session.** Once the PHY belongs to OTG, the
  Serial/JTAG peripheral behind `HWCDC` is dead, and this fork's T5S3 transport
  (`FREEINK_LOG_TRANSPORT_USB_CDC_WRITE`) writes unconditionally — every log line
  would block for the TX timeout against something that can never drain. Logs
  still reach the RTC ring buffer.
- **The menu row replaces USB Transfer rather than joining it**, per the same
  reasoning as the feature request: a board that can mount its card on the host
  has no use for the serial protocol. Both `NetworkMode` enumerators still exist
  in every build; only the listing differs, and the C3 is untouched.
- **The UI is rewritten** for this fork's `GUI`/`UITheme` layer — upstream's
  version is built on the FreeInkUI `UiAppHost`/`UiScreen` widget layer, which
  this fork does not use.
- **A `UIIcon::Usb` row icon**, generated from Lucide's `usb` glyph. Boards
  without the capability keep `UIIcon::Transfer` on that row.

## The row icon

`src/components/icons/usb.h` / `usb24.h` are generated from **Lucide**'s `usb`
glyph — the same source the rest of this icon set comes from (`radio_tower`,
`library`, `bookmark`, `moon`). Lucide is already vendored as a submodule inside
the SDK, so regenerating uses the in-tree copy rather than the network:

```sh
cd scripts
python3 convert_icon.py ../freeink-sdk/libs/assets/Icons/lucide/icons/usb.svg usb   32 32
python3 convert_icon.py ../freeink-sdk/libs/assets/Icons/lucide/icons/usb.svg usb24 24 24
```

One source SVG, two canvases — the output name is what selects the header and the
array (`usb.h`/`UsbIcon`, `usb24.h`/`Usb24Icon`), matching the `book`/`book24`
pairs already in the directory. `convert_icon.py` rotates 90° on the way out,
which is why the stored bitmaps look sideways when dumped; every icon here is
stored that way to match the panel's native scan orientation.

Lucide is ISC-licensed (`freeink-sdk/libs/assets/Icons/lucide/LICENSE`,
Copyright (c) Lucide Icons and Contributors).

Cost on a board that never shows it: ~366 bytes of flash, since `iconForName()`
cannot be proven unreachable. Same arrangement as the weather icons.

## Ending the session: the S3 cannot see the cable leave

Device-tested on a LilyGo T5 S3: transfers work, but pulling the cable left the
reader sitting on the *Connected* screen forever.

The cause is not in this firmware. Arduino's TinyUSB init passes
`otg_io_conf = NULL` (`cores/esp32/esp32-hal-tinyusb.c:140`), so no VBUS line is
routed to the OTG core through the GPIO matrix, and IDF forces B-session-valid
permanently on. The core never detects session end, no `DCD_EVENT_UNPLUGGED` is
raised, and **`tud_mounted()` stays true after the cable is gone** — so
`UsbMassStorageState::Disconnected` is unreachable and the session cannot end
itself. Eject still works (that is a SCSI `START STOP UNIT`, not a bus event),
and so do the timeouts; only unplug was affected.

So `UsbDriveActivity::cableLooksGone()` watches for it directly, using whichever
signal the board can offer:

| Signal | Source | Boards | Ambiguous? | Hold before acting |
| --- | --- | --- | --- | --- |
| VBUS present | BQ25896 `REG0B` — `VBUS_STAT[7:5]` + `PG_STAT[2]` | LilyGo T5 S3 | No — a physical reading of the input rail | 750 ms (debounce only) |
| Bus suspend | `tud_suspended()` — core-detected bus idle, unaffected by the forced B-valid | any | **Yes** — a host suspending an idle bus is identical | 8 s |

Notes on the choices:

- **VBUS, not charging state.** A full battery stops charging with the cable
  still attached, so `isCharging()` reports "unplugged" while plugged in.
  `BatteryMonitor::isExternalPowerPresent()` reads the input rail instead and
  stays true at 100%. (Upstream's #3223 uses `isCharging()` for its own,
  less failure-sensitive purpose and documents the same caveat.)
- **`known` is not optional.** A board with a gauge but no charger IC — the X4
  Pro's CW2017 — genuinely cannot see the input rail, and there is deliberately
  no gauge fallback, because the BQ27220 measures the battery rather than the
  input. It reports `known = false` and the activity ignores the reading rather
  than letting a permanent `false` masquerade as a permanent unplug. The X4 Pro
  therefore leans on the suspend path.
- **Why 8 s for suspend.** Acting on a false positive means rebooting out from
  under a mounted volume and losing whatever the host had cached — the exact
  thing "eject before unplugging" exists to prevent. An unplug is permanent, so
  waiting only costs latency; being wrong costs a corrupt card.
- **Only while `Connected`.** Before a host has been seen, the host-wait timeout
  owns the exit — and a battery-powered reader sitting unplugged at the "connect
  me" screen legitimately has no VBUS.

The charger read takes `HalI2cBus::Lock`: `BatteryMonitor` talks to `Wire`
directly, and on a touch board the bus is shared across tasks (GT911 from the
input sampler, and on the LilyGo the panel's PCA9535/TPS65185 power sequence
from the render task).

## Concurrency notes

While a session is live the MSC read/write callbacks run in the TinyUSB task and
drive the card **without** `HalStorage::StorageLock`. That is safe because by then
the filesystem is gone and nothing else in the firmware touches storage — the
exclusive loop guarantees it. Bus contention is a non-issue on both boards: the
X4 Pro's card is SDMMC, and the LilyGo's display is on the parallel LCD bus, not
the SPI bus its card shares with the (unused) LoRa radio.

`HalStorage::usbDriveState()` is polled from the main loop and only reads atomics
published by those callbacks, so it cannot deadlock against them.

## Device validation

**Validated on the LilyGo T5 S3** (2026-09-03): the host mounts the card,
transfers work over the SPI raw-block-device path, and pulling the cable now
ends the session and returns the reader to Home.

Still open:

- **X4 Pro — untested.** Nothing about it has run on hardware: neither the
  SDMMC path (which is upstream's, so it is the better-trodden half) nor the
  exit, which there depends entirely on the 8 s bus-suspend fallback since the
  CW2017 gauge cannot read the input rail. Worth confirming specifically that
  the USB Serial/JTAG console comes back after the exit reboot — that is what
  `handoffUsbOtgToSerialJtag()` exists for.
- **No spurious exit while mounted and idle.** The suspend fallback is the
  ambiguous signal; a host that selective-suspends an idle bus for over 8 s
  would end the session under a mounted volume. Not observed on the LilyGo,
  which never reaches that path because its VBUS reading answers first — but the
  X4 Pro has only that path.
- **The `AfterUSBPower` wake change.** MSC boards now stay awake on a
  USB-powered cold boot instead of sleeping, so the post-session reboot lands on
  a live console. Confirm it introduces no replug loop or battery-drain
  regression.
- **I/O-error handling.** The forced-disconnect path has no natural trigger
  short of pulling the card mid-session, so it remains unexercised.
- **Throughput.** Read/write speed over SPI-MSC on the LilyGo is unmeasured.
