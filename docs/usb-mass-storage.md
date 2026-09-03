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

USB-MSC needs two things: a USB-OTG-capable MCU wired to the physical port, and
storage that exposes a 512-byte-sector block device.

| Board | MCU | USB at the connector | SD transport | USB Drive |
| --- | --- | --- | --- | --- |
| Xteink X3 / X4 | ESP32-C3 | USB Serial/JTAG only — **the C3 has no OTG peripheral** | SPI | ✗ |
| Xteink X4 Pro | ESP32-S3 | native OTG | SDMMC 1-bit | ✓ |
| LilyGo T5 S3 | ESP32-S3 | native OTG | SPI | ✓ |

The C3 is a hard no: `SOC_USB_OTG_SUPPORTED` is 0, so `USBMSC` does not even
compile there. (This repo carries a separate, currently commented-out answer for
that case — `SwitchToUsbDriveActivity`, which reboots into a dedicated MSC
firmware in the `app1` OTA partition. Unrelated to this feature and untouched.)

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
- **No dedicated USB icon.** The row reuses `UIIcon::Transfer`, which is the icon
  it replaces, so nothing changes visually. Drawing a USB trident that sits well
  next to the existing hand-drawn 32×32/24×24 icons is open if anyone wants it.

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

**Not yet validated on hardware.** Builds clean for `x4pro`, `lilygo_t5s3` and
`default` (C3, unaffected), and the 779-test host suite passes. What still needs
a device:

- X4 Pro: does the host enumerate the card, and does the port come back as a
  working serial console after the exit reboot?
- LilyGo T5 S3: the SPI raw-access path is new code and has never run. Read
  throughput over SPI-MSC is also unmeasured.
- The `AfterUSBPower` wake change — MSC boards now stay awake on a USB-powered
  cold boot instead of sleeping, so that the post-session reboot lands on a live
  console. Confirm it does not introduce a replug/battery-drain regression.
- I/O-error handling: the forced-disconnect path has no natural trigger to test
  against short of pulling the card mid-session.
