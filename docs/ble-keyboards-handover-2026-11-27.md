# BLE keyboards and page turners: handover (2026-11-27)

Branch `chore/ble-c3-spike`, a measurement spike. **Never merge it.** It answers one question: what is the
cheapest way to take key presses from a Bluetooth LE keyboard or page turner, above all on the ESP32-C3 boards (X4,
X3), whose heap is the binding constraint. The topic's open items on master are in
`docs/future_work/ble-keyboards.md`; this file holds the measurements and the reasoning behind them.

Status: the build-time half is done and measured. The device half (heap on the X4) has not run: the X4 was not
connected. Every heap figure below that is not labelled "measured" is a prediction from the link map.

## What is on the branch

| File | What it does |
|---|---|
| `src/BleSpike.cpp` | `CMD:BLE:<op>` over the serial test API: `CTRL` / `CTRL_OFF` (Espressif's controller alone, driven with raw HCI over VHCI: reset, event mask, a 3 s passive scan), `HOST` / `HOST_OFF` (the SDK's `BleKeyboardHost` on NimBLE-Arduino: begin, a 3 s scan, end), `KEEP=0\|1` and `MEM` (whether the next boot keeps the controller's memory). Each reply lists free and largest-free-block after every step, and the since-boot minimum. |
| `src/SerialTestApi.cpp` | Registers `BLE` in `kCommands` (dev builds only). |
| `platformio.ini` | `env:default` only: adds `BleKeyboardHost` and `h2zero/NimBLE-Arduino@2.5.1` to `lib_deps`, and the pre-script below. |
| `scripts/ble_spike_flags.py` | A build middleware that gives only the BLE sources (NimBLE-Arduino, `BleKeyboardHost`, `BleSpike.cpp`) `-include scripts/ble_spike_nimble_c3.h` and `-DFREEINK_CAP_BLE_HID_HOST=1`, so every other object stays cached. |
| `scripts/ble_spike_nimble_c3.h` | Sizes NimBLE's host like upstream CrossPoint's tuned C3 core (central and observer only, 1 connection, 4 bonds, MTU 23, 4 KB host task, 12/12 msys blocks, 8 ACL buffers) without rebuilding the Arduino core. |
| `tools/ble-spike/map_attrib.py`, `map_buckets.py` | Diff two `firmware.map` files by output section and archive member, and group the result by component. |

Build: `pio run -e default` (fresh worktree on this machine: about 80 minutes; an incremental BLE rebuild about 18).
`python tools/ble-spike/map_buckets.py <master>/firmware.map <spike>/firmware.map` reproduces the tables below.

## Results: what the SDK's NimBLE host costs on the C3 (measured, build)

`env:default`, from origin/master 4739a68f5, SDK c4832207c, prebuilt Arduino core 3.3.11 (IDF v5.5.5 b774170ff46):

| | master | spike | delta |
|---|---|---|---|
| `RAM:` (static .data + .bss) | 60,596 | 69,380 | +8,784 |
| `Flash:` | 6,155,705 | 6,394,731 | +239,026 |
| free in the 6,553,600 B app slot | 397,895 | 158,869 | |
| `.iram0.text` | 70,190 | 93,656 | +23,466 |

By component (bytes; flash = IRAM + data + text + rodata, what the image stores):

| Component | IRAM | data | bss | text | rodata | flash |
|---|---|---|---|---|---|---|
| Controller + IDF glue (`libbtdm_app.a`, `libbt.a`, `libbtbb.a`) | +23,458 | +513 | +726 | +78,936 | +10,003 | +112,910 |
| NimBLE host (C) | 0 | +141 | +4,850 | +81,862 | +7,112 | +89,115 |
| NimBLE-Arduino C++ wrapper | 0 | +22 | +26 | +15,038 | +495 | +15,555 |
| SDK `BleKeyboardHost` + `HidKeymap` | 0 | +12 | +2,331 | +9,620 | +467 | +10,099 |
| Spike harness | 0 | +8 | +13 | +2,508 | +701 | +3,217 |
| Other: the merged string pool (BLE log and assert strings) | | | | | +10,136 | |
| Other: about 8 KB of small growth across unrelated objects | | | | +7,992 | | |

The string pool is the first `.str1.4` input section of `.flash.rodata` (it happens to be `Wire.cpp.o`'s), into
which the linker merges every object's string literals. The small growth elsewhere fits RISC-V call relaxation: with
about 200 KB more code in between, some calls that used the short form no longer reach their target.

### The finding that decides it: the controller's IRAM is heap

On the C3, IRAM is carved from the same SRAM as the heap (`.dram0.dummy` grew by 23,040 B to step over it). The
prebuilt controller puts 23,010 B of its own code in IRAM. That memory is gone **at every boot, whether BLE is ever
switched on or not**, as soon as the controller is linked. Any host pays it, NimBLE or a hand-written one. Predicted
boot heap for the spike with the controller's memory released at boot (`KEEP=0`): about 31.8 KB below master
(23,040 + 8,784).

Ways out that do not work, checked in source:

- Moving that code to flash by linker script alone. `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` is not a placement-only
  option. In IDF v5.5.5's `components/bt/controller/esp32c3/bt.c`, it allocates the controller interrupt without
  `ESP_INTR_FLAG_IRAM` (line 874) and skips the ROM function-table resets (line 1355). Above all it links a
  different library, `libbtdm_app_flash.a` (`components/bt/controller/CMakeLists.txt`, lines 15-22).
- Handing the IRAM to the heap while BLE is off. The prebuilt `sdkconfig.h` sets `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE`
  and `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_LOCK`. The split between the IRAM and DRAM regions is enforced and locked at
  boot.
- `esp_bt_controller_mem_release()` frees only the controller's data and bss and the ROM controller's reserved RAM,
  never its code.

### A second, avoidable cost: NimBLE keeps the controller's memory from boot

Arduino 3.3.11's `initArduino()` releases the BLE controller's memory at boot unless `bleInUse()` returns true
(`cores/esp32/esp32-hal-misc.c`, lines 349-355; the core's comment says about 36 KB, not yet measured here).
`NimBLEDevice.cpp:57` includes `esp32-hal-bt-mem.h` → `esp32-hal-alloc-ble-mem.h`, whose constructor sets
`_bleLibraryInUse`. Linking NimBLE therefore keeps that memory on every boot, for every user. `bleInUse()` is weak:
a strong `extern "C" bool bleInUse(void)` that reads a setting from NVS (NVS is initialised just before the call)
makes "BLE off" cost nothing here. Switching BLE on then takes a reboot. The spike does this (`CMD:BLE:KEEP`).

## Can the Espressif controller be made leaner, for HID only?

### Recreating it: no

The link layer needs microsecond scheduling on the C3's radio and baseband, whose registers are undocumented;
`libbtbb.a` and `libphy.a` are blobs as well. No open link layer runs on the C3 (Apache NimBLE's controller supports
other vendors' radios). Wi-Fi coexistence is in the blobs too.

### What decides what links: one constant table

`btdm_controller_init` (inside `libbtdm_app.a`, `arch_main.o`) references `r_ip_funcs_ro` (`ip_funcs.o`): one
2,532 B rodata section holding 601 function pointers, the controller's patches over the ROM link layer. It names every
patched function, advertising included, so everything it points to links whatever `bt.c`'s feature switches say.
The `ble_*_funcs_reset` functions that those switches gate live in `arch_main.o` too, but the table is referenced
from `btdm_controller_init` itself (`objdump -r arch_main.o`), not from them. **No configuration and no own copy of
`bt.c` trims the controller to HID only.**

Linked from the ROM-assisted blob (55 of its 102 objects, 104,188 B: 72,590 text, 8,588 rodata, 23,010 IRAM). The
largest pieces a central-only HID host never uses:

| Object | text | IRAM | What |
|---|---|---|---|
| `llm_adv.o` | 12,954 | 0 | advertising, legacy and extended |
| `lld_adv.o` | 1,614 | 5,418 | advertising, link-layer driver |
| `lld_per_adv.o`, `lld_sync.o` | 234 | 398 | periodic advertising and sync |
| `lld_test.o`, `bt_rw_v9.o` | 1,058 | 212 | direct test mode |

### Option A: a hand-built patch table (ROM-assisted blob, HID only)

The ROM keeps the original functions the table patches (`esp32c3.rom.*.ld`: `r_lld_adv_end = 0x40001014`, ...). An
own `r_ip_funcs_ro`, linked ahead of the archive, whose advertising entries point back at the ROM originals would
leave `llm_adv.o` and `lld_adv.o` unreferenced. Estimate: about 20 KB of flash and about 5.4 KB of IRAM (heap at every
boot). It is fragile: 601 entries to rebuild exactly, tied to one blob build (guard it by sha256, as
`scripts/iram_budget.py` guards `sections.ld`), and the ROM versions are unpatched. It still leaves about 17.5 KB of
IRAM. Not tried.

### Option B: the flash-only blob, swapped in at link time (no core rebuild)

Espressif ships both blobs in `espressif/esp32c3-bt-lib` at 0a08c4b32f3666003080b662a1a61794da24ff0f, the commit
IDF b774170ff46 pins. Its `libbtdm_app.a` is byte-for-byte the size of the Arduino package's (1,388,286 B). Archive
totals (measured with `objdump -h`):

| | IRAM | text | rodata | data + bss + DRAM |
|---|---|---|---|---|
| `libbtdm_app.a` (ROM-assisted, what we link) | 28,622 | 259,244 | 13,738 | 1,998 |
| `libbtdm_app_flash.a` (flash only) | 1,490 | 284,328 | 15,237 | 2,170 |

The flash-only blob replaces the ROM, so much more of it links (an upper bound of about 301 KB of flash, against
104 KB now), and its IRAM drops to 1.5 KB. The prebuilt `bt.c.obj` needs 39 symbols from the blob. The flash blob
lacks exactly 9 of them, the `ble_*_funcs_reset` functions that flash-only `bt.c` does not call. So the swap needs:

1. the flash blob under the name `libbtdm_app.a`, ahead of the framework's library directory, so that
   `[mapping:btdm]` (`linker_rw_bt_controller.lf`) still matches it in `sections.ld`;
2. nine empty `ble_*_funcs_reset` stubs, which is what flash-only `bt.c` amounts to (line 1355);
3. `-Wl,--wrap=esp_intr_alloc`, clearing `ESP_INTR_FLAG_IRAM` for the controller's interrupt sources, which is what
   flash-only `bt.c` does at line 874. IDF refuses an IRAM-flagged interrupt whose handler is not in IRAM, so a
   missing wrapper fails at init rather than at run time.

What it does at run time: with the interrupt not in IRAM, the controller is held off during every flash erase and
write. Espressif's Kconfig help warns that BLE "may not work properly during erasing flash" and recommends flash
auto-suspend, which the prebuilt core leaves off (`CONFIG_SPI_FLASH_AUTO_SUSPEND` unset). We write flash for the font
cache (`FlashArea`) and for NVS. Whether a connected page turner survives that (its supervision timeout is usually
several seconds) is untested.

Untested end to end. The linked size of the flash blob is the first measurement to take.

## The options on the C3, side by side

Static figures are measured or are bounds from archive sizes; the custom host's size (15-20 KB) is an estimate.
"Boot heap" is with the NVS-gated boot release, BLE off; "MMU pages" is the font cache's loss at 64 KB of app growth
per page.

| Design | Flash | Free in slot | Boot heap vs master | MMU pages |
|---|---|---|---|---|
| ROM blob + NimBLE (the spike, measured) | +239 KB | 159 KB | about −32 KB (predicted) | 4 |
| ROM blob + custom host | about +135 KB | about 260 KB | about −25 KB | 3 |
| ROM blob + custom host + option A | about +115 KB | about 280 KB | about −20 KB | 2 |
| Flash blob + custom host (option B) | ≤ about +335 KB | ≥ about 60 KB | about −5 KB | 6 |
| Flash blob + NimBLE | ≤ about +436 KB | −38 KB at the bound: no room in practice | about −11 KB | 7 |

On the C3 every design pays either about 23 KB of heap at every boot (ROM blob) or about 200 KB of flash (flash blob).
A custom host matters most with the flash blob, where it is the difference between fitting and not. Master's C3
sdkconfig trim (`docs/future_work/font-flash.md`, "The C3 sdkconfig trim", about −145 KB of flash) would make room
for the flash blob. It needs the core rebuild that would also set `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` (and auto-suspend)
the supported way.

On the S3 boards (8 MB of PSRAM) the heap is not the binding limit, so the SDK's `BleKeyboardHost` on NimBLE is the
shortest path there. Upstream puts NimBLE's host in PSRAM with a forced include (`s3_nimble_psram.h`). Not measured
here, and their flash headroom was not checked.

## A keypress-only host: what it needs

From the research for this spike (not built):

- **Transport:** H4 framing over VHCI (`esp_vhci_host_register_callback`, `esp_vhci_host_send_packet`,
  `esp_vhci_host_check_send_available`; all in the prebuilt `libbt.a`). `BleSpike.cpp`'s `CTRL` path is the start.
- **Link:** scan, connect, and the L2CAP connection-parameter update reply.
- **Security:** legacy Just Works pairing as the initiator, with the controller's `HCI_LE_Encrypt` and `HCI_LE_Rand`,
  so the host carries no AES. The bond (LTK, EDIV, Rand) is stored, and encryption is started on reconnect. LE Secure
  Connections can also run on the controller (it handles the P-256 and DHKey commands).
- **GATT, minimal:** first pairing discovers the HID service's report characteristics and writes their CCCDs. Cached
  handles serve every reconnect: HOGP requires a bonded remote to keep its CCCDs (Core spec Vol 3 Part G §3.3.3.3), so
  a reconnect is connect, encrypt, receive notifications. Stub replies cover the few ATT requests and the Security
  Request a remote may send.
- **Keys:** raw report bytes compared against learned codes, as upstream's page-turner learning does. No report-map
  parser.

No existing small host fits: BTstack's HID host is non-commercial only, ArduinoBLE cannot pair as the initiator, and
TrouBLE is Rust. The interop risk is in the remotes, not the code: some ignore the spec (no encryption, stale CCCDs),
and many cheap camera-shutter remotes are Bluetooth Classic, which the C3 cannot do. The SDK's own host uses only
legacy pairing without a passkey, and CrumBLE reports the Free3 and Free2 working.

Scope: a page turner serves reading. Typing into the existing text fields is marginal, and a text editor is out of
scope (`SCOPE.md`).

## Next steps, in order

1. **Link the flash blob** (option B; no device needed): fetch `libbtdm_app_flash.a` from `esp32c3-bt-lib` at
   0a08c4b3, add the stubs and the wrapper to the spike, build `default`, and record `Flash:`, `RAM:` and
   `.iram0.text` with `map_buckets.py`.
2. **Heap on the X4** (needs the device): flash `firmware.factory.bin` at 0x0 with esptool, copy the web bundle to the
   card, then read `CMD:STATE` and `CMD:BLE:MEM`, in this order:
   - master at Home and in a book;
   - the spike with `KEEP=1`: at Home, `CMD:BLE:CTRL`, `CMD:REBOOT`, `CMD:BLE:HOST`, `CMD:BLE:HOST_OFF`, and `HOST`
     again in a book;
   - the spike with `KEEP=0` (then `CMD:REBOOT`): at Home.

   Repeat with option B's build. Put master back afterwards.
3. **Decide the C3 design** from 1 and 2: the heap the X3 can give up against the flash it can give up. The S3 boards
   can take the SDK host independently.
4. Only then, if the C3 goes ahead with a lean blob: design the custom host (see above) and the boot gate.

## Traps met on the way

- **Overriding NimBLE's role options.** Undefining `CONFIG_BT_NIMBLE_ROLE_X` after `sdkconfig.h` also needs
  `#undef CONFIG_NIMBLE_ROLE_X`, or `nimconfig_rename.h` defines `CONFIG_BT_NIMBLE_ROLE_X` empty and every `#if` on it
  fails to parse.
- **Build middlewares chain.** One that returns `env.Object(node)` hands the next one a `NodeList`.
  `git_branch.py`'s `scope_git_defines` calls `node.srcnode()`, so an object-returning middleware must register after
  it and pass a `NodeList` through.
- **NimBLE's settings override.** NimBLE-Arduino compiles its own bundled host; `nimconfig.h` keeps every
  `CONFIG_BT_NIMBLE_*` that `sdkconfig.h` already defines, so `-D` flags do not override the prebuilt values. The
  forced include has to `#undef` and redefine after including `sdkconfig.h`.
