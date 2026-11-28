# BLE gate on the C3 core rebuild (2026-11-28)

Branch `spike/ble-c3-rebuild-gate`, never merged. `perf/c3-sdk-trim` (the rebuilt C3 core with the #4212 trim) plus the
measurement probe of `chore/ble-c3-spike` (`src/BleSpike.cpp`, `CMD:BLE`), with the flash-only controller selected the
supported way: `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` and upstream's sizes in `custom_sdkconfig`, and CrossPoint's
`scripts/patch_arduino_ble_controller.py` (#3785) linking `libbtdm_app_flash.a`. No link-time stand-ins.

## Build (`env:default`)

| | trim branch | gate | delta |
|---|---|---|---|
| `Flash:` | 6,015,995 | 6,396,223 | +380,228 (+235,472 against master); 157,377 B left in the slot |
| `RAM:` | 57,540 | 66,372 | +8,832 |
| `.iram0.text` | 70,250 | 71,762 | +1,512 |

`libbtdm_app_flash.a` links from the IDF package; `r_lld_adv_end` is at 0x422b35dc (flash), not in ROM.

## X3 (70:AF:09:5F:8A:84, app1), free / largest block

| | value | cost |
|---|---|---|
| Home after a boot, BLE linked, off, controller memory kept | 69,432 / 51,188 | −10,152 against the trim build, −1,412 against master |
| Same, memory released at boot (`KEEP=0`) | 70,608 / 51,188 | the boot gate returns 1,176-1,264 B |
| Home, controller alone up and scanning (`CTRL`) | 39,416 / 36,852 | 30.4 KB; `run_in_flash=1`, `cmds_ok=5`, 19 advertising reports in 3 s, up in 37 ms |
| Home, after `CTRL_OFF` | 69,396 / 51,188 | all back |
| Home, NimBLE host up (`HOST`, the SDK's `BleKeyboardHost`) | 18,424 / 15,860 | 51.3 KB |
| Home, after `HOST_OFF` | 64,624 / 25,588 | 4.8 KB stays allocated; the largest block halves; 5 more cycles change nothing |
| In a book (`/test/alice-illustrated.epub`, spine 3 page 2) | 62,300 / 57,332 | |
| In the book, controller alone up | 32,768 / 29,684 | 29.9 KB |
| In the book, after one controller cycle | 62,280 / 30,708 | the largest block stays at 30.7 KB, at Home too |
| In the book, NimBLE host up | 11,396 / 8,692 | |

No panic, abort or failed allocation in any of the runs. No HID remote was in pairing mode nearby (`devices=0`).

## What it decides

- The flash-only controller runs on the X3 through the rebuild. Not tested: a linked remote through flash writes
  (needs a remote), Wi-Fi alongside (the C3 never runs both).
- NimBLE beside the two-framebuffer reader leaves 11 KB free and an 8.7 KB largest block: not viable on the C3 without
  the reader going single-buffer while the radio is up.
- The controller alone costs about 30 KB, right at the analysis' threshold for a hand-written keypress-only host.
- One start/stop in a book halves the largest block for the rest of the boot: whatever host, the C3 radio must start
  at most once per boot, into room reserved for it.
- The boot gate (`bleInUse()` from a setting, reboot to enable) is worth 1.2 KB: not worth a reboot.

## The single-buffer session (2026-11-28, second analysis' scheme D), X3, a real remote

Build: this branch with the cut-down controller (`CONFIG_BT_CTRL_BLE_MAX_ACT=2`, no advertising or test mode, small
scan caches), NimBLE one connection, the link commands of `spike/ble-s3-probe` and `CMD:BLE:FB=0|S|1` (release the
secondary framebuffer / keep it released / take it back). `RAM:` 69,468 B, `Flash:` 6,403,073 B (97.7 % of the slot).
Remote: a Free2 on Kobo/PC firmware ("Free2-M"), its connection parameters accepted (`CMD:BLE:PARAMS=1`).

M1, in `alice-illustrated.epub`, free / largest block:

| | free | largest |
|---|---|---|
| reader, two framebuffers, radio off | 59,692 | 55,284 |
| secondary released | 112,864 | 55,284 |
| + NimBLE host up | 61,892 | 55,284 |
| + the Free2 bonded and linked (4.0 s) | 58,304 | 55,284 |

The radio's 51 KB lands in the released block's hole; the reader's 55 KB block stays whole.

M2, the soak book (`Through the Brazilian Wilderness.epub`, cache deleted), the secondary kept released, NimBLE up,
the Free2 linked, 30 page turns and three chapter jumps: **fails**.

- The reader fights the session: opening the book reallocates the secondary (leaving a 3,956 B largest block), and
  with it gone again the reader retries the realloc on every render (345 failed attempts), evicting caches and
  dropping Background-B each time.
- Builds have no scratch: 19 parses aborted (largest block 3-6 KB), 6 pictures dropped (`Inflate ring alloc failed:
  need 32768 contiguous bytes (scratch arena: none supplied)`), several chapters with 0 pages. Lowest free since boot
  4,972 B.
- The link: 13 drops by supervision timeout (reason 0x208, the remote's 6 s) during builds; reconnects failed at
  service discovery ("no HID service") until the builds stopped.
- After the session: radio stopped, 113,272 B free but a 40,948 B largest block; the secondary cannot come back
  until a reboot.

What it decides: releasing the secondary is not enough. Scheme D needs builds (and picture decodes) that run inside
the write buffer while the old page stays on the glass, a reader that knows the session (no realloc retries), the
radio's memory kept out of the general heap (a pool in the released block, so the heap keeps its shape), and a link
supervision timeout above the longest build stall (the SDK's own 8 s, not the remote's 6 s).

## E1: the CPU clock, not the builds, broke the link (2026-11-28)

The third analysis found that the C3 drops to 10 MHz in every refresh's waveform wait and after 500 ms idle
(`HalPowerManager`), refused only while Wi-Fi is up, and that this probe never held the clock while the host ran
(CrossPoint holds it, `upstream/develop src/main.cpp:310-314`; #953 does on the S3). `CMD:BLE:CLOCK=0|1` holds a
`HalPowerManager::Lock` while the host runs. On Settings with the secondary released (NimBLE up: 43,060 free /
30,708 largest), the Free2 linked, 40 refreshes (`BTN:DOWN`/`UP`, 2.5 s apart), the owner pressing 10 times:

| | link | presses delivered |
|---|---|---|
| clock held | up for all 110 s, no drop | 13 of 13 (a wake press, 10, a double press) |
| clock not held | dropped 31 s after the hold went; 6 reconnects failed at service discovery ("No HID input report", "Not a HID device") | 0 of 10 |

With the hold back on, the link came back by itself within seconds. So M2's 13 drops and its "no HID service"
reconnects were the clock; the soak's heap failures (aborted parses, dropped pictures, the secondary not coming back)
stand on their own.

Also seen: `BleHid.begin()` on Settings with both framebuffers (44,024 free / 30,708 largest) hung the loop task inside
NimBLE's init, with no reply and no log, until a reset. A C3 start needs a heap check before `begin()`, as #953 has on the S3.

## E2: the radio's memory in a pool inside the lent secondary framebuffer (2026-11-28)

`CMD:BLE:POOL=1` borrows the secondary framebuffer (52,272 B, never in the heap) and registers it as a private
`multi_heap`; the controller's OSI allocator (`btdm_osi_funcs_register`, its `_malloc`/`_malloc_internal`/`_free`
patched in the heap copy bt.c passes) and NimBLE-Arduino's `nimble_platform_mem_malloc/calloc/free` (`--wrap`, all
22 call sites) are served from it while it captures; frees go back to the block's own heap. `POOL=0` returns the
buffer only when the pool is empty. Clock held, the Free2 linked, `alice-illustrated.epub`, five sessions of 30 page
turns, the radio stopped and the buffer returned after each:

| every one of the 5 sessions | |
|---|---|
| link after a restart | 3,740-3,811 ms |
| pool used (peak) | 34,216 B (34,272), 91 allocations, 0 failed, 0 routed to the heap |
| general heap while the session runs, free / largest | ~39,700 / 36,852 (before: 59,768 / 51,188) |
| radio memory outside the pool (stacks, C++ objects, queues) | ~20.0 KB |
| pool after the stop | empty |
| secondary returned, largest block after | yes, 51,188 (unchanged) |

About 500 B stayed out of the general heap after the first session, nothing after the later ones; presses arrived in
every session. Against Fable's bar (pool < 44 KB, < 22 KB outside, pool empty after every stop, largest block
unchanged): passed. What it does not answer yet: a chapter build during a session (39.7 KB free, no secondary as
scratch): the radio has to yield the block (stop, return, build, borrow, restart: 3.8 s of a deaf remote per build).
The X4 (48,000 B block, its fast refresh needs the secondary) is not measured.
