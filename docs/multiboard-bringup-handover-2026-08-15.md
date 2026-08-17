# Multi-board bring-up — handover, 2026-08-15

*State (updated 2026-08-17): branch `fix/s3-build-config`, pushed. `master` (2.23)
merged in; all three envs build. Workstream **A done**, **C phases 1-3 done**,
**B0 done**, **B pin work done and hardware-validated on both C3 boards**.
**The LilyGo T5S3 BOOTS AND RUNS** — first S3 board in a working state.
X4 Pro reached the home-screen activity, surfaced three real bugs, then became
unflashable. See "Device validation".*

Targets: **Xteink X4 Pro** (lead) and **LilyGo T5S3**, both ESP32-S3 (Xtensa).
Shipped product is X3/X4 (ESP32-C3, RISC-V) and must not regress.

Plan: [multi-board-bringup-2026-08-14.md](multi-board-bringup-2026-08-14.md).
Touch (workstream C): [touch-input-migration-2026-08-14.md](touch-input-migration-2026-08-14.md).

Everything below is build-measured unless it says otherwise. **X4 and X3 (both
C3) pass**; **T5S3 boots and runs**; **X4 Pro** reached the home-screen activity
before the bugs below and is currently unflashable.

---

## Landed

**Workstream A — build (2026-08-15):**

```
59189129  docs: bring the bring-up plan in line with what shipped
8803b7a0  chore: bump freeink-sdk 56efd2e -> 76e61c4
02c324cc  fix: split arch-specific HAL code so the S3 boards link
b4b94068  fix: split build flags per MCU family so S3 envs build
f666334b  docs: plan multi-board bring-up for X4 Pro and LilyGo T5S3
```

**2026-08-16 — master merged, then workstreams C and B:**

```
d0867add  Merge branch 'master' into fix/s3-build-config   (2.23, SDK -> cc89c653)

           workstream C — touch (see the touch doc for detail)
cfd00bf8  feat(hal): HalGPIO touch passthrough                       phase 1
4ab2c188  feat(gfx): panel-native -> logical touch transform + tests  phase 2
7d59e493  feat(input): MappedInputManager touch layer                 phase 2
82be8458  fix(hal): I2C bus mutex + sampler stack 2048->4096          P1
6990c4b1  test(input): rowTouch/colTouch band arithmetic              phase 2
968a8493  feat(reader): touch reading controls + capability settings  phase 3
013f343c  feat(reader): wire page turns, menu gesture, idle timer     phase 3

           workstream B — board de-hardcoding
b5341302  feat(hal): board capability predicates                      B0 step 1
2799b34f  refactor(hal): route the safe deviceIsX3() sites            B0 step 2
1c3a43ea  refactor(hal): SPI/battery/gauge-I2C pins from the profile  B
748a7dd1  refactor(hal): display pins from the profile                 B
6f4e640f  refactor(hal): RTC addr / USB detect / battery ADC pin       B

           found by flashing an X4 Pro — all three are S3-only failures
bf8b25b3  fix(activity): real mux for the cross-task critical sections
edf203e3  fix(activity): pin the render task to a core on dual-core
2756237b  fix(hal): only take the SPI lock when SD is on that bus
3b394eff  refactor(hal): HalI2cBus owns I2C bus start-up

           2026-08-17 — LilyGo T5S3 bring-up, all found on hardware
138f47d6  fix(hal): SPI bus pins from whichever peripheral defines them
26c47e57  fix(build): PSRAM + quad flash for the T5S3        <-- the big one
7c9f6623  fix(log): serial logging on native-USB-CDC boards  <-- unblocked all
7a420191  fix(hal): board-support layer owns bus bring-up
0a0851eb  fix(power): no CPU frequency scaling on PSRAM boards
552d48f4  feat(hal): raw button trace for board bring-up
17df518a  fix(power): don't mistake a hardware reset for a power wake
```

**b4b94068 — workstream A.** `[base]` carried the C3 device set
(`FREEINK_DEVICE_X4/X3`), the X4 SPI overclock, and `WOLFSSL_SP_RISCV32`; every
env inherits `${base.build_flags}`, so no S3 env could build. Moved into new
`[c3]` / `[s3]` interpolation sections; each env now opts into exactly one MCU
family. Added `[env:x4pro]`.

**02c324cc — the two B link blockers.** Both C3/RISC-V assumptions in `lib/hal`:

- `HalPowerManager.cpp` — deep-sleep wakeup. Now branches on **SoC capability**
  (`SOC_PM_SUPPORT_EXT1_WAKEUP` vs `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`), not chip
  name, with an `#error` default.
- `HalSystem.cpp` — `__wrap_panic_print_backtrace` walked a RISC-V `RvExcFrame`.
  Only the SP extraction is arch-specific; Xtensa keeps it in `XtExcFrame::a1`.
  Gated on `__riscv` / `__XTENSA__`.

**8803b7a0 — SDK bump** `56efd2e` → `76e61c4` (submodule pointer), for SDK PR #39
so `FREEINK_FRONTLIGHT_LS` is available later.

---

## Verified state

| Env | Result | RAM | Flash |
|---|---|---|---|
| `default` (C3) | **SUCCESS** 786 s | 56,476 | 6,376,923 — **97.3 %** |
| `x4pro` | **SUCCESS** 766 s | 66,408 | 6,198,074 — **94.6 %** |
| `lilygo_t5s3` | **NOT BUILT SINCE THE FIX** | — | — |

*(Historical, as measured on 08-15. Superseded by the two tables below.)*

X4 Pro links — the first S3 build ever to do so in this repo. It would **not
boot**: none of the pin/peripheral de-hardcoding is done (see next section).

### Re-measured 2026-08-16, after merging `master` into this branch

`master` (2.23) merged in cleanly — the only overlapping file was
`platformio.ini`, and only its `version =` line. The submodule bump to
`cc89c653` came with it.

