# TinyUSB device build

Ported from CrossPoint Reader #3488 (Sung-jin Brian Hong).

The S3 boards with USB Drive (`FREEINK_CAP_USB_MSC`: X4 Pro, LilyGo T5S3, Murphy M3, BOOX Picco, and the experimental
X4 Classic and EEGO A4) keep pioarduino's prebuilt ESP32-S3 core, including its PSRAM configuration, and replace only
`libarduino_tinyusb.a` with a build of their own. Their board sections in `platformio.ini` interpolate the `[usb_msc]`
fragments. The C3 envs (no USB-OTG) and the Sticky (no USB Drive) are unchanged.

The prebuilt core's TinyUSB configuration enables the USB host stack whenever device mode is on
(`#define CFG_TUH_ENABLED CFG_TUD_ENABLED` in its `tusb_config.h`), and every device class with it, so their permanent buffers
stay linked even while USB Drive is off. With Arduino 3.3.11 that component held 30,927 B of static DRAM in
upstream's X4 Pro map, and the device build 5,735 B.

Measured here (2026-11-28, master cf5992f90 against this build, `RAM:` and `Flash:` of `pio run`; heap and the largest
internal block from `CMD:STATE` at Home, three boots each, identical):

| Board | `RAM:` | `Flash:` | heap at Home | largest internal block |
|---|---|---|---|---|
| X4 Pro | −25,224 | −30,840 | 186,192 → 211,672 (+25,480) | 139,252 → 163,828 (+24,576) |
| T5S3 | −25,216 | −31,064 | 132,512 → 157,728 (+25,216) | 90,100 → 114,676 (+24,576) |
| Picco | −25,224 | −30,840 | 182,052 → 207,532 (+25,480) | 139,252 → 163,828 (+24,576) |
| Murphy, X4 Classic, EEGO A4 | −25,224 / −25,224 / −25,216 | about −30.8 KB | not run | not run |

On the X4 Pro (exFAT, SDMMC), the T5S3 (FAT32, SPI) and the Picco (FAT32, SDMMC) USB Drive behaved as on master: the
host saw the card 4-6 s after the restart, a 1 MiB file was written, read back after a remount and deleted at the
same speed as on master, and an eject brought the reader back to Home with serial and screenshots working.

`scripts/tinyusb/crosspoint_tusb_config.h` includes the core's own `tusb_config.h` and switches off the host stack
and every device class but MSC and CDC. MSC, CDC, the endpoint sizes, the alignment and the FreeRTOS settings come from
the core. The same header is selected through `CFG_TUSB_CONFIG_FILE` for the Arduino core's USB sources and for the
replacement archive. Normal serial uses the separate USB Serial/JTAG peripheral (`ARDUINO_USB_MODE=1`); the PHY is
switched to OTG only while USB Drive runs.

PlatformIO fetches TinyUSB at the commit pinned in `[usb_msc]`. `lib_ignore` keeps the library finder from compiling it;
`scripts/build_tinyusb.py` builds only the seven device sources it needs and replaces the core's `-larduino_tinyusb`
link entry. Objects and the archive live under `.pio/build/<env>/`; nothing in the shared framework package is
touched. `scripts/configure_tinyusb.py` adds the overlay's content hash to the compiler flags, because SCons does not
follow an include named by a macro: editing the overlay rebuilds both the core's and the component's objects.

`scripts/build_tinyusb.py` checks the core's `versions.txt` (TinyUSB `53f8c53c2`, lib-builder `ee57070`) and stops on
any other. When pioarduino or Arduino is upgraded, inspect the new core's TinyUSB and lib-builder revisions, update the
pin and the guard together, and compare upstream's configuration and build recipe. Do not bypass the guard, and do not
link an older TinyUSB archive against newer headers.

Every link of an affected env runs `scripts/check_tinyusb.py`: it requires the MSC and CDC symbols and rejects the host
stack's and the unused classes'. Host tests (CI runs them in the host-test job):

```sh
python3 -m unittest discover -s test/tinyusb -v
pio run -e x4pro -e lilygo_t5s3 -e murphy -e picco
```

Before a release, on an S3 board:

1. Read `heap` and `imaxblock` (`CMD:STATE`) at Home after a fresh boot.
2. Start USB Drive; check that the host mounts the card, write a file and read it back, comparing hashes.
3. Eject, leave USB Drive, and check that the reader restarts with normal serial.
4. Check SD access and serial screenshots after it has reconnected.

It does not change the IPC task stacks or the core's IRAM placement.
