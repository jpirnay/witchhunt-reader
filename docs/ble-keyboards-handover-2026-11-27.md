# BLE keyboards and page turners: handover (2026-11-27)

Branch `chore/ble-c3-spike`, a measurement spike. **Never merge it.** It answers one question: what is the
cheapest way to take key presses from a Bluetooth LE keyboard or page turner, above all on the ESP32-C3 boards (X4,
X3), whose heap is the binding constraint. The topic's open items on master are in
`docs/future_work/ble-keyboards.md`; this file holds the measurements and the reasoning behind them.

Status: the build-time half is done and measured, for both of Espressif's controller libraries (the ROM-assisted
one the prebuilt core links, and the flash-only one swapped in at link time). The device half (heap on the X4, and
whether the swapped controller runs) has not run: the X4 was not connected. Every heap figure below that is not
labelled "measured" is a prediction from the link map.

## What is on the branch

| File | What it does |
|---|---|
| `src/BleSpike.cpp` | `CMD:BLE:<op>` over the serial test API: `CTRL` / `CTRL_OFF` (Espressif's controller alone, driven with raw HCI over VHCI: reset, event mask, a 3 s passive scan), `HOST` / `HOST_OFF` (the SDK's `BleKeyboardHost` on NimBLE-Arduino: begin, a 3 s scan, end), `KEEP=0\|1` and `MEM` (whether the next boot keeps the controller's memory). Each reply lists free and largest-free-block after every step, and the since-boot minimum. |
| `src/SerialTestApi.cpp` | Registers `BLE` in `kCommands` (dev builds only). |
| `platformio.ini` | `env:default` only: adds `BleKeyboardHost` and `h2zero/NimBLE-Arduino@2.5.1` to `lib_deps`, the two scripts below, and `custom_ble_spike_controller` (`rom` or `flash`; the branch is at `flash`). |
| `scripts/ble_spike_flags.py` | A build middleware that gives only the BLE sources (NimBLE-Arduino, `BleKeyboardHost`, `BleSpike.cpp`) `-include scripts/ble_spike_nimble_c3.h` and `-DFREEINK_CAP_BLE_HID_HOST=1`, so every other object stays cached; with `flash`, also `-DBLE_SPIKE_FLASH_CTRL=1`. |
| `scripts/ble_spike_nimble_c3.h` | Sizes NimBLE's host like upstream CrossPoint's tuned C3 core (central and observer only, 1 connection, 4 bonds, MTU 23, 4 KB host task, 12/12 msys blocks, 8 ACL buffers) without rebuilding the Arduino core. |
| `scripts/ble_spike_flash_ctrl.py` | With `flash`: the link-time swap to the flash-only controller ("Option B" below). |
| `tools/ble-spike/fetch_flash_controller.sh` | Fetches that library (not in the repository) into `.pio/ble-spike/flash-controller/` and checks its git blob id. |
| `tools/ble-spike/map_attrib.py`, `map_buckets.py` | Diff two `firmware.map` files by output section and archive member, and group the result by component. |

Build: `pio run -e default` (fresh worktree on this machine: about 80 minutes; an incremental BLE rebuild 9 to 18).
With `flash`, run `tools/ble-spike/fetch_flash_controller.sh` once first. `python tools/ble-spike/map_buckets.py
<master>/firmware.map <spike>/firmware.map` reproduces the tables below.

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
2,532 B rodata section holding 601 function pointers, the link layer's dispatch table. The `ble_*_funcs_reset`
functions that `bt.c`'s feature switches gate live in `arch_main.o` too, but the table is referenced from
`btdm_controller_init` itself (`objdump -r arch_main.o`), not from them. **No configuration and no own copy of
`bt.c` trims the controller to HID only.**

Most entries do not reach the blob at all. `esp32c3.rom.bt_funcs.ld` and `esp32c3.rom.eco3_bt_funcs.ld` bind 880
names (761 of them `r_*`) to ROM addresses with plain assignments, and a linker-script assignment wins over the blob's
own definition of the same name: in the spike's ELF, 810 of the 822 names both define resolve to ROM. Of the table's 601 entries, 488
resolve to ROM and 80 to the blob; of its 106 advertising entries, 103 resolve to ROM and 3 to the blob. What links
from the ROM-assisted blob are its patches and the functions the ROM lacks.

Linked from the ROM-assisted blob (55 of its 102 objects, 104,188 B: 72,590 text, 8,588 rodata, 23,010 IRAM). The
largest pieces a central-only HID host never uses:

| Object | text | IRAM | What |
|---|---|---|---|
| `llm_adv.o` | 12,954 | 0 | advertising, legacy and extended |
| `lld_adv.o` | 1,614 | 5,418 | advertising, link-layer driver |
| `lld_per_adv.o`, `lld_sync.o` | 234 | 398 | periodic advertising and sync |
| `lld_test.o`, `bt_rw_v9.o` | 1,058 | 212 | direct test mode |

### Option A: a hand-built patch table (ROM-assisted blob, HID only): not worth it

The idea was an own `r_ip_funcs_ro` whose advertising entries point back at the ROM originals, leaving `llm_adv.o`
and `lld_adv.o` unreferenced. The count above undoes it: 103 of the 106 advertising entries already point at ROM. The
advertising objects come in through a few blob-only functions instead: `ip_funcs.o` needs `r_ble_ll_qa_config_get`
from `lld_adv.o`, which needs `llm_ext_adv_get_own_addr` from `llm_adv.o` (the map's archive-member list). Dropping them
would mean patching the blob's own code paths, not a table, for at most the 20 KB of flash and 5.4 KB of IRAM those two
objects hold. Not tried, and not recommended.

### Option B: the flash-only blob, swapped in at link time (no core rebuild)

Espressif ships both blobs in `espressif/esp32c3-bt-lib` at 0a08c4b32f3666003080b662a1a61794da24ff0f, the commit
IDF b774170ff46 pins. Its `libbtdm_app.a` is byte-for-byte the size of the Arduino package's (1,388,286 B). Archive
totals (measured with `objdump -h`):

| | IRAM | text | rodata | data + bss + DRAM |
|---|---|---|---|---|
| `libbtdm_app.a` (ROM-assisted, what we link) | 28,622 | 259,244 | 13,738 | 1,998 |
| `libbtdm_app_flash.a` (flash only) | 1,490 | 284,328 | 15,237 | 2,170 |

The flash-only blob replaces the ROM, so much more of it links, and its IRAM drops to 1.5 KB. The prebuilt
`bt.c.obj` needs 39 symbols from the blob. The flash blob lacks exactly 9 of them, the `ble_*_funcs_reset` functions
that flash-only `bt.c` does not call. IDF's flash-only build differs from the ROM-assisted one in five places, and
`scripts/ble_spike_flash_ctrl.py` plus `src/BleSpike.cpp` stand in for each:

1. **The library** (`components/bt/controller/CMakeLists.txt`, lines 15-22): the flash blob, stored under the name
   `libbtdm_app.a` in a directory ahead of the framework's, so that `sections.ld`'s `*libbtdm_app.a` rules (from
   `[mapping:btdm]`, `linker_rw_bt_controller.lf`) still place its data and bss.
2. **The ROM scripts** (`components/esp_rom/CMakeLists.txt`, lines 122-125 and 289-291): `esp32c3.rom.bt_funcs.ld`
   and `esp32c3.rom.eco3_bt_funcs.ld` are left out. Kept, their assignments would bind 823 of the flash blob's own
   functions to ROM code.
3. **The interrupt** (`bt.c`, line 874): `-Wl,--wrap=esp_intr_alloc`. The wrapper clears `ESP_INTR_FLAG_IRAM` only
   for calls made while the spike initialises the controller (around `esp_bt_controller_init()` for `CTRL`, around
   `BleHid.begin()` for `HOST`) whose handler is outside IRAM, and counts them (`irq_iram_cleared` in both replies).
   IDF refuses an IRAM-flagged interrupt whose handler is not in IRAM, so a missing wrapper fails at init rather than
   at run time.
4. **The ROM-table resets** (`bt.c`, line 1355): nine empty `ble_*_funcs_reset` stubs.
5. **The runtime config** (`esp_bt.h`, line 386): `.run_in_flash = 1`. `BT_CONTROLLER_INIT_CONFIG_DEFAULT()` sets it
   from the option, and the macro expands in code we compile: `BleSpike.cpp` and NimBLE's two init paths
   (`NimBLEDevice.cpp:922`, `nimble_port.c:167`). So `scripts/ble_spike_nimble_c3.h` defines
   `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` for the BLE sources, and the `CTRL` reply shows `run_in_flash`.

Measured (link, 2026-11-27; the same spike with NimBLE, only the controller swapped). It links with no undefined
symbol. The map takes `libbtdm_app.a` only from `.pio/ble-spike/flash-controller`. All 698 linked names from the
dropped scripts resolve in the image, none at a ROM address (for example `r_lld_adv_end`: `0x422d6b94`, not
`0x40001014`).

| | ROM-assisted | flash-only | delta |
|---|---|---|---|
| Controller + IDF glue, flash (`map_buckets.py`) | 112,910 | 252,133 | +139,223 |
| Controller + IDF glue, IRAM | 23,458 | 1,506 | −21,952 |
| `.iram0.text` | 93,656 | 71,704 | −21,952 |
| `.dram0.dummy` (where the heap starts) | 93,696 | 72,192 | −21,504 |
| `RAM:` | 69,380 | 69,436 | +56 |
| `Flash:` | 6,394,731 | 6,535,315 | +140,584 |
| free in the app slot | 158,869 | **18,285** | |

So 243,867 B of the flash blob's 301 KB links (104,672 B of the ROM-assisted one did), and **NimBLE with the
flash-only controller fits**, with 18 KB left.
Measured against master, that build's `Flash:` is +379,610 B, `RAM:` +8,840 B and `.iram0.text` +1,514 B. The
predicted boot heap is about 10.4 KB below master (1,536 + 8,840), against about 31.8 KB for the ROM-assisted one.

The flash-only controller still uses ROM-reserved DRAM: `r_ip_funcs_p` comes from `esp32c3.rom.ld` (always linked,
in IDF's flash-only build too) at `0x3fcdff8c`. So the boot gate on `bleInUse()` stays necessary: BLE on keeps that
region.

What it does at run time: with the interrupt not in IRAM, the controller is held off during every flash erase and
write. Espressif's Kconfig help warns that BLE "may not work properly during erasing flash" and recommends flash
auto-suspend, which the prebuilt core leaves off (`CONFIG_SPI_FLASH_AUTO_SUSPEND` unset). We write flash for the font
cache (`FlashArea`) and for NVS. Whether a connected page turner survives that (its supervision timeout is usually
several seconds) is untested.

It links; whether it runs is untested. On the X4: `CMD:BLE:CTRL` should report `run_in_flash=1`,
`irq_iram_cleared` ≥ 1, `cmds_ok=5` and advertising reports; `CMD:BLE:HOST` (after a reboot) `irq_iram_cleared` ≥ 1
and devices found. A connected page turner then has to survive flash writes (a font-cache fill, a settings
save), and Wi-Fi has to work alongside it (coexistence; the spike never turns both on).

## The options on the C3, side by side

The two NimBLE rows are measured builds. The custom-host rows are derived: each takes the measured controller, the
spike's `CTRL` harness and about 5 KB of strings and relaxation, plus a host estimated at 15-20 KB. "Boot heap" is
predicted from the map, with the NVS-gated boot release and BLE off. "MMU pages" is the font cache's loss, one page per
64 KB of app growth.

| Design | Flash vs master | Free in slot | Boot heap vs master | MMU pages |
|---|---|---|---|---|
| ROM-assisted controller + NimBLE (measured) | +239,026 B | 158,869 B | about −31.8 KB | 4 |
| Flash-only controller + NimBLE (measured) | +379,610 B | 18,285 B | about −10.4 KB | 6 |
| ROM-assisted controller + custom host | about +140 KB | about 258 KB | about −25 KB | 3 |
| Flash-only controller + custom host | about +280 KB | about 118 KB | about −4 KB | 5 |

On the C3 every design pays either about 23 KB of heap at every boot (ROM-assisted) or about 140 KB more flash
(flash-only). A custom host saves about 100 KB of flash with either; with NimBLE, the flash-only build leaves only
18 KB of the slot for all future growth. The core-rebuild route below would buy that room back.

## The core-rebuild route

Everything above links the prebuilt Arduino core. A from-source core rebuild (`custom_sdkconfig`; ours is spelled
`custom_sdk_config` and inert, by decision) changes three things for BLE. The trim's option list, the check against our
code and the rebuild's measured costs are on master in `docs/future_work/font-flash.md`, "The C3 sdkconfig trim".

1. **Flash.** Upstream CrossPoint's #4212 trim compiles out ESP-IDF features we never switch on:
   - IPv6, PPP and NAT;
   - the Wi-Fi roaming assists and the hotspot's WPA3;
   - 11 of mbedTLS's 12 curves, Camellia and SHA-512.

   Upstream measured −145,440 B on its C3 release build, about the flash-only controller's +140 KB over the
   ROM-assisted one.
2. **The controller the supported way.**
   - `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` makes IDF link `libbtdm_app_flash.a`, build `bt.c` for it and leave out the
     two ROM scripts, so option B's five stand-ins go.
   - `CONFIG_SPI_FLASH_AUTO_SUSPEND=y` lets the controller's interrupt run during a flash erase, which removes the
     flash-only controller's main run-time risk. Whether the X4's and X3's flash chips support suspend is unchecked.
   - The controller's and NimBLE's sizes become sdkconfig entries, so `scripts/ble_spike_nimble_c3.h` goes.

   Upstream builds its page-turner support this way (`${ble_page_turner.custom_sdkconfig}` in its
   `[firmware_tuned]`).
3. **Heap.** Our `[firmware_tuned]` block goes live with the rebuild. It moves the Wi-Fi hot paths out of IRAM, about
   25-30 KB more heap at every boot by its comment, for slower Wi-Fi transfers. It also shrinks two timer stacks, about
   5.5 KB. Its NOTE totals 32-37 KB; that was never measured on a device. The Wi-Fi move alone is about the
   ROM-assisted controller's 23 KB of IRAM, a second way to pay for BLE.

A rough combination. Its inputs are upstream's trim figure, the block's comments and this spike's maps, never one build:

| Build | Free in the app slot | Boot heap vs master today |
|---|---|---|
| master today | 397,895 B | 0 |
| rebuild + trim + flash-only controller + NimBLE | about 160 KB | about +20 to +30 KB |
| rebuild + trim + flash-only controller + custom host | about 260 KB | about +30 to +35 KB |

The price is build infrastructure, not firmware (measured in a trial on 2026-09-10):

- builds take 28.5 to 38 minutes, on every C3 CI job;
- every S3 env re-downloads about 1 GB of framework packages unless upstream's `scripts/patch_pioarduino_cache.py` is
  ported;
- the reinstall is fragile on Windows;
- a removed component deletes its headers from the framework package every worktree shares.

Order, if taken on:

1. Port the cache patch.
2. Fix the name and take an X4 pass (heap, Wi-Fi speed, hotspot, TLS).
3. Add #4212's options and measure flash.
4. BLE on top, with `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` and auto-suspend.

## The S3 boards

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

1. ~~Link the flash blob~~: done, measured above.
2. **On the X4** (needs the device), for each controller (`custom_ble_spike_controller = rom`, then `flash`): flash
   `firmware.factory.bin` at 0x0 with esptool, copy the web bundle to the card, then read `CMD:STATE` and
   `CMD:BLE:MEM`, in this order:
   - master at Home and in a book;
   - the spike with `KEEP=1`: at Home, `CMD:BLE:CTRL`, `CMD:REBOOT`, `CMD:BLE:HOST`, `CMD:BLE:HOST_OFF`, and `HOST`
     again in a book;
   - the spike with `KEEP=0` (then `CMD:REBOOT`): at Home.

   With `flash`, also pair a page turner and fill the font cache while connected. Put master back afterwards. When
   switching controllers, `cmp` the new `firmware.bin` against the app inside `firmware.factory.bin`: a cached build
   can leave the other build's app in the factory image.
3. **Decide the C3 design** from the table above and 2: the heap the X3 can give up against the flash it can give
   up, on the prebuilt core or by the core-rebuild route. The S3 boards can take the SDK host independently.
4. Only then: design the custom host (see above) and the boot gate, if the C3 goes ahead.

## Traps met on the way

- **Overriding NimBLE's role options.** Undefining `CONFIG_BT_NIMBLE_ROLE_X` after `sdkconfig.h` also needs
  `#undef CONFIG_NIMBLE_ROLE_X`, or `nimconfig_rename.h` defines `CONFIG_BT_NIMBLE_ROLE_X` empty and every `#if` on it
  fails to parse.
- **Build middlewares chain.** One that returns `env.Object(node)` hands the next one a `NodeList`.
  `git_branch.py`'s `scope_git_defines` calls `node.srcnode()`, so an object-returning middleware must register after
  it and pass a `NodeList` through.
- **Linker-script assignments beat object definitions.** The ROM scripts' plain `r_x = 0x4000....;` lines win over a
  blob's own `r_x`, silently. Swapping a controller library without dropping its ROM scripts links a mix of flash and
  ROM code; check addresses with `nm` on the ELF (type `A` = ROM).
- **NimBLE's settings override.** NimBLE-Arduino compiles its own bundled host; `nimconfig.h` keeps every
  `CONFIG_BT_NIMBLE_*` that `sdkconfig.h` already defines, so `-D` flags do not override the prebuilt values. The
  forced include has to `#undef` and redefine after including `sdkconfig.h`.