| Env | Result | RAM | Flash |
|---|---|---|---|
| `default` (C3) | **SUCCESS** | 56,692 | 6,349,593 — **96.9 %** |
| `x4pro` | **SUCCESS** | 66,624 | 6,172,882 — **94.2 %** |
| `lilygo_t5s3` | **SUCCESS** | 66,628 | 6,174,219 — **94.2 %** |

**All three envs now link**, including LilyGo T5S3 for the first time since
workstream A. C3 flash **improved to ~204 KB free** (from 177 KB) — master's
font regeneration under `7897f54f`/`3934b192` more than paid for the merge.
C3 RAM +216 B, from master's power-button wake work.

**C3 delta: +94 bytes flash, RAM unchanged.** Attributed to the SDK bump, not the
HAL fix — SDK #39 adds 14 lines to the shared
`libs/display/FreeInkDisplay/src/bus/EpdBus.cpp`, which the C3 links, while both
HAL changes compile the identical C3 arm. **This is inference from the diff, not
an A/B measurement.** If it ever matters, build `02c324cc` and `8803b7a0`
separately.

**Flash is the binding constraint, not RAM.** ~177 KB free on C3, ~355 KB on
x4pro (the S3 carries one board profile; the C3 links both SSD1677 and UC8253).
Any *shared* addition is charged against the C3's 177 KB.

### After workstreams C and B (2026-08-16, final)

| Env | Result | RAM | Flash |
|---|---|---|---|
| `default` (C3) | **SUCCESS** | 56,700 | 6,350,011 — **96.9 %** |
| `x4pro` | **SUCCESS** | 66,656 | 6,174,590 — **94.2 %** |

Cumulative C3 cost of everything on 2026-08-16 (touch phases 1-3 + B0 + B) is
**+418 bytes flash and +8 bytes RAM** over the post-merge baseline — most of it
the touch call sites, which compile in even though every touch predicate folds to
false on the C3. ~199 KB flash still free.

670 host tests pass (`cd test/build_test && ctest -j4`), including the 15 new
`test/touch_transform` cases.

---

## Device validation

### ✅ X4 (C3) — flashed 2026-08-16 on `1c3a43ea`, PASSED

| Check | Result | What it validates |
|---|---|---|
| Boots, screen visible | ✅ | `SPI.begin()` pins sourced from `BoardConfig::ACTIVE`; also the `main.cpp` global reorder (`GfxRenderer` now constructs before `MappedInputManager`, inside the TU `BootHeapProbe` slots 4/5 bracket) |
| Navigate to a book and read it | ✅ | SD mount over the same SPI bus — `sd.miso` is the one pin that comes from a different profile struct than the rest |
| System Info shows sane battery | ✅ | `pinMode(batteryAdc)` now gated on `hasAdcBattery()` instead of `deviceIsX4()` |
| No ghosting | ✅ | `panelNeedsHalfRefreshSettle()` correctly resolves false on SSD1677 |

**This validates the pin-sourcing approach itself**, not just these four commits:
reading pins and capabilities out of the board profile produces a working device
on the shipped hardware. The rest of workstream B can proceed on that basis.
Combined with the X3 results below, **B0 is fully validated — every converted
predicate has been exercised on both its true and false arm.**

Transitively also exercised: the `master` 2.23 merge, touch phases 1–3 (inert on
C3 but compiled in), and `HalI2cBus` (a no-op `Lock` there, which the
byte-identical build already implied).

**Also confirmed on device:** Settings → Controls shows only the physical button
entries — **no tilt entries** — which is exactly what `SettingRequires::TiltSensor`
should produce on a board with no IMU. That is the `SettingRequires` refactor
(`968a8493`) verified on hardware, not just predicted. The Fast AA toggle lives
under Reader → Font, not Controls, and rides the same mechanism, so it is treated
as low-risk rather than separately checked.

**Still unverified on X4: USB electrical detect.** The `usbDetect` pinMode moved
from a `deviceIsX4()` test to a gauge-bus pin-conflict test, and nothing has
exercised the GPIO20 read.

Two things make this awkward to test, both worth knowing before trying:

1. **On X4 the serial log goes over the same USB.** Unplugging to test detection
   drops the connection you would be watching it on. The observable is visual —
   the lightning bolt in the battery icon, drawn from `gpio.isUsbConnected()` in
   `BaseTheme`/`LyraTheme` — not a log line. There is no serial logging of USB
   state transitions at all.
2. **A data cable makes the test inconclusive.** Detection is
   `usbSofActive || usbElectricalConnected`: the SOF (USB-serial-JTAG frame)
   path alone reports connected, so the bolt appears whether or not the GPIO20
   read works.

**The isolating test is a charge-only cable** (power, no data): SOF stays
inactive, so the bolt depends entirely on the converted GPIO path.

Attempted 2026-08-16 with the normal data cable: the charging indicator **did**
appear, so nothing is broken end to end — but the serial capture was live at the
time, meaning USB frames were flowing and `usbSofActive` was true by
construction. Inconclusive for the GPIO path, so this stays open.

### Sampler stack — measured baseline for P1

From the same session:

```
[MEM] btnSampler stack high-water=1516 bytes free (min ever)
```

The C3 sampler is 2048 bytes (the bump to 4096 is `FREEINK_CAP_TOUCH`-gated, so
the C3 is unaffected), which means a **measured peak of ~532 bytes** on the
button-only path. Note the code comment in `startInputSampler()` cites ~380 bytes
from an older measurement — the real figure is ~40 % higher, so the "4x headroom"
claim there is really closer to 3.8x.

This is the number to compare against when `samplerStackHighWater()` is finally
read on an X4 Pro: whatever `serviceTouch()`'s GT911 I2C path costs, it stacks on
top of ~532 bytes, and 4096 was chosen because it is what the SDK sizes its own
`update()`-calling task at. If the touch-board high-water lands anywhere near
2048 free, 4096 was the right call; if it barely moves, the bump could be
revisited.

### ✅ X3 (C3) — flashed 2026-08-16, PASSED

X3 is the board where every converted predicate returns **true**, so between the
two C3 boards **both arms of all four are now verified on hardware**:

