# The C3 core rebuild and its trim — design

> **Status.** Shipped for every ESP32-C3 env (X4 and X3: `default`, `gh_release`, `gh_release_rc`, `slim`, the
> `bench*` envs). The S3 envs keep pioarduino's prebuilt core. Config: `[firmware_tuned]` in `platformio.ini`.
> Scripts: `scripts/patch_pioarduino_cache.py` (the per-chip cache), `scripts/iram_budget.py` and
> `tools/iram-budget/` (the linker script generated against the rebuilt libraries).

## What it is

pioarduino links the Arduino core's ESP-IDF libraries prebuilt, compiled with Espressif's default configuration. An
env with `custom_sdkconfig` makes it rebuild those libraries from source with the options given there
(`builder/frameworks/arduino.py` in platform 55.03.311: only `custom_sdkconfig` sets the rebuild flag;
`custom_component_remove` applies only inside the rebuild). Until this change `[firmware_tuned]` spelled the option
`custom_sdk_config`, so every C3 build linked the prebuilt core and none of the block's options took effect.

The options, grouped (`platformio.ini` has the reason for each):

- **Task stacks** (ours, July 2026): esp_timer 8,192 → 4,096 B, the FreeRTOS timer service 4,096 → 2,560 B.
- **Network features the firmware never turns on** (CrossPoint #4212, Arthur Tazhitdinov): IPv6, PPP, IPv4 NAT and
  forwarding, Wi-Fi FTM, 802.11k/v/r and MBO, the hotspot's WPA3 and SAE-PK, mDNS browsing, protocomm security 1 and 2.
- **Ours on top of #4212**: enterprise (EAP) Wi-Fi and the GCMP cipher. No screen asks for an EAP identity, and
  Arduino's WiFi library compiles its EAP calls only with the option set.
- **mbedTLS** (#4212): Camellia, every curve but P-256, SHA-512. mbedTLS serves only Wi-Fi crypto; TLS is wolfSSL.
- **Arduino wrappers the firmware never includes** (CrossPoint #3857, Uri Tauber), and the unused components
  (cloud, Zigbee, Modbus, modem) removed from the rebuild.

Kept on purpose: WPA3-SAE, H2E and OWE for joining networks; P-256 and ECDSA; `GCM_SUPPORT_NON_AES_CIPHER` (ARIA is
on, and without it ARIA-GCM silently computes AES-GCM); the `network_provisioning` component (Arduino's WiFi library
includes its headers). Bluetooth stays as the prebuilt core has it (`CONFIG_BT_ENABLED=y`, nothing linked), so the
BLE work decides it (`docs/future_work/ble-keyboards.md`).

Every option was checked in the generated `framework-arduinoespressif32-libs/esp32c3/sdkconfig` against the prebuilt
one: the values that differ are the options above, their dependents (`WPA_*`, `PPP_SUPPORT`, the removed components'
own keys) and the flash mode and size, which now follow the board (DIO, 16 MB) instead of the lib builder's QIO, 4 MB.

## Measured

Builds, 2026-11-27, master cf5992f90 against this change, same worktree:

| | `default` master | `default` rebuilt | delta | `gh_release` delta |
|---|---|---|---|---|
| `Flash:` | 6,160,751 | 6,015,995 | **−144,756** | −144,976 (6,024,479 → 5,879,503) |
| `RAM:` (static) | 60,636 | 57,540 | −3,096 | −3,112 |
| `.iram0.text` | 70,190 | 70,250 | +60 | |
| `.dram0.bss` | 42,016 | 38,960 | −3,056 | |

Master built in a second worktree with this change's PlatformIO core (6.1.19) came to 6,160,951 B (`default`) and
6,024,703 B (`gh_release`): −144,956 and −145,200 B against it. Between worktrees, the library folders' hashed names
change the link order, and with it a few hundred bytes of call relaxation; the compile commands are otherwise identical.
The S3 envs' compile commands are identical to master's, and the T5S3 image is byte-identical but for its timestamps.

The flash comes out of `liblwip.a` (−53 KB of text), `libwpa_supplicant.a` (−30 KB), `libmbedcrypto.a` (−28 KB with
rodata), `libnet80211.a` (−11 KB), mDNS (−7 KB) and the merged string pool (−5 KB). At one font-cache MMU page per
64 KB of app, that is about two pages back on the C3.

X3 (70:AF:09:5F:8A:84), both builds in app1, free / largest block from `CMD:STATE` and the session-end lines:

| | master | rebuilt | delta |
|---|---|---|---|
| Home after a boot (three boots, identical) | 70,844 / 51,188 | 79,584 / 61,428 | +8,740 / +10,240 |
| `/test/alice-illustrated.epub`, spine 3 page 2 | 63,828 / 57,332 | 72,688 / 61,428 | +8,860 / +4,096 |
| KOReader Sync, before the TLS handshake | 78,628 / 69,620 | 89,256 / 61,428 | +10,628 / −8,192 |
| KOReader Sync, lowest free during the handshake | 66,828 | 77,556 | +10,728 |
| File Transfer on the home network (`/api/status`) | 109,624 / 69,620 | 120,108 / 114,676 | +10,484 / +45,056 |
| Hotspot (`/api/status`) | 104,204 / 98,292 | 114,064 / 106,484 | +9,860 / +8,192 |

So about +9 KB of heap at every boot: 3.1 KB of static RAM and 5.6 KB of the two stacks. The largest block before a
TLS handshake came out 8 KB smaller, still far above the 26,624 B the handshake asks for; elsewhere it grew.

Run on the rebuilt build, all passed: Wi-Fi join (WPA2, 3.0 s), `/api/status` by IP and by `crosspoint.local`, a
1 MiB upload and download over HTTP twice (byte-identical; 215 against 216 KB/s up, 663-676 against 684-688 KB/s
down), KOReader Sync authentication (TLS 1.3, P-256), NTP via Clock settings, the hotspot joined from a Linux host
with a 1 MiB round trip (140 against 142 KB/s up, 231 against 248 down, one run each). The X4 did not run it.

## What it does not do

- **The Wi-Fi IRAM options were already off.** The prebuilt C3 core has `CONFIG_ESP_WIFI_IRAM_OPT` and
  `CONFIG_ESP_WIFI_RX_IRAM_OPT` unset, so the "25-30 KB" that `[firmware_tuned]` used to promise for them was in every
  build already. The block's old total of 32-37 KB was never measured; +9 KB is.
- **ROM libc** (`CONFIG_LIBC_OPTIMIZED_MISALIGNED_ACCESS=n`, CrossPoint #3397 with its link fix-up
  `patch_arduino_rom_libc.py`): measured at 840 B of IRAM. Left out: it swaps the `memcpy`/`memmove`/`memcmp`/`str*`
  family for ROM versions that are slower on misaligned data, in our glyph and framebuffer paths, unbenchmarked.
- **The old `sdkconfig.defaults`.** The repository tracked one (2026-04 to 06: interrupt watchdog 10 s, mbedTLS TLS
  buffers, a 2,440 B TCP send buffer, a 16-entry TCP/IP mailbox). With the prebuilt core it was never read; the
  rebuild overwrites it with its generated file. It is untracked now (`.gitignore` has `/sdkconfig.*`) and its
  settings are not carried over: each would change behaviour that has run unchanged since.
- **The S3 boards.** Their envs extend `base`: the rebuild omits the prebuilt TinyUSB component graph that USB Drive
  links against (`docs/usb-mass-storage.md`), and every S3 board we ship has USB Drive. Their images are unchanged.

## Working with it

- **First build of an option set** rebuilds the core: under a minute on 32 cores with the IDF objects already in the
  build cache (2026-11-27), 28-38 min in the 2026-09-10 trial. A cold CI runner pays it once per cache key. `scripts/patch_pioarduino_cache.py` (CrossPoint #3242, #4214) keeps the
  result per chip: it patches the platform's `arduino.py` so that "a custom core is installed" is decided per chip
  (`<chip>/sdkconfig.orig`), and records each chip's request in `<chip>/sdkconfig.crosspoint`. An S3 build then
  leaves the C3 core alone. The CI cache keys hash the script.
- **A change of options changes the core's `sections.ld`.** `scripts/iram_budget.py` compares its sha256 with the
  one `ld/esp32c3-iram-budget/sections.ld` was generated from and stops the build. Regenerate with
  `tools/iram-budget/gen_sections_ld.py` (its docstring), then build again. Its `--check` reproduces the rebuilt core's
  file byte for byte.
- **A worktree on an older branch** (no `custom_sdkconfig` for the C3) that builds a C3 env next makes pioarduino
  reinstall the whole framework, about 1 GB, and the next build of this branch rebuilds the core. That is pioarduino's
  own reinstall path. To work on both without that, give one of them its own core directory
  (`PLATFORMIO_CORE_DIR`) with its own copy of the platform and the three framework packages.
- **The penv's PlatformIO core must be 6.1.19.** The rebuild runs a nested `pio run` from `~/.platformio/penv`; with
  pioarduino 6.2.0 there it fails at once with `No module named 'SCons.Tool.FortranCommon'` (the two cores fight over
  `tool-scons`). CI pins it (`ci.yml`, "Pin pioarduino core inside the platform penv").
- **An interrupted rebuild** can leave `.dummy/`, `CMakeLists.txt`, `sdkconfig.default`, `sdkconfig.defaults`,
  `managed_components/` and `dependencies.lock` in the project; delete them and `.pio/build/<env>`.
- **The fast C3 bootloader** (`bootloader/esp32c3/`) stays valid: the rebuild keeps ESP-IDF 5.5.5, which is all
  `scripts/c3_bootloader.py` requires, and changes nothing the bootloader shares with the app.
- **`lib_ignore = BLE`** makes pioarduino drop the IDF `bt` component from the rebuild
  (`[ESP-IDF] Ignoring components based on lib_ignore: bt`). A BLE build has to lift that for the C3.
