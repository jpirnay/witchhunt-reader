# BLE keyboards and page turners

Status: **measured on the C3 at build time for both of Espressif's controllers, not started.** A Bluetooth LE page turner serves reading; typing into the
existing text fields is marginal, and a text editor is out of scope (`SCOPE.md`). The measurements, the reasoning and
the instructions to repeat them are in the spike's handover:
[`docs/ble-keyboards-handover-2026-11-27.md` on `chore/ble-c3-spike`](https://github.com/jpirnay/witchhunt-reader/blob/3b26614c1a46dfef4b95a6ff29d88b456af52c19/docs/ble-keyboards-handover-2026-11-27.md).
That branch is a measurement spike and is never merged. Related: [font-flash.md](font-flash.md) ("The C3 sdkconfig
trim"), which needs the same core rebuild as the supported way to the flash-only controller.

## The C3 design: decide

**Open.** Which Espressif controller library and which host the X4 and X3 would use, if any.

**Why it matters.** On the C3 the controller, not the host, sets the floor, and it costs either heap or flash:

- The prebuilt core links the ROM-assisted controller (`libbtdm_app.a`): about 113 KB of flash, and 23 KB of its code
  in IRAM. On the C3, IRAM comes out of the heap, so the X3 and X4 lose those 23 KB at every boot once the controller
  is linked, BLE on or off. Neither the linker script nor the memory release can give it back (memory protection is
  locked; `esp_bt_controller_mem_release()` frees only data).
- Espressif's flash-only controller (`libbtdm_app_flash.a`) has 1.5 KB of IRAM, but it replaces the ROM's link layer:
  243,867 B of it links (measured), against 104,672 B of the ROM-assisted one. It can be swapped in at link time
  without a core rebuild (handover, "Option B"). Its interrupt is held off during flash writes, and the prebuilt core
  leaves flash auto-suspend off.
- Configuration cannot trim the controller to HID only, and a hand-built patch table would not either: most of the
  library's dispatch table already resolves to ROM, and the advertising code comes in through the library's own code
  paths.

**What was tried.** The SDK's `BleKeyboardHost` on NimBLE-Arduino 2.5.1, sized like upstream's tuned C3 build
(`env:default`, 2026-11-27), linked with each controller; the boot heap is predicted from the map:

| Build | `Flash:` vs master | Left in the app slot | `.iram0.text` | Boot heap vs master |
|---|---|---|---|---|
| ROM-assisted controller + NimBLE | +239,026 B | 158,869 B | +23,466 B | about −31.8 KB |
| Flash-only controller + NimBLE | +379,610 B | 18,285 B | +1,514 B | about −10.4 KB |

The host's share is about 105 KB of flash (NimBLE's C host and its C++ wrapper), the only part a hand-written
keypress-only host could shrink. Derived from these builds, with a custom host estimated at 15-20 KB: about +140 KB of
flash and about 25 KB less heap with the ROM-assisted controller, about +280 KB and about 4 KB less heap with the
flash-only one.

**Next step.**
1. On the X4: the heap readings and run checks the handover lists (`CMD:BLE` on the spike, for both controllers). With
   the flash-only controller, also a page turner kept connected through flash writes, and Wi-Fi alongside it.
2. Decide the trade: heap at every boot against flash and font-cache MMU pages (the C3 sdkconfig trim would buy flash
   back).

## Any BLE build: gate the controller's memory at boot

**Open.** Arduino 3.3.11's `initArduino()` releases the BLE controller's memory at boot unless `bleInUse()` is true
(`cores/esp32/esp32-hal-misc.c`; the core's comment says about 36 KB, not yet measured). NimBLE-Arduino includes
`esp32-hal-alloc-ble-mem.h`, whose constructor makes `bleInUse()` true, so linking it keeps that memory from boot for
every user. `bleInUse()` is weak: a strong definition that reads the BLE setting from NVS (initialised just before
the call) makes BLE off cost nothing there; switching BLE on then needs a reboot. The spike's `src/BleSpike.cpp`
has this. The flash-only controller needs it too: it still uses the ROM-reserved DRAM (`r_ip_funcs_p`, bound by
`esp32c3.rom.ld`).

**Next step.** Part of whichever design lands, on every board.

## The S3 boards

**Open, not started.** The X4 Pro, T5S3, Murphy M3 and Picco have 8 MB of PSRAM, so the heap is not the binding
limit there: the SDK's `BleKeyboardHost` on NimBLE is the short path (`FREEINK_CAP_BLE_HID_HOST`, `BoardConfig.h`).
Their flash headroom was not checked.
Upstream CrossPoint puts NimBLE's host in PSRAM with a forced include (`s3_nimble_psram.h`, the #3785 series). Not
measured here.

**Next step.** Independent of the C3 decision: build it for one S3 board, measure, and try a real page turner.