| Check | Result | What it validates |
|---|---|---|
| `[CLK] DS3231 Hardware via I2C found.` | ✅ | `rtcType() == Ds3231` true arm (X4 logged "board has no DS3231") |
| `[CLK] Got time from DS3231. Last deep sleep 0s` | ✅ | **the strongest single result** — see below |
| Tilt entries present in Settings → Controls | ✅ | `SettingRequires::TiltSensor` true arm (absent on X4) |
| Fast AA present under Reader → Font | ✅ | `SettingRequires::SelectableGrayscaleLut` true arm |
| No ghosting when paging | ✅ | `panelNeedsHalfRefreshSettle()` true arm — X3 genuinely needs the settle passes |
| Charging indicator | ✅ | (still SOF-ambiguous with a data cable, as on X4) |
| No I2C errors anywhere in the log | ✅ | no `lock == NULL`, no bus failures |

**Why "Got time from DS3231" is the key line.** `Wire.begin()` is called by
`HalPowerManager::begin()` under `hasI2cFuelGauge()`, using pins taken from the
**board profile** (`gauge.i2cSda=20, i2cScl=0, i2cHz=400000`) rather than the old
`X3_I2C_*` macros. `HalClock` then talks to the DS3231 at 0x68 over that same
already-initialised bus. A successful time read therefore proves the
profile-sourced pins are correct end to end — and the serial timestamps switched
to wall-clock (`19:06:09`) as a result, which is a second, independent tell.

That is `1c3a43ea`'s `Wire.begin()` conversion validated on the only board that
can validate it.

**Sampler stack on X3: `high-water=1508 bytes free`** of 2048, i.e. a ~540-byte
peak — within 8 bytes of the X4 figure. The ~532-540 byte baseline holds across
both C3 boards and both input topologies (ADC ladder on X3, same on X4).

**Re-flashed on `6f4e640f` (2026-08-16): no regressions.** That closes the one
gap left by the earlier runs — `748a7dd1` (display pins from the profile) and
`6f4e640f` (RTC address, USB-detect pin, battery ADC pin) had been
build-verified only. X3 is the right board to have confirmed them on: it is the
one that actually drives the DS3231 at the profile-sourced address, over the
profile-sourced I2C pins.

**Every workstream-B commit is therefore now hardware-validated on the C3.**

Not captured, same cause as on X4: the `[HW]` device-detection lines. They are
emitted before USB-CDC re-enumerates after reset, so a capture over the device's
own USB always misses them. A UART adapter would be needed.

### ⚙️ X4 Pro — first flash 2026-08-16: got to the home screen, then boot-looped

**Two single-core assumptions, both fixed, both found only by flashing.** Neither
is board-specific and neither was caused by this branch.

The crash was `assert failed: spinlock_acquire spinlock.h:84 (lock)`, symbolized
against a SHA-matched ELF to:

```
spinlock_acquire <- xPortEnterCriticalTimeout
  <- ActivityManager::requestUpdate()   ActivityManager.cpp:526 / :518
  <- Activity::requestUpdate()          Activity.cpp:9
  <- HomeActivity::onEnter()            HomeActivity.cpp:715
```

1. **`taskENTER_CRITICAL(nullptr)`** — four sections, eight calls, pre-existing.
   The single-core C3 port ignores the mux and just disables interrupts, so a
   null pointer is harmless there; the dual-core S3 does a real
   `spinlock_acquire()` and asserts the lock is non-null. Fixed with one
   file-static `portMUX_TYPE` (`bf8b25b3`).
2. **`xTaskCreate` for the render task** — lets the scheduler place it on either
   core. Now `xTaskCreatePinnedToCore(..., 1)` on dual-core parts (`edf203e3`),
   to keep long renders and cover decodes off CPU 0's idle watchdog, where the
   Arduino loop and system/WiFi tasks also live.

**Both were already solved on `upstream/develop`**, which was flashed on the same
board as a control and booted. Their `ActivityManager.cpp` has the identical
static `portMUX_TYPE` and the identical pinning with the same stated reason.
Keeping a known-good upstream build to hand turned a guess into a comparison and
is worth repeating for the next S3 problem.

**What the crash location already proves.** Reaching `HomeActivity::onEnter()`
means the X4 Pro had initialised the display, mounted SD and routed into the home
screen. The board profile, the profile-sourced SPI pins and the SDMMC storage
path all worked; it died on portable FreeRTOS code, not on anything
board-specific.

**Not yet re-flashed with the fixes.** Expected on the next attempt, neither a
regression: possible orientation/mirroring wrongness (the profile ships
`NO_FLIP` with the panel mount marked `PENDING hardware validation`) and a
nonsense battery reading (the CW2017 is still driven by the BQ27220 register
path — the known gap above).

### ✅ T5S3 — BOOTS AND RUNS (2026-08-17)

The first S3 board to reach a working state. Boots, mounts SD, drives the panel,
reaches the home screen, runs stably, logs, and both software-visible buttons
work.

| Evidence | Value |
|---|---|
| Heap with PSRAM live | `free=8,662,104` vs `325,956` internal |
| Largest contiguous block | `contig=6,160,372` |
| Boot to `setup_complete` | ~3.4 s (`paint+2672` dominates — a full panel refresh) |
| Sampler stack peak | **2088 bytes** (`high-water=2008 free` of 4096) |

**That stack number retroactively justifies the P1 bump.** The old size was 2048;
this board peaks at 2088. It would have overflowed. On the C3 the same path peaks
at ~540 bytes, so the GT911 servicing costs ~1120 bytes more — the 4096 was
necessary, not cautious.

#### Six fixes it took to get here

| Fix | Commit | Why the C3 never showed it |
|---|---|---|
| PSRAM (`qio_opi` + `BOARD_HAS_PSRAM`) | `26c47e57` | C3 has no PSRAM; this board's 960x540 framebuffer cannot fit in internal RAM |
| Serial init + log transport + TX timeout | `7c9f6623` | C3 has a usbDetect pin, so `Serial.begin()` ran; and its transport is plain `SERIAL` |
| `BoardT5S3::begin()` never called | `7a420191` | C3 has no board-support layer, no I2C expander, no LoRa on the SD bus |
| No CPU frequency scaling on PSRAM boards | `0a0851eb` | PSRAM clock derives from CPU/APB; C3 has no PSRAM to corrupt |
| SPI bus pins from the right peripheral | `138f47d6` | C3's panel owns the bus pins; T5S3's panel is parallel and the SD owns them |
| RST misread as a power-button wake | `17df518a` | C3 has usbDetect, so the `!usbConnected` inference is sound there |

**The pattern is worth internalising: every one was C3-shaped reasoning that held
on two boards and failed silently on a third.** That is the same defect B0
removed from `deviceIsX3()`, reappearing in build flags, bus ownership, power
management and wake classification. Expect more of it, and expect it to look like
a hardware fault rather than a logic bug.

**Method note.** Nearly every fix came from diffing against `upstream/feat-touch`
or reading the SDK's own `docs/lilygo-t5s3-support.md` — not from reasoning about
symptoms. Six rounds were spent debugging blind because the board produced no log
output at all; the single highest-value action was making logging work. Do that
first on the next board.

#### Button map (vendor-verified)

| Button | Connection | Status |
|---|---|---|
| BOOT | GPIO0 | the power button, by SDK design → `BTN_POWER` ✓ |
| "IO48" | **PCA9535 I2C expander** (the label is silkscreen, not the wiring) | user button → `BTN_DOWN` ✓ |
| **Home key** | **GT911 capacitive**, status bit `0x10` | → `BTN_CONFIRM` ✓ **device-validated 2026-08-17** |
| PWR | **unknown** — not on any pin we sample, and the vendor documents no function | see below |
| RST | hardware reset | ✓ reboots correctly since `17df518a` |

**The home key exists, and the vendor wiki does not mention it.** The wiki's
button list (*"RST + BOOT + IO48 + PWR"*) is not exhaustive — the same document
was already wrong about IO48's wiring. It was found by trace, not by reading:
`BoardConfig`'s `LILYGO_T5_PRO_GT911` also leaves `touch.hasHomeKey` false, while
`InputManager` reads the GT911 key bit unconditionally, so the key worked at the
driver level and was discarded at every consumer. **Treat vendor button lists as
a lower bound and probe the hardware.**

The vendor wiki says only *"Buttons: RST + BOOT + IO48 + PWR"* with **no
functional description**, and their reference firmware
(`examples/factory/main/utilities.h`) defines only `BOARD_BOOT_BTN (0)` and
`BOARD_PCA9535_INT (38)`. So PWR is not readable by firmware, but *why* is
unestablished. Working hypothesis: the board carries a BQ25896 charger (0x6B),
whose `/QON` pin is the conventional place to wire a power button for ship-mode
exit and power-path control. **Testable without a schematic:** on battery with
USB unplugged, hold PWR ~10 s — a hard power-off, revived by a press, confirms
`/QON`. The schematic is not in the vendor repo.

**Three software-visible inputs, one of which is power.** The home key lifts the
board from unusable to navigable, but touch phase 4 remains mandatory — see the
Back gap below.

#### The home key → CONFIRM routing, and the input path it exposed

Routed in `HalGPIO::sampleOnce()` when the board has a home key **and** leaves
`input.confirm` unassigned, so a board with a real Confirm pin is never shadowed.
`ButtonEventManager`'s existing FSM then supplies click / double / long for free.

**The bug worth remembering.** The first attempt pushed synthetic edges into the
raw edge queue only. It looked correct, logged correctly, and did nothing —
because the firmware has *two* input paths:

| Path | Read via | Consumers |
|---|---|---|
| Edge queue | `popRawEdge()` → `ButtonEventManager` | `RecentBooksActivity`, `SliderPickerActivity`, `BmpViewerActivity`, … |
| **Level/edge bitmask** | `wasPressed()` / `wasReleased()` / `isPressed()` | ~15 activities incl. `MenuListActivity` — i.e. the whole settings tree, and `HomeActivity` |

`HomeActivity` reads `wasReleased(Button::Confirm)`, so it never saw the key. The
fix injects the key into the level/edge accumulators **before they are latched**,
so both paths see it and the edge loop derives edges for free, exactly as for a
real button. **A synthetic input must be injected as low as the real ones are, or
it is only half-connected.**

The long press needed one extra turn: the SDK reports its long press *while the
key is still held* (~700 ms) and no tap follows, so releasing on that signal
classifies as Short — the opposite of the gesture. The synthetic level is instead
held until our own `LONG_PRESS_MS` (1000 ms) elapses, letting `applyTimeout()`
fire `Long`. Device trace confirming both:

```
[11506] HOMEKEY press=1 → DOWN idx=1 (CONFIRM) live=0x02
[11546] HOMEKEY tap=1   → UP   idx=1           live=0x00   → Home ▸ FileBrowser
[32686] HOMEKEY press=1 → DOWN idx=1           live=0x02
[33385] HOMEKEY long=1  held=1                             ← SDK long at ~700 ms
[33691] Exiting FileBrowser ▸ Reader                       ← classified Long
[33696] UP idx=1                                           ← released at ~1010 ms
```

#### Still open on the T5S3

- **No Back button — activities cannot be exited.** The most pressing usability
  gap. Note there is **no `BTN_BACK` action** in `BUTTON_ACTION`, and those
  actions are reader-scoped anyway, so this cannot be fixed with a setting; Back
  must be produced at the input-mapping layer. `Button::Back` resolves through
  `SETTINGS.frontButtonBack` to raw index `BTN_BACK = 0`, so the same synthesis
  the home key uses would reach every activity. Candidates: home-key **long** →
  Back (no cost to the click, but spends the long gesture), home-key **double** →
  Back (costs every single click a 300 ms `DOUBLE_WINDOW_MS` delay, since
  `hasDoubleAction()` defers the Short), IO48 long → Back, or touch phase 4.
- **Ghosting artifacts in the Reader** (observed 2026-08-17, first reader entry
  on this board). Not yet investigated; the panel is `LgfxEpd`, a different path
  from the SSD1677 boards where the `panelNeedsHalfRefreshSettle()` work was done.

- **PWR's actual function** (above).
- **The 20 MHz SD clamp is unproven.** SD may work purely because
  `BoardT5S3::begin()` deselects the LoRa radio sharing the SD SPI bus. Worth one
  40 MHz test before treating the clamp as required.
- **`BUTTON_TRACE=1` is bring-up scaffolding** in the LilyGo env and should come
  out once input work is settled. It costs the C3 nothing.
- **`Hardware detect: X4` is logged on this board** — a stale `deviceIsX3()`
  ternary in `main.cpp`. Cosmetic, but it misleads anyone reading a T5S3 log.
- **`-DFREEINK_LGFX_EPD_CONFIG`** — we set it, upstream does not. Harmless so far.

### ⬜ (superseded) T5S3 — not flashed, and now the lead S3 target

**Refocused here 2026-08-16** after the X4 Pro became unflashable mid-session
(recovery: hold BOOT/GPIO0 while resetting to force ROM download mode, then
`esptool.py erase_flash` — note GPIO0 is also the Up nav key on that board).

T5S3 starts from a much better place than the X4 Pro did: both dual-core fixes
(`bf8b25b3`, `edf203e3`) apply to it, and the I2C ownership fix below was never
X4-Pro-specific.

What differs from X4 Pro, and why most of it is *easier*:

| Aspect | X4 Pro | T5S3 |
|---|---|---|
| SD | native SDMMC | **SPI** (SCLK14 MISO21 MOSI13 CS12) — the path the C3 has always used |
| Display | SSD1677 over SPI | **`LgfxEpd`**, parallel bus, **no SPI display pins** |
| Gauge | CW2017 @ 0x63 — needs a new driver | **BQ27220 @ 0x55** — the same chip as the X3, so the existing read path should suit it as-is |
| RTC | BM8563 — needs a new driver | none (`NO_SENSORS`) |
| Touch | GT911 SDA39/SCL38, shared with gauge+RTC | GT911 SDA39/SCL40, **shared with the gauge** |

**Watch first if the screen stays blank.** The profile leaves every display pin
`PIN_UNASSIGNED` because the panel is on a parallel bus inside `LgfxEpdCon`.
`HalDisplay`'s constructor still hands those unassigned pins to `EInkDisplay`.
That is harmless *if* the LGFX path ignores them — which has not been verified.

## The I2C bus-ownership fix (`3b394eff`)

Worth understanding before the next S3 flash, because both touch boards hit it.

X4 Pro failed every I2C transaction with `ESP_ERR_INVALID_STATE` (259). Cause: a
double `Wire.begin()` on one port. The SDK's `InputManager` starts the bus for a
GT911 (`InputManager.cpp:1034`, at 100 kHz) during `inputMgr.begin()`, and our
`HalPowerManager::begin()` then started it again for the fuel gauge on the same
pins. The new ESP-IDF `i2c_master` driver fails the second call and leaves the
port unusable.

Invisible on the C3: no touch controller, so ours is the only `Wire.begin()` —
and there it is genuinely needed, since nothing else brings the bus up for the
X3's BQ27220 and DS3231. Upstream never hits it because their
`HalPowerManager` does no `Wire.begin()` at all.

The first fix gated the call. That was a patch in the wrong layer: it left a
single peripheral deciding the fate of a shared bus. `HalI2cBus` already owned
bus *serialization*, so it now owns *initialisation* too —
`HalI2cBus::ensureBusStarted()`, called from `main.cpp` right after
`gpio.begin()` so the touch driver gets first claim. It stands down when touch
already owns those pins and starts the bus otherwise.

Consequence where touch owns the bus: the gauge runs at the touch driver's
100 kHz rather than the profile's 400 kHz. Immaterial for a peripheral polled
seconds apart — sharing a bus means sharing its clock.

---

## Flash before continuing — (satisfied for X4, 2026-08-16)

**This was the blocking step, and X4 has now passed it.** Retained because the
same checklist applies to X3 and, later, to the S3 boards.

`1c3a43ea` changed how the *shipped* C3 sets up its display SPI bus, its ADC
battery pin and its fuel-gauge I2C. Every value was verified against the board
profiles one by one, but a wrong SPI pin is a **black screen, not a compile
error**, and nothing here has booted.

Flash an **X4 or X3** on `1c3a43ea` and check:

1. It boots and renders the home screen (validates `SPI.begin` from the profile).
2. SD mounts and a book opens (same bus).
3. Battery percentage is sane — on X3 that exercises the gauge I2C pins, on X4
   the ADC pin.
4. USB connect/disconnect is still detected.
5. Settings → Controls shows what it always did: X3 keeps Fast AA and the tilt
   entries, X4 has neither, neither shows touch settings. (This is a *prediction*
   from the profiles, not an observation.)
6. It boots at all after the `main.cpp` global reorder — `GfxRenderer` now
   constructs before `MappedInputManager`, which shifts static-init order in the
   TU that `BootHeapProbe` slots 4/5 bracket.

If that passes, the pin-sourcing approach is validated and the rest of B gets
much cheaper. If it fails, the bisect range is three small commits
(`b5341302`, `2799b34f`, `1c3a43ea`) rather than a dozen.

**Do not flash X4 Pro or T5S3 yet.** They still will not boot: `HalDisplay` holds
the C3 display pins, `HalClock` the DS3231 address, and X4 Pro needs the SDMMC
mount path. That is the rest of B.

---

## B0 — the board concept ✅ DONE (`b5341302`, `2799b34f`)

The single most important idea in this work, and the reason not to write X4 Pro
code first. **Implemented 2026-08-16** — the reasoning below is kept because it
is still the rule for every new call site; see "What B0 actually landed" after
it for the outcome.

`HalGPIO::DeviceType { X4, X3 }` + `deviceIsX3()` had **38 call sites** when this
was written (the "49" in the 08-15 draft was a miscount; 34 remain after B0), and
`deviceIsX3()` is a stand-in for six unrelated questions: hardware-RTC presence,
RTC-survives-deep-sleep, "has a DS3231 at 0x68", screen-tall-enough-for-a-hint,
physical-front-button-positions, and panel-needs-half-refresh-settle.

**On X4 Pro `deviceIsX3()` returns `false`, so every one of those silently takes
the X4 branch.** The board will not fail loudly; it will be subtly wrong in six
places that each look like an unrelated bug. Widening the enum to
`{X3, X4, X4Pro, LilyGo}` multiplies the conflation by four.

**Fix: capability predicates over `BoardConfig::ACTIVE`** — ask what the board can
do, not what it is:

| Predicate | Source in `BoardProfile` |
|---|---|
| `hasHardwareRtc()` | `sensors.rtcType != RtcType::None` |
| `rtcType()` | `sensors.rtcType` (`Ds3231` / `Pcf8563` / `Rx8130`) |
| `hasTouch()` | `touch.controller != TouchController::None` |
| `hasFrontlight()` / `hasColorTemperature()` | `frontlight` / `frontlight.gpioWarm` |
| `hasPhysicalButtons()` / topology | `inputStyle` |
| `uiScale()` | `uiScale` |
| battery source | `batteryGauge.gaugeAddr != 0` vs `batteryAdc` |

Legitimate exception: genuine silicon quirks (the X3 half-refresh settle passes)
key on `ACTIVE.displayController` **with a comment saying why**.

**Order:** (1) add the accessors — pure additions, no call-site churn, C3 should
come out byte-identical, which makes a clean gate; (2) convert only the ~15 sites
where X4 Pro's answer differs from X4's; (3) rename the residue to say "panel".

### What B0 actually landed

Steps 1 and 2 are done; step 3 (renaming the residue) is not.

`lib/hal/HalCapabilities.h` holds the predicates. Step 1 came out
**byte-identical on both targets**, exactly as the gate demanded. Step 2 converted
seven sites for **−2 bytes** on C3.

Predicate values were verified against the SDK profiles rather than assumed —
this table is the evidence that the C3 cannot have regressed:

| Predicate | X3 | X4 | X4 Pro |
|---|---|---|---|
| `hasTiltSensor` | Qmi8658 ✓ | none ✗ | None ✗ |
| `hasI2cFuelGauge` | 0x55 ✓ | `NO_GAUGE` ✗ | **0x63 ✓** |
| `rtcType() == Ds3231` | ✓ | None ✗ | Pcf8563 ✗ |
| `panelNeedsHalfRefreshSettle` | UC8253 ✓ | SSD1677 ✗ | SSD1677 ✗ |

Every X3/X4 column reproduces what `deviceIsX3()` returned. The X4 Pro column is
the class of bug this removes.

**Converted:** `HalTiltSensor::begin`, the `HalClock` DS3231 probe,
`HalPowerManager::begin` and its battery-path assert, and the three `HalDisplay`
half-refresh settle guards.

**One trap worth remembering.** `HalClock` asks `rtcType() == Ds3231`, *not*
`hasHardwareRtc()`. The latter is **true** on X4 Pro, whose BM8563 is
PCF8563-register-compatible and would return garbage under DS3231 register
offsets — so the obvious capability fix would have been the subtly wrong one.
Presence and protocol are different questions. The same trap applies to the fuel
gauge (see below).

**Deliberately not converted**, each needing its own reasoning: the GPIO13
battery-latch predicate (Xteink-specific; the SDK models it as `power.latch0`),
the dual X3+X4 runtime detection and `selectDevice()` calls (genuine board
identity — `deviceIsX3()` is the right question there), and the USB-poll
interval. `SettingDeviceTarget` was separately replaced by `SettingRequires`
during touch phase 3; see the touch doc.

Fold in the deliberate follow-up from `02c324cc`: the deep-sleep **wake level is
hardcoded LOW** and should read `BoardConfig::ACTIVE.input.powerActiveHigh`. Left
out on purpose — every board we build is active-LOW, so it was behaviour risk to
the shipped C3 for no present gain.

## Then B — de-hardcode the HAL (the real work) ⚙️ STARTED

Each must read `BoardConfig::ACTIVE` instead of the C3 macros in `HalGPIO.h:9-44`:

| Site | Current | Needs | State |
|---|---|---|---|
| `HalGPIO.cpp` | `SPI.begin(EPD_SCLK, SPI_MISO, …)` | board SPI pins | ✅ `1c3a43ea` |
| `HalGPIO.cpp` | `pinMode(BAT_GPIO0)`, `pinMode(UART0_RXD)` | board battery / USB detect | ✅ `1c3a43ea` |
| `HalPowerManager.cpp` | `Wire.begin(X3_I2C_SDA, …)` | board I2C bus(es) | ✅ `1c3a43ea` |
| `HalDisplay.cpp:17` | `EInkDisplay(EPD_SCLK, EPD_MOSI, …)` | board display pins | ✅ `748a7dd1` |
| `HalGPIO.cpp:531` | `digitalRead(UART0_RXD)` | per-board USB detection | ✅ `6f4e640f` |
| `HalPowerManager.cpp:344` | `BatteryMonitor(BAT_GPIO0)` | ADC pin from profile | ✅ `6f4e640f` |
| `HalClock.cpp:19` | `DS3231_ADDRESS = 0x68` | address from profile | ✅ `6f4e640f` |

**Every pin and address in the original B table is now sourced from
`BoardConfig::ACTIVE`.** The C3 macros in `HalGPIO.h:9-44` remain defined but are
no longer read by the HAL.

### What is left in B

| Item | Why it is not done |
|---|---|
| ~~`HalStorage` SDMMC mount for X4 Pro~~ | **Corrected 2026-08-16: little or nothing to do.** The SDK already handles it — `SDCardManager` is device-agnostic and selects the backend from `FREEINK_SD_SDMMC`, which is **auto-true for `FREEINK_DEVICE_X4PRO`**; `SdmmcBlockDevice` drives the `sd.powerEnable` (GPIO5, active-LOW) pulse itself; and `USE_BLOCK_DEVICE_INTERFACE=1` is already in the env. `HalStorage` only ever calls `SDCard.begin()` and backend-neutral methods. Earlier drafts called this "the largest remaining piece" — that was inferred from the plan, not from reading the SDK. **Unknown until hardware: whether it actually mounts.** |
| CW2017 gauge driver | X4 Pro reports no battery today. The read path is still BQ27220-specific (`I2C_ADDR_BQ27220`, `BQ27220_*_REG`); the gauge address alone is not enough. |
| BM8563 RTC driver | X4 Pro has no hardware clock today. `HalClock` correctly *skips* rather than mis-drives it, so this is missing function, not a bug. |
| `HalDisplay::begin()`'s `setDisplayX3()` | Still `deviceIsX3()`, and arguably correctly so — it selects the X3 panel facade within the dual C3 binary, which is board identity rather than capability. Revisit only if a non-Xteink UC8253 board appears. |
| Deep-sleep wake level | Still hardcoded LOW; should read `input.powerActiveHigh`. Deliberate hold-over from `02c324cc` — every board we build is active-LOW, so converting is behaviour risk to the shipped C3 for no present gain. |

**The milestone to aim at is unchanged: X4 Pro boots, mounts SD, renders a page.**
With the pin work done and storage handled by the SDK, there may be **no known
code blocker left** — the next honest step is to flash an X4 Pro and find out
what actually breaks, rather than to keep writing code against guesses.

### Found while checking storage: a spurious lock coupling on SDMMC boards

`HalStorage::StorageLock` takes `HalSpiBus::Lock` as its first member, so **every
SD operation holds the SPI bus lock**. That is correct on the C3, where the panel
and the SD card genuinely share one bus. On X4 Pro the SD card is on SDMMC and
shares nothing with the display, so the coupling is spurious: SD reads would
serialize against panel refreshes for no reason.

Not fixed yet, deliberately. It is a **performance** issue, not correctness, on a
board that does not boot yet, and the fix is more invasive than it looks —
`spiLock` is a RAII member whose declaration order is load-bearing (see the
comment in `HalStorage.cpp`), so making it conditional means an optional member
rather than an `if`. The gate would be `BoardConfig::ACTIVE.sdmmc.busWidth == 0`
(i.e. "SD really is on the SPI bus"), which is identity on the C3.

### What `1c3a43ea` landed, and the two bugs it found

Every macro was checked against the X3/X4 profiles before converting, so the C3
drives exactly the pins it always did:

```
EPD_SCLK  8 = display.sclk      SPI_MISO   7 = sd.miso
EPD_MOSI 10 = display.mosi      BAT_GPIO0  0 = batteryAdc
EPD_CS   21 = display.cs        UART0_RXD 20 = usbDetect
X3_I2C_SDA/SCL/FREQ 20/0/400000 = batteryGauge.i2cSda/i2cScl/i2cHz
```

Two gates that looked like device checks were really **pin-conflict** checks, and
both were latent X4 Pro bugs:

- The ADC-battery `pinMode` was gated on `deviceIsX4()`. On an S3 build
  `_deviceType` is never assigned — the C3 fingerprint probe is skipped by
  `if constexpr (buildTargetsXteinkC3())` — so it **defaults to `X4` and the test
  is true on X4 Pro**, whose `batteryAdc` is `PIN_UNASSIGNED`. It would have
  called `pinMode(-1)` at boot. Now gated on `hasAdcBattery()`.
- The USB-detect `pinMode` had the same gate. The real constraint is that the pin
  must not be shared with the fuel-gauge I2C bus: X3 lists `usbDetect = GPIO20`,
  which is also its gauge SDA and must stay an I2C line. X4 has no gauge and owns
  GPIO20; X4 Pro leaves `usbDetect` unassigned.

**That `_deviceType` default is worth internalising**: on any S3 board every
surviving `deviceIsX4()` reads true and every `deviceIsX3()` reads false, silently.

**Not converted, on purpose: the gauge read protocol.** `I2C_ADDR_BQ27220` and the
`BQ27220_*_REG` offsets stay hardcoded. X4 Pro's CW2017 answers at a different
address with different registers, so taking only the address from the profile
would talk BQ27220 registers to a CW2017 — the same presence-vs-protocol trap as
DS3231-vs-BM8563. Both gauges need their own read path.

X4 Pro also mounts SD over **SDMMC**, not SPI (`USE_BLOCK_DEVICE_INTERFACE=1` is
already in its env) — that reaches into `HalStorage`.

**Milestone to aim at: X4 Pro boots, mounts SD, renders a page.** Nothing else.

---

## Open decisions

1. **PSRAM.** Both S3 boards have 8 MB; neither env enables it (deliberate).
   Recommendation in the plan: enable but *don't spend* — divergent allocation
   policy per board would make every heap bug board-specific and strand the
   accumulated C3 heap knowledge.
2. ~~**Touch P1 — who services the I2C?**~~ **CLOSED 2026-08-16, implemented in
   `82be8458`.** Resolved as a HAL-owned I2C mutex (`lib/hal/HalI2cBus.h`) plus a
   sampler stack bump 2048 → 4096 under `FREEINK_CAP_TOUCH`. Two findings forced
   it: `InputManager::serviceTouch()` is **private** and `update()` always
   services touch, so buttons and touch cannot be split across tasks without an
   SDK change; and the `XTEINK_X4_PRO` profile wires **GT911 (0x5D), CW2017 gauge
   (0x63) and BM8563 RTC (0x51) all to bus 0** on SDA39/SCL38, so upstream's
   `batteryGauge.i2cBus`/`Wire1` separation (`a5109872`) is unavailable on the
   lead board. Note the SDK *does* ship a sampler — `beginAsync()` runs task
   `fi_input` at a 4096 stack — which is where the 4096 figure comes from; the
   earlier "upstream has no sampler, nothing to copy" note was wrong about the
   SDK. Full reasoning in the touch doc §5. Carry `i2cBus` anyway for
   LilyGo/Sticky. Re-measure `samplerStackHighWater()` on real hardware — 4096 is
   the SDK's number for its own task, not a measurement of ours.
3. **FUI adoption** (touch doc §3). Upstream replaced `MenuListActivity` with
   `UiListActivity` on FreeInkUI and their touch stack sits on top of it. It is a
   per-screen opt-in mixin, so incremental — but 34 of our 76 activities have no
   upstream counterpart. Explicitly *not* blocking board support.
4. **`FREEINK_FRONTLIGHT_LS`** — port with the frontlight work, or earlier?
5. ~~`papermono` / `sticky` in scope at all?~~ **Answered 2026-08-16: no — X4 Pro
   and T5S3 only.** Fewer board profiles linked, less flash, and no envs we cannot
   validate on hardware.
6. **When does the X4 Pro fuel gauge and RTC get a driver?** Both are currently
   *correctly skipped* rather than wrong: `HalClock` gates on
   `rtcType() == Ds3231` and the gauge read path is still BQ27220-specific. X4 Pro
   therefore has no battery reading and no hardware clock until someone writes the
   CW2017 and BM8563 paths. Not blocking "boots, mounts SD, renders a page".

---

## Gotchas that cost time

- **`Failed to install Python dependencies into penv` is not about your penv.**
  The pioarduino platform pins `platformio` to a GitHub zip, but that zip installs
  as `pioarduino` / `pioarduino-core` — **never** as `platformio`. The lookup in
  `penv_setup.py::get_packages_to_install()` therefore never matches, so the zip
  is re-downloaded on *every* build until GitHub answers **429** and the build
  dies with that message. The real error is invisible because the platform sends
  the install's stderr to `DEVNULL`. Workaround needing no changes:
  `unshare -rn "$PIO" run -e lilygo_t5s3` (deps are already present, so the
  skipped fetch is a no-op). Permanent fix is a one-block patch to that file in
  `~/.platformio` — **a platform update reverts it.**
- **`platformio.local.ini` silently breaks the C3 build after `b4b94068`.** That
  file is gitignored, is listed in `extra_configs`, and typically overrides
  `[env:default]` with `build_flags = ${base.build_flags} …`. Since workstream A
  moved the `FREEINK_DEVICE_*` set out of `[base]` into `[c3]`/`[s3]`, any local
  override that extends `[base]` now compiles with **no device selected** and
  dies in `BoardConfig.h` with *"no device selected"* **and** *"all selected
  devices must share one MCU family"* at once — a confusing pair that looks like
  a submodule or toolchain problem, not a local-config one. Fix: use
  `${c3.build_flags}`. Anyone with a local ini hits this on their first pull of
  the branch; the 08-15 numbers above were measured on Windows, where no such
  file existed.
- **`pio run ... | tail` reports `tail`'s exit code.** A failed build shows
  "exited with code 0". Always grep the output for `SUCCESS` / `FAILED`, or don't
  pipe.
- **`BoardConfig.h:88` hard-`#error`s on mixed MCU families.** This is a feature —
  an S3 binary cannot silently carry C3 profiles — but it means a misconfigured
  env fails at the *first* SDK translation unit with a confusing message.
- **X4 Pro + T5S3 *can* share a binary** (both `FREEINK_MCU_S3`); C3 and S3 never
  can. Whether a combined S3 binary is affordable is a flash question, still open.
- **Don't reason about flash from "text size".** The 2.22 MB figure in older notes
  is text only; the linked image is 6.38 MB of a 6.55 MB partition. Quote the
  `Flash:` line from a real build.
- **The SDK is a git submodule and moves fast.** Bump it in its own commit with
  its own C3 regression build; never fold it into board work.
- **A stale in-flight build is worthless after an SDK pull.** Kill and restart —
  it will otherwise mix old and new headers.

## Verification recipe

```bash
PIO="$HOME/.platformio/penv/Scripts/pio.exe"   # Windows; not on PATH
PIO="$HOME/.platformio/penv/bin/pio"           # Linux
"$PIO" run -e default      # C3 regression gate — MUST stay green
"$PIO" run -e x4pro
"$PIO" run -e lilygo_t5s3
```

Check `platformio.local.ini` first (see gotchas) — on Linux it is the most
likely reason a build fails for reasons that have nothing to do with your change.

Cold builds are ~13 min each; run them backgrounded. Gate for any B0/B change is
**zero flash/RAM delta on `env:default`** — B0 in particular should be
byte-identical, since it only adds unused accessors.

## Reference

- Upstream touch/board branches (**corrected 2026-08-16**):
  `feat-x4-papermono-support` has been **deleted**. The touch stack —
  `HalGPIO` passthrough, `MappedInputManager(gpio, renderer)`, `UiListActivity`,
  `ReaderUtils` — is now on **`upstream/develop`** (mainline). The active board
  branch is **`upstream/feat-touch`**, 42 commits ahead of develop, and it
  carries reference implementations for most of the workstream-B table above
  (SPI init `28d8ad56`, USB detect `c812091a`, battery `6acecd8e`, RTC/IMU HAL
  `06ff5aa4`, multi-I2C `a5109872`, LilyGo T5S3 `526cf1b6`). See §0.2 of the
  touch doc — **B is cheaper than this handover assumed.**
- crosspoint PR **#3032** — frontlight survives light sleep; **replaces** the old
  mutual exclusion. Do not port the exclusion as the design.
- SDK PRs **#38** (`FREEINK_X4PRO_FAST_DU_SHORTCUT`, opt-in, left off — validate
  on hardware first) and **#39** (RC_FAST frontlight + EpdBus BUSY grace).
- Our fork vs `upstream/develop`: **2973 ahead / 1153 behind** (2026-08-16). We
  are an independent product, not a tracking fork.
